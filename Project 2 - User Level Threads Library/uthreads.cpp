#include "uthreads.h"
#include <csetjmp>
#include <iostream>
#include <queue>
#include <unordered_map>
#include <set>
#include <list>
#include <csignal>
#include <sys/time.h>

#define JB_SP 6
#define JB_PC 7
#define MAIN_THREAD_ID 0
#define LIBRARY_FAILURE (-1)
#define LIBRARY_SUCCESS 0
#define MICROSECONDS_PER_SECOND 1000000
#define DEFAULT_TID (-1)
#define MIN_TID 0
#define MAX_TID 99
#define SIGSET_FAILURE (-1)
#define SIGPROCMASK_FAILURE (-1)
#define FORCED_PREEMPTION (-1)

#define LIB_ERR(text) \
    std::cerr << "thread library error: " << text << std::endl;

#define SYS_ERR(text) \
    std::cerr << "system error: " << text << std::endl;

// Error messages
const char* SYS_ERROR_SIGACTION = "sigaction error";
const char* SYS_ERROR_SETITIMER = "setitimer error";
const char* SYS_ERROR_ALLOC_SCHEDULER = "failed to allocate memory for scheduler";
const char* SYS_ERROR_ALLOC_MAIN_THREAD = "failed to allocate memory for main thread";
const char* SYS_ERROR_ALLOC_THREAD = "failed to allocate memory for new thread";

const char* LIB_ERROR_POSITIVE_QUANTUM = "quantum_usecs must be positive";
const char* LIB_ERROR_ENTRY_POINT_NULL = "entry_point is null";
const char* LIB_ERROR_MAX_THREADS = "failed to find a free tid, reached the maximum number of threads";
const char* LIB_ERROR_THREAD_NOT_FOUND = "thread not found";
const char* LIB_ERROR_TID_OUT_OF_RANGE = "thread is out of valid range (0-99)";
const char* LIB_ERROR_BLOCK_MAIN_THREAD = "can't block the main thread";
const char* LIB_ERROR_SLEEP_MAIN_THREAD = "the main thread can't be put to sleep";
static const char *const SYS_ERROR_SIGEMPTYSET_FAILURE = "sigemptyset failed";
static const char *const SYS_ERR_SIGADDSET = "sigaddset failed";
static const char *const SYS_ERR_SIGPROCMASK = "sigprocmask failed";

typedef unsigned long address_t;

address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
            : "=g" (ret)
            : "0" (addr));
    return ret;
}

void sys_err(const char* text);
void contextSwitchHandler(int sig);
bool isTidInValidRange(int tid);

/**
 * @class TCB
 * @brief Thread Control Block class representing a thread. Each thread has a unique TID, a stack, a state,
 * and a quantum count. The quantum count is the number of quantum passed where the thread was in RUNNING state.
 */
class TCB {
public:
    /**
     * @enum ThreadState
     * @brief Enumeration representing the state of a thread.
     */
    enum ThreadState {
        RUNNING, BLOCKED, READY
    };

private:
    int tid;                    // Thread ID
    char *stack;                // Pointer to dynamically allocated stack
    sigjmp_buf env{};           // Environment buffer for setjmp/longjmp
    int quantumCount;           // Number of quantums passed where the thread was in RUNNING state.
    ThreadState state;          // Current state of the thread

public:
    /**
     * @brief Constructor for TCB.
     * @param id Thread ID.
     * @param entry_point Entry point function for the thread.
     */
    TCB(int id, thread_entry_point entry_point) {
        tid = id;
        quantumCount = 0;
        state = READY;

        // Allocate stack dynamically
        stack = new char[STACK_SIZE];

        if (id == MAIN_THREAD_ID) {
            // use the current stack and PC
            tid = id;
            state = RUNNING;
            sigsetjmp(env, 1);
            return;
        }

        auto sp = (address_t) stack + STACK_SIZE - sizeof(address_t);
        auto pc = (unsigned long) entry_point;
        sigsetjmp(env, 1);
        (env->__jmpbuf)[JB_SP] = translate_address(sp);
        (env->__jmpbuf)[JB_PC] = translate_address(pc);
        if (sigemptyset(&env->__saved_mask) == SIGSET_FAILURE) {
            sys_err(SYS_ERROR_SIGEMPTYSET_FAILURE);
        }
    }

