//===-- sanitizer_allocator_primary64.h -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Part of the Sanitizer Allocator.
//
//===----------------------------------------------------------------------===//
#ifndef SANITIZER_ALLOCATOR_H
#error This file must be included inside sanitizer_allocator.h
#endif

#include "sanitizer_platform_limits_netbsd.h"
#include "sanitizer_platform_limits_posix.h"
#include "sanitizer_platform_limits_solaris.h"

template<class SizeClassAllocator> struct SizeClassAllocator64LocalCache;

// SizeClassAllocator64 -- allocator for 64-bit address space.
// The template parameter Params is a class containing the actual parameters.
//
// Space: a portion of address space of kSpaceSize bytes starting at SpaceBeg.
// If kSpaceBeg is ~0 then SpaceBeg is chosen dynamically by mmap.
// Otherwise SpaceBeg=kSpaceBeg (fixed address).
// kSpaceSize is a power of two.
// At the beginning the entire space is mprotect-ed, then small parts of it
// are mapped on demand.
//
// Region: a part of Space dedicated to a single size class.
// There are kNumClasses Regions of equal size.
//
// UserChunk: a piece of memory returned to user.
// MetaChunk: kMetadataSize bytes of metadata associated with a UserChunk.

// FreeArray is an array free-d chunks (stored as 4-byte offsets)
//
// A Region looks like this:
// UserChunk1 ... UserChunkN <gap> MetaChunkN ... MetaChunk1 FreeArray

struct SizeClassAllocator64FlagMasks {  //  Bit masks.
  enum {
    kRandomShuffleChunks = 1,
  };
};

template <typename Allocator>
class MemoryMapper {
 public:
  typedef typename Allocator::CompactPtrT CompactPtrT;

  explicit MemoryMapper(const Allocator &allocator) : allocator_(allocator) {}

  bool GetAndResetStats(uptr &ranges, uptr &bytes) {
    ranges = released_ranges_count_;
    released_ranges_count_ = 0;
    bytes = released_bytes_;
    released_bytes_ = 0;
    return ranges != 0;
  }

  u64 *MapPackedCounterArrayBuffer(uptr count) {
    buffer_.clear();
    buffer_.resize(count);
    return buffer_.data();
  }

  // ClusterTag. Releases [from, to) range of pages back to OS.
  void ReleasePageRangeToOS(uptr class_id, CompactPtrT from, CompactPtrT to) {
    const uptr region_base = allocator_.GetRegionBeginBySizeClass(class_id);
    const uptr from_page = allocator_.CompactPtrToPointer(region_base, from);
    const uptr to_page = allocator_.CompactPtrToPointer(region_base, to);
    ReleaseMemoryPagesToOS(from_page, to_page);
    released_ranges_count_++;
    released_bytes_ += to_page - from_page;
  }

 private:
  const Allocator &allocator_;
  uptr released_ranges_count_ = 0;
  uptr released_bytes_ = 0;
  InternalMmapVector<u64> buffer_;
};

template <class Params>
class SizeClassAllocator64 {
 public:

  bool is_clustertag = false;

  typedef typename Params::SizeClassMap HWSizeClassMap;
  static const u8 kSuperPoolShift = 40;                  // ClusterTag. The size of each SuperPool, currently set to 1TB.
  static const uptr kSuperPoolSize = 0x10000000000;      // 1 << 40
  static const uptr kSuperPoolMask = (1UL << kSuperPoolShift) - 1;
  static const u8 kPoolShift = 30;                       // ClusterTag. The size of each Pool, currently set to 1GB.
  static const uptr kPoolSize = 1 << kPoolShift;
  static const uptr kPoolMask = (1UL << kPoolShift) - 1;

  static const uptr superPoolBase = 0x0000010000000000;
  static const uptr superPoolEnd = SizeClassAllocator64::superPoolBase + SizeClassAllocator64::kSuperPoolSize*HWSizeClassMap::kBatchClassID /*31*/;
  uptr SuperPoolsBaseAddress[HWSizeClassMap::kBatchClassID];

  // ClusterTag. SuperPool's Pool linked list.
  CTPoolList* SuperPoolsPoolLinkedList[HWSizeClassMap::kBatchClassID];

  // ClusterTag. The SuperPool contains two Region lists: SuperPoolRegionList tracks all in-use Regions, while SuperPoolsFreedRegionList tracks Regions that contain available chunks.
  CTRegionList* SuperPoolsRegionList[HWSizeClassMap::kBatchClassID];
  CTRegionList* SuperPoolsFreedRegionList[HWSizeClassMap::kBatchClassID];

  CTSuperPoolChunkInfo* SuperPoolChunkInfo[HWSizeClassMap::kBatchClassID];

  u64 AllRegionsInPool[HWSizeClassMap::kBatchClassID];

  u8 AvailableChunksInRegion[HWSizeClassMap::kBatchClassID];
  uptr SuperPoolsChunkSize[HWSizeClassMap::kBatchClassID];
  uptr SuperPoolsRegionSize[HWSizeClassMap::kBatchClassID];

  u16 MinChunkInPerClass[HWSizeClassMap::kBatchClassID];
  u16 MaxChunkInPerClass[HWSizeClassMap::kBatchClassID];

  SpinMutex SuperPoolLocksForSizeClass[HWSizeClassMap::kBatchClassID];
  SpinMutex SuperPoolLockForLargeChunk;

  // ClusterTag. Initialize ClusterTag's memory allocator.
  void init_for_clustertag(){
    this->is_clustertag = true;

    this->SuperPoolsBaseAddress[0]   = superPoolBase;
    this->SuperPoolsChunkSize[0]     = 0;
    this->SuperPoolsRegionSize[0]    = 0;
    this->AllRegionsInPool[0]        = 0;
    this->AvailableChunksInRegion[0] = 0;
    for (int i=1; i<HWSizeClassMap::kBatchClassID; i++){
      this->SuperPoolsBaseAddress[i]    = superPoolBase + i*kSuperPoolSize;
      this->SuperPoolsChunkSize[i]      = SizeClassMap::Size(i);
      this->SuperPoolsRegionSize[i]     = this->SuperPoolsChunkSize[i] * 256;
      this->SuperPoolsPoolLinkedList[i] = initCTPoolList();

      // ClusterTag. Calculate how many chunks can fit in a Region after removing the RegionInfo header.
      this->AvailableChunksInRegion[i]  = 256 - RoundUpDivide(sizeof(CTRegionInfo), this->SuperPoolsChunkSize[i]); // 计算RegionInfo占据多少Chunk
      if (this->AvailableChunksInRegion[i] >= 256 || this->AvailableChunksInRegion[i] < 1){
        Printf("Cannot calculate the correct number of chunks for Region(class_id=%d), the current value %d is invalid.\n", i, this->AvailableChunksInRegion[i]);
        internal__exit(1);
      }

      // ClusterTag. Calculate the number of Regions that can be stored in the Pool, and round down.
      this->AllRegionsInPool[i] = this->kPoolSize / this->SuperPoolsRegionSize[i];

      this->SuperPoolChunkInfo[i] = (CTSuperPoolChunkInfo*) CTPersistentAllocatorAllocate(&ct_persistent_allocator, sizeof(CTSuperPoolChunkInfo));
      CTSuperPoolChunkInfoInit(this->SuperPoolChunkInfo[i], this->SuperPoolsChunkSize[i]);

      this->SuperPoolsRegionList[i] = (CTRegionList*) CTPersistentAllocatorAllocate(&ct_persistent_allocator, sizeof(CTRegionList));
      this->SuperPoolsRegionList[i]->head = nullptr;
      this->SuperPoolsFreedRegionList[i] = (CTRegionList*) CTPersistentAllocatorAllocate(&ct_persistent_allocator, sizeof(CTRegionList));
      this->SuperPoolsFreedRegionList[i]->head = nullptr;

      u16 minChunks = (2*page_size_) / this->SuperPoolsChunkSize[i];
      this->MinChunkInPerClass[i] = minChunks < 256 ? 256 : minChunks;
      this->MaxChunkInPerClass[i] = this->MinChunkInPerClass[i] + 256;
    }

    __sanitizer::superPoolBase = this->superPoolBase;
    __sanitizer::superPoolEnd = this->superPoolEnd;
  }

  static inline bool isSmallChunk(uptr untagged_ptr){
    if ((untagged_ptr & ~ct_AddressTagMask) < SizeClassAllocator64::superPoolBase || (untagged_ptr & ~ct_AddressTagMask) >= SizeClassAllocator64::superPoolEnd)
      return false;
    return true;
  }

