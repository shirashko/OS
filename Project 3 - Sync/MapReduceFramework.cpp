#include "MapReduceFramework.h"
#include "Barrier.h"
#include <pthread.h>
#include <semaphore.h>
#include <iostream>
#include <atomic>
#include <algorithm>
#include <memory>

const std::string SYSTEM_ERROR_MESSAGE = "system error: ";

const int PTHREAD_LIBRARY_SUCCESS = 0;
const int MAX_INT_31_BIT = 0x7FFFFFFF;
const int STAGE_BITS_SHIFT = 62;
const int TOTAL_KEYS_BITS_SHIFT = 31;

struct JobContext;
void system_failure_handler(const char* message);
bool isEqual(K2* key1, K2* key2);
K2 *findMaxKey(JobContext *job_context);

struct ThreadContext {
    unsigned int thread_id;
    IntermediateVec intermediate_mapped_vec; // The data after the map stage
    JobContext* job_context; // The job context that the thread belongs to
    int cur_total_keys_to_reduce;

    /**
     * @brief Constructor for ThreadContext.
     *
     * @param id The thread ID.
     * @param job_ctx The job context that the thread belongs to.
     */
    ThreadContext(unsigned int id, JobContext* job_ctx): thread_id(id),
                                                         job_context(job_ctx),
                                                         cur_total_keys_to_reduce(0) {}
};

struct JobContext {
    // User provide
    const MapReduceClient* client;
    const InputVec* input_data;
    OutputVec* output_data;
    int multi_thread_level;

    std::vector<pthread_t> pthreads;
    std::vector<ThreadContext*> thread_contexts;

    bool wait_for_job_was_called_flag; // Flag to prevent multiple runs of waitForJob

    std::vector<IntermediateVec>* all_sorted_inter_data;
    std::vector<IntermediateVec>* shuffled_data;

    // Sync related
    Barrier* barrier; // Before the shuffle stage (wait until all the pthreads finish the sort stage)
    pthread_mutex_t output_mutex; // For emit3 sync (output vector is shared)
    pthread_mutex_t reduce_mutex; // For reduce sync (shuffle vector is shared)
    std::atomic<int>* atomic_input_data_next_available_idx;
    pthread_mutex_t update_stage_mutex;
    std::atomic<int>* atomic_number_of_intermediate_pairs;
    std::atomic<uint64_t>* atomic_job_state_variables;

    /**
     * @brief Constructor for JobContext.
     *
     * @param client A reference to the MapReduceClient which contains the map and reduce functions.
     * @param input_data A reference to the input vector containing the input data pairs.
     * @param output_data A reference to the output vector where the output data pairs will be stored.
     * @param multiThreadLevel An integer specifying the number of threads to be used for the job.
     */
    JobContext(const MapReduceClient* client, const InputVec* input_data, OutputVec* output_data, int multiThreadLevel):
            client(client),
            input_data(input_data),
            output_data(output_data),
            multi_thread_level(multiThreadLevel),
            pthreads(multiThreadLevel),
            thread_contexts(),
            wait_for_job_was_called_flag(false),
            all_sorted_inter_data(new std::vector<IntermediateVec>(multiThreadLevel)),
            shuffled_data(new std::vector<IntermediateVec>()),
            barrier(new Barrier(multiThreadLevel)),
            output_mutex(PTHREAD_MUTEX_INITIALIZER),
            reduce_mutex(PTHREAD_MUTEX_INITIALIZER),
            atomic_input_data_next_available_idx(new std::atomic<int>(0)),
            update_stage_mutex(PTHREAD_MUTEX_INITIALIZER),
            atomic_number_of_intermediate_pairs(new std::atomic<int>(0)),
            atomic_job_state_variables(new std::atomic<uint64_t>(0))
    {
        // Initialize state variables
        uint64_t init_value = (uint64_t) input_data->size() << TOTAL_KEYS_BITS_SHIFT | (uint64_t) UNDEFINED_STAGE << STAGE_BITS_SHIFT;
        *atomic_job_state_variables = init_value;
    }


    /**
     * @brief Destructor for JobContext.
     */
    ~JobContext() {
        if (pthread_mutex_destroy(&update_stage_mutex) != 0) {
            system_failure_handler("error on pthread_mutex_destroy");
        }

        if (pthread_mutex_destroy(&output_mutex) != PTHREAD_LIBRARY_SUCCESS) {
            system_failure_handler("error on pthread_mutex_destroy");
        }

        if (pthread_mutex_destroy(&reduce_mutex) != PTHREAD_LIBRARY_SUCCESS) {
            system_failure_handler("error on pthread_mutex_destroy");
        }

        for (auto thread_context : thread_contexts) {
            delete thread_context;
        }

        delete barrier;
        delete all_sorted_inter_data;
        delete shuffled_data;
        delete atomic_input_data_next_available_idx;
        delete atomic_number_of_intermediate_pairs;
        delete atomic_job_state_variables;
    }

