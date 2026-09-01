#pragma once

#include <stddef.h>

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace bmalloc {
namespace classic {

void* tryMalloc(size_t);
void* malloc(size_t);
void* tryZeroedMalloc(size_t);
void* zeroedMalloc(size_t);
void* tryMemalign(size_t alignment, size_t);
void* memalign(size_t alignment, size_t);
void* tryZeroedMemalign(size_t alignment, size_t);
void* zeroedMemalign(size_t alignment, size_t);
void* tryRealloc(void*, size_t);
void* realloc(void*, size_t);
void free(void*);

size_t mallocSize(const void*);
size_t mallocGoodSize(size_t);

void scavengeThisThread();
void scavenge();
bool isEnabled();

#if defined(__APPLE__)
void setScavengerThreadQOSClass(qos_class_t);
#endif

} // namespace classic
} // namespace bmalloc
