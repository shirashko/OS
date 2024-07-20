# Virtual Memory - Hierarchical Page Tables

## Overview

This project implements a virtual memory interface using hierarchical page tables of arbitrary depth. The implementation simulates physical memory and handles virtual to physical address translation, page faults, and page evictions.

## Files

- `VirtualMemory.cpp`: The implementation of the virtual memory interface.
- `VirtualMemory.h`: The header file defining the virtual memory interface functions.
- `PhysicalMemory.cpp`: The implementation of the simulated physical memory.
- `PhysicalMemory.h`: The header file for the simulated physical memory functions.
- `MemoryConstants.h`: Contains constants for memory sizes and the depth of the page table tree.
- `SimpleTest.cpp`: A simple test file for the virtual memory functions.
- `Makefile`: A file with the required targets (`tar`, `clean`) for building and cleaning the project.
- `README`: This file with the required information.

## Description

### Virtual Memory

Virtual memory allows processes to use more memory than is physically available by mapping virtual addresses to physical addresses. This mapping is done using hierarchical page tables, which are more memory-efficient than a single-level page table.

### Hierarchical Page Tables

The virtual address is translated into a physical address through multiple levels of page tables. Each table resides in frames of the RAM. If a page is not in physical memory, a page fault occurs, and the page is brought into physical memory, potentially evicting another page.

### Key Concepts

- **Page**: A fixed-size block of virtual memory.
- **Frame**: A fixed-size block of physical memory.
- **Page Table**: A data structure used to map virtual pages to physical frames.
- **Page Fault**: An event that occurs when a page is not in physical memory.
- **Eviction**: The process of removing a page from physical memory to make space for another page.

## Implementation

The implementation includes the following main functions:

- `void VMinitialize()`: Initializes the virtual memory by clearing the root table.
- `int VMread(uint64_t virtualAddress, word_t* value)`: Reads a word from the given virtual address and stores it in `value`.
- `int VMwrite(uint64_t virtualAddress, word_t value)`: Writes a word to the given virtual address.
- `uint64_t mapVirtualAddressToPhysicalAddress(uint64_t virtualAddress)`: Maps a virtual address to a physical address using hierarchical page tables.

### Helper Functions

Several helper functions are used to manage the hierarchical page tables and handle page faults and evictions:

- `int abs(int x)`: Calculates the absolute value of an integer.
- `void attachChildToFather(word_t currentFrame, const word_t& nextFrame, const uint64_t& offset)`: Updates the link in the current table to point to the new table.
- `int getCyclicDist(uint64_t pageSwappedIn, uint64_t p)`: Calculates the cyclic distance between two pages.
- `bool isVirtualAddressValid(uint64_t virtualAddress)`: Checks if the given virtual address is valid.
- `void initializeFrameTable(word_t frameNumber)`: Initializes a frame table by writing 0 to all entries in the frame.
- `uint64_t extractPageTableIndex(uint64_t virtualAddress, uint64_t level)`: Extracts the p_i part of the given virtual address.
- `bool isValidTableFrameIndex(word_t frameIndex)`: Checks if the given frame index is valid.
- `bool isLeaf(uint64_t level)`: Checks if the given level is a leaf level.
- `void detachChildFromParent(word_t parentFrame, int childIndex)`: Detaches the child from the parent by writing 0 to the appropriate entry in the parent table.
- `bool isValidFrame(word_t frame)`: Checks if the given frame is a valid frame index.
- `void handleEviction(word_t& victimFrame, uint64_t pageNumber)`: Handles the eviction of a page.

## Compilation and Execution

### Prerequisites

- C++ compiler (e.g., g++)
- Make

### Compilation

To compile the project, run:

```bash
make
```

This will generate the `libVirtualMemory.a` library.

### Cleaning

To clean the project, run:

```bash
make clean
```

### Tar

To create a tar with `VirtualMemory.cpp` `Makefile` and `README`, run:

```bash
make tar
```

## Notes

- The implementation support different memory space and page sizes as defined in `MemoryConstants.h`, which can be easily replaced.
- No global variables or dynamic allocations are used.
- Many tests with different configurations are supplied in tests and basic tests directories.
- More relevant info can be found in resources directory.