    stage_t getJobStateFromAtomic() const {
        return static_cast<stage_t>(atomic_job_state_variables->load() >> STAGE_BITS_SHIFT);
    }

    /*
     * First thread arrives need to change to map prev_stage (use update_stage_mutex to avoid percentage override)
     */
    void updateState(stage_t prev_stage, stage_t new_stage, int total) {
        if (pthread_mutex_lock(&update_stage_mutex) != PTHREAD_LIBRARY_SUCCESS) {
            system_failure_handler("failed to lock a update_stage_mutex");
        }

        if (getJobStateFromAtomic() == prev_stage) {
            uint64_t map_init_state = (uint64_t) total << TOTAL_KEYS_BITS_SHIFT | (uint64_t) new_stage << STAGE_BITS_SHIFT;
            *atomic_job_state_variables = map_init_state;
        }

        if (pthread_mutex_unlock(&update_stage_mutex) != PTHREAD_LIBRARY_SUCCESS) {
            system_failure_handler("failed to unlock a update_stage_mutex");
        }
    }
};

/**
 * @brief This function is called by the map function of the client (MapReduceClient) to emit a key-value
 * pair to the framework.
 * @param key The key to emit.
 * @param value The value to emit.
 * @param context The context of the thread calling this function.
 */
void emit2 (K2* key, V2* value, void* context) {
    // The key-value pair is emitted to the intermediate_mapped_vec of the thread that called the map function.
    auto* thread_context = static_cast<ThreadContext*>(context);
    thread_context->intermediate_mapped_vec.emplace_back(key, value);
    thread_context->job_context->atomic_number_of_intermediate_pairs->fetch_add(1);
}

/**
 * @brief This function is called by the reduce function of the client (MapReduceClient) to emit a key-value
 * pair to the output vector.
 * Since the outputVec is shared resource for all the threads, the function ensures that the output vector is
 * updated in a thread-safe manner using semaphores.
 * @param key The key to emit.
 * @param value The value to emit.
 * @param context The context of the job calling this function.
 */
void emit3 (K3* key, V3* value, void* context) {
    auto* thread_context = static_cast<ThreadContext*>(context);
    auto* job_context = thread_context->job_context;

    if (pthread_mutex_lock(&job_context->output_mutex) != PTHREAD_LIBRARY_SUCCESS) {
        system_failure_handler("failed to lock a update_stage_mutex");
    }

    job_context->output_data->emplace_back(key, value);

    if (pthread_mutex_unlock(&job_context->output_mutex) != PTHREAD_LIBRARY_SUCCESS) {
        system_failure_handler("failed to unlock a update_stage_mutex");
    }

    job_context->atomic_job_state_variables->fetch_add(thread_context->cur_total_keys_to_reduce);
}

/**
 * @brief Executes the map stage for the given thread context. Each thread will take a pair from the input
 * data and call the map function of the client, until all the input data is processed.
 * @param thread_context The context of the thread executing this function.
 */
void mapStage(ThreadContext* thread_context) {
    JobContext* job_context = thread_context->job_context;
    auto* input_data = job_context->input_data;
    int input_data_cur_idx = job_context->atomic_input_data_next_available_idx->fetch_add(1);

    // Each thread will take a pair from the input data and call the map function of the client, until all the input
    // data is processed. Since the input data is shared between the threads, we are using atomic variable to prevent
    // threads from accessing the same data.
    while(input_data_cur_idx < (int) input_data->size()) {
        const auto& input_pair = input_data->at(input_data_cur_idx);
        thread_context->job_context->client->map(input_pair.first, input_pair.second, thread_context);
        // Finished process input pair (for example a string) - not relate to how many intermediate pairs this input data pair produced
        job_context->atomic_job_state_variables->fetch_add(1);
        input_data_cur_idx = job_context->atomic_input_data_next_available_idx->fetch_add(1);
    }
}

/**
 * Comparator by key for the sort stage.
 * @param pair1 to compare with
 * @param pair2 to compare with
 * @return boolean value indicating if the pairs are equal.
 */
bool comparatorByKey (const IntermediatePair &pair1, const IntermediatePair &pair2) {
    return *pair1.first < *pair2.first;
}

/**
 * @brief Sorts the intermediate data by key for the given thread context.
 * @param thread_context The context of the thread executing this function.
 */
void sortStage(ThreadContext *thread_context) {
    // We sort the intermediate data by the key, so the shuffle stage will be more efficient.
    std::sort(thread_context->intermediate_mapped_vec.begin(), thread_context->intermediate_mapped_vec.end(),
              comparatorByKey);
}

