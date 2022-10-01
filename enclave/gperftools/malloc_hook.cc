// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2005, Google Inc.
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// 
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat <opensource@google.com>

#include <config.h>

// Disable the glibc prototype of mremap(), as older versions of the
// system headers define this function with only four arguments,
// whereas newer versions allow an optional fifth argument:
#ifdef HAVE_MMAP
# define mremap glibc_mremap
# include <sys/mman.h>
# undef mremap
#endif

#include <stddef.h>
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#include <algorithm>
#include "base/logging.h"
#include "base/spinlock.h"
#include "maybe_emergency_malloc.h"
#include "maybe_threads.h"
#include "malloc_hook-inl.h"
#include <gperftools/malloc_hook.h>

// This #ifdef should almost never be set.  Set NO_TCMALLOC_SAMPLES if
// you're porting to a system where you really can't get a stacktrace.
#ifdef NO_TCMALLOC_SAMPLES
  // We use #define so code compiles even if you #include stacktrace.h somehow.
# define GetStackTrace(stack, depth, skip)  (0)
#else
# include <gperftools/stacktrace.h>
#endif

// __THROW is defined in glibc systems.  It means, counter-intuitively,
// "This function will never throw an exception."  It's an optional
// optimization tool, but we may need to use it to match glibc prototypes.
#ifndef __THROW    // I guess we're not on a glibc system
# define __THROW   // __THROW is just an optimization, so ok to make it ""
#endif

using std::copy;


// Declaration of default weak initialization function, that can be overridden
// by linking-in a strong definition (as heap-checker.cc does).  This is
// extern "C" so that it doesn't trigger gold's --detect-odr-violations warning,
// which only looks at C++ symbols.
//
// This function is declared here as weak, and defined later, rather than a more
// straightforward simple weak definition, as a workround for an icc compiler
// issue ((Intel reference 290819).  This issue causes icc to resolve weak
// symbols too early, at compile rather than link time.  By declaring it (weak)
// here, then defining it below after its use, we can avoid the problem.
extern "C" {
ATTRIBUTE_WEAK void MallocHook_ocall_InitAtFirstAllocation_HeapLeakChecker();
}

namespace {

void RemoveInitialHooksAndCallInitializers();  // below.

pthread_once_t once = PTHREAD_ONCE_INIT;

// These hooks are installed in MallocHook_ocall as the only initial hooks.  The first
// hook that is called will run RemoveInitialHooksAndCallInitializers (see the
// definition below) and then redispatch to any malloc hooks installed by
// RemoveInitialHooksAndCallInitializers.
//
// Note(llib): there is a possibility of a race in the event that there are
// multiple threads running before the first allocation.  This is pretty
// difficult to achieve, but if it is then multiple threads may concurrently do
// allocations.  The first caller will call
// RemoveInitialHooksAndCallInitializers via one of the initial hooks.  A
// concurrent allocation may, depending on timing either:
// * still have its initial malloc hook installed, run that and block on waiting
//   for the first caller to finish its call to
//   RemoveInitialHooksAndCallInitializers, and proceed normally.
// * occur some time during the RemoveInitialHooksAndCallInitializers call, at
//   which point there could be no initial hooks and the subsequent hooks that
//   are about to be set up by RemoveInitialHooksAndCallInitializers haven't
//   been installed yet.  I think the worst we can get is that some allocations
//   will not get reported to some hooks set by the initializers called from
//   RemoveInitialHooksAndCallInitializers.

void InitialNewHook(const void* ptr, size_t size) {
  perftools_pthread_ocall_once(&once, &RemoveInitialHooksAndCallInitializers);
  MallocHook_ocall::InvokeNewHook(ptr, size);
}

void InitialPreMMapHook(const void* start,
                               size_t size,
                               int protection,
                               int flags,
                               int fd,
                               off_t offset) {
  perftools_pthread_ocall_once(&once, &RemoveInitialHooksAndCallInitializers);
  MallocHook_ocall::InvokePreMmapHook(start, size, protection, flags, fd, offset);
}

void InitialPreSbrkHook(ptrdiff_t increment) {
  perftools_pthread_ocall_once(&once, &RemoveInitialHooksAndCallInitializers);
  MallocHook_ocall::InvokePreSbrkHook(increment);
}

// This function is called at most once by one of the above initial malloc
// hooks.  It removes all initial hooks and initializes all other clients that
// want to get control at the very first memory allocation.  The initializers
// may assume that the initial malloc hooks have been removed.  The initializers
// may set up malloc hooks and allocate memory.
void RemoveInitialHooksAndCallInitializers() {
  RAW_CHECK(MallocHook_ocall::RemoveNewHook(&InitialNewHook), "");
  RAW_CHECK(MallocHook_ocall::RemovePreMmapHook(&InitialPreMMapHook), "");
  RAW_CHECK(MallocHook_ocall::RemovePreSbrkHook(&InitialPreSbrkHook), "");

  // HeapLeakChecker is currently the only module that needs to get control on
  // the first memory allocation, but one can add other modules by following the
  // same weak/strong function pattern.
  MallocHook_ocall_InitAtFirstAllocation_HeapLeakChecker();
}

}  // namespace

