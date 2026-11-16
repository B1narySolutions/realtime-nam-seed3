#pragma once
#include <cstddef>
#include <cstdlib>
#include <new>

// Include in one translation unit only. Track C++ allocations used by vectors
// and model construction; the firmware processing path uses no C allocators.
// Volatile because GCC 16 may assume operator new leaves globals alone and
// reuse a count read before the call; volatile reads always reload.
static volatile std::size_t allocation_count = 0;

// GCC inlines these replacements, then sees free() on memory that came from
// operator new and reports -Wmismatched-new-delete on every vector. The
// pairs here do match; clang's warning of the same name is a real check.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

// Every form of operator new below counts through this one function.
static void *CountedAllocate(std::size_t size, std::size_t alignment) {
    allocation_count = allocation_count + 1;

    // aligned_alloc needs the size to be a nonzero multiple of the alignment.
    void *p = alignment <= alignof(std::max_align_t) ? std::malloc(size ? size : 1) : std::aligned_alloc(alignment, (size ? size + alignment - 1 : alignment) / alignment * alignment);
    if (!p)
        throw std::bad_alloc();
    return p;
}
void *operator new(std::size_t size) { return CountedAllocate(size, 0); }
void *operator new[](std::size_t size) { return CountedAllocate(size, 0); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

// Over-aligned types (alignas above 16) use these instead. The standard
// library sends them straight to aligned_alloc, so without them an aligned
// buffer allocated while processing would go uncounted.
void *operator new(std::size_t size, std::align_val_t alignment) { return CountedAllocate(size, static_cast<std::size_t>(alignment)); }
void *operator new[](std::size_t size, std::align_val_t alignment) { return CountedAllocate(size, static_cast<std::size_t>(alignment)); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }

// libc++ doesn't route new (std::nothrow) through a replaced operator new, so
// count those here too.
static void *CountedAllocateOrNull(std::size_t size, std::size_t alignment) noexcept {
    try {
        return CountedAllocate(size, alignment);
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
}
void *operator new(std::size_t size, const std::nothrow_t &) noexcept { return CountedAllocateOrNull(size, 0); }
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept { return CountedAllocateOrNull(size, 0); }
void *operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept { return CountedAllocateOrNull(size, static_cast<std::size_t>(alignment)); }
void *operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept { return CountedAllocateOrNull(size, static_cast<std::size_t>(alignment)); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { std::free(p); }