/*
 * Thread 0 will shuffle the intermediate data of all the threads.
 * It will go over all the intermediate data in a loop.
 * In each iteration, it will choose the maximum from the back of each thread vector which contains its maximum key.
 * After choosing the maximum key, it will go over all the intermediate data of all the threads from the back
 * and will choose the data with the same key as the maximum key and insert it into a new key_vector vector of
 * intermediate pairs. Note that it is possible that the maximum key will be found not only in the last element of the
 * vector but also in earlier positions. In this case, the thread will continue to search for the maximum key in the
 * previous elements of the vector until it will encounter a smaller key.
 * After the thread has finished going over all the intermediate data, it will insert the key_vector vector into the
 * shuffled_data vector of the job context.
 */
/**
 * @brief Thread 0 will shuffle the intermediate data of all the threads. It will go over all the
 * intermediate data in a loop.
 *
 * @param thread_context The context of the thread.
 */
void shuffleStage(ThreadContext *thread_context) {
    JobContext *job_context = thread_context->job_context;
    auto &all_sorted_inter_data = *job_context->all_sorted_inter_data;

    while (true) {
        K2 *max_key = findMaxKey(job_context);
        if (max_key == nullptr) {
            break; // no more keys to process
        }

        // Create a key_vector vector of all the intermediate pairs with the key and insert it into the shuffled_data
        std::vector<IntermediatePair> key_vector;

        for (auto &cur_vector_inter_data: all_sorted_inter_data) {
            // Make sure the current vector is not empty
            if (cur_vector_inter_data.empty()) {
                continue;
            }

            // Check if the key is equal to the max key until we encounter a smaller key
            while (!cur_vector_inter_data.empty() && isEqual(cur_vector_inter_data.back().first, max_key)) {
                key_vector.push_back(cur_vector_inter_data.back());
                job_context->atomic_job_state_variables->fetch_add(1);
                cur_vector_inter_data.pop_back();
            }
        }
        job_context->shuffled_data->push_back(key_vector);
    }
}


/**
 * @brief The reduce stage function for each thread. It goes over all the intermediate shuffled key vectors
 * and calls the reduce function of the client.
 * In this stage, we don't longer have a vector dedicated for each thread, but a lot of data we just want
 * to process, and each thread will try to process as much as possible.
 *
 * @param thread_context The context of the thread.
 */
void reduceStage(ThreadContext *thread_context) {
    JobContext *job_context = thread_context->job_context;
    auto &shuffled_data = *job_context->shuffled_data;
    auto &client = *job_context->client;

    while (true) {
        if (pthread_mutex_lock(&job_context->reduce_mutex) != 0) {
            system_failure_handler("failed to lock reduce_mutex");
        }

        if (shuffled_data.empty()) {
            if (pthread_mutex_unlock(&job_context->reduce_mutex) != 0) {
                system_failure_handler("failed to unlock reduce_mutex");
            }
            break;
        }

        auto key_vector = shuffled_data.back();
        shuffled_data.pop_back();

        if (pthread_mutex_unlock(&job_context->reduce_mutex) != 0) {
            system_failure_handler("failed to unlock reduce_mutex");
        }

        thread_context->cur_total_keys_to_reduce = (int) key_vector.size();
        client.reduce(&key_vector, thread_context);
    }
}

/**
 * @brief The entry point function for each thread.
 *
 * @param context The context of the thread.
 * @return void* The return value of the thread.
 */
void* threadEntryPoint(void* context) {
    auto* thread_context = static_cast<ThreadContext *>(context);
    JobContext* job_context = thread_context->job_context;

    // Can assume number of pairs is up to 2^31 - 1 (int max value)
    job_context->updateState(UNDEFINED_STAGE, MAP_STAGE, (int) job_context->input_data->size());

    mapStage(thread_context);
    sortStage(thread_context);

    // all_inter_data need to be initialized and populated before the shuffle stage
    job_context->all_sorted_inter_data->at(thread_context->thread_id) = thread_context->intermediate_mapped_vec;

    job_context->barrier->barrier();

    if (thread_context->thread_id == 0) {
        job_context->updateState(MAP_STAGE, SHUFFLE_STAGE, job_context->atomic_number_of_intermediate_pairs->load());
        shuffleStage(thread_context);
    }

    // Wait for the shuffle stage to end
    job_context->barrier->barrier();
    job_context->updateState(SHUFFLE_STAGE, REDUCE_STAGE, job_context->atomic_number_of_intermediate_pairs->load());

    reduceStage(thread_context);

    return nullptr;
}

