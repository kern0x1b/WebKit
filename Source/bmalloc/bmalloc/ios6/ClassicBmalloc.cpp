#include "../ClassicBmalloc.h"

#include "Cache.h"
#include "Heap.h"
#include "Object.h"
#include "ObjectType.h"
#include "PerProcess.h"
#include "StaticMutex.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace bmalloc {
namespace classic {

using namespace bmalloc_ios6;

static bool bmallocIsEnabled()
{
    std::unique_lock<StaticMutex> lock(PerProcess<Heap>::mutex());
    return PerProcess<Heap>::get()->environment().isBmallocEnabled();
}

static size_t sizeOfObject(void* object)
{
    if (!object)
        return 0;

    switch (objectType(object)) {
    case ObjectType::Small:
        return objectSize(Object(object).page()->sizeClass());
    case ObjectType::Large: {
        std::lock_guard<StaticMutex> lock(PerProcess<Heap>::mutex());
        return PerProcess<Heap>::getFastCase()->largeSize(lock, object);
    }
    }
    return 0;
}

void* tryMalloc(size_t size)
{
    return Cache::tryAllocate(size);
}

void* malloc(size_t size)
{
    return Cache::allocate(size);
}

void* tryZeroedMalloc(size_t size)
{
    void* result = Cache::tryAllocate(size);
    if (result)
        std::memset(result, 0, size);
    return result;
}

void* zeroedMalloc(size_t size)
{
    void* result = Cache::allocate(size);
    std::memset(result, 0, size);
    return result;
}

void* tryMemalign(size_t alignment, size_t size)
{
    return Cache::tryAllocate(alignment, size);
}

void* memalign(size_t alignment, size_t size)
{
    return Cache::allocate(alignment, size);
}

void* tryZeroedMemalign(size_t alignment, size_t size)
{
    void* result = Cache::tryAllocate(alignment, size);
    if (result)
        std::memset(result, 0, size);
    return result;
}

void* zeroedMemalign(size_t alignment, size_t size)
{
    void* result = Cache::allocate(alignment, size);
    std::memset(result, 0, size);
    return result;
}

void* realloc(void* object, size_t newSize)
{
    return Cache::reallocate(object, newSize);
}

void* tryRealloc(void* object, size_t newSize)
{
    if (!bmallocIsEnabled())
        return std::realloc(object, newSize);

    if (!object)
        return Cache::tryAllocate(newSize);

    size_t oldSize = sizeOfObject(object);
    void* result = Cache::tryAllocate(newSize);
    if (!result)
        return nullptr;

    std::memcpy(result, object, std::min(oldSize, newSize));
    Cache::deallocate(object);
    return result;
}

void free(void* object)
{
    Cache::deallocate(object);
}

size_t mallocSize(const void* object)
{
    if (!bmallocIsEnabled())
        return 0;
    return sizeOfObject(const_cast<void*>(object));
}

size_t mallocGoodSize(size_t size)
{
    if (!size)
        return 0;
    if (size <= smallMax)
        return objectSize(sizeClass(size));
    return size;
}

void scavengeThisThread()
{
    Cache::scavenge();
}

void scavenge()
{
    Cache::scavenge();

    std::unique_lock<StaticMutex> lock(PerProcess<Heap>::mutex());
    PerProcess<Heap>::get()->scavenge(lock, std::chrono::milliseconds(0));
}

bool isEnabled()
{
    return bmallocIsEnabled();
}

#if defined(__APPLE__)
void setScavengerThreadQOSClass(qos_class_t overrideClass)
{
    std::unique_lock<StaticMutex> lock(PerProcess<Heap>::mutex());
    PerProcess<Heap>::getFastCase()->setScavengerThreadQOSClass(overrideClass);
}
#endif

} // namespace classic
} // namespace bmalloc
