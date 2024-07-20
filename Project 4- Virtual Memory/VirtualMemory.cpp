#include "VirtualMemory.h"
#include "PhysicalMemory.h"

#define FAILURE 0
#define SUCCESS 1

#define FRAME_SIZE PAGE_SIZE
#define ROOT_TABLE_FRAME_NUMBER 0
#define ROOT_TABLE_LEVEL 0

#define INIT_ACCUMULATED_P 0

// Since frame 0 always contains the root table (which is never evicted), no row in any table will ever
// point to frame 0. Therefore, we put 0 in rows whose page is currently swapped out. If during any step
// of the translation we reach a row that contains the number 0, we know that this is not a real frame
// index, and we have reached a page fault.
#define INVALID_TABLE_FRAME 0

// Default value for the parent of the root table is itself (since it doesn't have an actual parent)
#define ROOT_TABLE_PARENT_INFO {0, 0}

// This is struct to encapsulate the relationship between a parent and a child in the hierarchical page table tree
struct PTEInfo;

// ---- helper functions definitions  ----
int abs(int x);
void attachChildToFather(word_t currentFrame, const word_t &nextFrame, const uint64_t &offset);
int getCyclicDist(uint64_t pageSwappedIn, uint64_t p);
bool isVirtualAddressValid(uint64_t virtualAddress);
void initializeFrameTable(word_t frameNumber);
uint64_t extractPageTableIndex(uint64_t virtualAddress, uint64_t level);
bool isValidTableFrameIndex(word_t frameIndex);
bool isLeaf(uint64_t level);
void detachChildFromParent(word_t parentFrame, int childIndex);
bool isValidFrame(word_t frame);
void handleEviction(word_t &victimFrame, uint64_t pageNumber);
void handlePageFault(word_t currentFrame, word_t& nextFrame, uint64_t p_level, uint64_t level, uint64_t pageNumber);
uint64_t mapVirtualAddressToPhysicalAddress(uint64_t virtualAddress);
// ---- Header file functions implementation ----

/**
 * Initialize the virtual memory.
 */
void VMinitialize() {
    initializeFrameTable(ROOT_TABLE_FRAME_NUMBER);
}

/**
 * Reads a word from the given virtual address and puts its content in *value.
 * @param virtualAddress The virtual address to read from.
 * @param value A pointer to store the read value.
 * @return 1 if successful, 0 if the virtual address is out of bounds.
 */
int VMread(uint64_t virtualAddress, word_t* value) {
    if (!isVirtualAddressValid(virtualAddress)) {
        return FAILURE;
    }

    // find the physical address of the given virtual address
    uint64_t physicalAddress = mapVirtualAddressToPhysicalAddress(virtualAddress);
    PMread(physicalAddress, value);
    return SUCCESS;
}

/**
 * Writes a word to the given virtual address.
 * @param virtualAddress The virtual address to write to.
 * @param value The value to write.
 * @return 1 on success, 0 on failure
 */
int VMwrite(uint64_t virtualAddress, word_t value) {
    if (!isVirtualAddressValid(virtualAddress)) {
        return FAILURE;
    }

    // find the physical address of the given virtual address
    uint64_t physicalAddress = mapVirtualAddressToPhysicalAddress(virtualAddress);
    PMwrite(physicalAddress, value);
    return SUCCESS;
}

// ---- helper functions and structs implementation ----

/**
 * Calculate the absolute value of an integer.
 * @param x The integer to calculate the absolute value of.
 * @return The absolute value of the given integer.
 */
int abs(int x) {
    return x < 0 ? -x : x;
}

/**
 * This struct is used for the DFS traversal to find an empty frame and for correct detachment
 * of the empty frame from its parent.
 */
struct PTEInfo {
    word_t frame;
    int offset;
};

/**
 * Get the physical address of the given frame and offset.
 * @param frame
 * @param offset
 * @return The physical address of the given frame and offset.
 */
