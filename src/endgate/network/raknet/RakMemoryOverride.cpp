#include "RakMemoryOverride.h"

#include <cstdlib>
#include <new>

void *(*rakMalloc_Ex)(size_t size, const char *file, unsigned int line) =
    [](size_t size, const char *, unsigned int) { return std::malloc(size); };

void *(*rakRealloc_Ex)(void *p, size_t size, const char *file, unsigned int line) =
    [](void *p, size_t size, const char *, unsigned int) { return std::realloc(p, size); };

void (*rakFree_Ex)(void *p, const char *file, unsigned int line) = [](void *p, const char *, unsigned int) {
    std::free(p);
};

void (*notifyOutOfMemory)(const char *file, const long line) = [](const char *, const long) {
    throw std::bad_alloc();
};