// Weak default initialization function that must go after its use.
extern "C" void MallocHook_ocall_InitAtFirstAllocation_HeapLeakChecker() {
  // Do nothing.
}

//namespace base { namespace internal {
namespace base_ocall { namespace internal {

// This lock is shared between all implementations of HookList::Add & Remove.
// The potential for contention is very small.  This needs to be a SpinLock and
// not a Mutex since it's possible for Mutex locking to allocate memory (e.g.,
// per-thread allocation in debug builds), which could cause infinite recursion.
static SpinLock_ocall hooklist_spinlock(base_ocall::LINKER_INITIALIZED);

template <typename T>
bool HookList<T>::Add(T value_as_t) {
  AtomicWord value = bit_cast<AtomicWord>(value_as_t);
  if (value == 0) {
    return false;
  }
  SpinLockHolder l(&hooklist_spinlock);
  // Find the first slot in data that is 0.
  int index = 0;
  while ((index < kHookListMaxValues) &&
         (base_ocall::subtle::NoBarrier_Load(&priv_data[index]) != 0)) {
    ++index;
  }
  if (index == kHookListMaxValues) {
    return false;
  }
  AtomicWord prev_num_hooks = base_ocall::subtle::Acquire_Load(&priv_end);
  base_ocall::subtle::NoBarrier_Store(&priv_data[index], value);
  if (prev_num_hooks <= index) {
    base_ocall::subtle::NoBarrier_Store(&priv_end, index + 1);
  }
  return true;
}

template <typename T>
void HookList<T>::FixupPrivEndLocked() {
  AtomicWord hooks_end = base_ocall::subtle::NoBarrier_Load(&priv_end);
  while ((hooks_end > 0) &&
         (base_ocall::subtle::NoBarrier_Load(&priv_data[hooks_end - 1]) == 0)) {
    --hooks_end;
  }
  base_ocall::subtle::NoBarrier_Store(&priv_end, hooks_end);
}

template <typename T>
bool HookList<T>::Remove(T value_as_t) {
  if (value_as_t == 0) {
    return false;
  }
  SpinLockHolder l(&hooklist_spinlock);
  AtomicWord hooks_end = base_ocall::subtle::NoBarrier_Load(&priv_end);
  int index = 0;
  while (index < hooks_end && value_as_t != bit_cast<T>(
             base_ocall::subtle::NoBarrier_Load(&priv_data[index]))) {
    ++index;
  }
  if (index == hooks_end) {
    return false;
  }
  base_ocall::subtle::NoBarrier_Store(&priv_data[index], 0);
  FixupPrivEndLocked();
  return true;
}

template <typename T>
int HookList<T>::Traverse(T* output_array, int n) const {
  AtomicWord hooks_end = base_ocall::subtle::Acquire_Load(&priv_end);
  int actual_hooks_end = 0;
  for (int i = 0; i < hooks_end && n > 0; ++i) {
    AtomicWord data = base_ocall::subtle::Acquire_Load(&priv_data[i]);
    if (data != 0) {
      *output_array++ = bit_cast<T>(data);
      ++actual_hooks_end;
      --n;
    }
  }
  return actual_hooks_end;
}

template <typename T>
T HookList<T>::ExchangeSingular(T value_as_t) {
  AtomicWord value = bit_cast<AtomicWord>(value_as_t);
  AtomicWord old_value;
  SpinLockHolder l(&hooklist_spinlock);
  old_value = base_ocall::subtle::NoBarrier_Load(&priv_data[kHookListSingularIdx]);
  base_ocall::subtle::NoBarrier_Store(&priv_data[kHookListSingularIdx], value);
  if (value != 0) {
    base_ocall::subtle::NoBarrier_Store(&priv_end, kHookListSingularIdx + 1);
  } else {
    FixupPrivEndLocked();
  }
  return bit_cast<T>(old_value);
}

// Initialize a HookList (optionally with the given initial_value in index 0).
#define INIT_HOOK_LIST { 0 }
#define INIT_HOOK_LIST_WITH_VALUE(initial_value)                \
  { 1, { reinterpret_cast<AtomicWord>(initial_value) } }

// Explicit instantiation for malloc_hook_test.cc.  This ensures all the methods
// are instantiated.
template struct HookList<MallocHook_ocall::NewHook>;

HookList<MallocHook_ocall::NewHook> new_hooks_ =
    INIT_HOOK_LIST_WITH_VALUE(&InitialNewHook);
HookList<MallocHook_ocall::DeleteHook> delete_hooks_ = INIT_HOOK_LIST;
HookList<MallocHook_ocall::PreMmapHook> premmap_hooks_ =
    INIT_HOOK_LIST_WITH_VALUE(&InitialPreMMapHook);
HookList<MallocHook_ocall::MmapHook> mmap_hooks_ = INIT_HOOK_LIST;
HookList<MallocHook_ocall::MunmapHook> munmap_hooks_ = INIT_HOOK_LIST;
HookList<MallocHook_ocall::MremapHook> mremap_hooks_ = INIT_HOOK_LIST;
HookList<MallocHook_ocall::PreSbrkHook> presbrk_hooks_ =
    INIT_HOOK_LIST_WITH_VALUE(InitialPreSbrkHook);
HookList<MallocHook_ocall::SbrkHook> sbrk_hooks_ = INIT_HOOK_LIST;

// These lists contain either 0 or 1 hooks.
HookList<MallocHook_ocall::MmapReplacement> mmap_replacement_ = { 0 };
HookList<MallocHook_ocall::MunmapReplacement> munmap_replacement_ = { 0 };

#undef INIT_HOOK_LIST_WITH_VALUE
#undef INIT_HOOK_LIST

} }  // namespace base_ocall::internal

