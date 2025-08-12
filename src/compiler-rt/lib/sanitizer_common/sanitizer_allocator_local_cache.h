//===-- sanitizer_allocator_local_cache.h -----------------------*- C++ -*-===//
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

#include "sanitizer_common.h"
//#include <pthread.h>
#include "my_tools.h"


// Cache used by SizeClassAllocator64.
template <class SizeClassAllocator>
struct SizeClassAllocator64LocalCache {
  typedef SizeClassAllocator Allocator;
  typedef MemoryMapper<Allocator> MemoryMapperT;


 public:
  void Init(AllocatorGlobalStats *s) {
    stats_.Init();
    if (s)
      s->Register(&stats_);
  }

  void CTInit(){
  }

  void Destroy(SizeClassAllocator *allocator, AllocatorGlobalStats *s) {
    if (allocator->is_clustertag){
      return;
    }

    Drain(allocator);
    if (s)
      s->Unregister(&stats_);
  }


  // ClusterTag. Allocate a small chunk (size < 0x10000)
  void *CTAllocateSmallChunk(SizeClassAllocator* allocator, uptr class_id) {
    CHECK_NE(class_id, 0UL);
    CHECK_LT(class_id, kNumClasses);

    // ClusterTag. Check if the CTPerClass is empty.
    CTPerClass *c = &ct_per_class_[class_id];
    if (UNLIKELY(c->count == 0)) {
      SpinMutexLock l(&(allocator->SuperPoolLocksForSizeClass[class_id]));
      if (UNLIKELY(!allocator->CTRefill(c, class_id)))
        return nullptr;
    }

    void* chunk = c->batch[c->count-1];
    c->batch[c->count-1] = 0;

    chunk = (void*)CTAddTagToPointer((uptr)chunk, c->chunk_tag[c->count-1]);

    c->count--;
    return chunk;
  }


  // ClusterTag. Allocate a large chunk (size > 0x10000) that is too large to be processed by the SizeClass allocator.
  void* CTAllocateLargeChunk(SizeClassAllocator* allocator, uptr original_size, uptr alignment){
    CHECK(IsPowerOfTwo(alignment));
    // ClusterTag. +16 is used to store the LargeChunkHeader.
    uptr map_size = RoundUpTo(original_size+16, page_size_);
    if (alignment > page_size_)
      map_size += alignment;

    // Overflow.
    if (map_size < original_size) {
      Report("WARNING: %s: LargeMmapAllocator allocation overflow: "
          "0x%zx bytes with 0x%zx alignment requested\n",
          SanitizerToolName, map_size, alignment);
      return nullptr;
    }

    SpinMutexLock l(&(allocator->SuperPoolLockForLargeChunk));
    uptr res_beg = 0;
    bool retu_status = allocator->CTAllocateLargeChunk(map_size, &res_beg);
    if (retu_status == false){
      Printf("Failed to allocate LargeChunk with size=0x%lx.\n", original_size);
      internal__exit(1);
    }

    CTLargeChunkHeader* chunkHeader = (CTLargeChunkHeader*)((u64)res_beg);
    chunkHeader->requested_size = original_size; // ClusterTag. Record the actually requested size in the chunk header.
    chunkHeader->allocated_size = map_size;      // ClusterTag. Record the actually allocated size in the chunk header.

    // ClusterTag. Generate a random tag for the LargeChunk.
    u8 tag = ((u8)(__sanitizer::genRandom() & 0xff) % 255) + 1;
    uptr taggedPtr = CTAddTagToPointer((uptr)((u64)res_beg+16), tag);

    return reinterpret_cast<void*>(taggedPtr);
  }


  // ClusterTag. Dispatch memory allocation based on the request size.
  void *CTAllocateCache(SizeClassAllocator *allocator, uptr size, uptr alignment) {
    uptr original_size = size;
    if (alignment > 8)
      size = RoundUpTo(size, alignment);

    void *res;
    if (SizeClassAllocator::CanAllocate(size, alignment)) {
      uptr class_id = SizeClassAllocator::ClassID(size);
      res = CTAllocateSmallChunk(allocator, class_id);
    }
    else {
      res = CTAllocateLargeChunk(allocator, original_size, alignment);
    }

    return res;
  }


