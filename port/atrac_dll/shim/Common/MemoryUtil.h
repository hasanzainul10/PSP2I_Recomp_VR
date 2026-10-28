/* Stand-in for PPSSPP's Common/MemoryUtil.h: the aligned allocation the
 * decoder's av_malloc / av_free use. */
#pragma once
#include <cstddef>
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
inline void *AllocateAlignedMemory(size_t size, size_t align) { return _aligned_malloc(size ? size : 1, align); }
inline void FreeAlignedMemory(void *p) { _aligned_free(p); }
#elif defined(__3DS__)
#include <malloc.h>
/* newlib (devkitARM) has memalign but no posix_memalign */
inline void *AllocateAlignedMemory(size_t size, size_t align) { return memalign(align, size ? size : 1); }
inline void FreeAlignedMemory(void *p) { free(p); }
#else
inline void *AllocateAlignedMemory(size_t size, size_t align) {
    void *p = nullptr;
    return posix_memalign(&p, align, size ? size : 1) == 0 ? p : nullptr;
}
inline void FreeAlignedMemory(void *p) { free(p); }
#endif