using base_ocall::internal::kHookListMaxValues;
using base_ocall::internal::new_hooks_;
using base_ocall::internal::delete_hooks_;
using base_ocall::internal::premmap_hooks_;
using base_ocall::internal::mmap_hooks_;
using base_ocall::internal::mmap_replacement_;
using base_ocall::internal::munmap_hooks_;
using base_ocall::internal::munmap_replacement_;
using base_ocall::internal::mremap_hooks_;
using base_ocall::internal::presbrk_hooks_;
using base_ocall::internal::sbrk_hooks_;

// These are available as C bindings as well as C++, hence their
// definition outside the MallocHook_ocall class.
extern "C"
int MallocHook_ocall_AddNewHook(MallocHook_ocall_NewHook hook) {
  RAW_VLOG(10, "AddNewHook(%p)", hook);
  return new_hooks_.Add(hook);
}

extern "C"
int MallocHook_ocall_RemoveNewHook(MallocHook_ocall_NewHook hook) {
  RAW_VLOG(10, "RemoveNewHook(%p)", hook);
  return new_hooks_.Remove(hook);
}

extern "C"
int MallocHook_ocall_AddDeleteHook(MallocHook_ocall_DeleteHook hook) {
  RAW_VLOG(10, "AddDeleteHook(%p)", hook);
  return delete_hooks_.Add(hook);
}

extern "C"
int MallocHook_ocall_RemoveDeleteHook(MallocHook_ocall_DeleteHook hook) {
  RAW_VLOG(10, "RemoveDeleteHook(%p)", hook);
  return delete_hooks_.Remove(hook);
}

