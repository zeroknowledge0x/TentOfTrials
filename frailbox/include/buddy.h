/**
 * @file buddy.h
 * @brief Buddy memory allocator for the frailbox sandbox.
 *
 * A power-of-two buddy allocator that supports individual free() of
 * allocations (unlike the arena which can only bulk-reset).  Memory
 * is obtained from mmap and managed in a binary-tree structure where
 * every block is a power-of-two size.
 *
 * Public API:
 *   buddy_create()   – initialise a pool of the given size
 *   buddy_destroy()  – release all memory back to the OS
 *   buddy_alloc()    – allocate at least `size` bytes (rounded up to
 *                      next power of two, minimum 16 bytes)
 *   buddy_free()     – return a previously-allocated block; coalesces
 *                      with its buddy when both halves are free
 *   buddy_stats()    – return current fragmentation / usage metrics
 *   buddy_reset()    – mark every block free (fast bulk reclaim)
 */

#ifndef FRAILBOX_BUDDY_H
#define FRAILBOX_BUDDY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Fragmentation / usage statistics                                    */
/* ------------------------------------------------------------------ */

typedef struct buddy_stats {
    /* Required fragmentation metrics */
    uint64_t total;              /* total pool capacity in bytes           */
    uint64_t used;               /* bytes currently allocated             */
    uint64_t free;               /* bytes currently free                  */
    uint64_t fragmented_bytes;   /* free bytes not in largest free block  */
    uint64_t allocation_count;   /* total buddy_alloc() calls             */
    uint64_t free_count;         /* total buddy_free() calls              */
    /* Extended metrics */
    uint64_t peak_allocated;     /* high-water mark of allocated bytes    */
    uint64_t split_count;        /* block splits performed                */
    uint64_t merge_count;        /* buddy merges (coalesces) performed    */
    uint64_t free_blocks;        /* number of free blocks right now       */
    uint64_t largest_free_block; /* largest contiguous free block (bytes) */
    double   fragmentation_ratio;/* 0.0 = perfect, 1.0 = fully fragged   */
} buddy_stats_t;

/* ------------------------------------------------------------------ */
/* Opaque pool handle                                                  */
/* ------------------------------------------------------------------ */

typedef struct buddy_pool buddy_pool_t;
typedef buddy_pool_t buddy_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/**
 * Create a buddy pool that manages at least `size` bytes.
 * The actual capacity is rounded up to the nearest power of two.
 * Returns NULL on failure.
 */
buddy_pool_t *buddy_create(size_t size);

/**
 * Destroy a buddy pool and release all memory to the OS.
 * Any pointers returned by buddy_alloc() become dangling.
 */
void buddy_destroy(buddy_pool_t *pool);

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

/**
 * Allocate at least `size` bytes from the pool.
 * The returned pointer is 16-byte aligned.
 * Returns NULL if the pool cannot satisfy the request.
 */
void *buddy_alloc(buddy_pool_t *pool, size_t size);

/**
 * Free a block previously returned by buddy_alloc().
 * Passing NULL is a safe no-op.
 * Double-free is detected and silently ignored.
 */
void buddy_free(buddy_pool_t *pool, void *ptr);

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

/**
 * Return a snapshot of current pool statistics.
 */
buddy_stats_t buddy_stats(const buddy_pool_t *pool);

/**
 * Bulk-reset: mark every block free (like arena_reset).
 * No memory is returned to the OS – the pool keeps its mmap region.
 */
void buddy_reset(buddy_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* FRAILBOX_BUDDY_H */