  static inline uptr getSuperPoolID(uptr untagged_ptr){
    if (!isSmallChunk(untagged_ptr)){
      Printf("Pointer %p is not a Small Chunk.\n", untagged_ptr);
      internal__exit(1);
    }

    return ((untagged_ptr-SizeClassAllocator64::superPoolBase) & ~kSuperPoolMask) >> SizeClassAllocator64::kSuperPoolShift;
  }

  static inline uptr getChunkSize(uptr untagged_ptr){
    if (!isSmallChunk(untagged_ptr)){
      Printf("Pointer %p is not a Small Chunk.\n", untagged_ptr);
      internal__exit(1);
    }

    return SizeClassMap::Size(getSuperPoolID(untagged_ptr));
  }

  static inline CTRegionInfo* getRegionInfo(uptr untagged_ptr){
    uptr region_size = getChunkSize(untagged_ptr) * 256;
    return (CTRegionInfo*)((untagged_ptr & ~kPoolMask) + ((untagged_ptr & kPoolMask) / region_size * region_size));
  }

  static inline CTRegionInfo* getRegionInfo(uptr untagged_ptr, uptr chunk_size){
    uptr region_size = chunk_size * 256;
    return (CTRegionInfo*)((untagged_ptr & ~kPoolMask) + ((untagged_ptr & kPoolMask) / region_size * region_size));
  }

  static inline void parseSmallChunkPtr(uptr untagged_ptr, uptr* superPoolID, uptr* poolID, u8* chunkID, uptr* chunk_size, CTRegionInfo** regionInfo){
    if (!isSmallChunk(untagged_ptr)){
      Printf("Pointer %p is not a Small Chunk.\n", untagged_ptr);
      internal__exit(1);
    }

    *superPoolID = ((untagged_ptr-SizeClassAllocator64::superPoolBase) & ~kSuperPoolMask) >> SizeClassAllocator64::kSuperPoolShift;
    *poolID = (untagged_ptr&kSuperPoolMask) >> SizeClassAllocator64::kPoolShift;
    *chunk_size = SizeClassMap::Size(*superPoolID);

    uptr region_size = *chunk_size * 256;
    *regionInfo = (CTRegionInfo*)((untagged_ptr & ~kPoolMask) + ((untagged_ptr & kPoolMask) / region_size * region_size));

    *chunkID = (untagged_ptr - (u64)(*regionInfo)) / *chunk_size;
  }

  // ClusterTag. Get the requested size of a chunk, handling both small and large chunks.
  static inline uptr CTGetRequestedSize(uptr untagged_ptr){
    uptr requested_size = 0;
    if (isSmallChunk(untagged_ptr)){
      uptr superPoolID = ((untagged_ptr-SizeClassAllocator64::superPoolBase) & ~kSuperPoolMask) >> SizeClassAllocator64::kSuperPoolShift;
      requested_size = SizeClassMap::Size(superPoolID);
    }
    else{
      CTLargeChunkHeader* chunkHeader = GetCTLargeChunkHeader((void*)untagged_ptr);
      requested_size = chunkHeader->requested_size;
    }
    return requested_size;
  }

  // ClusterTag. Allocate a new Pool from SuperPool and add it to the PoolList.
  NOINLINE CTPoolNode* CTAllocateNewPool(uptr class_id) {
    CTPoolList *pool_list = this->SuperPoolsPoolLinkedList[class_id];
    uptr superPoolBaseAddress = this->SuperPoolsBaseAddress[class_id];

    bool duplicate_pool = false;
    uptr new_pool_addr = 0;
    do {
      // ClusterTag. Generate a random PoolID.
      uptr randomPoolID = __sanitizer::generate_random_u64() % (this->kSuperPoolSize / this->kPoolSize);
      new_pool_addr = superPoolBaseAddress + (randomPoolID * this->kPoolSize);

      // ClusterTag. Check if the Pool has already been allocated in the pool_list.
      CTPoolNode *currentPool = pool_list->head;
      duplicate_pool = false;
      while (currentPool != nullptr) {
        if ((uptr) (currentPool->addr) == new_pool_addr) {
          duplicate_pool = true;
          break;
        }
        currentPool = currentPool->next;
      }
    } while (duplicate_pool);

    CTPoolNode* pool_node = appendCTPoolList(pool_list, new_pool_addr);

    // ClusterTag. Allocate the entire 1GB of space from the Pool.
    bool pool_map_status = CTFixedMapOrDie(new_pool_addr, this->kPoolSize);
    if (pool_map_status == false) {
      Printf("SuperPool=0x%lx Pool=0x%lx Failed to allocate Pool. This may be because another memory allocator has already claimed this space. Consider using pre-allocation + forced reallocation to protect this space, or modify the SuperPool base address.\n", superPoolBaseAddress, new_pool_addr);
      internal__exit(1);
    }

    return pool_node;
  }

  // ClusterTag. Allocate a new Region from a Pool and update region info.
  NOINLINE uptr CTAllocateNewRegion(uptr class_id) {
    CHECK_NE(class_id, 0UL);
    CHECK_LT(class_id, kNumClasses);

    uptr region_size = this->SuperPoolsRegionSize[class_id];
    CHECK_EQ(IsAligned(region_size, page_size_), true);

    CTPoolList* pool_list = this->SuperPoolsPoolLinkedList[class_id];
    CTPoolNode* findPoolNode = 0;
    CTPoolNode* currentPool = pool_list->head;
    while (currentPool != nullptr) {
      if (currentPool->density * __sanitizer::ct_pool_density < this->AllRegionsInPool[class_id]){
        findPoolNode = currentPool;
        break;
      }
      currentPool = currentPool->next;
    }

    if (findPoolNode == 0)
      findPoolNode = CTAllocateNewPool(class_id);

    uptr new_region = 0;
    uptr randomRegionID = 0;
    u8 searchCount = 0;
    do{
      searchCount += 1;
      randomRegionID = __sanitizer::generate_random_u64() % this->AllRegionsInPool[class_id];
      if (((CTRegionInfo*)(findPoolNode->addr + (region_size * randomRegionID)))->status == 0) {
        new_region = findPoolNode->addr + (region_size * randomRegionID);
      }
    } while(new_region == 0);

    CTRegionInfo* regionInfo = (CTRegionInfo*) new_region;
    regionInfo->status = 1;
    regionInfo->freed_num = 0;
    regionInfo->in_free_list = 0;
    regionInfo->parent_pool = findPoolNode;
    regionInfo->insertInOrder(this->SuperPoolsRegionList[class_id]);
    findPoolNode->density += 1;
    this->SuperPoolChunkInfo[class_id]->region_num += 1;

    return new_region;
  }