extern "C"
int MallocHook_ocall_AddPreMmapHook(MallocHook_ocall_PreMmapHook hook) {
  RAW_VLOG(10, "AddPreMmapHook(%p)", hook);
  return premmap_hooks_.Add(hook);
}

extern "C"
int MallocHook_ocall_RemovePreMmapHook(MallocHook_ocall_PreMmapHook hook) {
  RAW_VLOG(10, "RemovePreMmapHook(%p)", hook);
  return premmap_hooks_.Remove(hook);
}

extern "C"
int MallocHook_ocall_SetMmapReplacement(MallocHook_ocall_MmapReplacement hook) {
  RAW_VLOG(10, "SetMmapReplacement(%p)", hook);
  // NOTE this is a best effort CHECK. Concurrent sets could succeed since
  // this test is outside of the Add spin lock.
  RAW_CHECK(mmap_replacement_.empty(), "Only one MMapReplacement is allowed.");
  return mmap_replacement_.Add(hook);
}

extern "C"
int MallocHook_ocall_RemoveMmapReplacement(MallocHook_ocall_MmapReplacement hook) {
  RAW_VLOG(10, "RemoveMmapReplacement(%p)", hook);
  return mmap_replacement_.Remove(hook);
}

extern "C"
int MallocHook_ocall_AddMmapHook(MallocHook_ocall_MmapHook hook) {
  RAW_VLOG(10, "AddMmapHook(%p)", hook);
  return mmap_hooks_.Add(hook);
}

extern "C"
int MallocHook_ocall_RemoveMmapHook(MallocHook_ocall_MmapHook hook) {
  RAW_VLOG(10, "RemoveMmapHook(%p)", hook);
  return mmap_hooks_.Remove(hook);
}

extern "C"
int MallocHook_ocall_AddMunmapHook(MallocHook_ocall_MunmapHook hook) {
  RAW_VLOG(10, "AddMunmapHook(%p)", hook);
  return munmap_hooks_.Add(hook);
}

extern "C"
int MallocHook_ocall_RemoveMunmapHook(MallocHook_ocall_MunmapHook hook) {
  RAW_VLOG(10, "RemoveMunmapHook(%p)", hook);
  return munmap_hooks_.Remove(hook);
}

extern "C"
int MallocHook_ocall_SetMunmapReplacement(MallocHook_ocall_MunmapReplacement hook) {
  RAW_VLOG(10, "SetMunmapReplacement(%p)", hook);
  // NOTE this is a best effort CHECK. Concurrent sets could succeed since
  // this test is outside of the Add spin lock.
  RAW_CHECK(munmap_replacement_.empty(),
            "Only one MunmapReplacement is allowed.");
  return munmap_replacement_.Add(hook);
}

extern "C"
int MallocHook_ocall_RemoveMunmapReplacement(MallocHook_ocall_MunmapReplacement hook) {
  RAW_VLOG(10, "RemoveMunmapReplacement(%p)", hook);
  return munmap_replacement_.Remove(hook);
}

extern "C"
int MallocHook_ocall_AddMremapHook(MallocHook_ocall_MremapHook hook) {
  RAW_VLOG(10, "AddMremapHook(%p)", hook);
  return mremap_hooks_.Add(hook);
}

extern "C"
int MallocHook_ocall_RemoveMremapHook(MallocHook_ocall_MremapHook hook) {
  RAW_VLOG(10, "RemoveMremapHook(%p)", hook);
  return mremap_hooks_.Remove(hook);
}

extern "C"
int MallocHook_ocall_AddPreSbrkHook(MallocHook_ocall_PreSbrkHook hook) {
  RAW_VLOG(10, "AddPreSbrkHook(%p)", hook);
  return presbrk_hooks_.Add(hook);
}

extern "C"
int MallocHook_ocall_RemovePreSbrkHook(MallocHook_ocall_PreSbrkHook hook) {
  RAW_VLOG(10, "RemovePreSbrkHook(%p)", hook);
  return presbrk_hooks_.Remove(hook);
}

extern "C"
int MallocHook_ocall_AddSbrkHook(MallocHook_ocall_SbrkHook hook) {
  RAW_VLOG(10, "AddSbrkHook(%p)", hook);
  return sbrk_hooks_.Add(hook);
}

