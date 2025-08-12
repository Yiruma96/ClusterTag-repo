#include <inttypes.h> // for PRId64
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>
#include "std_testcase.h"

#ifndef _WIN32
#include <wchar.h>
#endif


// ClusterTag. Define memory block sizes
const size_t block_sizes[] = { 0x20, 0x40, 0x60, 0x80, 0xa0, 0xc0, 0xe0,
                               0x100, 0x120, 0x140, 0x160, 0x180, 0x1a0,
                               0x1c0, 0x1e0, 0x200, 0x300, 0x400, 0x600,
                               0x800, 0xc00, 0x1000, 0x1800, 0x2000,
                               0x3000, 0x4000, 0x6000, 0x8000, 0xc000, 0x10000 };
// ClusterTag. Calculate array length
const size_t num_sizes = sizeof(block_sizes) / sizeof(block_sizes[0]);

// ClusterTag. Define endFree function as the number of memory allocation and deallocation operations simulated for each unit memory block
#define FREE_OPERATIONS 5000
// ClusterTag. Define beforeMalloc function and endMalloc function as the number of memory allocation operations simulated for each unit memory block
#define MALLOC_OPERATIONS 5000


// ClusterTag. Memory block structure
typedef struct Block {
    void *ptr;  // ClusterTag. Pointer to allocated memory block
} Block;

// ClusterTag. Memory allocator structure
typedef struct SimMemoryAllocator {
    Block *allocated_blocks;  // ClusterTag. Array of allocated memory blocks
    int allocated_count;      // ClusterTag. Current number of allocated memory blocks
    int max_operations;       // ClusterTag. Maximum number of operations
    double alloc_prob;        // ClusterTag. Allocation probability
    double dealloc_prob;      // ClusterTag. Deallocation probability
    int operation_count;      // ClusterTag. Current operation count
    int block_size;           // ClusterTag. Memory block size that this memory simulation allocator is responsible for allocating
} SimMemoryAllocator;

// ClusterTag. Initialize memory allocator
void initializeSimAllocator(SimMemoryAllocator *allocator, int block_size, int max_operations, double alloc_prob, double dealloc_prob) {
    allocator->block_size = block_size;
    allocator->allocated_blocks = (Block *)malloc(sizeof(Block) * max_operations);
    allocator->allocated_count = 0;
    allocator->max_operations = max_operations;
    allocator->alloc_prob = alloc_prob;
    allocator->dealloc_prob = dealloc_prob;
    allocator->operation_count = 0;
}

// ClusterTag. Normalize probabilities
void normalizeProbabilities(SimMemoryAllocator *allocator) {
    if (allocator->alloc_prob + allocator->dealloc_prob > 1.0) {
        double total_prob = allocator->alloc_prob + allocator->dealloc_prob;
        allocator->alloc_prob /= total_prob;
        allocator->dealloc_prob /= total_prob;
    }
}

// ClusterTag. Allocate memory block
void allocateBlock(SimMemoryAllocator *allocator) {
    if (allocator->allocated_count < allocator->max_operations) {
        void* new_block = malloc(allocator->block_size);
        allocator->allocated_blocks[allocator->allocated_count].ptr = new_block;
        allocator->allocated_count++;
    }
}

// ClusterTag. Free a random memory block
void freeRandomBlock(SimMemoryAllocator *allocator) {
    if (allocator->allocated_count > 0) {
        // ClusterTag. Randomly select a memory block to free
        int index = rand() % allocator->allocated_count;
        void* free_block = allocator->allocated_blocks[index].ptr;
        free(free_block);

        // ClusterTag. Replace the current freed block with the last memory block
        allocator->allocated_blocks[index] = allocator->allocated_blocks[--allocator->allocated_count];
    }
}

// ClusterTag. Run simulation
void runSimulation(SimMemoryAllocator *allocator) {
    // ClusterTag. Normalize probabilities proportionally
    normalizeProbabilities(allocator);

    // ClusterTag. First initialize and allocate ten memory blocks
    for (int i=0; i<10; i++) {
        allocateBlock(allocator);
        allocator->operation_count++;
    }

    while (allocator->operation_count < allocator->max_operations) {
        // ClusterTag. Determine operation type based on probability
        double op_choice = (double)rand() / RAND_MAX;

        if (op_choice < allocator->alloc_prob) {
            allocateBlock(allocator);
        } else {
            freeRandomBlock(allocator);
        }

        // ClusterTag. Increase operation count
        allocator->operation_count++;
    }
}

void allocateMemoryRandomFree(size_t block_size) {
    // ClusterTag. Initialize memory allocator
    SimMemoryAllocator allocator;
    initializeSimAllocator(&allocator, block_size, FREE_OPERATIONS, 0.6, 0.4);

    // ClusterTag. Run simulation
    runSimulation(&allocator);
}

// ClusterTag. Unified memory allocation function
void allocateMemoryNoFree(size_t block_size) {
    size_t alloc_count = rand() % MALLOC_OPERATIONS + 1000;
    for (size_t i = 0; i < alloc_count; ++i) {
        malloc(block_size);
    }
}

// ClusterTag. Randomly perform 5000-6000 allocation operations for each size of memory block
void beforeMalloc(){
    srand(time(0));
    for (size_t i = 0; i < num_sizes; ++i) {
        size_t block_size = block_sizes[i];
        allocateMemoryNoFree(block_size);
    }
}

// ClusterTag. Randomly perform 5000-6000 allocation operations for each size of memory block
void endMalloc(){
    srand(time(0));
    for (size_t i = 0; i < num_sizes; ++i) {
        size_t block_size = block_sizes[i];
        allocateMemoryNoFree(block_size);
    }
}

