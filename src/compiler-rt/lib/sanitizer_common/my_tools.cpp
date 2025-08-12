#include "my_tools.h"

#include "sanitizer_allocator_internal.h"

// ClusterTag. 
#include <time.h>
#include <sys/syscall.h>
#include "stdio.h"
#include "stdlib.h"
#include <fcntl.h>

//using namespace __sanitizer;
namespace __sanitizer {


// ClusterTag. ================Exported global variables=================

u32 my_random_state;

uptr ct_shadow_memory_dynamic_address;
uptr ct_shadow_memory_dynamic_end;
uptr ct_AddressTagMask;
uptr ct_AddressTagShift;
uptr ct_ShadowScale;

uptr page_size_;

// ClusterTag. Used to indicate the periodic trigger time of FreeList
u64 free_list_interval_ns;

// ClusterTag. Used to indicate the density in the Pool
int ct_pool_density;

// ClusterTag. Used to indicate whether to randomize the batch of pointer arrays refilled to CTPerClass
bool ct_randomize_region;

uptr superPoolBase;
uptr superPoolEnd;

// ClusterTag. Persistent memory allocator
CTPersistentAllocator ct_persistent_allocator;
SpinMutex global_persistent_allocator_mutex;


// ClusterTag. ========================Our Persistent Allocator========================
// ClusterTag. Initialize the persistent allocator with a fixed size
bool CTPersistentAllocatorInit(CTPersistentAllocator* persistentAllocator){
  u64 allocator_length = page_size_ * 16;

  // ClusterTag. Use mmap to allocate a 16-page block of persistent memory.
  persistentAllocator->base_addr = (uptr)MmapAlignedOrDieOnFatalError(allocator_length, page_size_, "persistent_allocator");

  persistentAllocator->end_addr = (u64)persistentAllocator->base_addr + allocator_length;
  persistentAllocator->cur_loc  = persistentAllocator->base_addr;
  persistentAllocator->last_loc = 0;

  return true;
}

// ClusterTag. Allocate memory from the persistent allocator
uptr CTPersistentAllocatorAllocate(CTPersistentAllocator* persistentAllocator, u64 size) {
  size = RoundUpTo(size, 0x10);

  // ClusterTag. Check if the allocation limit has been exceeded.
  if (persistentAllocator->cur_loc + size >= persistentAllocator->end_addr) {
    Printf("[ERROR] Requested memory space exceeded the limit of the persistent memory allocator: [0x%lx-0x%lx] > [0x%lx-0x%lx]\n",
           persistentAllocator->cur_loc, persistentAllocator->cur_loc+size,
           persistentAllocator->base_addr, persistentAllocator->end_addr);
    internal__exit(1);
  }

  internal_memset((void*)persistentAllocator->cur_loc, 0, size);
  persistentAllocator->last_loc = persistentAllocator->cur_loc;
  persistentAllocator->cur_loc  = persistentAllocator->cur_loc + size;
  return persistentAllocator->last_loc;
}

// ClusterTag. Free memory from the persistent allocator (only top chunk allowed)
void CTPersistentAllocatorFree(CTPersistentAllocator* persistentAllocator, uptr ptr, bool noCheck) {
  if (ptr == 0 || noCheck) {
    return;
  }

  // ClusterTag. Only the chunk at the top of the stack can be freed
  if (ptr != persistentAllocator->last_loc) {
    Printf("[ERROR] Out-of-order deallocation in persistent memory allocator: "
           "the memory to be freed is not at the top of the stack."
           " Pointer to free: 0x%lx, current stack top: [0x%lx-0x%lx].\n",
           ptr, persistentAllocator->last_loc, persistentAllocator->cur_loc);
    internal__exit(1);
  }

  persistentAllocator->cur_loc  = persistentAllocator->last_loc;
  persistentAllocator->last_loc = 0;
}


// ClusterTag. ========================Time===============

// ClusterTag. Get the current time in seconds and nanoseconds
void CTGetCurTime(u32* sec, long* nsec){
  struct timespec cur_time;
  clock_gettime(CLOCK_MONOTONIC, &cur_time);

  *sec  = (u32)cur_time.tv_sec;
  *nsec = cur_time.tv_nsec;
}

// ClusterTag. ========================Some Tool Functions=================================

// ClusterTag. Convert a string to an integer (max two digits)
int stringToInteger(const char* str) {
  int result = 0;
  int i = 0;

  if (str[i] == '\0') {
    Printf("The string to be converted is an empty string.\n");
    internal__exit(1);
  }

  while (str[i] != '\0') {
    if (i >= 2) {
      Printf("The string to be converted has more than two digits: %s\n", str);
      internal__exit(1);
    }

    if (str[i] < '0' || str[i] > '9') {
      Printf("ClusterTag. The character to be converted is invalid %s\n", str);
      internal__exit(1);
    }

    result = result * 10 + (str[i] - '0');
    i++;
  }

  return result;
}

// ClusterTag. ========================CTSuperPoolChunkInfo==========================

// ClusterTag. Initialize the CTSuperPoolChunkInfo for the super pool
void CTSuperPoolChunkInfoInit(CTSuperPoolChunkInfo* queue, u64 chunk_size){
  queue->free_chunk_num = 0;
  queue->free_region_num = 0;
  queue->region_num = 0;
  if (chunk_size > page_size_)
    queue->trigger_free = 1;
  else
    queue->trigger_free = page_size_ / chunk_size;
  queue->last_release_at_ns =  MonotonicNanoTime();
}

// ClusterTag. ========================Random Generator========================

// ClusterTag. Generate a random seed
u32 genRandomSeed() {
  u32 seed;
  do {
    if (UNLIKELY(!GetRandom((void*)(&seed), sizeof(seed), /*blocking=*/false))) {
      seed = static_cast<u32>(
          (NanoTime() >> 12) ^
          (reinterpret_cast<uptr>(__builtin_frame_address(0)) >> 4));
    }
  } while (!seed);
  return seed;
}

// ClusterTag. Generate a random 32-bit integer
u32 genRandom() {
  my_random_state ^= my_random_state << 13;
  my_random_state ^= my_random_state >> 17;
  my_random_state ^= my_random_state << 5;

  return my_random_state;
}

// ClusterTag. Generate a random 64-bit integer
u64 generate_random_u64() {
  u64 num = 0;
  for (int i = 0; i < sizeof(u64); i += sizeof(u32)) {
    num = (num << (8 * sizeof(u32))) | (genRandom() & 0xFFFFFFFF);
  }
  return num;
}

// ClusterTag. ========================Cluster Release=========================

// ClusterTag. Initialize the free pages range tracker
void CTFreePagesRangeTrackerInit(CTFreePagesRangeTracker* range_tracker, uptr page_start){
  range_tracker->in_the_range = false;
  range_tracker->current_page = page_start;
  range_tracker->current_range_start_page = 0;
}

// ClusterTag. For each sequential and continuous page passed in, call NextPage once. The cumulative release logic is as follows:
// ClusterTag. If it cannot be released, directly release the previously accumulated page range;
// ClusterTag. If it can be released, then:
// ClusterTag.      If in_the_range=false, it means there is no accumulated range before, so initialize current_range_start_page and in_the_range;
// ClusterTag.      If in_the_range=true, it means there is an accumulated range before, so just accumulate current_page once
void CTFreePagesRangeTrackerNextPage(CTFreePagesRangeTracker* range_tracker, bool freed) {
  if (freed) {
    if (!range_tracker->in_the_range) {
      range_tracker->current_range_start_page = range_tracker->current_page;
      range_tracker->in_the_range = true;
    }
  } else {
    CTFreePagesRangeTrackerCloseOpenedRange(range_tracker);
  }
  range_tracker->current_page += page_size_;
}

// ClusterTag. Finalize the free pages range tracker
void CTFreePagesRangeTrackerDone(CTFreePagesRangeTracker* range_tracker) {
  CTFreePagesRangeTrackerCloseOpenedRange(range_tracker);
}

// ClusterTag. Fragmented release of the space in this Region
void CTFreePagesRangeTrackerCloseOpenedRange(CTFreePagesRangeTracker* range_tracker) {
  if (range_tracker->in_the_range) {

    CTAdvise(range_tracker->current_range_start_page, range_tracker->current_page-range_tracker->current_range_start_page);

    // ClusterTag. Check whether the shadow memory corresponding to the fragment of this Region can form a complete page; if so, release it as well
    uptr range_shadow_start_page = RoundUpTo(range_tracker->current_range_start_page, page_size_);
    uptr range_shadow_end_page   = RoundDownTo(range_tracker->current_page, page_size_);
    if (range_shadow_end_page > range_shadow_start_page) {
      CTAdvise(range_shadow_start_page, range_shadow_end_page - range_shadow_start_page);
    }

    range_tracker->in_the_range = false;
  }
}


// ClusterTag. ========================CTPoolList========================

// ClusterTag. Initialize a new pool list
CTPoolList* initCTPoolList() {
  CTPoolList* list = (CTPoolList*) CTPersistentAllocatorAllocate(&ct_persistent_allocator, sizeof(CTPoolList));
  list->head = nullptr;
  list->tail = nullptr;
  return list;
}

// ClusterTag. Append a new pool node to the pool list
CTPoolNode* appendCTPoolList(CTPoolList* list, uptr pool_addr) {
  CTPoolNode* newPool = (CTPoolNode*) CTPersistentAllocatorAllocate(&ct_persistent_allocator, sizeof(CTPoolNode));

  newPool->addr = pool_addr;
  newPool->density = 0;
  newPool->next = nullptr;

  if (list->head == nullptr) {
    list->head = newPool;
    list->tail = newPool;
  } else {
    list->tail->next = newPool;
    list->tail = newPool;
  }
  return newPool;
}

// ClusterTag. Traverse and print all pool nodes in the pool list
void traverseCTPoolList(CTPoolList* list) {
  CTPoolNode* currentNode = list->head;
  while (currentNode != nullptr) {
    Printf("%p\n", currentNode->addr);
    currentNode = currentNode->next;
  }
}

// ClusterTag. Free all nodes in the pool list
void freeCTPoolList(CTPoolList* list) {
  if (list == nullptr) return;

  CTPoolNode* currentNode = list->head;
  while (currentNode != nullptr) {
    CTPoolNode* nextNode = currentNode->next;
    InternalFree(currentNode);
    currentNode = nextNode;
  }
  InternalFree(list);
}

// ClusterTag. ========================CTPoolList========================


// ClusterTag. ========================Memory Management with Kernel========================

// ClusterTag. Allocate memory using the internal allocator
void* CTInternalAlloc(u64 size){
  return InternalAlloc(size, nullptr, 8);
}

// ClusterTag. Free memory using the internal allocator
void CTInternalFree(void* addr){
  InternalFree(addr, nullptr);
}

// ClusterTag. Map memory at a fixed address, or die on failure
bool CTFixedMapOrDie(u64 addr, u64 length){
  uptr retu = internal_mmap((void*)addr, length, 0b11 /*PROT_READ | PROT_WRITE*/, 0b100010 /*MAP_ANON | MAP_PRIVATE | NO MAP_FIXED!!!*/, -1 /*fd*/, 0 /*offset*/);
  if ((u64)retu == addr)
    return true;
  else{
    int reserrno;
    if (UNLIKELY(internal_iserror(retu, &reserrno))) {
      Printf("Allocation failed, error code %d reserrno=%d\n", retu, reserrno);
      internal__exit(1);
    }
    else{
      if (UNLIKELY(internal_munmap((void*)retu, length))) {
        Printf("Allocation failed. Additionally, was unable to free a different address returned by the kernel.\n");
        internal__exit(1);
      }
      return false;
    }
  }
}

// ClusterTag. Force map memory at a fixed address
void CTFixedMapForced(u64 addr, u64 length){
  uptr retu = internal_mmap((void*)addr, length, 0b11 /*PROT_READ | PROT_WRITE*/, 0b110010 /*MAP_ANON | MAP_PRIVATE | MAP_FIXED!!!*/, -1 /*fd*/, 0 /*offset*/);
  int reserrno;
  if (UNLIKELY(internal_iserror(retu, &reserrno))) {
    Printf("Failed to request memory using mmap+Force+Fixed, error code %d reserrno=%d\n", retu, reserrno);
    internal__exit(1);
  }
}

// ClusterTag. Advise the kernel to release memory
void CTAdvise(u64 addr, u64 length){
  uptr retu = internal_madvise(addr, length, 4/*MADV_DONTNEED	defined in bits/mman.h*/);
  int reserrno;
  if (UNLIKELY(internal_iserror(retu, &reserrno))) {
    // ClusterTag. If it is an error code, print the error code and exit directly; this is a runtime exception behavior of the program
    Printf("Failed to release memory using advise, error code %d reserrno=%d\n", retu, reserrno);
    internal__exit(1);
  }
}


// ClusterTag. Map memory or die on failure
uptr CTMapOrDie(u64 length) {
  uptr retu =
      internal_mmap(nullptr, length, 0b11 /*PROT_READ | PROT_WRITE*/,
                    0b100010 /*MAP_ANON | MAP_PRIVATE | NO MAP_FIXED!!!*/,
                    -1 /*fd*/, 0 /*offset*/);

  int reserrno;
  if (UNLIKELY(internal_iserror(retu, &reserrno))) {
    Printf("Request failed, error code %d reserrno=%d\n", retu, reserrno);
    internal__exit(1);
  } else {
    return retu;
  }
}

// ClusterTag. ========================Memory Management with Kernel========================

}