extern "C"
int MallocHook_ocall_RemoveSbrkHook(MallocHook_ocall_SbrkHook hook) {
  RAW_VLOG(10, "RemoveSbrkHook(%p)", hook);
  return sbrk_hooks_.Remove(hook);
}

// The code below is DEPRECATED.
extern "C"
MallocHook_ocall_NewHook MallocHook_ocall_SetNewHook(MallocHook_ocall_NewHook hook) {
  RAW_VLOG(10, "SetNewHook(%p)", hook);
  return new_hooks_.ExchangeSingular(hook);
}

extern "C"
MallocHook_ocall_DeleteHook MallocHook_ocall_SetDeleteHook(MallocHook_ocall_DeleteHook hook) {
  RAW_VLOG(10, "SetDeleteHook(%p)", hook);
  return delete_hooks_.ExchangeSingular(hook);
}

extern "C"
MallocHook_ocall_PreMmapHook MallocHook_ocall_SetPreMmapHook(MallocHook_ocall_PreMmapHook hook) {
  RAW_VLOG(10, "SetPreMmapHook(%p)", hook);
  return premmap_hooks_.ExchangeSingular(hook);
}

extern "C"
MallocHook_ocall_MmapHook MallocHook_ocall_SetMmapHook(MallocHook_ocall_MmapHook hook) {
  RAW_VLOG(10, "SetMmapHook(%p)", hook);
  return mmap_hooks_.ExchangeSingular(hook);
}

extern "C"
MallocHook_ocall_MunmapHook MallocHook_ocall_SetMunmapHook(MallocHook_ocall_MunmapHook hook) {
  RAW_VLOG(10, "SetMunmapHook(%p)", hook);
  return munmap_hooks_.ExchangeSingular(hook);
}

extern "C"
MallocHook_ocall_MremapHook MallocHook_ocall_SetMremapHook(MallocHook_ocall_MremapHook hook) {
  RAW_VLOG(10, "SetMremapHook(%p)", hook);
  return mremap_hooks_.ExchangeSingular(hook);
}

extern "C"
MallocHook_ocall_PreSbrkHook MallocHook_ocall_SetPreSbrkHook(MallocHook_ocall_PreSbrkHook hook) {
  RAW_VLOG(10, "SetPreSbrkHook(%p)", hook);
  return presbrk_hooks_.ExchangeSingular(hook);
}

extern "C"
MallocHook_ocall_SbrkHook MallocHook_ocall_SetSbrkHook(MallocHook_ocall_SbrkHook hook) {
  RAW_VLOG(10, "SetSbrkHook(%p)", hook);
  return sbrk_hooks_.ExchangeSingular(hook);
}
// End of DEPRECATED code section.

// Note: embedding the function calls inside the traversal of HookList would be
// very confusing, as it is legal for a hook to remove itself and add other
// hooks.  Doing traversal first, and then calling the hooks ensures we only
// call the hooks registered at the start.
#define INVOKE_HOOKS(HookType, hook_list, args) do {                    \
    HookType hooks[kHookListMaxValues];                                 \
    int num_hooks = hook_list.Traverse(hooks, kHookListMaxValues);      \
    for (int i = 0; i < num_hooks; ++i) {                               \
      (*hooks[i])args;                                                  \
    }                                                                   \
  } while (0)

// There should only be one replacement. Return the result of the first
// one, or false if there is none.
#define INVOKE_REPLACEMENT(HookType, hook_list, args) do {              \
    HookType hooks[kHookListMaxValues];                                 \
    int num_hooks = hook_list.Traverse(hooks, kHookListMaxValues);      \
    return (num_hooks > 0 && (*hooks[0])args);                          \
  } while (0)


void MallocHook_ocall::InvokeNewHookSlow(const void* p, size_t s) {
  if (tcmalloc_ocall::IsEmergencyPtr(p)) {
    return;
  }
  INVOKE_HOOKS(NewHook, new_hooks_, (p, s));
}

void MallocHook_ocall::InvokeDeleteHookSlow(const void* p) {
  if (tcmalloc_ocall::IsEmergencyPtr(p)) {
    return;
  }
  INVOKE_HOOKS(DeleteHook, delete_hooks_, (p));
}

