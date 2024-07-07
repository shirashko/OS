// OS 24 Project1

#include "memory_latency.h"
#include "measure.h"
#include <cmath>
#include <sstream>
#include <string>
#include <iostream>


#define GALOIS_POLYNOMIAL ((1ULL << 63) | (1ULL << 62) | (1ULL << 60) | (1ULL << 59))

#define INITIAL_SIZE 100
#define MIN_MAX_SIZE 100 // Minimum valid value for max_size
#define MIN_FACTOR 1.0  // Minimum value for growth factor (need to be > 1)
#define MIN_REPEAT 1     // Minimum valid value for repeat times

#define NUMBER_OF_ARGS 4
#define ARG_PROGRAM_NAME 0
#define ARG_MAX_SIZE 1
#define ARG_FACTOR 2
#define ARG_REPEAT 3

#define NANOS_PER_SECOND 1000000000ull

const std::string USAGE_MESSAGE =
        "Usage: ./memory_latency max_size factor repeat\n"
        "  max_size: Maximum size of the memory array in bytes (integer >= " + std::to_string(MIN_MAX_SIZE) + ")\n"
        "  factor: Growth factor for memory sizes, must be > " + std::to_string(MIN_FACTOR) + " (decimal)\n"
                                                                                                                                                                                                    "  repeat: Number of times to repeat each measurement (integer >= " + std::to_string(MIN_REPEAT) + ")\n";
const std::string INVALID_INPUT_ERROR =
        "Error: Invalid input arguments.\n"
        "Conditions: max_size >= " + std::to_string(MIN_MAX_SIZE) + ", factor > " + std::to_string(MIN_FACTOR)
        + ", repeat >= " + std::to_string(MIN_REPEAT) + ".\n";

const std::string MEMORY_ALLOCATION_ERROR = "Memory allocation failed for size "; // Append the size in bytes.
const std::string MEASUREMENT_FAILURE_ERROR = "Failed to perform measurements due to memory allocation failure.";


/**
 * Converts the struct timespec to time in nano-seconds. Convert the seconds to nanoseconds and add the nanoseconds part.
 * A nanosecond is a unit of time in the International System of Units (SI), and it represents one billionth of a
 * second. In the world of technology, computing, and communications, you'll find nanoseconds are commonly used to
 * measure things like processing speeds and data transfer rates.
 * @param time - the struct timespec to convert.
 * @return - the value of time in nano-seconds.
 */
uint64_t nanosectime(struct timespec t)
{
    return t.tv_sec * NANOS_PER_SECOND + t.tv_nsec;
}

/**
* Measures the average latency of accessing a given array in a sequential order.
* @param repeat - the number of times to repeat the measurement for and average on.
* @param arr - an allocated (not empty) array to preform measurement on.
* @param arr_size - the length of the array arr.
* @param zero - a variable containing zero in a way that the compiler doesn't "know" it in compilation time.
* @return struct measurement containing the measurement with the following fields:
*      double baseline - the average time (ns) taken to preform the measured operation without memory access.
*      double access_time - the average time (ns) taken to preform the measured operation with memory access.
*      uint64_t rnd - the variable used to randomly access the array, returned to prevent compiler optimizations.
*/
struct measurement measure_sequential_latency(uint64_t repeat, array_element_t* arr, uint64_t arr_size, uint64_t zero)
{
    // Ensure the repeat count is at least as large as the array size to avoid index out of bounds.
    repeat = arr_size > repeat ? arr_size:repeat; // Make sure repeat >= arr_size

    // Baseline measurement setup: no memory access involved
    struct timespec t0;
    timespec_get(&t0, TIME_UTC);
    register uint64_t rnd=12345; // Initial seed for the pseudo-random number generator

    // Baseline measurement: perform pseudo-random calculations without accessing the array
    for (register uint64_t i = 0; i < repeat; i++)
    {
        register uint64_t index = rnd % arr_size; // Calculate a random index in the array
        rnd ^= index & zero; // XOR operation with zero doesn't change anything. This operation is done with zero,
        // which the compiler is unaware of the fact it is zero, to prevent the compiler from optimizing it out.
        rnd++;
    }
    struct timespec t1;
    timespec_get(&t1, TIME_UTC); // Get the time after the baseline measurement