uint64_t getPhysicalAddress(int frame, uint64_t offset) {
    return frame * FRAME_SIZE + offset;
}

/**
 * Check if the given virtual address is valid.
 * @param virtualAddress The virtual address to check.
 * @return True if the virtual address is valid, false otherwise.
 */
bool isVirtualAddressValid(uint64_t virtualAddress) {
    return virtualAddress < VIRTUAL_MEMORY_SIZE;
}

/**
 *  Initialize a frame table by writing 0 to all entries in the frame.
 *  @param frameNumber The frame number to initialize.
 */
void initializeFrameTable(word_t frameNumber) {
    uint64_t baseAddress = frameNumber * FRAME_SIZE;
    uint64_t endAddress = baseAddress + FRAME_SIZE;

    for (uint64_t physicalAddress = baseAddress; physicalAddress < endAddress; physicalAddress++) {
        PMwrite(physicalAddress, INVALID_TABLE_FRAME);
    }
}

/**
 * Extract the p_i part of the given virtual address.
 * @param virtualAddress The virtual address to extract the p_i part from.
 * @param level The level of the table (range 0-TABLES_DEPTH-1) to extract the p_i part from.
 * @return The p_i part of the given virtual address.
 */
uint64_t extractPageTableIndex(uint64_t virtualAddress, uint64_t level) {
    // Calculate the number of bits to shift right to get the p_i part
    int shiftAmount = OFFSET_WIDTH * (TABLES_DEPTH - level);

    // We need a mask so the left bits which stays after the left shift (for the p_i-1,...p1) will be removed as well
    // Mask to get only the p_i part (the highest OFFSET_WIDTH bits)
    uint64_t mask = (1LL << OFFSET_WIDTH) - 1;

    // Shift the virtual address right to align p_i with the lowest bits, and apply the mask
    return (virtualAddress >> shiftAmount) & mask;
}

/**
 * Check if the given frame index is valid (i.e., not equal to 0 or within a valid range).
 * @param frameIndex The frame index to check.
 */
bool isValidTableFrameIndex(word_t frameIndex) {
    return frameIndex != 0 && frameIndex < NUM_FRAMES;
}

void findVictimFrame(const word_t DFSRoot, int level, word_t accumulatedP, int &maxCyclicDist,
                     const word_t pageSwappedIn, word_t &argmaxPageFrame, word_t &pageToEvict,
                     const PTEInfo parentInfo, PTEInfo& evictedPageParentInfo) {

    // Base case
    if (level == TABLES_DEPTH) {
        // When reaching the leaf node, the accumulatedP is the page number
        int cyclicDist = getCyclicDist(pageSwappedIn, accumulatedP);
        if (cyclicDist > maxCyclicDist) {
            maxCyclicDist = cyclicDist;
            argmaxPageFrame = DFSRoot;
            pageToEvict = accumulatedP;
            evictedPageParentInfo = parentInfo;
        }
        return;
    }

    // go over all children of the current table and recursively call the function with the appropriate arguments
    word_t childFrame = INVALID_TABLE_FRAME;
    for (int offset = 0; offset < PAGE_SIZE; offset++) {
        PMread(DFSRoot * PAGE_SIZE + offset, &childFrame);
        if (childFrame != INVALID_TABLE_FRAME) { // There is a path that might lead to a leaf node with the page to be evicted
            findVictimFrame(childFrame, level + 1,
                            (accumulatedP << OFFSET_WIDTH) | offset, maxCyclicDist,
                            pageSwappedIn, argmaxPageFrame, pageToEvict,
                            {DFSRoot, offset}, evictedPageParentInfo);
        }
    }
}


/**
 * Calculate the cyclic distance between two pages.
 * @param pageSwappedIn The page that needs to be swapped in.
 * @param p The page to find the cyclic distance from.
 * @return The cyclic distance between the two pages.
 */
