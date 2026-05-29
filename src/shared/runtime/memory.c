#include "runtime.h"
#include "platform.h"

#include <stdint.h>

#define NEWOS_RUNTIME_ALLOC_LOCK_NONE 0
#define NEWOS_RUNTIME_ALLOC_LOCK_ATOMIC 1
#define NEWOS_RUNTIME_ALLOC_LOCK_PTHREAD 2

#ifndef NEWOS_RUNTIME_ALLOC_LOCK
#if defined(NEWOS_RUNTIME_THREAD_SAFE_ALLOC) && NEWOS_RUNTIME_THREAD_SAFE_ALLOC && defined(NEWOS_HAVE_PTHREAD) && NEWOS_HAVE_PTHREAD
#define NEWOS_RUNTIME_ALLOC_LOCK NEWOS_RUNTIME_ALLOC_LOCK_PTHREAD
#elif defined(NEWOS_RUNTIME_THREAD_SAFE_ALLOC) && NEWOS_RUNTIME_THREAD_SAFE_ALLOC
#define NEWOS_RUNTIME_ALLOC_LOCK NEWOS_RUNTIME_ALLOC_LOCK_ATOMIC
#else
#define NEWOS_RUNTIME_ALLOC_LOCK NEWOS_RUNTIME_ALLOC_LOCK_NONE
#endif
#endif

#if NEWOS_RUNTIME_ALLOC_LOCK == NEWOS_RUNTIME_ALLOC_LOCK_PTHREAD && defined(NEWOS_HAVE_PTHREAD) && NEWOS_HAVE_PTHREAD
#include <pthread.h>
#define RT_ALLOC_LOCK_PTHREAD 1
#else
#define RT_ALLOC_LOCK_PTHREAD 0
#endif

#define RT_ALIGN ((size_t)16U)
#define RT_SMALL_CLASS_COUNT 8U
#define RT_SMALL_MAX_SIZE ((size_t)2048U)
#define RT_SMALL_MAGIC 0x534d414cU
#define RT_LARGE_MAGIC 0x4c415247U
#define RT_ARENA_DEFAULT_BLOCK_SIZE ((size_t)16384U)

#ifndef RT_LARGE_CACHE_MAX_BYTES
#define RT_LARGE_CACHE_MAX_BYTES 8388608U
#endif

#ifndef RT_LARGE_CACHE_MAX_COUNT
#define RT_LARGE_CACHE_MAX_COUNT 32U
#endif

typedef struct RtSmallBlock RtSmallBlock;
typedef struct RtLargeBlock RtLargeBlock;

struct RtSmallBlock {
    RtSmallBlock *next;
    size_t requested;
    size_t reserved;
    unsigned int class_index;
    unsigned int magic;
};

struct RtLargeBlock {
    RtLargeBlock *next;
    size_t requested;
    size_t mapping_size;
    unsigned int class_index;
    unsigned int magic;
};

struct RtArenaBlock {
    RtArenaBlock *next;
    size_t used;
    size_t size;
    size_t mapping_size;
    unsigned char data[];
};

static const size_t rt_small_classes[RT_SMALL_CLASS_COUNT] = {
    16U, 32U, 64U, 128U, 256U, 512U, 1024U, 2048U
};

static RtSmallBlock *rt_small_free[RT_SMALL_CLASS_COUNT];
static RtLargeBlock *rt_large_cache;
static size_t rt_large_cache_bytes;
static size_t rt_large_cache_count;

#if RT_ALLOC_LOCK_PTHREAD
static pthread_mutex_t rt_alloc_mutex = PTHREAD_MUTEX_INITIALIZER;

static void rt_alloc_lock(void) {
    (void)pthread_mutex_lock(&rt_alloc_mutex);
}

static void rt_alloc_unlock(void) {
    (void)pthread_mutex_unlock(&rt_alloc_mutex);
}
#elif NEWOS_RUNTIME_ALLOC_LOCK == NEWOS_RUNTIME_ALLOC_LOCK_ATOMIC
static volatile int rt_alloc_atomic_lock;