// ClusterTag. Randomly perform 5000 allocation/deallocation operations for each size of memory block
void endFree(){
    srand(time(0));
    for (size_t i = 0; i < num_sizes; ++i) {
        size_t block_size = block_sizes[i];
        allocateMemoryRandomFree(block_size);
    }
}


void printLine (const char * line)
{
    if(line != NULL) 
    {
        printf("%s\n", line);
    }
}

void printWLine (const wchar_t * line)
{
    if(line != NULL) 
    {
        wprintf(L"%ls\n", line);
    }
}

void printIntLine (int intNumber)
{
    printf("%d\n", intNumber);
}

void printShortLine (short shortNumber)
{
    printf("%hd\n", shortNumber);
}

void printFloatLine (float floatNumber)
{
    printf("%f\n", floatNumber);
}

void printLongLine (long longNumber)
{
    printf("%ld\n", longNumber);
}

void printLongLongLine (int64_t longLongIntNumber)
{
    printf("%" PRId64 "\n", longLongIntNumber);
}

void printSizeTLine (size_t sizeTNumber)
{
    printf("%zu\n", sizeTNumber);
}

void printHexCharLine (char charHex)
{
    printf("%02x\n", charHex);
}

void printWcharLine(wchar_t wideChar) 
{
    /* ISO standard dictates wchar_t can be ref'd only with %ls, so we must make a
     * string to print a wchar */
    wchar_t s[2];
        s[0] = wideChar;
        s[1] = L'\0';
    printf("%ls\n", s);
}

void printUnsignedLine(unsigned unsignedNumber) 
{
    printf("%u\n", unsignedNumber);
}

void printHexUnsignedCharLine(unsigned char unsignedCharacter) 
{
    printf("%02x\n", unsignedCharacter);
}

void printDoubleLine(double doubleNumber) 
{
    printf("%g\n", doubleNumber);
}

void printStructLine (const twoIntsStruct * structTwoIntsStruct)
{
    printf("%d -- %d\n", structTwoIntsStruct->intOne, structTwoIntsStruct->intTwo);
}

void printBytesLine(const unsigned char * bytes, size_t numBytes)
{
    size_t i;
    for (i = 0; i < numBytes; ++i)
    {
        printf("%02x", bytes[i]);
    }
    puts("");	/* output newline */
}

/* Decode a string of hex characters into the bytes they represent.  The second
 * parameter specifies the length of the output buffer.  The number of bytes
 * actually written to the output buffer is returned. */
size_t decodeHexChars(unsigned char * bytes, size_t numBytes, const char * hex)
{
    size_t numWritten = 0;

    /* We can't sscanf directly into the byte array since %02x expects a pointer to int,
     * not a pointer to unsigned char.  Also, since we expect an unbroken string of hex
     * characters, we check for that before calling sscanf; otherwise we would get a
     * framing error if there's whitespace in the input string. */
    while (numWritten < numBytes && isxdigit(hex[2 * numWritten]) && isxdigit(hex[2 * numWritten + 1]))
    {
        int byte;
        sscanf(&hex[2 * numWritten], "%02x", &byte);
        bytes[numWritten] = (unsigned char) byte;
        ++numWritten;
    }

    return numWritten;
}

/* Decode a string of hex characters into the bytes they represent.  The second
 * parameter specifies the length of the output buffer.  The number of bytes
 * actually written to the output buffer is returned. */
 size_t decodeHexWChars(unsigned char * bytes, size_t numBytes, const wchar_t * hex)
 {
    size_t numWritten = 0;

    /* We can't swscanf directly into the byte array since %02x expects a pointer to int,
     * not a pointer to unsigned char.  Also, since we expect an unbroken string of hex
     * characters, we check for that before calling swscanf; otherwise we would get a
     * framing error if there's whitespace in the input string. */
    while (numWritten < numBytes && iswxdigit(hex[2 * numWritten]) && iswxdigit(hex[2 * numWritten + 1]))
    {
        int byte;
        swscanf(&hex[2 * numWritten], L"%02x", &byte);
        bytes[numWritten] = (unsigned char) byte;
        ++numWritten;
    }

    return numWritten;
}

/* The two functions always return 1 or 0, so a tool should be able to 
   identify that uses of these functions will always return these values */
int globalReturnsTrue() 
{
    return 1;
}

int globalReturnsFalse() 
{
    return 0;
}

int globalReturnsTrueOrFalse() 
{
    // ClusterTag. Probabilistic enable or disable, no need to set this threshold for dynamic testing, so we enable by default
    return 1;
//    return (rand() % 2);
}

/* The variables below are declared "const", so a tool should
   be able to identify that reads of these will always return their 
   initialized values. */
const int GLOBAL_CONST_TRUE = 1; /* true */
const int GLOBAL_CONST_FALSE = 0; /* false */
const int GLOBAL_CONST_FIVE = 5; 

/* The variables below are not defined as "const", but are never
   assigned any other value, so a tool should be able to identify that
   reads of these will always return their initialized values. */
int globalTrue = 1; /* true */
int globalFalse = 0; /* false */
int globalFive = 5; 

/* define a bunch of these as empty functions so that if a test case forgets
   to make their's statically scoped, we'll get a linker error */
void good1() { }
void good2() { }
void good3() { }
void good4() { }
void good5() { }
void good6() { }
void good7() { }
void good8() { }
void good9() { }

/* shouldn't be used, but just in case */
void bad1() { }
void bad2() { }
void bad3() { }
void bad4() { }
void bad5() { }
void bad6() { }
void bad7() { }
void bad8() { }
void bad9() { }

/* define global argc and argv */

#ifdef __cplusplus
extern "C" {
#endif

int globalArgc = 0;
char** globalArgv = NULL;

#ifdef __cplusplus
}
#endif