/**
 * Starts a MapReduce job using the given client, input vector, and output vector with the specified number of threads.
 * This function initializes the job context and the thread contexts, creates the specified number of threads,
 * and starts the MapReduce process. The job status is initially set to the MAP_STAGE with 0% completion.
 *
 * @param client A reference to the MapReduceClient which contains the map and reduce functions.
 * @param inputVec A reference to the input vector containing the input data pairs.
 * @param outputVec A reference to the output vector where the output data pairs will be stored.
 * @param multiThreadLevel An integer specifying the number of threads to be used for the job.
 * @return JobHandle A handle to the created MapReduce job.
 *
 * @throws std::runtime_error If a thread cannot be created, the function will call the system_failure_handler.
 */
JobHandle startMapReduceJob(const MapReduceClient& client, const InputVec& inputVec, OutputVec& outputVec, int multiThreadLevel) {
    // Initialize the job context and the pthreads contexts
    auto* job_context = new JobContext(&client, &inputVec, &outputVec, multiThreadLevel);

    // We will define the input vector size to avoid dividing by zero in percentage calculation in getJobState
    uint64_t init_value = (uint64_t) inputVec.size() << TOTAL_KEYS_BITS_SHIFT | (uint64_t) UNDEFINED_STAGE << STAGE_BITS_SHIFT;
    job_context->atomic_job_state_variables->store(init_value);

    for (int i = 0; i < multiThreadLevel; ++i) {
        auto* thread_context = new ThreadContext(i, job_context);
        job_context->thread_contexts.push_back(thread_context);
        if(pthread_create(&job_context->pthreads[i], nullptr, threadEntryPoint, thread_context) != PTHREAD_LIBRARY_SUCCESS) {
            system_failure_handler("failed to create a thread");
        }
    }
    return job_context;
}

/**
 * Waits for the job to finish. If the job has already been waited for, this function does nothing.
 * @param job The job to wait for.
 */
void waitForJob(JobHandle job) {
    auto* job_context = static_cast<JobContext*>(job);

    // Prevents calling pthread_join twice
    if(job_context == nullptr || job_context->wait_for_job_was_called_flag) {
        return;
    }
    job_context->wait_for_job_was_called_flag = true;

    for(int i = 0; i < job_context->multi_thread_level; i++) {
        if(pthread_join(job_context->pthreads[i], nullptr) != PTHREAD_LIBRARY_SUCCESS) {
            system_failure_handler("failed to join a thread");
        }
    }
}

/**
 * Gets the current state of the job.
 * @param job The job to get the state of.
 * @param state The state of the job.
 */
void getJobState(JobHandle job, JobState* state) {
    auto* job_context = static_cast<JobContext*>(job);
    uint64_t job_state = job_context->atomic_job_state_variables->load();

    // Extract the number of total keys to process by the lower 31 bits from the job_state variable
    uint64_t total_keys = (job_state >> TOTAL_KEYS_BITS_SHIFT) & MAX_INT_31_BIT;

    // Extract the stage (note to our selves: do not use getJobStateFromAtomic because we need sync between stage and percentage)
    auto stage = static_cast<stage_t>(job_state >> STAGE_BITS_SHIFT);

    // Extract the number of processed keys
    uint64_t processed_keys = job_state & MAX_INT_31_BIT;

    // Calculate the percentage of completion with safety of zero division
    float percentage = (total_keys == 0) ? 0 : (100.0f * float(processed_keys) / float(total_keys));

    // Update the state
    state->stage = stage;
    state->percentage = percentage;
}

/**
 * Closes the job handle and frees all resources associated with it. Before releasing the job handle, the function
 * waits for the job to finish (for all pthreads to finish).
 * @param job
 */
void closeJobHandle(JobHandle job) {
    auto* job_context = static_cast<JobContext*>(job);
    waitForJob(job); // Wait for all the threads to finish before closing the job handle
    delete job_context;
}

// * helper functions *

/**
 * @brief This function is called when a system error occurs. It prints the error message and exits the program
 * with an error.
 * @param message The error message to be printed.
 */
void system_failure_handler(const char* message) {
    std::cout << SYSTEM_ERROR_MESSAGE << message << std::endl;
    exit(1);
}


/**
 * @brief Compares two keys for equality.
 * @param key1 The first key to compare.
 * @param key2 The second key to compare.
 * @return True if the keys are equal, false otherwise.
 */
bool isEqual(K2* key1, K2* key2) {
    return ! (*key1 < *key2) and ! (*key2 < *key1);
}

/**
 * @brief Finds the maximum key among all the intermediate data of all threads.
 * @param job_context The context of the job.
 * @return The maximum key found.
 */
K2 *findMaxKey(JobContext *job_context) {
    auto &all_sorted_inter_data = *job_context->all_sorted_inter_data;
    K2 *max_key = nullptr;
    for (const auto &cur_vector_inter_data: all_sorted_inter_data) {
        if (cur_vector_inter_data.empty()) {
            continue;
        }
        auto *key = cur_vector_inter_data.back().first;
        if (max_key == nullptr || *max_key < *key) {
            max_key = key;
        }
    }
    return max_key;
}