  // ClusterTag. Try to refill chunks from the freed region list.
  inline bool CTRefillFromRegionList(CTPerClass *c, uptr class_id, CTSuperPoolChunkInfo* spc, u64 need_chunk_min, u64 need_chunk_max) {
    if (spc->free_chunk_num < need_chunk_min)
      return false;

    CTRegionList* region_freed_list = this->SuperPoolsFreedRegionList[class_id];
    u8 max                              = this->AvailableChunksInRegion[class_id];
    u64 chunk_size                      = this->SuperPoolsChunkSize[class_id];
    CTRegionInfo* cur_region;
    cur_region = region_freed_list->head;

    // ClusterTag. Select a random offset and move cur_region pointer
    u64 random_region_idx;
    if (spc->free_region_num > 64)
      random_region_idx = __sanitizer::genRandom() % 64;
    else
      random_region_idx = __sanitizer::genRandom() % spc->free_region_num;

    CTRegionInfo* prev_region = nullptr;
    CTRegionInfo* start_region = nullptr;
    for (u64 i = 0; i < random_region_idx; i++) {
      prev_region = cur_region;
      cur_region = cur_region->freed_next;
    }
    start_region = cur_region;

    // ClusterTag. Debug information.
    // Printf("CTRefill: chunk_size=0x%lx, total_free_regions=%llu, random_start=%ld\n",
    //    chunk_size, spc->free_region_num, random_region_idx);

    bool is_cycle = false;
    c->class_size = chunk_size;
    c->count = 0;
    u16 batch_idx = 0;
    u8 last_tag;
    u8 prev_tag;
    int first_ele;
    u8 cur_chunk_tag;
    u8 cur_freed_chunk_id;
    while (true) {

      // ClusterTag. If we collected enough chunks before traversing the entire list,
      // connect the previous node with the current region's next node
      if (batch_idx >= need_chunk_min) {
        // If prev_region is not nullptr, it means the random start point is not 0
        if (prev_region != nullptr) {
          if (is_cycle) {
            // Use cur_region as the start point and prev_region as the end point
            prev_region->freed_next = nullptr;
            region_freed_list->head = cur_region;
            }
          else
            // Connect prev_region and cur_region
            prev_region->freed_next = cur_region;
        }
        else {
          // When the random start point is 0, is_cycle must be false,
          // because we can't reach here, it will be handled and terminated by cur_region == start_region at the end
          // In this case, just set cur_region as the new head of the list
          region_freed_list->head = cur_region;
        }
        
        return true;
      }

      last_tag = 0;
      first_ele = -1;
      cur_chunk_tag = 0;
      for (int i = 256 - max; i < 256; i++) {
        cur_chunk_tag = cur_region->chunk_last_tag[i];

        if (cur_chunk_tag != 0) {
          if (UNLIKELY(first_ele == -1))
            first_ele = batch_idx;
          c->chunk_tag[batch_idx + 1] = cur_chunk_tag;
          last_tag = cur_chunk_tag;

          c->batch[batch_idx] = (void *) ((uptr) cur_region + chunk_size * i);
          batch_idx += 1;

          cur_region->chunk_last_tag[i] = 0;
        }
      }
      c->chunk_tag[first_ele] = last_tag;

      c->count += cur_region->freed_num;
      spc->free_chunk_num -= cur_region->freed_num;
      spc->free_region_num -= 1;
      cur_region->freed_num = 0;
      cur_region->in_free_list = 0;

      cur_region = cur_region->freed_next;
      
      // ClusterTag. If we reach the end of the list, wrap around to the head
      if (cur_region == nullptr) {
        cur_region = region_freed_list->head;
        is_cycle = true;
      }
      
      // ClusterTag. Check if we've returned to the starting point (traversed the entire list)
      if (cur_region == start_region) {
        region_freed_list->head = nullptr;

        if (c->count == 0)
          return false;
        else
          return true;
      }

    }
  }

  // ClusterTag. Refill the chunk batch for a class, randomizing if needed.
  NOINLINE bool CTRefill(CTPerClass *c, uptr class_id) {
    bool refill_from_free_list = false;
    uptr chunk_size = this->SuperPoolsChunkSize[class_id];
    u8 available_chunk_num = this->AvailableChunksInRegion[class_id];

    CTSuperPoolChunkInfo* spc = this->SuperPoolChunkInfo[class_id];
    if (spc->free_chunk_num >= this->MinChunkInPerClass[class_id])
      refill_from_free_list = CTRefillFromRegionList(c, class_id, spc,
                                                         this->MinChunkInPerClass[class_id],
                                                         this->MaxChunkInPerClass[class_id]);

    if (refill_from_free_list)
      return true;

    uptr region_addr = CTAllocateNewRegion(class_id);
    uptr available_chunk_addr = region_addr + (chunk_size * (256-available_chunk_num));
    c->class_size = chunk_size;
    c->count = available_chunk_num;
    for (int i=0; i<available_chunk_num; i++){
      c->batch[i] = (void*)(available_chunk_addr);
      c->chunk_tag[i] = i+1;
      available_chunk_addr += chunk_size;
    }

    for (int i = available_chunk_num-1; i > 0; i--) {
      u32 rand_num = genRandom();
      int j = rand_num % (i + 1);

      u8 temp = c->chunk_tag[i];
      c->chunk_tag[i] = c->chunk_tag[j];
      c->chunk_tag[j] = temp;
    }

    if (__sanitizer::ct_randomize_region){
      for (int i = available_chunk_num-1; i > 0; i--) {
        u32 rand_num = genRandom();
        int j = rand_num % (i + 1);

        void* temp = c->batch[i];
        c->batch[i] = c->batch[j];
        c->batch[j] = temp;
      }
    }

    return true;
  }

  // ClusterTag. Release a region and advise the OS to free its memory.
  void CTReleaseRegion(void* region_base, uptr class_id){
    uptr region_size = this->SuperPoolsRegionSize[class_id];

    CHECK(IsAligned((uptr)region_base, page_size_));
    CHECK(IsAligned(region_size, page_size_));

    ((CTRegionInfo*)(region_base))->status = 0;
    CTAdvise((uptr)region_base, region_size);
  }

  inline void check_fast_release_path (u64 chunk_size, uptr* full_pages_chunk_count_max, bool* same_chunk_count_per_page){
    if (chunk_size <= page_size_ && page_size_ % chunk_size == 0) {
      *full_pages_chunk_count_max = page_size_ / chunk_size;
      *same_chunk_count_per_page = true;
    } else if (chunk_size <= page_size_ && page_size_ % chunk_size != 0 &&
               chunk_size % (page_size_ % chunk_size) == 0) {
      *full_pages_chunk_count_max = page_size_ / chunk_size + 1;
      *same_chunk_count_per_page = true;
    } else if (chunk_size <= page_size_) {
      *full_pages_chunk_count_max = page_size_ / chunk_size + 2;
      *same_chunk_count_per_page = false;
    } else if (chunk_size > page_size_ && chunk_size % page_size_ == 0) {
      *full_pages_chunk_count_max = 1;
      *same_chunk_count_per_page = true;
    } else if (chunk_size > page_size_) {
      *full_pages_chunk_count_max = 2;
      *same_chunk_count_per_page = false;
    } else {
      UNREACHABLE("All chunk_size/page_size ratios must be handled.");
    }
  }

  // ClusterTag. Release fragments of a region, tracking freed pages and updating counters.
  inline void CTReleaseRegionFragment(uptr num_counters, u8* counters, uptr chunk_size, uptr full_pages_chunk_count_max, bool same_chunk_count_per_page,
                                          u8 max_chunks_in_region, CTRegionInfo* cur_region_info, u8 page_shift){
    u64 cur_chunk_offset;

    internal_memset(counters, 0, num_counters);
    if (chunk_size <= page_size_ && page_size_ % chunk_size == 0) {
      cur_chunk_offset = (256-max_chunks_in_region) * chunk_size;
      for (int i = 256-max_chunks_in_region; i <= 255 ; i++){
        if (cur_region_info->chunk_last_tag[i] != 0){
          counters[cur_chunk_offset>>page_shift] += 1;
        }
        cur_chunk_offset += chunk_size;
      }
    } else {
      cur_chunk_offset = (256-max_chunks_in_region) * chunk_size;
      for (int i = 256-max_chunks_in_region; i <= 255 ; i++) {
        if (cur_region_info->chunk_last_tag[i] != 0){
          for(u64 j = cur_chunk_offset>>page_shift; j <= (cur_chunk_offset+chunk_size-1)>>page_shift; j++)
            counters[j] += 1;
        }
        cur_chunk_offset += chunk_size;
      }
    }

    CTFreePagesRangeTracker range_tracker;
    CTFreePagesRangeTrackerInit(&range_tracker, (uptr)cur_region_info);
    if (same_chunk_count_per_page) {
      for (uptr i = 0; i < num_counters; i++)
        CTFreePagesRangeTrackerNextPage(&range_tracker, counters[i] == full_pages_chunk_count_max);
    } else {
      const uptr pn = chunk_size < page_size_ ? page_size_ / chunk_size : 1;
      const uptr pnc = pn * chunk_size;
      uptr prev_page_boundary = 0;
      uptr current_boundary = 0;
      for (uptr i = 0; i < num_counters; i++) {
        uptr page_boundary = prev_page_boundary + page_size_;
        uptr chunks_per_page = pn;
        if (current_boundary < page_boundary) {
          if (current_boundary > prev_page_boundary)
            chunks_per_page++;
          current_boundary += pnc;
          if (current_boundary < page_boundary) {
            chunks_per_page++;
            current_boundary += chunk_size;
          }
        }
        prev_page_boundary = page_boundary;

        CTFreePagesRangeTrackerNextPage(&range_tracker, counters[i] == chunks_per_page);
      }
    }
    CTFreePagesRangeTrackerDone(&range_tracker);
  }