int getCyclicDist(uint64_t pageSwappedIn, uint64_t p) {
    int dist = abs ((int)(pageSwappedIn - p));
    int upperDist = NUM_PAGES - dist;
    int lowerDist = dist;
    return upperDist < lowerDist ? upperDist : lowerDist;
}

/**
 * Check if the given level is a leaf level.
 * @param level The level to check.
 * @return True if the level is a leaf level, false otherwise.
 */
bool isLeaf(uint64_t level) {
    return level == TABLES_DEPTH - 1;
}

/**
 * Detach the child from the parent by writing 0 to the appropriate entry in the parent table.
 * @param parentFrame The frame of the parent table.
 * @param childIndex The index of the child in the parent table.
 */
void detachChildFromParent(word_t parentFrame, int childIndex) {
    PMwrite(getPhysicalAddress(parentFrame, childIndex), INVALID_TABLE_FRAME);
}

/**
 * Check if the given frame is a valid frame index.
 * @param frame The frame to check.
 * @return True if the frame is a valid frame index, false otherwise.
 */
bool isValidFrame(word_t frame) {
    return frame > INVALID_TABLE_FRAME && frame < NUM_FRAMES;
}

/**
 * Finds a new frame to allocate for a page. (handles both cases of empty frame and unused frame)
 *
 * @param parentToNotEmpty      The frame number of the path.
 * @param DFSRoot       The frame number to perform DFS (Depth-First Search) from.
 * @param maxFrame           The maximum frame number encountered so far.
 * @param emptyFrame    Reference to store the possible empty frame found.
 * @param level              The current level of the page table tree.
 * @param parentInfo                  The parentInfo frame number.
 * @param evictedPageParentInfo          Reference to store the parentInfo frame number to save.
 * @return                        The maximum frame number encountered during the search process.
 */
void getFrameWithoutEvictionDFS (const word_t parentToNotEmpty, word_t DFSRoot, int &maxFrame,
                                 word_t &emptyFrame, int level, PTEInfo parentInfo, PTEInfo &evictedPageParentInfo)
{
    if (level == TABLES_DEPTH) {
        return;
    }

    bool isEmptyTable = true;
    word_t childFrame = INVALID_TABLE_FRAME;

    // Searching for the DFSRoot node if all the table entries are empty -> so the table is empty
    for (int offset = 0; offset < PAGE_SIZE; ++offset)
    {
        PMread (getPhysicalAddress(DFSRoot, offset), &childFrame);
        if (childFrame > maxFrame)
        {
            maxFrame = childFrame;
        }
        if (childFrame != INVALID_TABLE_FRAME) {
            isEmptyTable = false;
            getFrameWithoutEvictionDFS(parentToNotEmpty, childFrame, maxFrame,
                                       emptyFrame, level + 1,
                                       {DFSRoot, offset}, evictedPageParentInfo);
        }
    }
    if (isEmptyTable && (DFSRoot != parentToNotEmpty))
    {
        emptyFrame = DFSRoot;
        evictedPageParentInfo = parentInfo;
    }
}

void handleFrameWithoutEviction(const word_t currentFrame, word_t& nextFrame) {
    word_t emptyFrame = 0;
    int maxFrame = 0;
    PTEInfo evictedPageParentInfo = ROOT_TABLE_PARENT_INFO;
    getFrameWithoutEvictionDFS(currentFrame, INVALID_TABLE_FRAME, maxFrame,
                               emptyFrame, ROOT_TABLE_LEVEL, ROOT_TABLE_PARENT_INFO,
                               evictedPageParentInfo);

    // case 1: empty frame was found
    if (emptyFrame != INVALID_TABLE_FRAME)
    {
        nextFrame = emptyFrame;
        detachChildFromParent(evictedPageParentInfo.frame, evictedPageParentInfo.offset);
    }

        // case 2: unused frame was found
    else if (isValidFrame(maxFrame + 1))  {// case two unused frame
        nextFrame = maxFrame + 1;
        return;
    }

    // case 3: no empty or unused frame was found
}