    /**
     * @brief Sets the state of the thread to the given newState.
     * @param newState The new state to set the thread to.
     */
    void setState(ThreadState newState) {
        state = newState;
    }

    sigjmp_buf &getEnv() {
        return env;
    }

    /**
     * @brief Returns the number of quantums passed where the thread was in RUNNING state.
     * @return The number of quantums passed where the thread was in RUNNING state.
     */
    int getQuantums() const {
        return quantumCount;
    }

    /**
     * @brief Returns the state of the thread.
     * @return The state of the thread.
     */
    ThreadState getState() const {
        return state;
    }

    int getTid() const {
        return tid;
    }

    /**
     * @brief Increments the quantum count of the thread.
     */
    void incrementQuantums() {
        quantumCount++;
    }

    /**
     * @brief Destructor for TCB.
     */
    ~TCB() {
        delete[] stack;  // Free the dynamically allocated stack
    }

};

/**
 * @class Scheduler
 * @brief Scheduler class managing threads. The scheduler is responsible for managing the threads data structures,
 * the timer signal, and the context switching logic.
 */
class Scheduler {

private:
    int totalQuantumCount;
    int currentRunningTid;
    int quantum_usecs;
    std::unordered_map<int, TCB *> threadTable; //matching TID to TCB

    struct sigaction sa{};
    struct itimerval timer{};
    std::list<int> readyThreadsList;
    std::set<int> blockedThreadsSet;
    std::unordered_map<int, int> sleepingThreadsMap; // key: tid, value: sleep time

    bool shouldDeleteLastRanThread = false;

public:

    int last_ran_tid = MAIN_THREAD_ID;

    /**
     * @brief Constructs a new Scheduler object with the given quantum_usecs.
     * @param quantum_usecs The length of a quantum in micro-seconds.
     */
    explicit Scheduler(int quantum_usecs) {
        this->quantum_usecs = quantum_usecs;
        totalQuantumCount = 0;
        currentRunningTid = MAIN_THREAD_ID;
        readyThreadsList = std::list<int>();
        blockedThreadsSet = std::set<int>();
    }

    /**
     * @brief Sets up the signal handler for the timer signal. The handler is set to contextSwitchHandler.
     */
    void setupTimerSignalHandler() {
        sa.sa_handler = &contextSwitchHandler;
        if (sigemptyset(&sa.sa_mask) == SIGSET_FAILURE) {
            sys_err(SYS_ERROR_SIGEMPTYSET_FAILURE);
        }
        sa.sa_flags = 0;

        if (sigaction(SIGVTALRM, &sa, nullptr) < 0) {
            sys_err(SYS_ERROR_SIGACTION);
        }
    }

    /**
     * @brief Initializes the timer signal.
     */
    void initTimer() {
        configureTimer();
        setUpTimer();
    }

    /**
     * @brief Configures the timer signal.
     */
    void configureTimer() {
        timer.it_value.tv_sec = quantum_usecs / MICROSECONDS_PER_SECOND;
        timer.it_value.tv_usec = quantum_usecs % MICROSECONDS_PER_SECOND;
        timer.it_interval.tv_sec = quantum_usecs / MICROSECONDS_PER_SECOND;
        timer.it_interval.tv_usec = quantum_usecs % MICROSECONDS_PER_SECOND;
    }

    /**
     * @brief Sets up the timer signal.
     */
    void setUpTimer() {
        if (setitimer(ITIMER_VIRTUAL, &timer, nullptr) < 0) {
            sys_err(SYS_ERROR_SETITIMER);
        }
    }

    /**
     * @brief Adds the given TCB to the threadTable and makes it ready.
     * @param tcb The TCB to add to the threadTable and make ready.
     */
    void addThreadAndMakeReady(TCB *tcb) {
        threadTable[tcb->getTid()] = tcb;
        readyThreadsList.push_back(tcb->getTid());
    }