  // ClusterTag. Release the shadow memory associated with a region.
  inline void CTReleaseShadowMemory(uptr region_base, uptr region_size) {
    uptr unmap_lower              = CTUntaggedPointerToShadow((uptr)region_base);
    uptr aligned_unmap_lower      = RoundDownTo(unmap_lower, page_size_);
    bool aligned_unmap_lower_free = false;
    uptr unmap_upper              = CTUntaggedPointerToShadow((uptr)region_base + region_size);
    uptr aligned_unmap_upper      = RoundUpTo(unmap_upper, page_size_);
    bool aligned_unmap_upper_free = false;

    CTRegionInfo* region_info = (CTRegionInfo*) region_base;
    if (region_info->prev == nullptr) {
      aligned_unmap_lower_free = true;
    } else {
      uptr prev_region_end = (uptr)(region_info->prev)+region_size;
      if (CTUntaggedPointerToShadow(prev_region_end) <= aligned_unmap_lower)
        aligned_unmap_lower_free = true;
    }
    if (region_info->next == nullptr) {
      aligned_unmap_upper_free = true;
    } else {
      uptr next_region_start = (uptr)(region_info->next);
      if (CTUntaggedPointerToShadow(next_region_start) >= aligned_unmap_upper)
        aligned_unmap_upper_free = true;
    }

    if (aligned_unmap_lower_free && aligned_unmap_upper_free){
      CTAdvise(aligned_unmap_lower, aligned_unmap_upper-aligned_unmap_lower);
    }
  }

  u64 CHUNK_RECYCLE_RATE = 1 /*1,2,4,8,16,...*/ * page_size_;
  // ClusterTag. Periodically release regions and shadow memory for a class.
  void CTReleasePeriodically(uptr class_id){
    CTRegionInfo* prev_region  = nullptr;
    CTRegionInfo* cur_region   = nullptr;
    CTRegionInfo* _cur_region  = nullptr;
    CTSuperPoolChunkInfo *spc  = this->SuperPoolChunkInfo[class_id];
    uptr cur_pool_addr;
    CTRegionList region_free_list;
    region_free_list.head = nullptr;
    u8 max_chunks_in_region = this->AvailableChunksInRegion[class_id];
    u64 chunk_size = this->SuperPoolsChunkSize[class_id];
    u64 region_size = this->SuperPoolsRegionSize[class_id];
    CTRegionList* region_list       = this->SuperPoolsRegionList[class_id];
    CTRegionList* region_freed_list = this->SuperPoolsFreedRegionList[class_id];
    u8 max_free_chunks = RoundUpDivide(CHUNK_RECYCLE_RATE, chunk_size);
    u8 page_shift = Log2(page_size_);
    u64 free_chunk_in_region_list = 0;
    u64 old_free_chunk_in_spc = spc->free_chunk_num;

    uptr full_pages_chunk_count_max;
    bool same_chunk_count_per_page;
    check_fast_release_path(chunk_size, &full_pages_chunk_count_max, &same_chunk_count_per_page);
    const u64 num_counters = RoundUpDivide(chunk_size*256, page_size_);
    SpinMutexLock l(&global_persistent_allocator_mutex);
    u8* counters = (u8*)CTPersistentAllocatorAllocate(&ct_persistent_allocator, num_counters);

    // ClusterTag. Debug information.
    // u64 free_chunk_in_super_pool = spc->free_chunk_num;
    // u64 all_chunk_in_super_pool  = spc->region_num * max_chunks_in_region;
    // float density = 1 - (float)free_chunk_in_super_pool/all_chunk_in_super_pool;
    // int density_int_part = (int)density;
    // int density_frac_part = (int)((density - density_int_part) * 10000);
    // Printf(">>> Periodically triggered release for SuperPool: chunk_size=0x%lx all_region=%ld free_region=%ld free chunk=[%ld/%ld] chunk utilization=%d.%04d\n",
    //      this->SuperPoolsChunkSize[class_id], spc->region_num, spc->free_region_num, free_chunk_in_super_pool, all_chunk_in_super_pool, density_int_part, density_frac_part);

    prev_region = nullptr;
    cur_region = region_freed_list->head;
    while (cur_region != nullptr) {
      if (cur_region->freed_num == max_chunks_in_region) {
        free_chunk_in_region_list += cur_region->freed_num;

        _cur_region = cur_region;
        cur_region->in_free_list = 0;
        if (prev_region == nullptr)
          region_freed_list->head = cur_region->freed_next;
        else
          prev_region->freed_next = cur_region->freed_next;
        cur_region = cur_region->freed_next;

        _cur_region->deleteFromList(region_list);
        CTReleaseShadowMemory((uptr)_cur_region, region_size);
        _cur_region->parent_pool->density -= 1;

        spc->free_chunk_num -= max_chunks_in_region;
        spc->region_num -= 1;
        spc->free_region_num -= 1;

        CTReleaseRegion(_cur_region, class_id);
      }

      else if (cur_region->freed_num > max_free_chunks){
        free_chunk_in_region_list += cur_region->freed_num;
        CTReleaseRegionFragment(num_counters, counters, chunk_size, full_pages_chunk_count_max, same_chunk_count_per_page, max_chunks_in_region, cur_region, page_shift);

        prev_region = cur_region;
        cur_region = cur_region->freed_next;
      }

      else {
        free_chunk_in_region_list += cur_region->freed_num;

        prev_region = cur_region;
        cur_region = cur_region->freed_next;
      }
    }

    CTPersistentAllocatorFree(&ct_persistent_allocator, (uptr)counters);
  }

  // ClusterTag. Allocate a large chunk and map its memory, checking for blacklisted regions.
  bool CTAllocateLargeChunk(uptr map_size, uptr* res_beg){
    uptr addr_alignment = 4096*16;

    CHECK(IsAligned(map_size, page_size_));
    uptr large_chunk_beg = (uptr)MmapAlignedOrDieOnFatalError(map_size, addr_alignment, "region");

    if ((void*)large_chunk_beg == nullptr){
      Printf("Memory allocation exception, mmap returned an error code\n");
      internal__exit(1);
    }
    if (CTMapBlackCheck(large_chunk_beg, map_size) == false){
      Printf("Failed to apply for large chunk [0x%lx, 0x%lx], it is in a blacklist such as SuperPool or Shadow Memory\n", large_chunk_beg, large_chunk_beg+map_size);
      internal__exit(1);
    }
    if (IsAligned(large_chunk_beg, addr_alignment) == false){
      Printf("Failed to apply for large chunk [0x%lx, 0x%lx], it is not aligned\n", large_chunk_beg, large_chunk_beg+map_size);
      internal__exit(1);
    }
    *res_beg = large_chunk_beg;
    return true;
  }

  // ClusterTag. Deallocate a large chunk and release its shadow memory.
  void CTDeallocateLargeChunk(void *p){
    CTLargeChunkHeader* chunkHeader = GetCTLargeChunkHeader(p);

    void *target_shadow_memory_start = (void *)(RoundDownTo(CTUntaggedPointerToShadow((uptr)chunkHeader), page_size_));
    void *target_shadow_memory_end = (void *)(RoundUpTo(CTUntaggedPointerToShadow((uptr)chunkHeader+chunkHeader->allocated_size), page_size_));
    CTFixedMapForced((u64)target_shadow_memory_start, (u64)target_shadow_memory_end - (u64)target_shadow_memory_start);

    UnmapOrDie((void*)((u64)chunkHeader), chunkHeader->allocated_size);
  }


  using AddressSpaceView = typename Params::AddressSpaceView;
  static const uptr kSpaceBeg = Params::kSpaceBeg;
  static const uptr kSpaceSize = Params::kSpaceSize;
  static const uptr kMetadataSize = Params::kMetadataSize;
  typedef typename Params::SizeClassMap SizeClassMap;
  typedef typename Params::MapUnmapCallback MapUnmapCallback;

  static const bool kRandomShuffleChunks =
      Params::kFlags & SizeClassAllocator64FlagMasks::kRandomShuffleChunks;

  typedef SizeClassAllocator64<Params> ThisT;
  typedef SizeClassAllocator64LocalCache<ThisT> AllocatorCache;
  typedef MemoryMapper<ThisT> MemoryMapperT;

  // When we know the size class (the region base) we can represent a pointer
  // as a 4-byte integer (offset from the region start shifted right by 4).
  typedef u32 CompactPtrT;
  static const uptr kCompactPtrScale = 4;
  CompactPtrT PointerToCompactPtr(uptr base, uptr ptr) const {
    return static_cast<CompactPtrT>((ptr - base) >> kCompactPtrScale);
  }
  uptr CompactPtrToPointer(uptr base, CompactPtrT ptr32) const {
    return base + (static_cast<uptr>(ptr32) << kCompactPtrScale);
  }

