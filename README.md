# Operating Systems Projects

This repository contains multiple projects related to Operating Systems. Each project demonstrates different concepts and techniques in operating systems, such as memory latency, user-level threads, and synchronization using MapReduce.

## Course Overview

This repository provides a comprehensive look into various aspects of operating systems design and implementation. Each project is designed to highlight specific concepts and techniques, and includes source code, detailed explanations, and any additional resources required.

## Projects

Each project directory contains the source code, a detailed README, and any additional resources required.

### Project 1 - Memory Latency

This project explores memory latency and its impact on system performance.

### Project 2 - User Level Threads Library

This project involves the implementation of a user-level threads library. It covers thread creation, scheduling, and context switching.

### Project 3 - Sync

This project is a MapReduce framework. It supports parallel processing of large datasets using multiple threads. The framework is designed to divide the work among several threads, process the data in parallel, and then combine the results.

## Installation

1. Clone the repository:
   ```bash
   git clone git@github.com:shirashko/OS.git
   ```

2. Navigate into the specific project directory and rename it as you desire:
   ```bash
   cd OS
   mv "Project 3 - Sync" ../mapreduce-framework
   cd ../mapreduce-framework
   ```

3. Build the project:
   ```bash
   make
   ```
