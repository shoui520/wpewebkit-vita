/*
 * Copyright (C) 2026 Vita port contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/OSAllocator.h>

#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <kubridge.h>
#include <limits>
#include <psp2/kernel/sysmem.h>
#include <wtf/Assertions.h>
#include <wtf/Lock.h>
#include <wtf/PageBlock.h>

namespace WTF {

#if !USE(SYSTEM_MALLOC)
#error Vita page allocation requires the system malloc backend.
#endif

// Reservations are physically backed for their whole lifetime. Logical commit
// and decommit neither change page tables nor return memory to the system.
// Keep the original native UID: a pointer lookup must not create another handle.
struct Allocation {
    void* base;
    size_t requestedSize;
    SceUID uid;
    bool writable;
    bool executable;
    Allocation* next;
};

static Lock allocationLock;
static Allocation* allocations WTF_GUARDED_BY_LOCK(allocationLock);

static void* tryAllocate(size_t bytes, size_t alignment, void* address, bool writable, bool executable, bool jitCageEnabled, unsigned guardPages)
{
    if (address || !writable || jitCageEnabled || guardPages) {
        errno = ENOTSUP;
        return nullptr;
    }
    if (!bytes || alignment < pageSize() || (alignment & (alignment - 1)) || bytes > std::numeric_limits<SceSize>::max() - (pageSize() - 1)) {
        errno = EINVAL;
        return nullptr;
    }
    size_t mappedSize = (bytes + pageSize() - 1) & ~(pageSize() - 1);
    auto* allocation = static_cast<Allocation*>(std::malloc(sizeof(Allocation)));
    if (!allocation) {
        errno = ENOMEM;
        return nullptr;
    }
    SceKernelAllocMemBlockOpt options { };
    options.size = sizeof(options);
    options.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    options.alignment = alignment;
    SceUID uid = sceKernelAllocMemBlock(executable ? "WTFJIT" : "WTFPages", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, mappedSize, &options);
    if (uid < 0) {
        std::free(allocation);
        errno = ENOMEM;
        return nullptr;
    }
    void* base = nullptr;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        RELEASE_ASSERT(!sceKernelFreeMemBlock(uid));
        std::free(allocation);
        errno = ENOMEM;
        return nullptr;
    }
    RELEASE_ASSERT(!(reinterpret_cast<uintptr_t>(base) & (alignment - 1)));
    std::memset(base, 0, mappedSize);
    // JSC's ARM32 repatching writes aligned instruction words directly. Change
    // the complete block once so those stores retain their atomic semantics.
    if (executable && kuKernelMemProtect(base, mappedSize, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_EXEC)) {
        RELEASE_ASSERT(!sceKernelFreeMemBlock(uid));
        std::free(allocation);
        errno = ENOTSUP;
        return nullptr;
    }
    Locker locker { allocationLock };
    *allocation = { base, bytes, uid, writable, executable, allocations };
    allocations = allocation;
    return base;
}

void* OSAllocator::tryReserveAndCommit(size_t bytes, Usage, void* address, bool writable, bool executable, bool jitCageEnabled, unsigned guardPages)
{
    return tryAllocate(bytes, pageSize(), address, writable, executable, jitCageEnabled, guardPages);
}

void* OSAllocator::tryReserveUncommitted(size_t bytes, Usage usage, void* address, bool writable, bool executable, bool jitCageEnabled, unsigned guardPages)
{
    return tryReserveAndCommit(bytes, usage, address, writable, executable, jitCageEnabled, guardPages);
}

void* OSAllocator::tryReserveUncommittedAligned(size_t bytes, size_t alignment, Usage, void* address, bool writable, bool executable, bool jitCageEnabled, unsigned guardPages)
{
    return tryAllocate(bytes, alignment, address, writable, executable, jitCageEnabled, guardPages);
}

void* OSAllocator::reserveAndCommit(size_t bytes, Usage usage, void* address, bool writable, bool executable, bool jitCageEnabled, unsigned guardPages)
{
    void* result = tryReserveAndCommit(bytes, usage, address, writable, executable, jitCageEnabled, guardPages);
    RELEASE_ASSERT(result);
    return result;
}

void* OSAllocator::reserveUncommitted(size_t bytes, Usage usage, void* address, bool writable, bool executable, bool jitCageEnabled, unsigned guardPages)
{
    void* result = tryReserveUncommitted(bytes, usage, address, writable, executable, jitCageEnabled, guardPages);
    RELEASE_ASSERT(result);
    return result;
}

void OSAllocator::commit(void* address, size_t bytes, bool writable, bool executable)
{
    // fastCommitAlignedMemory also supplies runtime-heap allocations here.
    RELEASE_ASSERT(!bytes || (address && isPageAligned(address) && isPageAligned(bytes)));
    RELEASE_ASSERT(writable);
    if (!executable || !bytes)
        return;
    auto start = reinterpret_cast<uintptr_t>(address);
    Locker locker { allocationLock };
    for (auto* allocation = allocations; allocation; allocation = allocation->next) {
        auto base = reinterpret_cast<uintptr_t>(allocation->base);
        if (start >= base && start - base < allocation->requestedSize && bytes <= allocation->requestedSize - (start - base)) {
            RELEASE_ASSERT(allocation->executable);
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void OSAllocator::decommit(void* address, size_t bytes)
{
    // Content and physical backing remain until the complete allocation is freed.
    RELEASE_ASSERT(!bytes || (address && isPageAligned(address) && isPageAligned(bytes)));
}

void OSAllocator::hintMemoryNotNeededSoon(void*, size_t)
{
}

void OSAllocator::releaseDecommitted(void* address, size_t bytes, unsigned guardPages)
{
    RELEASE_ASSERT(!guardPages);
    Locker locker { allocationLock };
    for (auto** link = &allocations; *link; link = &(*link)->next) {
        auto* allocation = *link;
        if (allocation->base != address)
            continue;
        // Only the complete original reservation can release its native block.
        RELEASE_ASSERT(allocation->requestedSize == bytes);
        RELEASE_ASSERT(!sceKernelFreeMemBlock(allocation->uid));
        *link = allocation->next;
        std::free(allocation);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool OSAllocator::tryProtect(void* address, size_t bytes, bool readable, bool writable)
{
    if (!bytes)
        return true;
    // Permission changes and subrange splitting require a separate validated
    // backend. Never report success for an unsupported protection request.
    if (!readable || !isPageAligned(address) || !isPageAligned(bytes)) {
        errno = ENOTSUP;
        return false;
    }
    auto start = reinterpret_cast<uintptr_t>(address);
    Locker locker { allocationLock };
    for (auto* allocation = allocations; allocation; allocation = allocation->next) {
        auto base = reinterpret_cast<uintptr_t>(allocation->base);
        if (start >= base && start - base < allocation->requestedSize && bytes <= allocation->requestedSize - (start - base) && writable == allocation->writable)
            return true;
    }
    errno = ENOTSUP;
    return false;
}

void OSAllocator::protect(void* address, size_t bytes, bool readable, bool writable)
{
    RELEASE_ASSERT(tryProtect(address, bytes, readable, writable));
}

} // namespace WTF