  // If heap_start is nonzero, assumes kSpaceSize bytes are already mapped R/W
  // at heap_start and places the heap there.  This mode requires kSpaceBeg ==
  // ~(uptr)0.
  void Init(s32 release_to_os_interval_ms, uptr heap_start = 0) {
    uptr TotalSpaceSize = kSpaceSize + AdditionalSize();
    PremappedHeap = heap_start != 0;
    if (PremappedHeap) {
      CHECK(!kUsingConstantSpaceBeg);
      NonConstSpaceBeg = heap_start;
      uptr RegionInfoSize = AdditionalSize();
      RegionInfoSpace =
          address_range.Init(RegionInfoSize, PrimaryAllocatorName);
      CHECK_NE(RegionInfoSpace, ~(uptr)0);
      CHECK_EQ(RegionInfoSpace,
               address_range.MapOrDie(RegionInfoSpace, RegionInfoSize,
                                      "SizeClassAllocator: region info"));
      MapUnmapCallback().OnMap(RegionInfoSpace, RegionInfoSize);
    } else {
      if (kUsingConstantSpaceBeg) {
        CHECK(IsAligned(kSpaceBeg, SizeClassMap::kMaxSize));
        CHECK_EQ(kSpaceBeg,
                 address_range.Init(TotalSpaceSize, PrimaryAllocatorName,
                                    kSpaceBeg));
      } else {
        // Combined allocator expects that an 2^N allocation is always aligned
        // to 2^N. For this to work, the start of the space needs to be aligned
        // as high as the largest size class (which also needs to be a power of
        // 2).
        NonConstSpaceBeg = address_range.InitAligned(
            TotalSpaceSize, SizeClassMap::kMaxSize, PrimaryAllocatorName);
        CHECK_NE(NonConstSpaceBeg, ~(uptr)0);
      }
      RegionInfoSpace = SpaceEnd();
      MapWithCallbackOrDie(RegionInfoSpace, AdditionalSize(),
                           "SizeClassAllocator: region info");
    }
    SetReleaseToOSIntervalMs(release_to_os_interval_ms);
    // Check that the RegionInfo array is aligned on the CacheLine size.
    DCHECK_EQ(RegionInfoSpace % kCacheLineSize, 0);
  }

  s32 ReleaseToOSIntervalMs() const {
    return atomic_load(&release_to_os_interval_ms_, memory_order_relaxed);
  }

  void SetReleaseToOSIntervalMs(s32 release_to_os_interval_ms) {
    atomic_store(&release_to_os_interval_ms_, release_to_os_interval_ms,
                 memory_order_relaxed);
  }

  void ForceReleaseToOS() {
    MemoryMapperT memory_mapper(*this);
    for (uptr class_id = 1; class_id < kNumClasses; class_id++) {
      Lock l(&GetRegionInfo(class_id)->mutex);
      MaybeReleaseToOS(&memory_mapper, class_id, true /*force*/);
    }
  }

  static bool CanAllocate(uptr size, uptr alignment) {
    return size <= SizeClassMap::kMaxSize &&
      alignment <= SizeClassMap::kMaxSize;
  }

  NOINLINE void ReturnToAllocator(MemoryMapperT *memory_mapper,
                                  AllocatorStats *stat, uptr class_id,
                                  const CompactPtrT *chunks, uptr n_chunks) {
    RegionInfo *region = GetRegionInfo(class_id);
    uptr region_beg = GetRegionBeginBySizeClass(class_id);
    CompactPtrT *free_array = GetFreeArray(region_beg);

    Lock l(&region->mutex);
    uptr old_num_chunks = region->num_freed_chunks;
    uptr new_num_freed_chunks = old_num_chunks + n_chunks;
    // Failure to allocate free array space while releasing memory is non
    // recoverable.
    if (UNLIKELY(!EnsureFreeArraySpace(region, region_beg,
                                       new_num_freed_chunks))) {
      Report("FATAL: Internal error: %s's allocator exhausted the free list "
             "space for size class %zd (%zd bytes).\n", SanitizerToolName,
             class_id, ClassIdToSize(class_id));
      Die();
    }
    for (uptr i = 0; i < n_chunks; i++)
      free_array[old_num_chunks + i] = chunks[i];
    region->num_freed_chunks = new_num_freed_chunks;
    region->stats.n_freed += n_chunks;

    MaybeReleaseToOS(memory_mapper, class_id, false /*force*/);
  }

  NOINLINE bool GetFromAllocator(AllocatorStats *stat, uptr class_id,
                                 CompactPtrT *chunks, uptr n_chunks) {
    RegionInfo *region = GetRegionInfo(class_id);
    uptr region_beg = GetRegionBeginBySizeClass(class_id);
    CompactPtrT *free_array = GetFreeArray(region_beg);

    Lock l(&region->mutex);
#if SANITIZER_WINDOWS
    /* On Windows unmapping of memory during __sanitizer_purge_allocator is
    explicit and immediate, so unmapped regions must be explicitly mapped back
    in when they are accessed again. */
    if (region->rtoi.last_released_bytes > 0) {
      MmapFixedOrDie(region_beg, region->mapped_user,
                                      "SizeClassAllocator: region data");
      region->rtoi.n_freed_at_last_release = 0;
      region->rtoi.last_released_bytes = 0;
    }
#endif
    if (UNLIKELY(region->num_freed_chunks < n_chunks)) {
      if (UNLIKELY(!PopulateFreeArray(stat, class_id, region,
                                      n_chunks - region->num_freed_chunks)))
        return false;
      CHECK_GE(region->num_freed_chunks, n_chunks);
    }
    region->num_freed_chunks -= n_chunks;
    uptr base_idx = region->num_freed_chunks;
    for (uptr i = 0; i < n_chunks; i++)
      chunks[i] = free_array[base_idx + i];
    region->stats.n_allocated += n_chunks;
    return true;
  }


  bool PointerIsMine(const void *p) const {
    // ClusterTag. 
    if (is_clustertag){
      Printf("Can not invoke PointerIsMine\n");
      internal__exit(1);
    }

    uptr P = reinterpret_cast<uptr>(p);
    if (kUsingConstantSpaceBeg && (kSpaceBeg % kSpaceSize) == 0)
      return P / kSpaceSize == kSpaceBeg / kSpaceSize;
    return P >= SpaceBeg() && P < SpaceEnd();
  }

  uptr GetRegionBegin(const void *p) {
    // ClusterTag. 
    if (is_clustertag){
      Printf("Can not invoke GetRegionBegin\n");
      internal__exit(1);
    }

    if (kUsingConstantSpaceBeg)
      return reinterpret_cast<uptr>(p) & ~(kRegionSize - 1);
    uptr space_beg = SpaceBeg();
    return ((reinterpret_cast<uptr>(p)  - space_beg) & ~(kRegionSize - 1)) +
        space_beg;
  }

  uptr GetRegionBeginBySizeClass(uptr class_id) const {
    // ClusterTag.
    if (is_clustertag){
      Printf("Can not invoke GetRegionBeginBySizeClass\n");
      internal__exit(1);
    }

    return SpaceBeg() + kRegionSize * class_id;
  }

  uptr GetSizeClass(const void *p) {
    // ClusterTag.
    if (is_clustertag){
      Printf("Can not invoke GetSizeClass\n");
      internal__exit(1);
    }

    if (kUsingConstantSpaceBeg && (kSpaceBeg % kSpaceSize) == 0)
      return ((reinterpret_cast<uptr>(p)) / kRegionSize) % kNumClassesRounded;
    return ((reinterpret_cast<uptr>(p) - SpaceBeg()) / kRegionSize) %
           kNumClassesRounded;
  }

  void *GetBlockBegin(const void *p) {
    uptr class_id = GetSizeClass(p);
    if (class_id >= kNumClasses) return nullptr;
    uptr size = ClassIdToSize(class_id);
    if (!size) return nullptr;
    uptr chunk_idx = GetChunkIdx((uptr)p, size);
    uptr reg_beg = GetRegionBegin(p);
    uptr beg = chunk_idx * size;
    uptr next_beg = beg + size;
    const RegionInfo *region = AddressSpaceView::Load(GetRegionInfo(class_id));
    if (region->mapped_user >= next_beg)
      return reinterpret_cast<void*>(reg_beg + beg);
    return nullptr;
  }

  uptr GetActuallyAllocatedSize(void *p) {
    // ClusterTag.
    if (is_clustertag){
      Printf("Can not invoke GetActuallyAllocatedSize\n");
      internal__exit(1);
    }

    CHECK(PointerIsMine(p));
    return ClassIdToSize(GetSizeClass(p));
  }

  static uptr ClassID(uptr size) { return SizeClassMap::ClassID(size); }