    /**
     * @brief Returns true if the given tid is the main thread, false otherwise.
     * @param tid The thread ID to check if it is the main thread.
     * @return True if the given tid is the main thread, false otherwise.
     */
    static bool isMainThread(int tid) {
        return tid == MAIN_THREAD_ID;
    }

    /**
     * @brief Returns the total number of quantumCount since the library was initialized, including the current quantumCount.
     * @return The total number of quantumCount.
     */
    int getTotalQuantums() const {
        return totalQuantumCount;
    }

    void incrementTotalQuantums() {
        totalQuantumCount++;
    }

    void setRunningTid(int tid) { // todo: maybe add range validation
        currentRunningTid = tid;
    }

    /**
     * @brief Blocks the thread with the given tid for the given number of quantums.
     * @param tid The thread ID to block.
     * @param sleep_num_quantums The number of quantums to block the thread for.
     */
    void blockThreadForSleep(int tid, int sleep_num_quantums) {
        sleepingThreadsMap[tid] = sleep_num_quantums + 1;
        threadTable[tid]->setState(TCB::BLOCKED);
    }

    /**
     * @brief Blocks the thread with the given tid.
     * @param tid The thread ID to block.
     */
    void blockThread(int tid) {
        blockedThreadsSet.insert(tid);
        threadTable[tid]->setState(TCB::BLOCKED);
    }

    /**
     * @brief Makes the thread with the given tid ready.
     * @param tid The thread ID to make ready.
     */
    void makeReady(int tid) {
        readyThreadsList.push_back(tid);
        threadTable[tid]->setState(TCB::READY);
    }

    /**
     * @brief Sets the main thread as running and increments its quantum count.
     * This function should be called only once, when the library is initialized.
     */
    void initMainThread()
    {
        // Create the main thread descriptor and set it to running
        TCB *mainThread = new (std::nothrow) TCB(MAIN_THREAD_ID, nullptr);
        if (mainThread == nullptr) {
            sys_err(SYS_ERROR_ALLOC_MAIN_THREAD);
        }
        threadTable[MAIN_THREAD_ID] = mainThread;
        currentRunningTid = MAIN_THREAD_ID;
        threadTable[MAIN_THREAD_ID]->setState(TCB::RUNNING);
        threadTable[MAIN_THREAD_ID]->incrementQuantums();
        totalQuantumCount++;
    }

    /**
     * @brief Checks if the thread with the given tid is ready.
     * @param tid The thread ID to check if it is ready.
     * @return True if the thread with the given tid is ready, false otherwise.
     */
    bool isReady(int tid) {
        return threadTable[tid]->getState() == TCB::READY;
    }

    /**
     * @brief Checks if the thread with the given tid is blocked (because sleeping or was blocked or both).
     * @param tid The thread ID to check if it is blocked.
     * @return True if the thread with the given tid is blocked, false otherwise.
     */
    bool isBlocked(int tid) {
        return threadTable[tid]->getState();
    }

    /**
     * @brief Checks if the thread with the given tid is running.
     * @param tid The thread ID to check if it is running.
     * @return True if the thread with the given tid is running, false otherwise.
     */
    bool isRunning(int tid) {
        return threadTable[tid]->getState() == TCB::RUNNING;
    }

    /**
     * @brief Checks if the thread with the given tid exists.
     * @param tid The thread ID to check if it exists.
     * @return True if the thread with the given tid exists, false otherwise.
     */
    bool doesTidExist(int tid) {
        return threadTable.find(tid) != threadTable.end();
    }

    /**
     * @brief Checks if the thread with the given tid is explicitly blocked.
     * @param tid The thread ID to check if it is explicitly blocked.
     * @return True if the thread with the given tid is explicitly blocked, false otherwise.
     */
    bool isThreadExplicitlyBlocked(int tid) {
        return threadTable[tid]->getState() == TCB::BLOCKED and blockedThreadsSet.find(tid) != blockedThreadsSet.end();
    }

