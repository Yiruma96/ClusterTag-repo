//===-- sanitizer_allocator_dlsym.h -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Hack: Sanitizer initializer calls dlsym which may need to allocate and call
// back into uninitialized sanitizer.
//
//===----------------------------------------------------------------------===//

#ifndef SANITIZER_ALLOCATOR_DLSYM_H
#define SANITIZER_ALLOCATOR_DLSYM_H

#include "sanitizer_allocator_internal.h"

namespace __sanitizer {

template <typename Details>
struct DlSymAllocator {
  static bool Use() {
    // Fuchsia doesn't use dlsym-based interceptors.
    return !SANITIZER_FUCHSIA && UNLIKELY(Details::UseImpl());
  }

  static bool PointerIsMine(const void *ptr) {
    // ClusterTag. Check if in our persistent allocator
    if ((uptr)ptr < ct_persistent_allocator.end_addr && (uptr)ptr >= ct_persistent_allocator.base_addr)
      return true;
    else
      return false;

    // Fuchsia doesn't use dlsym-based interceptors.
//    return !SANITIZER_FUCHSIA &&
//           UNLIKELY(internal_allocator()->FromPrimary(ptr));
  }

  static void *Allocate(uptr size_in_bytes) {
    // ClusterTag. Use our persistent allocator
    void* ptr = (void*)CTPersistentAllocatorAllocate(&ct_persistent_allocator, size_in_bytes);

//    void *ptr = InternalAlloc(size_in_bytes, nullptr, kWordSize);
//    CHECK(internal_allocator()->FromPrimary(ptr));
//    Details::OnAllocate(ptr, internal_allocator()->GetActuallyAllocatedSize(ptr));
    return ptr;
  }

  static void *Callocate(SIZE_T nmemb, SIZE_T size) {
    void *ptr = InternalCalloc(nmemb, size);
    CHECK(internal_allocator()->FromPrimary(ptr));
    Details::OnAllocate(ptr,
                        internal_allocator()->GetActuallyAllocatedSize(ptr));
    return ptr;
  }

  static void Free(void *ptr) {
    // ClusterTag. Use our persistent allocator
//    CTPersistentAllocatorFree(&ct_persistent_allocator, (uptr)ptr);

//    uptr size = internal_allocator()->GetActuallyAllocatedSize(ptr);
//    Details::OnFree(ptr, size);
//    InternalFree(ptr);
  }

  static void *Realloc(void *ptr, uptr new_size) {
    if (!ptr)
      return Allocate(new_size);
    CHECK(internal_allocator()->FromPrimary(ptr));
    if (!new_size) {
      Free(ptr);
      return nullptr;
    }
    uptr size = internal_allocator()->GetActuallyAllocatedSize(ptr);
    uptr memcpy_size = Min(new_size, size);
    void *new_ptr = Allocate(new_size);
    if (new_ptr)
      internal_memcpy(new_ptr, ptr, memcpy_size);
    Free(ptr);
    return new_ptr;
  }

  static void OnAllocate(const void *ptr, uptr size) {}
  static void OnFree(const void *ptr, uptr size) {}
};

}  // namespace __sanitizer

#endif  // SANITIZER_ALLOCATOR_DLSYM_H