  void *GetMetaData(const void *p) {
    // ClusterTag.
    if (is_clustertag){
      Printf("Can not invoke GetMetaData\n");
      internal__exit(1);
    }

    CHECK(kMetadataSize);
    uptr class_id = GetSizeClass(p);
    uptr size = ClassIdToSize(class_id);
    if (!size)
      return nullptr;
    uptr chunk_idx = GetChunkIdx(reinterpret_cast<uptr>(p), size);
    uptr region_beg = GetRegionBeginBySizeClass(class_id);
    return reinterpret_cast<void *>(GetMetadataEnd(region_beg) -
                                    (1 + chunk_idx) * kMetadataSize);
  }

  uptr TotalMemoryUsed() {
    uptr res = 0;
    for (uptr i = 0; i < kNumClasses; i++)
      res += GetRegionInfo(i)->allocated_user;
    return res;
  }

  // Test-only.
  void TestOnlyUnmap() {
    UnmapWithCallbackOrDie((uptr)address_range.base(), address_range.size());
  }

  static void FillMemoryProfile(uptr start, uptr rss, bool file, uptr *stats) {
    for (uptr class_id = 0; class_id < kNumClasses; class_id++)
      if (stats[class_id] == start)
        stats[class_id] = rss;
  }

  void PrintStats(uptr class_id, uptr rss) {
    RegionInfo *region = GetRegionInfo(class_id);
    if (region->mapped_user == 0) return;
    uptr in_use = region->stats.n_allocated - region->stats.n_freed;
    uptr avail_chunks = region->allocated_user / ClassIdToSize(class_id);
    Printf(
        "%s %02zd (%6zd): mapped: %6zdK allocs: %7zd frees: %7zd inuse: %6zd "
        "num_freed_chunks %7zd avail: %6zd rss: %6zdK releases: %6zd "
        "last released: %6lldK region: 0x%zx\n",
        region->exhausted ? "F" : " ", class_id, ClassIdToSize(class_id),
        region->mapped_user >> 10, region->stats.n_allocated,
        region->stats.n_freed, in_use, region->num_freed_chunks, avail_chunks,
        rss >> 10, region->rtoi.num_releases,
        region->rtoi.last_released_bytes >> 10,
        SpaceBeg() + kRegionSize * class_id);
  }

  void PrintStats() {
    uptr rss_stats[kNumClasses];
    for (uptr class_id = 0; class_id < kNumClasses; class_id++)
      rss_stats[class_id] = SpaceBeg() + kRegionSize * class_id;
    GetMemoryProfile(FillMemoryProfile, rss_stats);

    uptr total_mapped = 0;
    uptr total_rss = 0;
    uptr n_allocated = 0;
    uptr n_freed = 0;
    for (uptr class_id = 1; class_id < kNumClasses; class_id++) {
      RegionInfo *region = GetRegionInfo(class_id);
      if (region->mapped_user != 0) {
        total_mapped += region->mapped_user;
        total_rss += rss_stats[class_id];
      }
      n_allocated += region->stats.n_allocated;
      n_freed += region->stats.n_freed;
    }

    Printf("Stats: SizeClassAllocator64: %zdM mapped (%zdM rss) in "
           "%zd allocations; remains %zd\n", total_mapped >> 20,
           total_rss >> 20, n_allocated, n_allocated - n_freed);
    for (uptr class_id = 1; class_id < kNumClasses; class_id++)
      PrintStats(class_id, rss_stats[class_id]);
  }

  // ForceLock() and ForceUnlock() are needed to implement Darwin malloc zone
  // introspection API.
  void ForceLock() SANITIZER_NO_THREAD_SAFETY_ANALYSIS {
    for (uptr i = 0; i < kNumClasses; i++) {
      GetRegionInfo(i)->mutex.Lock();
    }
  }

  void ForceUnlock() SANITIZER_NO_THREAD_SAFETY_ANALYSIS {
    for (int i = (int)kNumClasses - 1; i >= 0; i--) {
      GetRegionInfo(i)->mutex.Unlock();
    }
  }

  // Iterate over all existing chunks.
  // The allocator must be locked when calling this function.
  void ForEachChunk(ForEachChunkCallback callback, void *arg) {
    for (uptr class_id = 1; class_id < kNumClasses; class_id++) {
      RegionInfo *region = GetRegionInfo(class_id);
      uptr chunk_size = ClassIdToSize(class_id);
      uptr region_beg = SpaceBeg() + class_id * kRegionSize;
      uptr region_allocated_user_size =
          AddressSpaceView::Load(region)->allocated_user;
      for (uptr chunk = region_beg;
           chunk < region_beg + region_allocated_user_size;
           chunk += chunk_size) {
        // Too slow: CHECK_EQ((void *)chunk, GetBlockBegin((void *)chunk));
        callback(chunk, arg);
      }
    }
  }

  static uptr ClassIdToSize(uptr class_id) {
    return SizeClassMap::Size(class_id);
  }

  static uptr AdditionalSize() {
    return RoundUpTo(sizeof(RegionInfo) * kNumClassesRounded,
                     GetPageSizeCached());
  }

  typedef SizeClassMap SizeClassMapT;
  static const uptr kNumClasses = SizeClassMap::kNumClasses;
  static const uptr kNumClassesRounded = SizeClassMap::kNumClassesRounded;


  // A packed array of counters. Each counter occupies 2^n bits, enough to store
  // counter's max_value. Ctor will try to allocate the required buffer via
  // mapper->MapPackedCounterArrayBuffer and the caller is expected to check
  // whether the initialization was successful by checking IsAllocated() result.
  // For the performance sake, none of the accessors check the validity of the
  // arguments, it is assumed that index is always in [0, n) range and the value
  // is not incremented past max_value.
  class PackedCounterArray {
    private:
      const u64 n;
      u64 counter_size_bits_log;
      u64 counter_mask;
      u64 packing_ratio_log;
      u64 bit_offset_mask;
      u64* buffer;

   public:
    template <typename MemoryMapper>
    PackedCounterArray(u64 num_counters, u64 max_value, MemoryMapper *mapper)
        : n(num_counters) {
      CHECK_GT(num_counters, 0);
      CHECK_GT(max_value, 0);
      constexpr u64 kMaxCounterBits = sizeof(*buffer) * 8ULL;
      // Rounding counter storage size up to the power of two allows for using
      // bit shifts calculating particular counter's index and offset.
      uptr counter_size_bits =
          RoundUpToPowerOfTwo(MostSignificantSetBitIndex(max_value) + 1);
      CHECK_LE(counter_size_bits, kMaxCounterBits);
      counter_size_bits_log = Log2(counter_size_bits);
      counter_mask = ~0ULL >> (kMaxCounterBits - counter_size_bits);

      uptr packing_ratio = kMaxCounterBits >> counter_size_bits_log;
      CHECK_GT(packing_ratio, 0);
      packing_ratio_log = Log2(packing_ratio);
      bit_offset_mask = packing_ratio - 1;

      buffer = mapper->MapPackedCounterArrayBuffer(
          RoundUpTo(n, 1ULL << packing_ratio_log) >> packing_ratio_log);
    }

    bool IsAllocated() const {
      return !!buffer;
    }

    u64 GetCount() const {
      return n;
    }

    uptr Get(uptr i) const {
      DCHECK_LT(i, n);
      uptr index = i >> packing_ratio_log;
      uptr bit_offset = (i & bit_offset_mask) << counter_size_bits_log;
      return (buffer[index] >> bit_offset) & counter_mask;
    }

    void Inc(uptr i) const {
      DCHECK_LT(Get(i), counter_mask);
      uptr index = i >> packing_ratio_log;
      uptr bit_offset = (i & bit_offset_mask) << counter_size_bits_log;
      buffer[index] += 1ULL << bit_offset;
    }

    void IncRange(uptr from, uptr to) const {
      DCHECK_LE(from, to);
      for (uptr i = from; i <= to; i++)
        Inc(i);
    }
  };

  template <class MemoryMapperT>
  class FreePagesRangeTracker {
   public:
    FreePagesRangeTracker(MemoryMapperT *mapper, uptr class_id)
        : memory_mapper(mapper),
          class_id(class_id),
          page_size_scaled_log(Log2(GetPageSizeCached() >> kCompactPtrScale)) {}

    void NextPage(bool freed) {
      if (freed) {
        if (!in_the_range) {
          current_range_start_page = current_page;
          in_the_range = true;
        }
      } else {
        CloseOpenedRange();
      }
      current_page++;
    }

    void Done() {
      CloseOpenedRange();
    }

   private:
    void CloseOpenedRange() {
      if (in_the_range) {
        memory_mapper->ReleasePageRangeToOS(
            class_id, current_range_start_page << page_size_scaled_log,
            current_page << page_size_scaled_log);
        in_the_range = false;
      }
    }