    /**
     * @brief Checks if the thread with the given tid is sleeping.
     * @param tid The thread ID to check if it is sleeping.
     * @return True if the thread with the given tid is sleeping, false otherwise.
     */
    bool isThreadSleeping(int tid) {
        return sleepingThreadsMap.find(tid) != sleepingThreadsMap.end() and
               threadTable[tid]->getState() == TCB::BLOCKED;
    }

    /**
     * @brief Returns the TCB of the thread with the given tid.
     * @param tid The thread ID to get the TCB of.
     * @return The TCB of the thread with the given tid.
     */
    TCB *getThreadById(int tid) {
        return threadTable[tid];
    }

    /**
     * @brief Returns the tid of the currently running thread.
     * @return The tid of the currently running thread.
     */
    int getRunningTID() const {
        return currentRunningTid;
    }

    /**
     * @brief Sets the thread with the given tid to the RUNNING state.
     * Also increments the quantum count of the thread and the total quantum count.
     * This function assumes that when setting a new running thread, a quantum has passed.
     * @param tid The thread ID to set as running.
    */
    void setThreadRunningAndIncrementQuantum(int tid) {
        currentRunningTid = tid;
        TCB* thread = getThreadById(currentRunningTid);
        if (thread) {
            thread->setState(TCB::RUNNING);
            thread->incrementQuantums();
            totalQuantumCount++;
        }
    }


    void deallocateResources() {
        for (auto &thread : threadTable) {
            delete thread.second;
        }
        threadTable.clear();
        readyThreadsList.clear();
        blockedThreadsSet.clear();
        sleepingThreadsMap.clear();
    }

    /**
     * @brief Terminates the thread with the given tid. This function should be called only
     * when the timer signal is blocked.
     * @param tid
     */
    void terminateThread(int tid) {
        if (isMainThread(tid)) {
            deallocateResources();
            return;
        }

        // Need to release the memory of the thread (the heap memory)
        TCB *thread_to_terminate = threadTable[tid];

        if (isRunning(tid)) {  // Thread terminate itself
            setShouldDeleteLastRanThread(true);
            contextSwitchHandler(FORCED_PREEMPTION);
            return;
        } else if (isReady(tid)) {
            removeThreadFromReadyList(tid);
        } else if (isThreadExplicitlyBlocked(tid)) {// blocked
            removeThreadFromBlockedSet(tid);
        }
        // Thread can be blocked and sleeping at the same time
        if (isThreadSleeping(tid)) {
            removeFromSleepingThreads(tid);
        }

        threadTable.erase(tid);
        delete thread_to_terminate;
    }

    /**
     * @brief Decrements the sleep time for each thread in the sleepingThreadsMap.
     * If the sleep time reaches zero, the thread is woken up.
     */
    void decrementSleepingThreadsQuantum() {
        // Iterate over the sleepingThreadsMap using an iterator
        for (auto it = sleepingThreadsMap.begin(); it != sleepingThreadsMap.end(); /* no increment */) {
            // Decrement the sleep time for the current thread
            it->second--;

            // Check if the sleep time has reached zero
            if (it->second == 0) { // Wake up the thread
                int tid = it->first;
                // If the thread is ! in the blockedThreadsSet, it means it's not blocked and should be made ready
                if (! isThreadExplicitlyBlocked(tid)) {
                    makeReady(tid);
                }
                // Erase the thread from the sleepingThreadsMap and update the iterator
                it = sleepingThreadsMap.erase(it);
            } else {
                // Move to the next thread in the map
                ++it;
            }
        }
    }

    int getNextTidToRun() {
        return readyThreadsList.front();
    }

    void removeNextThreadToRunFromReadyList() {
        readyThreadsList.pop_front();
    }