  // ClusterTag. Deallocate a small chunk (size < 0x10000)
  void CTDeallocateSmallChunk(SizeClassAllocator* allocator, u8 tag, void *p){
    uptr superPoolID /* =class_id */, poolID, chunk_size;
    u8 chunk_id;
    CTRegionInfo* region_info;
    SizeClassAllocator::parseSmallChunkPtr((uptr)p, &superPoolID, &poolID, &chunk_id, &chunk_size, &region_info);

    SpinMutexLock l(&(allocator->SuperPoolLocksForSizeClass[superPoolID]));

    region_info->freed_num += 1;
    region_info->chunk_last_tag[chunk_id] = tag;

    CTSuperPoolChunkInfo* spc = allocator->SuperPoolChunkInfo[superPoolID];
    spc->free_chunk_num += 1;

    // ClusterTag. Maintain the SuperPoolsFreedRegionList.
    if (!region_info->in_free_list) {
      region_info->insertAtFreedListHead(allocator->SuperPoolsFreedRegionList[superPoolID]);
      spc->free_region_num += 1;
      region_info->in_free_list = 1;
    }

    // ClusterTag. Periodically trigger memory release.
    if (spc->free_chunk_num >= spc->trigger_free){
      if (spc->last_release_at_ns + __sanitizer::free_list_interval_ns < MonotonicNanoTime()) {
        allocator->CTReleasePeriodically(superPoolID);
        spc->last_release_at_ns = MonotonicNanoTime();
      }
    }
  }

  // ClusterTag. Dispatch memory deallocation based on the pointer
  void CTDeallocateCache(SizeClassAllocator *allocator, u8 tag, void* untaggedPtr) {
    if (SizeClassAllocator::isSmallChunk((uptr)untaggedPtr)){
      CTDeallocateSmallChunk(allocator, tag, untaggedPtr);
    } else {
      allocator->CTDeallocateLargeChunk(untaggedPtr);
    }
  }


 public:

  void *Allocate(SizeClassAllocator *allocator, uptr class_id) {
    CHECK_NE(class_id, 0UL);
    CHECK_LT(class_id, kNumClasses);
    PerClass *c = &per_class_[class_id];
    if (UNLIKELY(c->count == 0)) {
      if (UNLIKELY(!Refill(c, allocator, class_id)))
        return nullptr;
      DCHECK_GT(c->count, 0);
    }
    CompactPtrT chunk = c->chunks[--c->count];
    stats_.Add(AllocatorStatAllocated, c->class_size);
    return reinterpret_cast<void *>(allocator->CompactPtrToPointer(
        allocator->GetRegionBeginBySizeClass(class_id), chunk));
  }

  void Deallocate(SizeClassAllocator *allocator, uptr class_id, void *p) {
    CHECK_NE(class_id, 0UL);
    CHECK_LT(class_id, kNumClasses);
    // If the first allocator call on a new thread is a deallocation, then
    // max_count will be zero, leading to check failure.
    PerClass *c = &per_class_[class_id];
    InitCache(c);
    if (UNLIKELY(c->count == c->max_count))
      DrainHalfMax(c, allocator, class_id);
    CompactPtrT chunk = allocator->PointerToCompactPtr(
        allocator->GetRegionBeginBySizeClass(class_id),
        reinterpret_cast<uptr>(p));
    c->chunks[c->count++] = chunk;
    stats_.Sub(AllocatorStatAllocated, c->class_size);
  }

  void Drain(SizeClassAllocator *allocator) {
    MemoryMapperT memory_mapper(*allocator);
    for (uptr i = 1; i < kNumClasses; i++) {
      PerClass *c = &per_class_[i];
      while (c->count > 0) Drain(&memory_mapper, c, allocator, i, c->count);
    }
  }


 private:
  typedef typename Allocator::SizeClassMapT SizeClassMap;
  static const uptr kNumClasses = SizeClassMap::kNumClasses;
  typedef typename Allocator::CompactPtrT CompactPtrT;

  struct PerClass {
    u32 count;
    u32 max_count;
    uptr class_size;
    CompactPtrT chunks[2 * SizeClassMap::kMaxNumCachedHint];
  };

