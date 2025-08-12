//===-- hwasan_allocator.cpp ------------------------ ---------------------===//
//===-- hwasan_allocator.cpp ------------------------ ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of HWAddressSanitizer.
//
// HWAddressSanitizer allocator.
//===----------------------------------------------------------------------===//

#include "sanitizer_common/sanitizer_atomic.h"
#include "sanitizer_common/sanitizer_errno.h"
#include "sanitizer_common/sanitizer_stackdepot.h"
#include "hwasan.h"
#include "hwasan_allocator.h"
#include "hwasan_checks.h"
#include "hwasan_mapping.h"
#include "hwasan_malloc_bisect.h"
#include "hwasan_thread.h"
#include "hwasan_report.h"

namespace __hwasan {

static Allocator allocator;
static AllocatorCache fallback_allocator_cache;
static SpinMutex fallback_mutex;
static atomic_uint8_t hwasan_allocator_tagging_enabled;

static constexpr tag_t kFallbackAllocTag = 0xBB & kTagMask;
static constexpr tag_t kFallbackFreeTag = 0xBC;

enum RightAlignMode {
  kRightAlignNever,
  kRightAlignSometimes,
  kRightAlignAlways
};

// Initialized in HwasanAllocatorInit, an never changed.
static ALIGNED(16) u8 tail_magic[kShadowAlignment - 1];

bool HwasanChunkView::IsAllocated() const {
  return metadata_ && metadata_->alloc_context_id &&
         metadata_->get_requested_size();
}

// Aligns the 'addr' right to the granule boundary.
static uptr AlignRight(uptr addr, uptr requested_size) {
  uptr tail_size = requested_size % kShadowAlignment;
  if (!tail_size) return addr;
  return addr + kShadowAlignment - tail_size;
}

uptr HwasanChunkView::Beg() const {
  if (metadata_ && metadata_->right_aligned)
    return AlignRight(block_, metadata_->get_requested_size());
  return block_;
}
uptr HwasanChunkView::End() const {
  return Beg() + UsedSize();
}
uptr HwasanChunkView::UsedSize() const {
  return metadata_->get_requested_size();
}
u32 HwasanChunkView::GetAllocStackId() const {
  return metadata_->alloc_context_id;
}

uptr HwasanChunkView::ActualSize() const {
  return allocator.GetActuallyAllocatedSize(reinterpret_cast<void *>(block_));
}

bool HwasanChunkView::FromSmallHeap() const {
  return allocator.FromPrimary(reinterpret_cast<void *>(block_));
}

void GetAllocatorStats(AllocatorStatCounters s) {
  allocator.GetStats(s);
}

uptr GetAliasRegionStart() {
#if defined(HWASAN_ALIASING_MODE)
  constexpr uptr kAliasRegionOffset = 1ULL << (kTaggableRegionCheckShift - 1);
  uptr AliasRegionStart =
      __hwasan_shadow_memory_dynamic_address + kAliasRegionOffset;

  CHECK_EQ(AliasRegionStart >> kTaggableRegionCheckShift,
           __hwasan_shadow_memory_dynamic_address >> kTaggableRegionCheckShift);
  CHECK_EQ(
      (AliasRegionStart + kAliasRegionOffset - 1) >> kTaggableRegionCheckShift,
      __hwasan_shadow_memory_dynamic_address >> kTaggableRegionCheckShift);
  return AliasRegionStart;
#else
  return 0;
#endif
}

void HwasanAllocatorInit() {
  atomic_store_relaxed(&hwasan_allocator_tagging_enabled,
                       !flags()->disable_allocator_tagging);
  SetAllocatorMayReturnNull(common_flags()->allocator_may_return_null);

  allocator.Init(common_flags()->allocator_release_to_os_interval_ms, GetAliasRegionStart(), true /*is_clustertag*/);

  fallback_allocator_cache.CTInit();

  for (uptr i = 0; i < sizeof(tail_magic); i++)
    tail_magic[i] = GetCurrentThread()->GenerateRandomTag();
}

void HwasanAllocatorLock() { allocator.ForceLock(); }

void HwasanAllocatorUnlock() { allocator.ForceUnlock(); }

void AllocatorSwallowThreadLocalCache(AllocatorCache *cache) {
  allocator.SwallowCache(cache);
}

static uptr TaggedSize(uptr size) {
  if (!size) size = 1;
  uptr new_size = RoundUpTo(size, kShadowAlignment);
  CHECK_GE(new_size, size);
  return new_size;
}


// ClusterTag. Allocates memory using Combined Allocator, writes tag to shadow memory
static void *HwasanAllocate(StackTrace *stack, uptr orig_size, uptr alignment,
                            bool zeroise) {
  if (orig_size > kMaxAllowedMallocSize) {
    if (AllocatorMayReturnNull()) {
      Report("WARNING: HWAddressSanitizer failed to allocate 0x%zx bytes\n",
             orig_size);
      return nullptr;
    }
    ReportAllocationSizeTooBig(orig_size, kMaxAllowedMallocSize, stack);
  }

  if (UNLIKELY(IsRssLimitExceeded())) {
    if (AllocatorMayReturnNull())
      return nullptr;
    ReportRssLimitExceeded(stack);
  }

  alignment = Max(alignment, kShadowAlignment);
  uptr size = TaggedSize(orig_size);

  // ClusterTag. Allocation returns chunk address and class_id.
  SpinMutexLock l(&fallback_mutex);
  AllocatorCache *cache = &fallback_allocator_cache;
  void * allocated = allocator.Allocate(cache, size, alignment);

  // ClusterTag. Mask the returned pointer and extract its tag.
  void *allocated_untagged = UntagPtr(allocated);
  tag_t tag = GetTagFromPointer((uptr)allocated);

  if (UNLIKELY(!allocated_untagged)) {
    SetAllocatorOutOfMemory();
    if (AllocatorMayReturnNull())
      return nullptr;
    ReportOutOfMemory(size, stack);
  }

  // ClusterTag. Zero or fill the allocated memory according to options.
  if (zeroise) {
    internal_memset(allocated_untagged, 0, size);
  } else if (flags()->max_malloc_fill_size > 0) {
    uptr fill_size = Min(size, (uptr)flags()->max_malloc_fill_size);
    internal_memset(allocated_untagged, flags()->malloc_fill_byte, fill_size);
  }
  if (size != orig_size) {
    u8 *tail = reinterpret_cast<u8 *>(allocated_untagged) + orig_size;
    uptr tail_length = size - orig_size;
    internal_memcpy(tail, tail_magic, tail_length - 1);
    // Short granule is excluded from magic tail, so we explicitly untag.
    tail[tail_length - 1] = 0;
  }

  // ClusterTag. Write tag to shadow memory.
  if (InTaggableRegion(reinterpret_cast<uptr>(allocated_untagged)) &&
      (flags()->tag_in_malloc || flags()->tag_in_free) &&
      atomic_load_relaxed(&hwasan_allocator_tagging_enabled)) {
    if (flags()->tag_in_malloc && malloc_bisect(stack, orig_size)) {

      uptr tag_size = orig_size ? orig_size : 1;
      uptr full_granule_size = RoundDownTo(tag_size, kShadowAlignment);
      TagMemoryAligned((uptr)allocated_untagged, full_granule_size, tag);
      if (full_granule_size != tag_size) {
        u8 *short_granule = reinterpret_cast<u8 *>(allocated_untagged) + full_granule_size;
        TagMemoryAligned((uptr)short_granule, kShadowAlignment, tag_size % kShadowAlignment);

        short_granule[kShadowAlignment - 1] = tag;
      }
    } else {
      TagMemoryAligned((uptr)allocated_untagged, size, 0);
    }
  }

  RunMallocHooks(allocated_untagged, size);
  return allocated;
}

static bool PointerAndMemoryTagsMatch(void *tagged_ptr) {
  CHECK(tagged_ptr);
  uptr tagged_uptr = reinterpret_cast<uptr>(tagged_ptr);
  if (!InTaggableRegion(tagged_uptr))
    return true;
  tag_t mem_tag = *reinterpret_cast<tag_t *>(
      MemToShadow(reinterpret_cast<uptr>(UntagPtr(tagged_ptr))));
  return PossiblyShortTagMatches(mem_tag, tagged_uptr, 1);
}

static bool CheckInvalidFree(StackTrace *stack, void *untagged_ptr,
                             void *tagged_ptr) {
  // This function can return true if halt_on_error is false.
  if (!MemIsApp(reinterpret_cast<uptr>(untagged_ptr)) ||
      !PointerAndMemoryTagsMatch(tagged_ptr)) {
    ReportInvalidFree(stack, reinterpret_cast<uptr>(tagged_ptr));
    return true;
  }
  return false;
}

// ClusterTag. Deallocates memory using Combined Allocator
static void HwasanDeallocate(StackTrace *stack, void *tagged_ptr) {
  CHECK(tagged_ptr);
  RunFreeHooks(tagged_ptr);

  void *untagged_ptr = UntagPtr(tagged_ptr);
  tag_t pointer_tag = GetTagFromPointer(reinterpret_cast<uptr>(tagged_ptr));
  void *aligned_ptr = reinterpret_cast<void *>(RoundDownTo(reinterpret_cast<uptr>(untagged_ptr), kShadowAlignment));
  bool isLargeChunk = !PrimaryAllocator::isSmallChunk((uptr)untagged_ptr);

  if (pointer_tag == 0) {
    Printf("Pointer p=%p has no tag, possibly freeing chunk from another allocator\n", tagged_ptr);
    internal__exit(1);
  }

  uptr orig_size = PrimaryAllocator::CTGetRequestedSize((uptr) untagged_ptr);

  if (!PointerAndMemoryTagsMatch(tagged_ptr)) {
    Printf("[Temporal Error-Double Free] Pointer P=%p tag mismatch\n", tagged_ptr);
    ReportInvalidFree(stack, reinterpret_cast<uptr>(tagged_ptr));
  }

  CHECK(IsAligned((uptr) untagged_ptr, 16));
  if (isLargeChunk) {
    uptr shadow_memory_ptr = MemToShadow((uptr) untagged_ptr);
    if (*(u8 *) ((u64) shadow_memory_ptr - 1) != 0) {
      Printf("Pointer P=%p is Large Chunk but not at chunk start\n", tagged_ptr);
      internal__exit(1);
    }
  } else {
    uptr region_offset = (u64) untagged_ptr - (uptr) PrimaryAllocator::getRegionInfo((u64) untagged_ptr, orig_size);
    if (region_offset % orig_size != 0) {
      Printf("Pointer P=%p is Small Chunk but not aligned to 0x%lx\n", (uptr) tagged_ptr, orig_size);
      internal__exit(1);
    }
  }

  if (isLargeChunk) {
    uptr tagged_size = TaggedSize(orig_size);
    if (flags()->free_checks_tail_magic && orig_size && tagged_size != orig_size) {
      uptr tail_size = tagged_size - orig_size - 1;
      CHECK_LT(tail_size, kShadowAlignment);
      void *tail_beg = reinterpret_cast<void *>(reinterpret_cast<uptr>(aligned_ptr) + orig_size);
      tag_t short_granule_memtag = *(reinterpret_cast<tag_t *>(reinterpret_cast<uptr>(tail_beg) + tail_size));
      if (tail_size &&
          (internal_memcmp(tail_beg, tail_magic, tail_size) ||
           (pointer_tag != short_granule_memtag)))
        ReportTailOverwritten(stack, reinterpret_cast<uptr>(tagged_ptr), orig_size, tail_magic);
    }
  }

  if (flags()->max_free_fill_size > 0) {
    if (!isLargeChunk){
      uptr fill_size = Min(TaggedSize(orig_size), (uptr)flags()->max_free_fill_size);
      internal_memset(aligned_ptr, flags()->free_fill_byte, fill_size);
    }
  }

  if (!isLargeChunk)
    TagMemoryAligned(reinterpret_cast<uptr>(aligned_ptr), TaggedSize(orig_size), 0);

  SpinMutexLock l(&fallback_mutex);
  AllocatorCache *cache = &fallback_allocator_cache;
  allocator.Deallocate(cache, allocator.is_clustertag ? (void*)AddTagToPointer((uptr)aligned_ptr, pointer_tag) : aligned_ptr);
}

// ClusterTag. Reallocates memory
static void *HwasanReallocate(StackTrace *stack, void *tagged_ptr_old,
                              uptr new_size, uptr alignment) {
  void *untagged_ptr_old = UntagPtr(tagged_ptr_old);
  tag_t pointer_tag_old = GetTagFromPointer(reinterpret_cast<uptr>(tagged_ptr_old));
  bool isLargeChunk = !PrimaryAllocator::isSmallChunk((uptr)untagged_ptr_old);

  // ClusterTag. Check 1: Verify pointer tag is present in high bits.
  if (pointer_tag_old == 0){
    Printf("Pointer p=%p has no tag in high bits, which may indicate that it is freeing a chunk allocated by another memory allocator\n", tagged_ptr_old);
    internal__exit(1);
  }

  // ClusterTag. Calculate the original requested size of the chunk.
  uptr old_size = PrimaryAllocator::CTGetRequestedSize((uptr)untagged_ptr_old);

  // ClusterTag. Check 2: Temporal error check by comparing pointer tag and shadow memory.
  if (!PointerAndMemoryTagsMatch(tagged_ptr_old)) {
    Printf("[Temporal Error-Double Free] Pointer P=%p does not match its high bit tag\n", tagged_ptr_old);
  // ClusterTag. Note: ScopedReport R(flags()->halt_on_error) at function start will call exit(99) at the end, so no need to call exit explicitly here.
    ReportInvalidFree(stack, reinterpret_cast<uptr>(tagged_ptr_old));
  }

  // ClusterTag. Check 3: Upper boundary alignment check.
  CHECK(IsAligned((uptr) untagged_ptr_old, 16));
  if (isLargeChunk) {
    // ClusterTag. For Large Chunk, check if pointer is at chunk start (previous shadow memory is 0).
    uptr shadow_memory_ptr = MemToShadow((uptr) untagged_ptr_old);
    if (*(u8 *) ((u64) shadow_memory_ptr - 1) != 0) {
      Printf("Pointer P=%p is a Large Chunk, but it is not at the chunk header\n", tagged_ptr_old);
      internal__exit(1);
    }
  } else {
    // ClusterTag. For Small Chunk, check if pointer is properly aligned to chunk size.
    uptr region_offset =
            (u64) untagged_ptr_old - (uptr) PrimaryAllocator::getRegionInfo((u64) untagged_ptr_old, old_size);
    if (region_offset % old_size != 0) {
      Printf("Pointer P=%p is a Small Chunk, but it is not aligned to 0x%lx\n", (uptr) tagged_ptr_old, old_size);
      internal__exit(1);
    }
  }

  // ClusterTag. Allocate a new region with the given new_size.
  void *tagged_ptr_new = HwasanAllocate(stack, new_size, alignment, false /*zeroise*/);

  if (tagged_ptr_old && tagged_ptr_new) {

    internal_memcpy(
        UntagPtr(tagged_ptr_new), untagged_ptr_old,
        Min(new_size, old_size));

    HwasanDeallocate(stack, tagged_ptr_old);
  }

  return tagged_ptr_new;
}

static void *HwasanCalloc(StackTrace *stack, uptr nmemb, uptr size) {
  if (UNLIKELY(CheckForCallocOverflow(size, nmemb))) {
    if (AllocatorMayReturnNull())
      return nullptr;
    ReportCallocOverflow(nmemb, size, stack);
  }
  return HwasanAllocate(stack, nmemb * size, sizeof(u64), true);
}


HwasanChunkView FindHeapChunkByAddress(uptr address) {
  Printf("FindHeapChunkByAddress is not implemented\n");
  internal__exit(1);
}

static uptr AllocationSize(const void *tagged_ptr) {
  const void *untagged_ptr = UntagPtr(tagged_ptr);
  if (!untagged_ptr) return 0;

  return PrimaryAllocator::CTGetRequestedSize((uptr)untagged_ptr);
}

void *hwasan_malloc(uptr size, StackTrace *stack) {
  return SetErrnoOnNull(HwasanAllocate(stack, size, sizeof(u64), false));
}

void *hwasan_calloc(uptr nmemb, uptr size, StackTrace *stack) {
  return SetErrnoOnNull(HwasanCalloc(stack, nmemb, size));
}

void *hwasan_realloc(void *ptr, uptr size, StackTrace *stack) {
  if (!ptr)
    return SetErrnoOnNull(HwasanAllocate(stack, size, sizeof(u64), false));
  if (size == 0) {
    HwasanDeallocate(stack, ptr);
    return nullptr;
  }
  return SetErrnoOnNull(HwasanReallocate(stack, ptr, size, sizeof(u64)));
}

void *hwasan_reallocarray(void *ptr, uptr nmemb, uptr size, StackTrace *stack) {
  if (UNLIKELY(CheckForCallocOverflow(size, nmemb))) {
    errno = errno_ENOMEM;
    if (AllocatorMayReturnNull())
      return nullptr;
    ReportReallocArrayOverflow(nmemb, size, stack);
  }
  return hwasan_realloc(ptr, nmemb * size, stack);
}

void *hwasan_valloc(uptr size, StackTrace *stack) {
  return SetErrnoOnNull(
      HwasanAllocate(stack, size, GetPageSizeCached(), false));
}

void *hwasan_pvalloc(uptr size, StackTrace *stack) {
  uptr PageSize = GetPageSizeCached();
  if (UNLIKELY(CheckForPvallocOverflow(size, PageSize))) {
    errno = errno_ENOMEM;
    if (AllocatorMayReturnNull())
      return nullptr;
    ReportPvallocOverflow(size, stack);
  }
  // pvalloc(0) should allocate one page.
  size = size ? RoundUpTo(size, PageSize) : PageSize;
  return SetErrnoOnNull(HwasanAllocate(stack, size, PageSize, false));
}

void *hwasan_aligned_alloc(uptr alignment, uptr size, StackTrace *stack) {
  if (UNLIKELY(!CheckAlignedAllocAlignmentAndSize(alignment, size))) {
    errno = errno_EINVAL;
    if (AllocatorMayReturnNull())
      return nullptr;
    ReportInvalidAlignedAllocAlignment(size, alignment, stack);
  }
  return SetErrnoOnNull(HwasanAllocate(stack, size, alignment, false));
}

void *hwasan_memalign(uptr alignment, uptr size, StackTrace *stack) {
  if (UNLIKELY(!IsPowerOfTwo(alignment))) {
    errno = errno_EINVAL;
    if (AllocatorMayReturnNull())
      return nullptr;
    ReportInvalidAllocationAlignment(alignment, stack);
  }
  return SetErrnoOnNull(HwasanAllocate(stack, size, alignment, false));
}

int hwasan_posix_memalign(void **memptr, uptr alignment, uptr size,
                        StackTrace *stack) {
  if (UNLIKELY(!CheckPosixMemalignAlignment(alignment))) {
    if (AllocatorMayReturnNull())
      return errno_EINVAL;
    ReportInvalidPosixMemalignAlignment(alignment, stack);
  }
  void *ptr = HwasanAllocate(stack, size, alignment, false);
  if (UNLIKELY(!ptr))
    // OOM error is already taken care of by HwasanAllocate.
    return errno_ENOMEM;
  CHECK(IsAligned((uptr)ptr, alignment));
  *memptr = ptr;
  return 0;
}

void hwasan_free(void *ptr, StackTrace *stack) {
  return HwasanDeallocate(stack, ptr);
}

}  // namespace __hwasan

using namespace __hwasan;

void __hwasan_enable_allocator_tagging() {
  atomic_store_relaxed(&hwasan_allocator_tagging_enabled, 1);
}

void __hwasan_disable_allocator_tagging() {
  atomic_store_relaxed(&hwasan_allocator_tagging_enabled, 0);
}

uptr __sanitizer_get_current_allocated_bytes() {
  uptr stats[AllocatorStatCount];
  allocator.GetStats(stats);
  return stats[AllocatorStatAllocated];
}

uptr __sanitizer_get_heap_size() {
  uptr stats[AllocatorStatCount];
  allocator.GetStats(stats);
  return stats[AllocatorStatMapped];
}

uptr __sanitizer_get_free_bytes() { return 1; }

uptr __sanitizer_get_unmapped_bytes() { return 1; }

uptr __sanitizer_get_estimated_allocated_size(uptr size) { return size; }

int __sanitizer_get_ownership(const void *p) { return AllocationSize(p) != 0; }

uptr __sanitizer_get_allocated_size(const void *p) { return AllocationSize(p); }