    /**
     * @brief Switches to the next thread in the readyThreadsList.
     */
    void switchToNextThread() {
        setRunningTid(getNextTidToRun());
        removeNextThreadToRunFromReadyList();

        TCB* runningThread = getThreadById(currentRunningTid);
        runningThread->setState(TCB::RUNNING);
        runningThread->incrementQuantums();

        // Now we will jump and will be using other env (and stack) so if a thread need to terminate itself this is the place
        if (ShouldDeleteLastRanThread()) {
            auto last_thread_to_delete = getThreadById(last_ran_tid);
            delete last_thread_to_delete;
            removeThreadFromMainTable(last_ran_tid);
            setShouldDeleteLastRanThread(false);
        }

        siglongjmp(getThreadById(currentRunningTid)->getEnv(), 1);
    }

    int findSmallestAvailableTid() {
        for (int i = MIN_TID; i <= MAX_TID; i++) {
            if (!doesTidExist(i)) {
                return i;
            }
        }
        return DEFAULT_TID;
    }

    /**
     * @brief Remove a thread from the sleeping threads map.
     * @param tid Thread ID.
     */
    void removeFromSleepingThreads(int tid) {
        sleepingThreadsMap.erase(tid);
    }

    /**
     * @brief Remove a thread from the ready list.
     * @param tid Thread ID.
     */
    void removeThreadFromReadyList(int tid) {
        readyThreadsList.remove(tid);
    }

    void removeThreadFromMainTable(int tid) {
        threadTable.erase(tid);
    }

    /**
     * @brief Remove a thread from the blocked set.
     * @param tid Thread ID.
     */
    void removeThreadFromBlockedSet(int tid) {
        blockedThreadsSet.erase(tid);
    }

    bool ShouldDeleteLastRanThread() const {
        return shouldDeleteLastRanThread;
    }

    void setShouldDeleteLastRanThread(bool val) {
        shouldDeleteLastRanThread = val;
    }
};

// Global scheduler object
Scheduler *scheduler = nullptr; // We assume the library is initialized before any other function is called

/**
 * @brief Blocks the timer signal.
 */
void blockTimerSignal() {
    sigset_t set;
    if (sigemptyset(&set) == SIGSET_FAILURE) {
        sys_err(SYS_ERROR_SIGEMPTYSET_FAILURE);
    }
    if (sigaddset(&set, SIGVTALRM) == SIGSET_FAILURE) {
        sys_err(SYS_ERR_SIGADDSET);
    }
    if (sigprocmask(SIG_BLOCK, &set, nullptr) == SIGPROCMASK_FAILURE) {
        sys_err(SYS_ERR_SIGPROCMASK);
    }
}

/**
 * @brief Unblocks the timer signal.
 */
void unblockTimerSignal() {
    sigset_t set;
    if (sigemptyset(&set) == SIGSET_FAILURE) {
        sys_err(SYS_ERROR_SIGEMPTYSET_FAILURE);
    }
    if (sigaddset(&set, SIGVTALRM) == SIGSET_FAILURE) {
        sys_err(SYS_ERR_SIGADDSET);
    }
    if (sigprocmask(SIG_UNBLOCK, &set, nullptr) == SIGPROCMASK_FAILURE) {
        sys_err(SYS_ERR_SIGPROCMASK);
    }
}


/**
 * @brief The context switch handler for the timer signal.
 * @param sig The signal number. Not used in current implementation.
 */
void contextSwitchHandler(int sig) {
    scheduler->incrementTotalQuantums();
    scheduler->decrementSleepingThreadsQuantum();
    int prev_tid = scheduler->getRunningTID();
    scheduler->last_ran_tid = prev_tid;

    // In case the current thread has terminated itself
    if (! scheduler->doesTidExist(prev_tid)) {
        scheduler->switchToNextThread();
        scheduler->setUpTimer();
        unblockTimerSignal();
        return;
    }

    TCB *prevThread = scheduler->getThreadById(prev_tid);

    int ret_val = sigsetjmp(prevThread->getEnv(), 1);
    if (ret_val == 0) {
        // When thread makes context switch because it goes to sleep or block itself, shouldn't go to ready list
        if (! scheduler->isBlocked(prev_tid) && ! scheduler->ShouldDeleteLastRanThread()) {
            scheduler->makeReady(prev_tid);
        }
        scheduler->switchToNextThread();
    }

    scheduler->setUpTimer();
}