  PerClass per_class_[kNumClasses];
  AllocatorStats stats_;

  CTPerClass ct_per_class_[kNumClasses];

  void InitCache(PerClass *c) {
    if (LIKELY(c->max_count))
      return;
    for (uptr i = 1; i < kNumClasses; i++) {
      PerClass *c = &per_class_[i];
      const uptr size = Allocator::ClassIdToSize(i);
      c->max_count = 2 * SizeClassMap::MaxCachedHint(size);
      c->class_size = size;
    }
    DCHECK_NE(c->max_count, 0UL);
  }

  NOINLINE bool Refill(PerClass *c, SizeClassAllocator *allocator,
                       uptr class_id) {
    InitCache(c);
    const uptr num_requested_chunks = c->max_count / 2;
    if (UNLIKELY(!allocator->GetFromAllocator(&stats_, class_id, c->chunks,
                                              num_requested_chunks)))
      return false;
    c->count = num_requested_chunks;
    return true;
  }

  NOINLINE void DrainHalfMax(PerClass *c, SizeClassAllocator *allocator,
                             uptr class_id) {
    MemoryMapperT memory_mapper(*allocator);
    Drain(&memory_mapper, c, allocator, class_id, c->max_count / 2);
  }

  void Drain(MemoryMapperT *memory_mapper, PerClass *c,
             SizeClassAllocator *allocator, uptr class_id, uptr count) {
    CHECK_GE(c->count, count);
    const uptr first_idx_to_drain = c->count - count;
    c->count -= count;
    allocator->ReturnToAllocator(memory_mapper, &stats_, class_id,
                                 &c->chunks[first_idx_to_drain], count);
  }
};

// Cache used by SizeClassAllocator32.
template <class SizeClassAllocator>
struct SizeClassAllocator32LocalCache {
  typedef SizeClassAllocator Allocator;
  typedef typename Allocator::TransferBatch TransferBatch;

  void Init(AllocatorGlobalStats *s) {
    stats_.Init();
    if (s)
      s->Register(&stats_);
  }

  // Returns a TransferBatch suitable for class_id.
  TransferBatch *CreateBatch(uptr class_id, SizeClassAllocator *allocator,
                             TransferBatch *b) {
    if (uptr batch_class_id = per_class_[class_id].batch_class_id)
      return (TransferBatch*)Allocate(allocator, batch_class_id);
    return b;
  }

  // Destroys TransferBatch b.
  void DestroyBatch(uptr class_id, SizeClassAllocator *allocator,
                    TransferBatch *b) {
    if (uptr batch_class_id = per_class_[class_id].batch_class_id)
      Deallocate(allocator, batch_class_id, b);
  }

  void Destroy(SizeClassAllocator *allocator, AllocatorGlobalStats *s) {
    Drain(allocator);
    if (s)
      s->Unregister(&stats_);
  }

  void *Allocate(SizeClassAllocator *allocator, uptr class_id) {
    CHECK_NE(class_id, 0UL);
    CHECK_LT(class_id, kNumClasses);
    PerClass *c = &per_class_[class_id];
    if (UNLIKELY(c->count == 0)) {
      if (UNLIKELY(!Refill(c, allocator, class_id)))
        return nullptr;
      DCHECK_GT(c->count, 0);
    }
    void *res = c->batch[--c->count];
    PREFETCH(c->batch[c->count - 1]);
    stats_.Add(AllocatorStatAllocated, c->class_size);
    return res;
  }

  void Deallocate(SizeClassAllocator *allocator, uptr class_id, void *p) {
    CHECK_NE(class_id, 0UL);
    CHECK_LT(class_id, kNumClasses);
    // If the first allocator call on a new thread is a deallocation, then
    // max_count will be zero, leading to check failure.
    PerClass *c = &per_class_[class_id];
    InitCache(c);
    if (UNLIKELY(c->count == c->max_count))
      Drain(c, allocator, class_id);
    c->batch[c->count++] = p;
    stats_.Sub(AllocatorStatAllocated, c->class_size);
  }

  void Drain(SizeClassAllocator *allocator) {
    for (uptr i = 1; i < kNumClasses; i++) {
      PerClass *c = &per_class_[i];
      while (c->count > 0)
        Drain(c, allocator, i);
    }
  }

