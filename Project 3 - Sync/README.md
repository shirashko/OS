# MapReduce Framework

This project is a MapReduce framework implemented in C++. It supports parallel processing of large datasets using multiple threads. The framework is designed to divide the work among several threads, process the data in parallel, and then combine the results.

## Installation

1. Clone the repository:
   ```bash
   cd ..
   mv "Project 3 - Sync" ../mapreduce-framework
   cd ../mapreduce-framework
   ```

2. Build the project:
   ```bash
   make
   ```

## Usage

To use the MapReduce framework, you need to implement a class that inherits from `MapReduceClient`. This class should override the `map` and `reduce` methods.

### Structure

The key components of the framework are:

- `MapReduceClient`: Abstract class that defines the `map` and `reduce` functions.
- `JobHandle`: Manages the overall state and coordination of the MapReduce job.
- `ThreadContext`: Stores the state for each thread.
- `Barrier`: Synchronization primitive to ensure that all threads reach a certain point before any can proceed.
- `emit2` and `emit3`: Functions to emit intermediate and final key-value pairs.

### Functions

#### MapReduceClient

```cpp
class MapReduceClient {
public:
    virtual void map(const K1* key, const V1* value, void* context) const = 0;
    virtual void reduce(const IntermediateVec* pairs, void* context) const = 0;
};
```

#### JobControl Functions

- `JobHandle startMapReduceJob(const MapReduceClient& client, const InputVec& inputVec, OutputVec& outputVec, int multiThreadLevel);`
- `void waitForJob(JobHandle job);`
- `void getJobState(JobHandle job, JobState* state);`
- `void closeJobHandle(JobHandle job);`

### Example

Look in Resources/SampleClient.cpp
