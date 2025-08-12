//
// Created by root on 4/17/24.
//
// NOTE: 头文件并不能被随意引入在其他头文件中，如果目的地位于一个特殊的命名空间中的话，my_tools.h中的所有对象也都会被施加以该命名空间，从而导致与表项不符


#ifndef LLVM_MY_TOOLS_H
#define LLVM_MY_TOOLS_H

#include "sanitizer_common.h"

namespace __sanitizer {

// ==================结构体的前向声明===========================
struct IntervalNode;
struct __bloomfilter;
struct RBTreeNode;
struct RBTree;
struct CTPerClass;
struct CTPerClassRegionStatistic;
struct CTLargeChunkHeader;
struct CTRegionInfo;
struct CTPersistentAllocator;



extern uptr ct_shadow_memory_dynamic_address;
extern uptr ct_shadow_memory_dynamic_end;
extern uptr ct_AddressTagMask;
extern uptr ct_AddressTagShift;
extern uptr ct_ShadowScale;
extern uptr page_size_;
extern bool hwasan_need_trace_allocator;
extern const char* hwasan_trace_allocator_output;
extern bool hwasan_need_coarse_trace_allocator;
extern bool ct_randomize_region;
extern u64 free_list_interval_ns;
extern int ct_pool_density;

extern CTPersistentAllocator ct_persistent_allocator;
extern SpinMutex global_persistent_allocator_mutex;

extern uptr superPoolBase;
extern uptr superPoolEnd;

// ==================CTPersistentAllocator====================

typedef struct CTPersistentAllocator {
    uptr base_addr;
    uptr cur_loc;
    uptr last_loc;
    uptr end_addr;
} CTPersistentAllocator;

// 分配
extern bool CTPersistentAllocatorInit(CTPersistentAllocator* persistentAllocator);
extern uptr CTPersistentAllocatorAllocate(CTPersistentAllocator* persistentAllocator, u64 size);
extern void CTPersistentAllocatorFree(CTPersistentAllocator* persistentAllocator, uptr ptr, bool noCheck=0);

// ==================Random Generator==================
extern u32 my_random_state;

u32 genRandomSeed();
u32 genRandom();
u64 generate_random_u64();

// ==================CTPoolList======================

// 定义单链表节点
struct CTPoolNode {
    uptr addr;  // 记录该Pool的地址
    u64 density; // 记录该Pool中有多少活跃的Region
    CTPoolNode* next;
};

// 定义单链表
struct CTPoolList {
    CTPoolNode* head;
    CTPoolNode* tail;
};

CTPoolList* initCTPoolList();
CTPoolNode* appendCTPoolList(CTPoolList* list, uptr pool_addr);
void traverseCTPoolList(CTPoolList* list);
void freeCTPoolList(CTPoolList* list);


// ==================Memory Management with Kernel=========================

bool CTFixedMapOrDie(u64 addr, u64 length);
void CTFixedMapForced(u64 addr, u64 length);
uptr CTMapOrDie(u64 length);
void CTAdvise(u64 addr, u64 length);

// ClusterTag. We avoid allocating Large Chunks in Shadow Memory and the SuperPool reserved for Small Chunks.
inline bool CTMapBlackCheck(uptr addr, u64 length){
  if ((addr >= ct_shadow_memory_dynamic_address && addr <= ct_shadow_memory_dynamic_end) || (addr+length >= ct_shadow_memory_dynamic_address && addr+length <= ct_shadow_memory_dynamic_end))
    return false;

  if ((addr >= superPoolBase && addr < superPoolEnd) || (addr+length >= superPoolBase && addr+length < superPoolEnd))
    return false;

  return true;
}

void* CTInternalAlloc(u64 size);
void CTInternalFree(void* p);

// ===================CTFreePagesRangeTracker===================

typedef struct CTFreePagesRangeTracker {
    bool in_the_range;
    uptr current_page;
    uptr current_range_start_page;
} CTFreePagesRangeTracker;

void CTFreePagesRangeTrackerInit(CTFreePagesRangeTracker* range_tracker, uptr page_start);
void CTFreePagesRangeTrackerNextPage(CTFreePagesRangeTracker* range_tracker, bool freed);
void CTFreePagesRangeTrackerDone(CTFreePagesRangeTracker* range_tracker);
void CTFreePagesRangeTrackerCloseOpenedRange(CTFreePagesRangeTracker* range_tracker);


// ===================CTSuperPoolChunkInfo======================

typedef struct CTSuperPoolChunkInfo{
  u64 free_chunk_num;      // Number of reusable chunks in the Region list
  u64 region_num;          // Number of Regions in the Region list
  u64 free_region_num;     // Number of Regions in the Freed Region list
  u32 trigger_free;        // When free_chunk_num reaches trigger_free, a periodic release is attempted
  u64 last_release_at_ns;  // The timestamp of the last release on the FreeList
} CTSuperPoolChunkInfo;

void CTSuperPoolChunkInfoInit(CTSuperPoolChunkInfo* spc_info, u64 chunk_size);


// ==================Tag Manipulation Functions.===================

inline uptr CTAddTagToPointer(uptr ptr, u8 tag) {
  return (ptr & ~ct_AddressTagMask) | ((uptr)tag << ct_AddressTagShift);
}
inline u8 CTGetTagFromPointer(uptr taggedPtr){
  return (taggedPtr & ct_AddressTagMask) >> ct_AddressTagShift;
}
inline uptr CTUntagPointer(uptr taggedPtr){
  return taggedPtr & ~ct_AddressTagMask;
}
inline uptr CTUntaggedPointerToShadow(uptr untaggedPtr) {
  return (untaggedPtr >> ct_ShadowScale) + ct_shadow_memory_dynamic_address;
}
inline uptr CTTaggedPointerToShadow(uptr taggedPtr) {
  uptr untaggedPtr = CTUntagPointer(taggedPtr);
  return (untaggedPtr >> ct_ShadowScale) + ct_shadow_memory_dynamic_address;
}
inline u64 CTChunkToShadowLength(u64 chunkLength){
  return chunkLength >> ct_ShadowScale;
}

// ==================Some Tool Functions==========================

inline uptr RoundUpDivide(uptr a, uptr b){
  return (a+b-1) / b;
}
int stringToInteger(const char* str);

void CTGetCurTime(u32* sec, long* nsec);


// ==================CTPerClass==================

struct CTPerClass {
  u32 count;
  void* batch[1024];
  u8 chunk_tag[1024];
  uptr class_size;