/**
 * @brief initializes the thread library.
 *
 * Once this function returns, the main thread (tid == 0) will be set as RUNNING. There is no need to
 * provide an entry_point or to create a stack for the main thread - it will be using the "regular" stack and PC.
 * You may assume that this function is called before any other thread library function, and that it is called
 * exactly once.
 * The input to the function is the length of a quantumCount in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs) {
    if (quantum_usecs <= 0) {
        LIB_ERR(LIB_ERROR_POSITIVE_QUANTUM)
        return LIBRARY_FAILURE;
    }

    // Using std::nothrow to handle allocation failure
    scheduler = new (std::nothrow) Scheduler(quantum_usecs);
    if (scheduler == nullptr) {
        sys_err(SYS_ERROR_ALLOC_SCHEDULER);
    }

    scheduler->initMainThread();

    scheduler->setupTimerSignalHandler();
    scheduler->initTimer();

    return LIBRARY_SUCCESS;
}

/**
 * @brief Creates a new thread, whose entry point is the function entry_point with the signature
 * void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
 * limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 * It is an error to call this function with a null entry_point.
 *
 * @return On success, return the ID of the created thread. On failure, return -1.
*/
int uthread_spawn(thread_entry_point entry_point) {
    blockTimerSignal();
    if (entry_point == nullptr) {
        LIB_ERR(LIB_ERROR_ENTRY_POINT_NULL)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }

    int tid = scheduler->findSmallestAvailableTid();

    if (tid == DEFAULT_TID) {
        LIB_ERR(LIB_ERROR_MAX_THREADS)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }

    TCB *newThread = new (std::nothrow) TCB(tid, entry_point);
    if (newThread == nullptr) {
        sys_err(SYS_ERROR_ALLOC_THREAD);
    }
    scheduler->addThreadAndMakeReady(newThread);

    unblockTimerSignal();
    return tid;
}


/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
 *
 * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
 * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
 * process using exit(0) (after releasing the assigned library memory).
 *
 * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
 * itself or the main thread is terminated, the function does ! return.
*/
int uthread_terminate(int tid) {
    blockTimerSignal();
    if (! isTidInValidRange(tid)) {
        LIB_ERR(LIB_ERROR_TID_OUT_OF_RANGE)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }
    if (! scheduler->doesTidExist(tid)) {
        LIB_ERR(LIB_ERROR_THREAD_NOT_FOUND)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }

    // Need to release the memory of the thread (the heap memory)
    if (Scheduler::isMainThread(tid)) {
        scheduler->terminateThread(tid);
        delete scheduler;
        exit(0);
    }

    scheduler->terminateThread(tid);

    unblockTimerSignal();
    return LIBRARY_SUCCESS;
}


/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_block(int tid) {
    blockTimerSignal();
    if (! isTidInValidRange(tid)) {
        LIB_ERR(LIB_ERROR_TID_OUT_OF_RANGE)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }
    if (! scheduler->doesTidExist(tid)) {
        LIB_ERR(LIB_ERROR_THREAD_NOT_FOUND)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }
    if (Scheduler::isMainThread(tid)) {
        LIB_ERR(LIB_ERROR_BLOCK_MAIN_THREAD)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }

    // If the thread is already explicitly blocked, do nothing
    if (scheduler->isThreadExplicitlyBlocked(tid)) {
        unblockTimerSignal();
        return LIBRARY_SUCCESS;
    }

    if (scheduler->isRunning(tid)) {
        scheduler->blockThread(tid);
        contextSwitchHandler(FORCED_PREEMPTION);
    }

    if (scheduler->isReady(tid)) {
        scheduler->removeThreadFromReadyList(tid);
        scheduler->blockThread(tid);
    }

    if (scheduler->isThreadSleeping(tid)) {
        scheduler->removeFromSleepingThreads(tid);
        scheduler->blockThread(tid);
    }

    unblockTimerSignal();
    return LIBRARY_SUCCESS;
}