static void rt_alloc_lock(void) {
    while (__atomic_exchange_n(&rt_alloc_atomic_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        while (__atomic_load_n(&rt_alloc_atomic_lock, __ATOMIC_RELAXED) != 0) {
        }
    }
}

static void rt_alloc_unlock(void) {
    __atomic_store_n(&rt_alloc_atomic_lock, 0, __ATOMIC_RELEASE);
}
#else
static void rt_alloc_lock(void) {
}

static void rt_alloc_unlock(void) {
}
#endif

static size_t rt_align_up(size_t value, size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static size_t rt_page_align(size_t size) {
    return rt_align_up(size, platform_page_size());
}

static int rt_mul_overflow(size_t left, size_t right, size_t *out) {
    if (left != 0U && right > ((size_t)-1) / left) return 1;
    *out = left * right;
    return 0;
}

static unsigned int rt_class_for_size(size_t size) {
    unsigned int class_index;
    for (class_index = 0U; class_index < RT_SMALL_CLASS_COUNT; ++class_index) {
        if (size <= rt_small_classes[class_index]) return class_index;
    }
    return RT_SMALL_CLASS_COUNT;
}

static int rt_fill_small_class(unsigned int class_index) {
    size_t payload_size = rt_small_classes[class_index];
    size_t block_size = sizeof(RtSmallBlock) + payload_size;
    size_t mapping_size = rt_page_align(block_size * 64U);
    unsigned char *cursor;
    unsigned char *end;

    if (mapping_size < block_size) return 0;
    cursor = (unsigned char *)platform_allocate_pages(mapping_size);
    if (cursor == 0) return 0;
    end = cursor + mapping_size;

    while (cursor + block_size <= end) {
        RtSmallBlock *block = (RtSmallBlock *)cursor;
        block->requested = 0U;
        block->reserved = 0U;
        block->class_index = class_index;
        block->magic = RT_SMALL_MAGIC;
        block->next = rt_small_free[class_index];
        rt_small_free[class_index] = block;
        cursor += block_size;
    }

    return 1;
}

static RtLargeBlock *rt_large_cache_take(size_t mapping_size) {
    RtLargeBlock *best = 0;
    RtLargeBlock *best_prev = 0;
    RtLargeBlock *prev = 0;
    RtLargeBlock *block = rt_large_cache;

    while (block != 0) {
        if (block->mapping_size >= mapping_size && (best == 0 || block->mapping_size < best->mapping_size)) {
            best = block;
            best_prev = prev;
        }
        prev = block;
        block = block->next;
    }

    if (best == 0) return 0;
    if (best_prev == 0) rt_large_cache = best->next;
    else best_prev->next = best->next;
    rt_large_cache_bytes -= best->mapping_size;
    --rt_large_cache_count;
    best->next = 0;
    return best;
}

static int rt_large_cache_put(RtLargeBlock *block) {
#if RT_LARGE_CACHE_MAX_BYTES > 0U && RT_LARGE_CACHE_MAX_COUNT > 0U
    if (block->mapping_size > RT_LARGE_CACHE_MAX_BYTES) return 0;
    while ((rt_large_cache_bytes + block->mapping_size > RT_LARGE_CACHE_MAX_BYTES || rt_large_cache_count >= RT_LARGE_CACHE_MAX_COUNT) && rt_large_cache != 0) {
        RtLargeBlock *evicted = rt_large_cache;
        rt_large_cache = evicted->next;
        rt_large_cache_bytes -= evicted->mapping_size;
        --rt_large_cache_count;
        (void)platform_free_pages(evicted, evicted->mapping_size);
    }
    if (rt_large_cache_bytes + block->mapping_size > RT_LARGE_CACHE_MAX_BYTES || rt_large_cache_count >= RT_LARGE_CACHE_MAX_COUNT) return 0;
    block->requested = 0U;
    block->next = rt_large_cache;
    rt_large_cache = block;
    rt_large_cache_bytes += block->mapping_size;
    ++rt_large_cache_count;
    return 1;
#else
    (void)block;
    return 0;
#endif
}

void *memcpy(void *dst, const void *src, size_t count) {
    volatile unsigned char *out = (volatile unsigned char *)dst;
    const volatile unsigned char *in = (const volatile unsigned char *)src;

    if ((((uintptr_t)out ^ (uintptr_t)in) & (sizeof(size_t) - 1U)) == 0U) {
        while (count != 0U && ((uintptr_t)out & (sizeof(size_t) - 1U)) != 0U) {
            *out++ = *in++;
            --count;
        }
        while (count >= sizeof(size_t)) {
            *(volatile size_t *)out = *(const volatile size_t *)in;
            out += sizeof(size_t);
            in += sizeof(size_t);
            count -= sizeof(size_t);
        }
    }

    while (count != 0U) {
        *out++ = *in++;
        --count;
    }

    return dst;
}

int memcmp(const void *left, const void *right, size_t count) {
    const unsigned char *left_bytes = (const unsigned char *)left;
    const unsigned char *right_bytes = (const unsigned char *)right;
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (left_bytes[index] != right_bytes[index]) {
            return (int)left_bytes[index] - (int)right_bytes[index];
        }
    }
    return 0;
}

void *memset(void *buffer, int byte_value, size_t count) {
    volatile unsigned char *out = (volatile unsigned char *)buffer;
    unsigned char value = (unsigned char)byte_value;
    size_t word = 0U;
    size_t index;

    for (index = 0U; index < sizeof(size_t); ++index) {
        word = (word << 8U) | value;
    }

    while (count != 0U && ((uintptr_t)out & (sizeof(size_t) - 1U)) != 0U) {
        *out++ = value;
        --count;
    }
    while (count >= sizeof(size_t)) {
        *(volatile size_t *)out = word;
        out += sizeof(size_t);
        count -= sizeof(size_t);
    }
    while (count != 0U) {
        *out++ = value;
        --count;
    }

    return buffer;
}

void *memmove(void *dst, const void *src, size_t count) {
    volatile unsigned char *out = (volatile unsigned char *)dst;
    const volatile unsigned char *in = (const volatile unsigned char *)src;

    if (out == in || count == 0U) return dst;
    if (out < in || out >= in + count) return memcpy(dst, src, count);

    out += count;
    in += count;
    while (count != 0U) {
        *--out = *--in;
        --count;
    }

    return dst;
}

void rt_memset(void *buffer, int byte_value, size_t count) {
    (void)memset(buffer, byte_value, count);
}

static void *rt_malloc_unlocked(size_t size) {
    unsigned int class_index;

    if (size == 0U) return 0;
    class_index = rt_class_for_size(size);
    if (class_index < RT_SMALL_CLASS_COUNT) {
        RtSmallBlock *block;
        if (rt_small_free[class_index] == 0 && !rt_fill_small_class(class_index)) return 0;
        block = rt_small_free[class_index];
        rt_small_free[class_index] = block->next;
        block->next = 0;
        block->requested = size;
        return (void *)(block + 1);
    }

    if (size > ((size_t)-1) - sizeof(RtLargeBlock)) return 0;
    {
        size_t mapping_size = rt_page_align(sizeof(RtLargeBlock) + size);
        RtLargeBlock *block;
        if (mapping_size < size) return 0;
        block = rt_large_cache_take(mapping_size);
        if (block == 0) block = (RtLargeBlock *)platform_allocate_pages(mapping_size);
        if (block == 0) return 0;
        block->next = 0;
        block->requested = size;
        if (block->mapping_size == 0U) block->mapping_size = mapping_size;
        block->class_index = RT_SMALL_CLASS_COUNT;
        block->magic = RT_LARGE_MAGIC;
        return (void *)(block + 1);
    }
}

void *rt_malloc(size_t size) {
    void *ptr;

    rt_alloc_lock();
    ptr = rt_malloc_unlocked(size);
    rt_alloc_unlock();
    return ptr;
}

static void rt_free_unlocked(void *ptr) {
    RtSmallBlock *small;
    RtLargeBlock *large;

    if (ptr == 0) return;
    small = ((RtSmallBlock *)ptr) - 1;
    if (small->magic == RT_SMALL_MAGIC && small->class_index < RT_SMALL_CLASS_COUNT) {
        unsigned int class_index = small->class_index;
        small->requested = 0U;
        small->next = rt_small_free[class_index];
        rt_small_free[class_index] = small;
        return;
    }

    large = ((RtLargeBlock *)ptr) - 1;
    if (large->magic == RT_LARGE_MAGIC) {
        if (!rt_large_cache_put(large)) (void)platform_free_pages(large, large->mapping_size);
    }
}

void rt_free(void *ptr) {
    rt_alloc_lock();
    rt_free_unlocked(ptr);
    rt_alloc_unlock();
}

void *rt_realloc(void *ptr, size_t size) {
    RtSmallBlock *small;
    RtLargeBlock *large;
    size_t old_size;
    void *next;

    if (ptr == 0) return rt_malloc(size);
    if (size == 0U) {
        rt_free(ptr);
        return 0;
    }

    rt_alloc_lock();
    small = ((RtSmallBlock *)ptr) - 1;
    if (small->magic == RT_SMALL_MAGIC && small->class_index < RT_SMALL_CLASS_COUNT) {
        old_size = small->requested;
        if (size <= rt_small_classes[small->class_index]) {
            small->requested = size;
            rt_alloc_unlock();
            return ptr;
        }
    } else {
        large = ((RtLargeBlock *)ptr) - 1;
        if (large->magic != RT_LARGE_MAGIC) {
            rt_alloc_unlock();
            return 0;
        }
        old_size = large->requested;
        if (size > RT_SMALL_MAX_SIZE && size <= large->mapping_size - sizeof(RtLargeBlock)) {
            large->requested = size;
            rt_alloc_unlock();
            return ptr;
        }
    }

    next = rt_malloc_unlocked(size);
    if (next == 0) {
        rt_alloc_unlock();
        return 0;
    }
    memcpy(next, ptr, old_size < size ? old_size : size);
    rt_free_unlocked(ptr);
    rt_alloc_unlock();
    return next;
}

void *rt_malloc_array(size_t count, size_t item_size) {
    size_t size;
    if (rt_mul_overflow(count, item_size, &size)) return 0;
    return rt_malloc(size);
}

void *rt_realloc_array(void *ptr, size_t count, size_t item_size) {
    size_t size;
    if (rt_mul_overflow(count, item_size, &size)) return 0;
    return rt_realloc(ptr, size);
}

void rt_arena_init(RtArena *arena, size_t default_block_size) {
    if (arena == 0) return;
    arena->blocks = 0;
    arena->default_block_size = default_block_size == 0U ? RT_ARENA_DEFAULT_BLOCK_SIZE : rt_align_up(default_block_size, RT_ALIGN);
}

void rt_arena_reset(RtArena *arena) {
    RtArenaBlock *block;
    if (arena == 0) return;
    block = arena->blocks;
    while (block != 0) {
        block->used = 0U;
        block = block->next;
    }
}

void rt_arena_destroy(RtArena *arena) {
    RtArenaBlock *block;
    if (arena == 0) return;
    block = arena->blocks;
    while (block != 0) {
        RtArenaBlock *next = block->next;
        (void)platform_free_pages(block, block->mapping_size);
        block = next;
    }
    arena->blocks = 0;
}

static RtArenaBlock *rt_arena_new_block(size_t payload_size) {
    size_t total_size;
    size_t mapping_size;
    RtArenaBlock *block;

    if (payload_size > ((size_t)-1) - sizeof(RtArenaBlock)) return 0;
    total_size = sizeof(RtArenaBlock) + payload_size;
    mapping_size = rt_page_align(total_size);
    if (mapping_size < total_size) return 0;
    block = (RtArenaBlock *)platform_allocate_pages(mapping_size);
    if (block == 0) return 0;
    block->next = 0;
    block->used = 0U;
    block->size = mapping_size - sizeof(RtArenaBlock);
    block->mapping_size = mapping_size;
    return block;
}

void *rt_arena_alloc(RtArena *arena, size_t size) {
    RtArenaBlock *block;
    size_t offset;

    if (arena == 0 || size == 0U) return 0;
    size = rt_align_up(size, RT_ALIGN);
    block = arena->blocks;
    if (block == 0 || block->used + size < block->used || block->used + size > block->size) {
        size_t payload_size = size > arena->default_block_size ? size : arena->default_block_size;
        RtArenaBlock *next = rt_arena_new_block(payload_size);
        if (next == 0) return 0;
        next->next = arena->blocks;
        arena->blocks = next;
        block = next;
    }

    offset = block->used;
    block->used += size;
    return block->data + offset;
}

void *rt_arena_alloc_array(RtArena *arena, size_t count, size_t item_size) {
    size_t size;
    if (rt_mul_overflow(count, item_size, &size)) return 0;
    return rt_arena_alloc(arena, size);
}

static void rt_sort_swap(unsigned char *left, unsigned char *right, size_t item_size) {
    size_t index;
    for (index = 0U; index < item_size; ++index) {
        unsigned char temp = left[index];
        left[index] = right[index];
        right[index] = temp;
    }
}

static void rt_sort_sift_down(unsigned char *base, size_t start, size_t end, size_t item_size, int (*compare)(const void *, const void *)) {
    size_t root = start;
    while (root * 2U + 1U <= end) {
        size_t child = root * 2U + 1U;
        size_t swap_index = root;
        if (compare(base + swap_index * item_size, base + child * item_size) < 0) swap_index = child;
        if (child + 1U <= end && compare(base + swap_index * item_size, base + (child + 1U) * item_size) < 0) swap_index = child + 1U;
        if (swap_index == root) return;
        rt_sort_swap(base + root * item_size, base + swap_index * item_size, item_size);
        root = swap_index;
    }
}

void rt_sort(void *base_ptr, size_t count, size_t item_size, int (*compare)(const void *, const void *)) {
    unsigned char *base = (unsigned char *)base_ptr;
    size_t start;
    size_t end;
    if (base == 0 || count < 2U || item_size == 0U || compare == 0) return;
    start = (count - 2U) / 2U + 1U;
    while (start > 0U) {
        start -= 1U;
        rt_sort_sift_down(base, start, count - 1U, item_size, compare);
    }
    end = count - 1U;
    while (end > 0U) {
        rt_sort_swap(base, base + end * item_size, item_size);
        end -= 1U;
        rt_sort_sift_down(base, 0U, end, item_size, compare);
    }
}