    MemoryMapperT *const memory_mapper = nullptr;
    const uptr class_id = 0;
    const uptr page_size_scaled_log = 0;
    bool in_the_range = false;
    uptr current_page = 0;
    uptr current_range_start_page = 0;
  };

  // Iterates over the free_array to identify memory pages containing freed
  // chunks only and returns these pages back to OS.
  // allocated_pages_count is the total number of pages allocated for the
  // current bucket.
  template <typename MemoryMapper>
  static void ReleaseFreeMemoryToOS(CompactPtrT *free_array,
                                    uptr free_array_count, uptr chunk_size,
                                    uptr allocated_pages_count,
                                    MemoryMapper *memory_mapper,
                                    uptr class_id) {
    const uptr page_size = GetPageSizeCached();

    // Figure out the number of chunks per page and whether we can take a fast
    // path (the number of chunks per page is the same for all pages).
    uptr full_pages_chunk_count_max;
    bool same_chunk_count_per_page;
    if (chunk_size <= page_size && page_size % chunk_size == 0) {
      // Same number of chunks per page, no cross overs.
      full_pages_chunk_count_max = page_size / chunk_size;
      same_chunk_count_per_page = true;
    } else if (chunk_size <= page_size && page_size % chunk_size != 0 &&
        chunk_size % (page_size % chunk_size) == 0) {
      // Some chunks are crossing page boundaries, which means that the page
      // contains one or two partial chunks, but all pages contain the same
      // number of chunks.
      full_pages_chunk_count_max = page_size / chunk_size + 1;
      same_chunk_count_per_page = true;
    } else if (chunk_size <= page_size) {
      // Some chunks are crossing page boundaries, which means that the page
      // contains one or two partial chunks.
      full_pages_chunk_count_max = page_size / chunk_size + 2;
      same_chunk_count_per_page = false;
    } else if (chunk_size > page_size && chunk_size % page_size == 0) {
      // One chunk covers multiple pages, no cross overs.
      full_pages_chunk_count_max = 1;
      same_chunk_count_per_page = true;
    } else if (chunk_size > page_size) {
      // One chunk covers multiple pages, Some chunks are crossing page
      // boundaries. Some pages contain one chunk, some contain two.
      full_pages_chunk_count_max = 2;
      same_chunk_count_per_page = false;
    } else {
      UNREACHABLE("All chunk_size/page_size ratios must be handled.");
    }

    PackedCounterArray counters(allocated_pages_count, full_pages_chunk_count_max, memory_mapper);
    if (!counters.IsAllocated())
      return;

    const uptr chunk_size_scaled = chunk_size >> kCompactPtrScale;
    const uptr page_size_scaled = page_size >> kCompactPtrScale;
    const uptr page_size_scaled_log = Log2(page_size_scaled);

    // Iterate over free chunks and count how many free chunks affect each
    // allocated page.
    if (chunk_size <= page_size && page_size % chunk_size == 0) {
      // Each chunk affects one page only.
      for (uptr i = 0; i < free_array_count; i++)
        counters.Inc(free_array[i] >> page_size_scaled_log);
    } else {
      // In all other cases chunks might affect more than one page.
      for (uptr i = 0; i < free_array_count; i++) {
        counters.IncRange(
            free_array[i] >> page_size_scaled_log,
            (free_array[i] + chunk_size_scaled - 1) >> page_size_scaled_log);
      }
    }

    // Iterate over pages detecting ranges of pages with chunk counters equal
    // to the expected number of chunks for the particular page.
    FreePagesRangeTracker<MemoryMapper> range_tracker(memory_mapper, class_id);
    if (same_chunk_count_per_page) {
      // Fast path, every page has the same number of chunks affecting it.
      for (uptr i = 0; i < counters.GetCount(); i++)
        range_tracker.NextPage(counters.Get(i) == full_pages_chunk_count_max);
    } else {
      // Show path, go through the pages keeping count how many chunks affect
      // each page.
      const uptr pn =
          chunk_size < page_size ? page_size_scaled / chunk_size_scaled : 1;
      const uptr pnc = pn * chunk_size_scaled;
      // The idea is to increment the current page pointer by the first chunk
      // size, middle portion size (the portion of the page covered by chunks
      // except the first and the last one) and then the last chunk size, adding
      // up the number of chunks on the current page and checking on every step
      // whether the page boundary was crossed.
      uptr prev_page_boundary = 0;
      uptr current_boundary = 0;
      for (uptr i = 0; i < counters.GetCount(); i++) {
        uptr page_boundary = prev_page_boundary + page_size_scaled;
        uptr chunks_per_page = pn;
        if (current_boundary < page_boundary) {
          if (current_boundary > prev_page_boundary)
            chunks_per_page++;
          current_boundary += pnc;
          if (current_boundary < page_boundary) {
            chunks_per_page++;
            current_boundary += chunk_size_scaled;
          }
        }
        prev_page_boundary = page_boundary;

        range_tracker.NextPage(counters.Get(i) == chunks_per_page);
      }
    }
    range_tracker.Done();
  }

 private:
  friend class MemoryMapper<ThisT>;

  ReservedAddressRange address_range;

  static const uptr kRegionSize = kSpaceSize / kNumClassesRounded;
  // FreeArray is the array of free-d chunks (stored as 4-byte offsets).
  // In the worst case it may require kRegionSize/SizeClassMap::kMinSize
  // elements, but in reality this will not happen. For simplicity we
  // dedicate 1/8 of the region's virtual space to FreeArray.
  static const uptr kFreeArraySize = kRegionSize / 8;

  static const bool kUsingConstantSpaceBeg = kSpaceBeg != ~(uptr)0;
  uptr NonConstSpaceBeg;
  uptr SpaceBeg() const {
    return kUsingConstantSpaceBeg ? kSpaceBeg : NonConstSpaceBeg;
  }
  uptr SpaceEnd() const { return  SpaceBeg() + kSpaceSize; }
  // kRegionSize must be >= 2^32.
  COMPILER_CHECK((kRegionSize) >= (1ULL << (SANITIZER_WORDSIZE / 2)));
  // kRegionSize must be <= 2^36, see CompactPtrT.
  COMPILER_CHECK((kRegionSize) <= (1ULL << (SANITIZER_WORDSIZE / 2 + 4)));
  // Call mmap for user memory with at least this size.
  static const uptr kUserMapSize = 1 << 16;
  // Call mmap for metadata memory with at least this size.
  static const uptr kMetaMapSize = 1 << 16;
  // Call mmap for free array memory with at least this size.
  static const uptr kFreeArrayMapSize = 1 << 16;

  atomic_sint32_t release_to_os_interval_ms_;

  uptr RegionInfoSpace;

  // True if the user has already mapped the entire heap R/W.
  bool PremappedHeap;

  struct Stats {
    uptr n_allocated;
    uptr n_freed;
  };

  struct ReleaseToOsInfo {
    uptr n_freed_at_last_release;
    uptr num_releases;
    u64 last_release_at_ns;
    u64 last_released_bytes;
  };

  struct ALIGNED(SANITIZER_CACHE_LINE_SIZE) RegionInfo {
    Mutex mutex;
    uptr num_freed_chunks;  // Number of elements in the freearray.
    uptr mapped_free_array;  // Bytes mapped for freearray.
    uptr allocated_user;  // Bytes allocated for user memory.
    uptr allocated_meta;  // Bytes allocated for metadata.
    uptr mapped_user;  // Bytes mapped for user memory.
    uptr mapped_meta;  // Bytes mapped for metadata.
    u32 rand_state;  // Seed for random shuffle, used if kRandomShuffleChunks.
    bool exhausted;  // Whether region is out of space for new chunks.
    Stats stats;
    ReleaseToOsInfo rtoi;
  };
  COMPILER_CHECK(sizeof(RegionInfo) % kCacheLineSize == 0);

  RegionInfo *GetRegionInfo(uptr class_id) const {
    DCHECK_LT(class_id, kNumClasses);
    RegionInfo *regions = reinterpret_cast<RegionInfo *>(RegionInfoSpace);
    return &regions[class_id];
  }

  uptr GetMetadataEnd(uptr region_beg) const {
    return region_beg + kRegionSize - kFreeArraySize;
  }

  uptr GetChunkIdx(uptr chunk, uptr size) const {
    if (!kUsingConstantSpaceBeg)
      chunk -= SpaceBeg();

    uptr offset = chunk % kRegionSize;
    // Here we divide by a non-constant. This is costly.
    // size always fits into 32-bits. If the offset fits too, use 32-bit div.
    if (offset >> (SANITIZER_WORDSIZE / 2))
      return offset / size;
    return (u32)offset / (u32)size;
  }