void MallocHook_ocall::InvokePreMmapHookSlow(const void* start,
                                       size_t size,
                                       int protection,
                                       int flags,
                                       int fd,
                                       off_t offset) {
  INVOKE_HOOKS(PreMmapHook, premmap_hooks_, (start, size, protection, flags, fd,
                                            offset));
}

void MallocHook_ocall::InvokeMmapHookSlow(const void* result,
                                    const void* start,
                                    size_t size,
                                    int protection,
                                    int flags,
                                    int fd,
                                    off_t offset) {
  INVOKE_HOOKS(MmapHook, mmap_hooks_, (result, start, size, protection, flags,
                                       fd, offset));
}

bool MallocHook_ocall::InvokeMmapReplacementSlow(const void* start,
                                           size_t size,
                                           int protection,
                                           int flags,
                                           int fd,
                                           off_t offset,
                                           void** result) {
  INVOKE_REPLACEMENT(MmapReplacement, mmap_replacement_,
                      (start, size, protection, flags, fd, offset, result));
}

void MallocHook_ocall::InvokeMunmapHookSlow(const void* p, size_t s) {
  INVOKE_HOOKS(MunmapHook, munmap_hooks_, (p, s));
}

bool MallocHook_ocall::InvokeMunmapReplacementSlow(const void* p,
                                             size_t s,
                                             int* result) {
  INVOKE_REPLACEMENT(MunmapReplacement, munmap_replacement_, (p, s, result));
}

void MallocHook_ocall::InvokeMremapHookSlow(const void* result,
                                      const void* old_addr,
                                      size_t old_size,
                                      size_t new_size,
                                      int flags,
                                      const void* new_addr) {
  INVOKE_HOOKS(MremapHook, mremap_hooks_, (result, old_addr, old_size, new_size,
                                           flags, new_addr));
}

void MallocHook_ocall::InvokePreSbrkHookSlow(ptrdiff_t increment) {
  INVOKE_HOOKS(PreSbrkHook, presbrk_hooks_, (increment));
}

void MallocHook_ocall::InvokeSbrkHookSlow(const void* result, ptrdiff_t increment) {
  INVOKE_HOOKS(SbrkHook, sbrk_hooks_, (result, increment));
}

#undef INVOKE_HOOKS

#ifndef NO_TCMALLOC_SAMPLES

DEFINE_ATTRIBUTE_SECTION_VARS(google_malloc);
DECLARE_ATTRIBUTE_SECTION_VARS(google_malloc);
  // actual functions are in debugallocation.cc or tcmalloc.cc
DEFINE_ATTRIBUTE_SECTION_VARS(malloc_hook);
DECLARE_ATTRIBUTE_SECTION_VARS(malloc_hook);
  // actual functions are in this file, malloc_hook.cc, and low_level_alloc.cc

#define ADDR_IN_ATTRIBUTE_SECTION(addr, name) \
  (reinterpret_cast<uintptr_t>(ATTRIBUTE_SECTION_START(name)) <= \
     reinterpret_cast<uintptr_t>(addr) && \
   reinterpret_cast<uintptr_t>(addr) < \
     reinterpret_cast<uintptr_t>(ATTRIBUTE_SECTION_STOP(name)))

// Return true iff 'caller' is a return address within a function
// that calls one of our hooks via MallocHook_ocall:Invoke*.
// A helper for GetCallerStackTrace.
static inline bool InHookCaller(const void* caller) {
  return ADDR_IN_ATTRIBUTE_SECTION(caller, google_malloc) ||
         ADDR_IN_ATTRIBUTE_SECTION(caller, malloc_hook);
  // We can use one section for everything except tcmalloc_or_debug
  // due to its special linkage mode, which prevents merging of the sections.
}

#undef ADDR_IN_ATTRIBUTE_SECTION

static bool checked_sections = false;