 private:
  typedef typename Allocator::SizeClassMapT SizeClassMap;
  static const uptr kBatchClassID = SizeClassMap::kBatchClassID;
  static const uptr kNumClasses = SizeClassMap::kNumClasses;
  // If kUseSeparateSizeClassForBatch is true, all TransferBatch objects are
  // allocated from kBatchClassID size class (except for those that are needed
  // for kBatchClassID itself). The goal is to have TransferBatches in a totally
  // different region of RAM to improve security.
  static const bool kUseSeparateSizeClassForBatch =
      Allocator::kUseSeparateSizeClassForBatch;

  struct PerClass {
    uptr count;
    uptr max_count;
    uptr class_size;
    uptr batch_class_id;
    void *batch[2 * TransferBatch::kMaxNumCached];
  };
  PerClass per_class_[kNumClasses];
  AllocatorStats stats_;

  void InitCache(PerClass *c) {
    if (LIKELY(c->max_count))
      return;
    const uptr batch_class_id = SizeClassMap::ClassID(sizeof(TransferBatch));
    for (uptr i = 1; i < kNumClasses; i++) {
      PerClass *c = &per_class_[i];
      const uptr size = Allocator::ClassIdToSize(i);
      const uptr max_cached = TransferBatch::MaxCached(size);
      c->max_count = 2 * max_cached;
      c->class_size = size;
      // Precompute the class id to use to store batches for the current class
      // id. 0 means the class size is large enough to store a batch within one
      // of the chunks. If using a separate size class, it will always be
      // kBatchClassID, except for kBatchClassID itself.
      if (kUseSeparateSizeClassForBatch) {
        c->batch_class_id = (i == kBatchClassID) ? 0 : kBatchClassID;
      } else {
        c->batch_class_id = (size <
          TransferBatch::AllocationSizeRequiredForNElements(max_cached)) ?
              batch_class_id : 0;
      }
    }
    DCHECK_NE(c->max_count, 0UL);
  }

  NOINLINE bool Refill(PerClass *c, SizeClassAllocator *allocator,
                       uptr class_id) {
    InitCache(c);
    TransferBatch *b = allocator->AllocateBatch(&stats_, this, class_id);
    if (UNLIKELY(!b))
      return false;
    CHECK_GT(b->Count(), 0);
    b->CopyToArray(c->batch);
    c->count = b->Count();
    DestroyBatch(class_id, allocator, b);
    return true;
  }

  NOINLINE void Drain(PerClass *c, SizeClassAllocator *allocator,
                      uptr class_id) {
    const uptr count = Min(c->max_count / 2, c->count);
    const uptr first_idx_to_drain = c->count - count;
    TransferBatch *b = CreateBatch(
        class_id, allocator, (TransferBatch *)c->batch[first_idx_to_drain]);
    // Failure to allocate a batch while releasing memory is non recoverable.
    // TODO(alekseys): Figure out how to do it without allocating a new batch.
    if (UNLIKELY(!b)) {
      Report("FATAL: Internal error: %s's allocator failed to allocate a "
             "transfer batch.\n", SanitizerToolName);
      Die();
    }
    b->SetFromArray(&c->batch[first_idx_to_drain], count);
    c->count -= count;
    allocator->DeallocateBatch(&stats_, class_id, b);
  }

 public:

  // ClusterTag. Keep the same function declarations as Cache64
  void CTInit(){
  }

  void *CTAllocateCache(SizeClassAllocator *allocator, uptr size, uptr alignment){
    Printf("HWASanAllocateCache cannot be called in SizeClassAllocator32LocalCache\n");
    internal__exit(1);
  }

  void CTDeallocateRegion(SizeClassAllocator *allocator, void *p){
    Printf("HWASanDeallocateRegion function cannot be called in SizeClassAllocator32LocalCache\n");
    internal__exit(1);
  }



  void CTDeallocateCache(SizeClassAllocator *allocator, u8 tag, void *p) {
    Printf("HWASanDeallocateCache function cannot be called in SizeClassAllocator32LocalCache\n");
    internal__exit(1);
  }
};
