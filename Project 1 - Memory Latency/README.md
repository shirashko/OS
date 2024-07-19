# Project 1 - Memory Latency

## Assignment 1: Understanding WhatIDo - System Call Tracing with strace

### Description
This part of the assignment aims to help us become familiar with the `strace` command. `strace` is a Linux command that traces system calls and signals of a program. It is an important tool to debug programs.

### Instructions
1. Download `WhatIDo` / Whatever other program into an empty folder.
2. Run the program using `strace`.
3. Follow the `strace` output.

### Findings
The `WhatIDo` program follows these specific steps:
1. **Initialization and Error Handling**:
   - Argument Check: Checks whether exactly one command-line argument has been provided.
   - Library Setup: Loads necessary libraries and configurations, preparing the environment for file operations.
2. **File Creation and Manipulation**:
   - Creates a directory named "Welcome" and three files within it: "Welcome", "To", and "OS-2024".
   - Writes specific messages into these files.
3. **Cleanup and Exit**:
   - Deletes all created files and the directory, then exits successfully.

## Assignment 2: Measuring Memory Access Latency Across Cache Levels and RAM

### Background
In this assignment, we measure memory read latency for the cache levels and RAM access in the target machine. We use a partially implemented C code designed to measure memory access times as accurately as possible.

### Steps
1. **Complete the missing parts in the memory latency program**.
2. **Set-up a virtual machine (VM)**.
3. **Use the memory latency program to measure memory access latency** inside the VM.
4. **Plot the results in a graph**.
5. **Include the cache level sizes** in your graph.
6. **Explain the results** in this README file.

### The Memory Latency Program
The program measures the average memory access latency of different memory levels by:
- Allocating an array with the desired memory size and accessing it.
- Measuring access latency for random and sequential access patterns.

### Compilation
To compile the program, use the following command:
```sh
g++ -std=c++11 -O3 -Wall memory_latency.cpp measure.cpp -o memory_latency