static inline void CheckInHookCaller() {
  if (!checked_sections) {
    INIT_ATTRIBUTE_SECTION_VARS(google_malloc);
    if (ATTRIBUTE_SECTION_START(google_malloc) ==
        ATTRIBUTE_SECTION_STOP(google_malloc)) {
      RAW_LOG(ERROR, "google_malloc section is missing, "
                     "thus InHookCaller is broken!");
    }
    INIT_ATTRIBUTE_SECTION_VARS(malloc_hook);
    if (ATTRIBUTE_SECTION_START(malloc_hook) ==
        ATTRIBUTE_SECTION_STOP(malloc_hook)) {
      RAW_LOG(ERROR, "malloc_hook section is missing, "
                     "thus InHookCaller is broken!");
    }
    checked_sections = true;
  }
}

#endif // !NO_TCMALLOC_SAMPLES

// We can improve behavior/compactness of this function
// if we pass a generic test function (with a generic arg)
// into the implementations for GetStackTrace instead of the skip_count.
extern "C" int MallocHook_ocall_GetCallerStackTrace(void** result, int max_depth,
                                              int skip_count) {
#if defined(NO_TCMALLOC_SAMPLES)
  return 0;
#elif !defined(HAVE_ATTRIBUTE_SECTION_START)
  // Fall back to GetStackTrace and good old but fragile frame skip counts.
  // Note: this path is inaccurate when a hook is not called directly by an
  // allocation function but is daisy-chained through another hook,
  // search for MallocHook_ocall::(Get|Set|Invoke)* to find such cases.
  return GetStackTrace(result, max_depth, skip_count + int(DEBUG_MODE));
  // due to -foptimize-sibling-calls in opt mode
  // there's no need for extra frame skip here then
#else
  CheckInHookCaller();
  // MallocHook_ocall caller determination via InHookCaller works, use it:
  static const int kMaxSkip = 32 + 6 + 3;
    // Constant tuned to do just one GetStackTrace call below in practice
    // and not get many frames that we don't actually need:
    // currently max passsed max_depth is 32,
    // max passed/needed skip_count is 6
    // and 3 is to account for some hook daisy chaining.
  static const int kStackSize = kMaxSkip + 1;
  void* stack[kStackSize];
  int depth = GetStackTrace(stack, kStackSize, 1);  // skip this function frame
  if (depth == 0)   // silenty propagate cases when GetStackTrace does not work
    return 0;
  for (int i = 0; i < depth; ++i) {  // stack[0] is our immediate caller
    if (InHookCaller(stack[i])) {
      RAW_VLOG(10, "Found hooked allocator at %d: %p <- %p",
                   i, stack[i], stack[i+1]);
      i += 1;  // skip hook caller frame
      depth -= i;  // correct depth
      if (depth > max_depth) depth = max_depth;
      copy(stack + i, stack + i + depth, result);
      if (depth < max_depth  &&  depth + i == kStackSize) {
        // get frames for the missing depth
        depth +=
          GetStackTrace(result + depth, max_depth - depth, 1 + kStackSize);
      }
      return depth;
    }
  }
  RAW_LOG(WARNING, "Hooked allocator frame not found, returning empty trace");
    // If this happens try increasing kMaxSkip
    // or else something must be wrong with InHookCaller,
    // e.g. for every section used in InHookCaller
    // all functions in that section must be inside the same library.
  return 0;
#endif
}

// On systems where we know how, we override mmap/munmap/mremap/sbrk
// to provide support for calling the related hooks (in addition,
// of course, to doing what these functions normally do).

#if defined(__linux)
#ifndef TCMALLOC_SGX   /*SGX don't support mmap*/
# include "malloc_hook_mmap_linux.h"
#endif

#elif defined(__FreeBSD__)
# include "malloc_hook_mmap_freebsd.h"

#else

/*static*/void* MallocHook_ocall::UnhookedMMap(void *start, size_t length, int prot,
                                         int flags, int fd, off_t offset) {
  void* result;
  if (!MallocHook_ocall::InvokeMmapReplacement(
          start, length, prot, flags, fd, offset, &result)) {
    result = mmap(start, length, prot, flags, fd, offset);
  }
  return result;
}

/*static*/int MallocHook_ocall::UnhookedMUnmap(void *start, size_t length) {
  int result;
  if (!MallocHook_ocall::InvokeMunmapReplacement(start, length, &result)) {
    result = munmap(start, length);
  }
  return result;
}

#endif