    // Memory access measurement:
    struct timespec t2;
    timespec_get(&t2, TIME_UTC); // Get the time before the memory access measurement
    rnd=(rnd & zero) ^ 12345; // Reset rnd for memory access measurement to 12345
    for (register uint64_t i = 0; i < repeat; i++)
    {
        register uint64_t index = rnd % arr_size;
        rnd ^= arr[index] & zero; // This time access the array as well
        rnd++;
    }
    struct timespec t3;
    timespec_get(&t3, TIME_UTC); // Get the time after the memory access measurement

    // Calculate baseline and memory access times:
    double baseline_per_cycle=(double)(nanosectime(t1)- nanosectime(t0))/(repeat);
    double memory_per_cycle=(double)(nanosectime(t3)- nanosectime(t2))/(repeat);
    struct measurement result;

    result.baseline = baseline_per_cycle;
    result.access_time = memory_per_cycle;
    result.rnd = rnd; // Return the last used rnd value to ensure it is not optimized out
    return result;
}

/**
 * Performs the measurements for the given input arguments.
 * The measurements are printed to stdout in the following format:
 *     mem_size_1,offset_1,offset_sequential_1
 * @param max_size
 * @param factor
 * @param repeat
 * @param zero
 * @return true if the measurements were successful, false otherwise.
 */
bool perform_measurements(uint64_t max_size, float factor, uint64_t repeat, uint64_t zero) {
    uint64_t current_size = INITIAL_SIZE;
    while (current_size <= max_size) {
        array_element_t* arr = (array_element_t*)malloc(current_size);
        if (arr == nullptr) {
            std::cerr << MEMORY_ALLOCATION_ERROR << current_size << " bytes." << std::endl;
            return false;
        }

        uint64_t number_of_elements = current_size / sizeof(array_element_t);

        // Initialize the array with non-zero, distinct values
        for (uint64_t i = 0; i < number_of_elements; ++i) {
            arr[i] = i;
        }

        measurement random_measure = measure_latency(repeat, arr, number_of_elements, zero);
        measurement sequential_measure = measure_sequential_latency(repeat, arr, number_of_elements, zero);

        double offset_random = random_measure.access_time - random_measure.baseline;
        double offset_sequential = sequential_measure.access_time - sequential_measure.baseline;

        std::cout << current_size << "," << offset_random << "," << offset_sequential << std::endl;
        free(arr);
        current_size = (uint64_t)(ceil(current_size * factor));
    }
    return true;
}

/**
 * Runs the logic of the memory_latency program. Measures the access latency for random and sequential memory access
 * patterns.
 * Usage: './memory_latency max_size factor repeat' where:
 *      - max_size - the maximum size in bytes of the array to measure access latency for.
 *      - factor - the factor in the geometric series representing the array sizes to check.
 *      - repeat - the number of times each measurement should be repeated for and averaged on.
 * The program will print output to stdout in the following format:
 *      mem_size_1,offset_1,offset_sequential_1
 *      mem_size_2,offset_2,offset_sequential_2
 *              ...
 *              ...
 *              ...
 */
int main(int argc, char* argv[])
{
    // zero==0, but the compiler doesn't know it. Use as the zero arg of measure_latency and measure_sequential_latency.
    struct timespec t_dummy;
    timespec_get(&t_dummy, TIME_UTC);
    // Creating a zero variable that the compiler doesn't "know" it in compilation time.
    const uint64_t zero = nanosectime(t_dummy)>NANOS_PER_SECOND?0:nanosectime(t_dummy);

    if (argc < NUMBER_OF_ARGS) {
        std::cerr << USAGE_MESSAGE << std::endl;
        return -1;
    }

    try {
        const uint64_t max_size = std::stoull(argv[ARG_MAX_SIZE]);
        const float factor = std::stof(argv[ARG_FACTOR]);
        const uint64_t repeat = std::stoull(argv[ARG_REPEAT]);

        if (max_size < MIN_MAX_SIZE || factor <= MIN_FACTOR || repeat < MIN_REPEAT) {
            std::cerr << INVALID_INPUT_ERROR
                      << "Provided max_size = " << max_size
                      << ", factor = " << factor
                      << ", repeat = " << repeat << std::endl;
            return -1;
        }

        if (!perform_measurements(max_size, factor, repeat, zero)) {
            std::cerr << MEASUREMENT_FAILURE_ERROR << std::endl;
            return -1;
        }
    } catch (const std::invalid_argument& e) {
        std::cerr << "Invalid argument: " << e.what() << std::endl;
        return -1;
    } catch (const std::out_of_range& e) {
        std::cerr << "Out of range: " << e.what() << std::endl;
        return -1;
    }

    return EXIT_SUCCESS;
}