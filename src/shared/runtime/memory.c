#include "runtime.h"

#if !defined(NEWOS_FREESTANDING)
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
#else
#include "syscall.h"
#if defined(__x86_64__)
#define RT_LINUX_SYS_BRK 12
#elif defined(__aarch64__)
#define RT_LINUX_SYS_BRK 214
#else
#error "rt allocator needs a Linux brk syscall number for this architecture"
#endif
#endif

/*
 * Keep these implementations strictly byte-wise and volatile-backed so the
 * compiler does not fold them back into builtin memcpy or memset calls.
 * That would recurse here in hosted optimized builds.
 */
void *memcpy(void *dst, const void *src, size_t count) {
    volatile unsigned char *out = (volatile unsigned char *)dst;
    const volatile unsigned char *in = (const volatile unsigned char *)src;
    size_t i;

    for (i = 0; i < count; ++i) {
        out[i] = in[i];
    }

    return dst;
}

void *memset(void *buffer, int byte_value, size_t count) {
    volatile unsigned char *out = (volatile unsigned char *)buffer;
    unsigned char value = (unsigned char)byte_value;
    size_t i;

    for (i = 0; i < count; ++i) {
        out[i] = value;
    }

    return buffer;
}

void *memmove(void *dst, const void *src, size_t count) {
    volatile unsigned char *out = (volatile unsigned char *)dst;
    const volatile unsigned char *in = (const volatile unsigned char *)src;
    size_t i;

    if (out == in) {
        return dst;
    }

    if (out < in) {
        for (i = 0; i < count; ++i) {
            out[i] = in[i];
        }
    } else {
        for (i = count; i > 0; --i) {
            out[i - 1] = in[i - 1];
        }
    }

    return dst;
}

void rt_memset(void *buffer, int byte_value, size_t count) {
    (void)memset(buffer, byte_value, count);
}

#if !defined(NEWOS_FREESTANDING)
void *rt_malloc(size_t size) {
    return malloc(size);
}

void *rt_realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

void rt_free(void *ptr) {
    free(ptr);
}
#else
typedef struct RtAllocBlock RtAllocBlock;

struct RtAllocBlock {
    size_t size;
    int free;
    RtAllocBlock *next;
};

static RtAllocBlock *rt_alloc_head;
static RtAllocBlock *rt_alloc_tail;
static unsigned long rt_alloc_break;

static size_t rt_alloc_align(size_t value) {
    const size_t alignment = sizeof(void *) * 2U;
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static int rt_alloc_init_break(void) {
    if (rt_alloc_break != 0UL) return 0;
    rt_alloc_break = (unsigned long)linux_syscall1(RT_LINUX_SYS_BRK, 0);
    return rt_alloc_break == 0UL ? -1 : 0;
}

static void rt_alloc_split(RtAllocBlock *block, size_t size) {
    RtAllocBlock *next;
    if (block->size <= size + sizeof(RtAllocBlock) + sizeof(void *) * 2U) return;
    next = (RtAllocBlock *)((unsigned char *)(block + 1) + size);
    next->size = block->size - size - sizeof(RtAllocBlock);
    next->free = 1;
    next->next = block->next;
    block->size = size;
    block->next = next;
    if (rt_alloc_tail == block) rt_alloc_tail = next;
}

static RtAllocBlock *rt_alloc_find_free(size_t size) {
    RtAllocBlock *block = rt_alloc_head;
    while (block != 0) {
        if (block->free && block->size >= size) return block;
        block = block->next;
    }
    return 0;
}

static RtAllocBlock *rt_alloc_grow(size_t size) {
    unsigned long block_start;
    unsigned long block_end;
    RtAllocBlock *block;
    if (rt_alloc_init_break() != 0) return 0;
    block_start = rt_alloc_align(rt_alloc_break);
    block_end = block_start + sizeof(RtAllocBlock) + size;
    if (block_end < block_start) return 0;
    if ((unsigned long)linux_syscall1(RT_LINUX_SYS_BRK, (long)block_end) != block_end) return 0;
    rt_alloc_break = block_end;
    block = (RtAllocBlock *)block_start;
    block->size = size;
    block->free = 0;
    block->next = 0;
    if (rt_alloc_tail != 0) rt_alloc_tail->next = block;
    else rt_alloc_head = block;
    rt_alloc_tail = block;
    return block;
}

static void rt_alloc_coalesce(void) {
    RtAllocBlock *block = rt_alloc_head;
    while (block != 0 && block->next != 0) {
        if (block->free && block->next->free) {
            block->size += sizeof(RtAllocBlock) + block->next->size;
            block->next = block->next->next;
            if (block->next == 0) rt_alloc_tail = block;
        } else {
            block = block->next;
        }
    }
}

void *rt_malloc(size_t size) {
    RtAllocBlock *block;
    if (size == 0U) size = 1U;
    size = rt_alloc_align(size);
    block = rt_alloc_find_free(size);
    if (block != 0) {
        block->free = 0;
        rt_alloc_split(block, size);
        return block + 1;
    }
    block = rt_alloc_grow(size);
    return block == 0 ? 0 : (void *)(block + 1);
}

void *rt_realloc(void *ptr, size_t size) {
    RtAllocBlock *block;
    void *next;
    size_t copy_size;
    if (ptr == 0) return rt_malloc(size);
    if (size == 0U) {
        rt_free(ptr);
        return 0;
    }
    block = ((RtAllocBlock *)ptr) - 1;
    size = rt_alloc_align(size);
    if (block->size >= size) {
        rt_alloc_split(block, size);
        return ptr;
    }
    next = rt_malloc(size);
    if (next == 0) return 0;
    copy_size = block->size < size ? block->size : size;
    memcpy(next, ptr, copy_size);
    rt_free(ptr);
    return next;
}

void rt_free(void *ptr) {
    RtAllocBlock *block;
    if (ptr == 0) return;
    block = ((RtAllocBlock *)ptr) - 1;
    block->free = 1;
    rt_alloc_coalesce();
}
#endif

static void rt_sort_swap(unsigned char *left, unsigned char *right, size_t item_size) {
    size_t i;
    for (i = 0U; i < item_size; ++i) {
        unsigned char temp = left[i];
        left[i] = right[i];
        right[i] = temp;
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