  SpinMutex perclass_fallback_mutex;
};

struct CTPerClassRegionStatistic {
  u64 highFreeWithLongTimeRegionNum;
  u64 allRegionNum;

  void show(CTPerClass* perClass){
    Printf("Region Size with [0x%lx]: HighFreeWithLongTimeRegionNum/AllRegion=[%llu/%llu]\n",
      perClass->class_size, highFreeWithLongTimeRegionNum, allRegionNum);
  }
};

struct __attribute__((packed))
CTLargeChunkHeader {
  u64 allocated_size;
  u64 requested_size;
};

inline CTLargeChunkHeader* GetCTLargeChunkHeader(const void *p){
  return (CTLargeChunkHeader*)((u64)p - 16 /*chunkheader size*/);
}

struct CTRegionList {
    CTRegionInfo* head;
};


// ==================CTRegionInfo==================

struct CTRegionInfo {
  // ClusterTag. Stores the tag for each chunk, where a value of 0 means
  // the chunk is in use, and a non-zero value indicates the chunk is free
  // and is the tag it last used.
  u8 chunk_last_tag[256];

  CTPoolNode* parent_pool;
  // ClusterTag. SuperPoolsRegionList
  CTRegionInfo* prev;
  CTRegionInfo* next;
  // ClusterTag. SuperPoolsFreedRegionList
  CTRegionInfo* freed_next;
  // ClusterTag. This flag indicates whether the ClusterTag is on the Free list.
  u8 in_free_list;

  // ClusterTag. Records how many chunks in this region are in a freed state.
  u8 freed_num;
  // ClusterTag. 1 indicates the Region is active; 0 indicates the Region is unallocated.
  u8 status;

  inline void insertAtFreedListHead(CTRegionList* list){
    this->freed_next = list->head;
    list->head = this;
  }

  inline void deleteFromList(CTRegionList* list){
    if (this->prev != nullptr) {
      this->prev->next = this->next;
    } else {
      list->head = this->next;
    }

    if (this->next != nullptr) {
      this->next->prev = this->prev;
    }
  }

  inline void insertAtHead(CTRegionList* list){
    this->prev = nullptr;
    this->next = list->head;

    if (list->head != nullptr) {
      list->head->prev = this;
    }

    list->head = this;
  }

  inline void insertInOrder(CTRegionList* list){
    if (list->head == nullptr || this < list->head) {
      insertAtHead(list);
      return;
    }

    CTRegionInfo* current = list->head;
    while (current->next != nullptr && current->next < this) {
      current = current->next;
    }

    this->next = current->next;
    this->prev = current;
    if (current->next != nullptr) {
      current->next->prev = this;
    }
    current->next = this;
  }

};


}

#endif  // LLVM_MY_TOOLS_H