  CompactPtrT *GetFreeArray(uptr region_beg) const {
    return reinterpret_cast<CompactPtrT *>(GetMetadataEnd(region_beg));
  }

  bool MapWithCallback(uptr beg, uptr size, const char *name) {
    if (PremappedHeap)
      return beg >= NonConstSpaceBeg &&
             beg + size <= NonConstSpaceBeg + kSpaceSize;
    uptr mapped = address_range.Map(beg, size, name);
    if (UNLIKELY(!mapped))
      return false;
    CHECK_EQ(beg, mapped);
    MapUnmapCallback().OnMap(beg, size);
    return true;
  }

  void MapWithCallbackOrDie(uptr beg, uptr size, const char *name) {
    if (PremappedHeap) {
      CHECK_GE(beg, NonConstSpaceBeg);
      CHECK_LE(beg + size, NonConstSpaceBeg + kSpaceSize);
      return;
    }
    CHECK_EQ(beg, address_range.MapOrDie(beg, size, name));
    MapUnmapCallback().OnMap(beg, size);
  }

  void UnmapWithCallbackOrDie(uptr beg, uptr size) {
    if (PremappedHeap)
      return;
    MapUnmapCallback().OnUnmap(beg, size);
    address_range.Unmap(beg, size);
  }

  bool EnsureFreeArraySpace(RegionInfo *region, uptr region_beg,
                            uptr num_freed_chunks) {
    uptr needed_space = num_freed_chunks * sizeof(CompactPtrT);
    if (region->mapped_free_array < needed_space) {
      uptr new_mapped_free_array = RoundUpTo(needed_space, kFreeArrayMapSize);
      CHECK_LE(new_mapped_free_array, kFreeArraySize);
      uptr current_map_end = reinterpret_cast<uptr>(GetFreeArray(region_beg)) +
                             region->mapped_free_array;
      uptr new_map_size = new_mapped_free_array - region->mapped_free_array;
      if (UNLIKELY(!MapWithCallback(current_map_end, new_map_size,
                                    "SizeClassAllocator: freearray")))
        return false;
      region->mapped_free_array = new_mapped_free_array;
    }
    return true;
  }

  // Check whether this size class is exhausted.
  bool IsRegionExhausted(RegionInfo *region, uptr class_id,
                         uptr additional_map_size) {
    if (LIKELY(region->mapped_user + region->mapped_meta +
               additional_map_size <= kRegionSize - kFreeArraySize))
      return false;
    if (!region->exhausted) {
      region->exhausted = true;
      Printf("%s: Out of memory. ", SanitizerToolName);
      Printf("The process has exhausted %zuMB for size class %zu.\n",
             kRegionSize >> 20, ClassIdToSize(class_id));
    }
    return true;
  }

  NOINLINE bool PopulateFreeArray(AllocatorStats *stat, uptr class_id,
                                  RegionInfo *region, uptr requested_count) {
    // region->mutex is held.
    const uptr region_beg = GetRegionBeginBySizeClass(class_id);
    const uptr size = ClassIdToSize(class_id);

    const uptr total_user_bytes =
        region->allocated_user + requested_count * size;
    // Map more space for chunks, if necessary.
    if (LIKELY(total_user_bytes > region->mapped_user)) {
      if (UNLIKELY(region->mapped_user == 0)) {
        if (!kUsingConstantSpaceBeg && kRandomShuffleChunks)
          // The random state is initialized from ASLR.
          region->rand_state = static_cast<u32>(region_beg >> 12);
        // Postpone the first release to OS attempt for ReleaseToOSIntervalMs,
        // preventing just allocated memory from being released sooner than
        // necessary and also preventing extraneous ReleaseMemoryPagesToOS calls
        // for short lived processes.
        // Do it only when the feature is turned on, to avoid a potentially
        // extraneous syscall.
        if (ReleaseToOSIntervalMs() >= 0)
          region->rtoi.last_release_at_ns = MonotonicNanoTime();
      }

      // Do the mmap for the user memory.
      const uptr user_map_size =
          RoundUpTo(total_user_bytes - region->mapped_user, kUserMapSize);

      if (UNLIKELY(IsRegionExhausted(region, class_id, user_map_size)))
        return false;
      if (UNLIKELY(!MapWithCallback(region_beg + region->mapped_user,
                                    user_map_size,
                                    "SizeClassAllocator: region data")))
        return false;
      stat->Add(AllocatorStatMapped, user_map_size);
      region->mapped_user += user_map_size;
    }

    const uptr new_chunks_count =
        (region->mapped_user - region->allocated_user) / size;

    if (kMetadataSize) {
      // Calculate the required space for metadata.
      const uptr total_meta_bytes =
          region->allocated_meta + new_chunks_count * kMetadataSize;
      const uptr meta_map_size = (total_meta_bytes > region->mapped_meta) ?
          RoundUpTo(total_meta_bytes - region->mapped_meta, kMetaMapSize) : 0;
      // Map more space for metadata, if necessary.
      if (meta_map_size) {
        if (UNLIKELY(IsRegionExhausted(region, class_id, meta_map_size)))
          return false;
        if (UNLIKELY(!MapWithCallback(
            GetMetadataEnd(region_beg) - region->mapped_meta - meta_map_size,
            meta_map_size, "SizeClassAllocator: region metadata")))
          return false;
        region->mapped_meta += meta_map_size;
      }
    }

    // If necessary, allocate more space for the free array and populate it with
    // newly allocated chunks.
    const uptr total_freed_chunks = region->num_freed_chunks + new_chunks_count;
    if (UNLIKELY(!EnsureFreeArraySpace(region, region_beg, total_freed_chunks)))
      return false;
    CompactPtrT *free_array = GetFreeArray(region_beg);
    for (uptr i = 0, chunk = region->allocated_user; i < new_chunks_count;
         i++, chunk += size)
      free_array[total_freed_chunks - 1 - i] = PointerToCompactPtr(0, chunk);

    if (kRandomShuffleChunks)
      RandomShuffle(&free_array[region->num_freed_chunks], new_chunks_count,
                    &region->rand_state);

    // All necessary memory is mapped and now it is safe to advance all
    // 'allocated_*' counters.
    region->num_freed_chunks += new_chunks_count;
    region->allocated_user += new_chunks_count * size;
    CHECK_LE(region->allocated_user, region->mapped_user);
    region->allocated_meta += new_chunks_count * kMetadataSize;
    CHECK_LE(region->allocated_meta, region->mapped_meta);
    region->exhausted = false;

    // TODO(alekseyshl): Consider bumping last_release_at_ns here to prevent
    // MaybeReleaseToOS from releasing just allocated pages or protect these
    // not yet used chunks some other way.

    return true;
  }

  // Attempts to release RAM occupied by freed chunks back to OS. The region is
  // expected to be locked.
  //
  // TODO(morehouse): Support a callback on memory release so HWASan can release
  // aliases as well.
  void MaybeReleaseToOS(MemoryMapperT *memory_mapper, uptr class_id,
                        bool force) {
    RegionInfo *region = GetRegionInfo(class_id);
    const uptr chunk_size = ClassIdToSize(class_id);
    const uptr page_size = GetPageSizeCached();

    uptr n = region->num_freed_chunks;
    if (n * chunk_size < page_size)
      return;  // No chance to release anything.
    if ((region->stats.n_freed -
         region->rtoi.n_freed_at_last_release) * chunk_size < page_size) {
      return;  // Nothing new to release.
    }

    if (!force) {
      s32 interval_ms = ReleaseToOSIntervalMs();
      if (interval_ms < 0)
        return;

      if (region->rtoi.last_release_at_ns + interval_ms * 1000000ULL >
          MonotonicNanoTime()) {
        return;  // Memory was returned recently.
      }
    }

    ReleaseFreeMemoryToOS(
        GetFreeArray(GetRegionBeginBySizeClass(class_id)), n, chunk_size,
        RoundUpTo(region->allocated_user, page_size) / page_size, memory_mapper,
        class_id);

    uptr ranges, bytes;
    if (memory_mapper->GetAndResetStats(ranges, bytes)) {
      region->rtoi.n_freed_at_last_release = region->stats.n_freed;
      region->rtoi.num_releases += ranges;
      region->rtoi.last_released_bytes = bytes;
    }
    region->rtoi.last_release_at_ns = MonotonicNanoTime();
  }
};