void handleEviction(word_t &victimFrame, uint64_t pageNumber) {
    int maxDist = 0;
    word_t pageToEvict = 0;
    PTEInfo parentOfEvicted = ROOT_TABLE_PARENT_INFO;
    findVictimFrame(INVALID_TABLE_FRAME, ROOT_TABLE_LEVEL, INIT_ACCUMULATED_P, maxDist,
                    (word_t) pageNumber, victimFrame, pageToEvict,
                    ROOT_TABLE_PARENT_INFO, parentOfEvicted);

    PMevict (victimFrame, pageToEvict);
    detachChildFromParent(parentOfEvicted.frame, parentOfEvicted.offset);
}

/**
 * Find an available frame in the physical memory to store a new page.
 * Use one of three methods: empty table frame, unused frame, or evict a page.
 * @param currentFrame the frame of the current table, which is the parent of the next table which we
 *        need to find a frame for.
 * @param nextFrame the reference we need to update in the current table to point to the next table.
 * @param p_level The index of the page in the current table.
 * @param level The level of the current table in the tree.
 * @param pageNumber The page number of the virtual address we are looking for.
 */
void handlePageFault(word_t currentFrame, word_t& nextFrame, uint64_t p_level, uint64_t level, uint64_t pageNumber) {
    handleFrameWithoutEviction(currentFrame, nextFrame);
    if (nextFrame == INVALID_TABLE_FRAME) { // No empty or unused frame was found -> need to evict a page
        handleEviction(nextFrame, pageNumber);
    }

    // in each of the scenario we need to attach the new frame to the parent
    attachChildToFather(currentFrame, nextFrame, p_level);

    // Need to distinguish between a leaf and a table and act accordingly
    if (isLeaf(level)) { // In case of page fault due to a leaf node, can restore data from disk
        PMrestore(nextFrame, pageNumber); // Restore the page from the disk to the new frame
    } else { // In case of page fault due to a table node, need to initialize the table
        initializeFrameTable(nextFrame);
    }
}


/**
 * Update the link in the current table to point to the new table
 * @param currentFrame The frame of the current table.
 * @param nextFrame The frame of the next table.
 * @param offset The offset in the current table to update.
 */
void attachChildToFather(word_t currentFrame, const word_t &nextFrame, const uint64_t &offset) {
    PMwrite(currentFrame * FRAME_SIZE + offset, nextFrame);
}

/**
 * Map the given virtual address to a physical address using hierarchical page table.
 * @param virtualAddress The virtual address to map.
 * @return The physical address corresponding to the given virtual address.

 */
uint64_t mapVirtualAddressToPhysicalAddress(uint64_t virtualAddress) {
    uint64_t pageNumber = virtualAddress >> OFFSET_WIDTH;

    word_t currentFrame = ROOT_TABLE_FRAME_NUMBER;
    auto tableDepth = TABLES_DEPTH;
    for (int level = 0; level < tableDepth; level++) {
        uint64_t p_level = extractPageTableIndex(virtualAddress, level); // The offset in current table
        word_t nextFrame;
        PMread(getPhysicalAddress(currentFrame, p_level), &nextFrame);
        if (!isValidTableFrameIndex(nextFrame)) {
            handlePageFault(currentFrame, nextFrame, p_level, level, pageNumber);
            // After calling handlePageFault, nextFrame will be updated to the frame number of the new table, and we can continue the translation
        }

        currentFrame = nextFrame;
    }

    // Once the loop completes, `currentFrame` will be the frame containing the target page
    // Calculate the physical address using the offset
    auto mask = (1LL << OFFSET_WIDTH) - 1;
    auto offset = virtualAddress & mask;
    uint64_t physicalAddress = currentFrame * FRAME_SIZE + offset;
    return physicalAddress;
}