/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid) {
    blockTimerSignal();
    if (! isTidInValidRange(tid)) {
        LIB_ERR(LIB_ERROR_TID_OUT_OF_RANGE)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }
    if (! scheduler->doesTidExist(tid)) {
        LIB_ERR(LIB_ERROR_THREAD_NOT_FOUND)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }

    // if no one blocked me, do nothing
    if (! scheduler->isThreadExplicitlyBlocked(tid)) {
        unblockTimerSignal();
        return LIBRARY_SUCCESS;
    }

    // If got here, the thread is blocked - may be also sleeping

    scheduler->removeThreadFromBlockedSet(tid); // remove from blocked set (if sleeping or not)

    // If the thread is blocked and is not sleeping - resume it
    if (! scheduler->isThreadSleeping(tid)) {
        scheduler->makeReady(tid);
    }

    unblockTimerSignal();
    return LIBRARY_SUCCESS;
}

/**
 * @brief Blocks the RUNNING thread for num_quantums quantumCount.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the READY queue.
 * If the thread which was just RUNNING should also be added to the READY queue, or if multiple threads wake up
 * at the same time, the order in which they're added to the end of the READY queue doesn't matter.
 * The number of quantumCount refers to the number of times a new quantumCount starts, regardless of the reason. Specifically,
 * the quantumCount of the thread which has made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid == 0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_sleep(int num_quantums) {
    blockTimerSignal();
    if (num_quantums <= 0) {
        LIB_ERR(LIB_ERROR_POSITIVE_QUANTUM)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }

    int running_tid = scheduler->getRunningTID();

    if (Scheduler::isMainThread(running_tid)) {
        LIB_ERR(LIB_ERROR_SLEEP_MAIN_THREAD)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }

    scheduler->blockThreadForSleep(running_tid, num_quantums);
    // Context Switch handler will take care of the woke up.

    // context switch to the next thread
    contextSwitchHandler(FORCED_PREEMPTION);

    unblockTimerSignal();
    return LIBRARY_SUCCESS;
}


/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
*/
int uthread_get_tid() {
    return scheduler->getRunningTID();
}


/**
 * @brief Returns the total number of quantumCount since the library was initialized, including the current quantumCount.
 *
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantumCount starts, regardless of the reason, this number should be increased by 1.
 *
 * @return The total number of quantumCount.
*/
int uthread_get_total_quantums() {
    return scheduler->getTotalQuantums();
}


/**
 * @brief Returns the number of quantumCount the thread with ID tid was in RUNNING state.
 *
 * On the first time a thread runs, the function should return 1. Every additional quantumCount that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state when this function is called, include
 * also the current quantumCount). If no thread with ID tid exists it is considered an error.
 *
 * @return On success, return the number of quantumCount of the thread with ID tid. On failure, return -1.
*/
int uthread_get_quantums(int tid) {
    blockTimerSignal();
    if (! isTidInValidRange(tid)) {
        LIB_ERR(LIB_ERROR_TID_OUT_OF_RANGE)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }
    if (! scheduler->doesTidExist(tid)) {
        LIB_ERR(LIB_ERROR_THREAD_NOT_FOUND)
        unblockTimerSignal();
        return LIBRARY_FAILURE;
    }
    auto *thread = scheduler->getThreadById(tid);
    int quantums = thread->getQuantums();
    unblockTimerSignal();
    return quantums;
}

/**
 * @brief Prints a system error message, deallocates library resources, and exits the program.
 *
 * This function prints a system error message, deallocates resources
 * used by the library, and exits the program with a status code of 1.
 * It is used to handle critical system errors that require immediate
 * termination of the program.
 *
 * @param text The error message to be printed.
 */
void sys_err(const char* text) {
    SYS_ERR(text)
    if (scheduler) {
        scheduler->deallocateResources();
        delete scheduler;
        scheduler = nullptr;
    }
    exit(1);
}

bool isTidInValidRange(int tid) {
    return tid <= MAX_TID and tid >= MIN_TID;
}