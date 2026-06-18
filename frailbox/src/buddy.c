/**
 * @file buddy.c
 * @brief Buddy memory allocator implementation for frailbox.
 *
 * The pool is backed by a single mmap region whose size is rounded up
 * to the nearest power of two.  A binary tree of "bookkeeping nodes"
 * tracks which blocks are free, split, or fully allocated.
 *
 * The tree is stored in a flat array (`nodes[]`) indexed so that:
 *   - node[0] is the root (covers the whole pool)
 *   - children of node[i] are at 2*i + 1 (left) and 2*i + 2 (right)
 *
 * Each node stores the order (log2 of its block size) and a status
 * byte: FREE, SPLIT, or FULL.
 */

#include "buddy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define BUDDY_MIN_ORDER   4          /* 2^4 = 16 bytes – minimum block  */
#define BUDDY_MAX_ORDER   32         /* 2^32 = 4 GiB – practical limit  */
#define NODE_FREE         0
#define NODE_SPLIT        1
#define NODE_FULL         2

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline uint32_t next_power_of_two(size_t v)
{
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
#if SIZE_MAX > 0xFFFFFFFFu
    v |= v >> 32;
#endif
    return (uint32_t)(v + 1);
}

static inline uint32_t order_of(size_t v)
{
    uint32_t o = 0;
    while ((size_t)(1u << o) < v) o++;
    return o;
}

/* ------------------------------------------------------------------ */
/* Pool structure                                                      */
/* ------------------------------------------------------------------ */

struct buddy_pool {
    void    *base;             /* mmap'd region start                    */
    size_t   capacity;         /* always a power of two                  */
    uint32_t min_order;        /* BUDDY_MIN_ORDER                        */
    uint32_t max_order;        /* order of the whole pool                */

    uint8_t *nodes;            /* status byte per tree node              */
    size_t   node_count;       /* total nodes in the tree                */

    /* Fast-stats counters */
    uint64_t total_allocated;
    uint64_t peak_allocated;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t split_count;
    uint64_t merge_count;
};

/* ------------------------------------------------------------------ */
/* Tree helpers                                                        */
/* ------------------------------------------------------------------ */

static inline size_t node_index_for_depth(uint32_t depth)
{
    /* First node index at a given depth: 2^depth - 1 */
    return ((size_t)1 << depth) - 1;
}

static inline size_t total_nodes(uint32_t max_order, uint32_t min_order)
{
    uint32_t levels = max_order - min_order + 1;
    /* Full binary tree with 'levels' levels: 2^levels - 1 nodes,
     * but only leaf level matters for counting; the formula above
     * gives the exact count for a complete tree. */
    return ((size_t)1 << levels) - 1;
}

/* ------------------------------------------------------------------ */
/* Core recursive helpers                                              */
/* ------------------------------------------------------------------ */

/**
 * Attempt to allocate from node `idx` at tree-depth `depth`.
 * The node covers a block of size 2^(max_order - depth).
 *
 * Returns the offset (from pool base) of the allocated block,
 * or (size_t)-1 on failure.
 */
static size_t alloc_from(buddy_pool_t *pool, size_t idx, uint32_t depth,
                         uint32_t req_order)
{
    uint32_t node_order = pool->max_order - depth;

    if (pool->nodes[idx] == NODE_FULL)
        return (size_t)-1;

    /* Leaf? */
    if (node_order == req_order) {
        if (pool->nodes[idx] == NODE_FREE) {
            pool->nodes[idx] = NODE_FULL;
            return 0; /* offset relative to subtree – caller accumulates */
        }
        /* SPLIT at leaf level means children are below min_order – shouldn't happen */
        return (size_t)-1;
    }

    /* We need to go deeper. Split if currently free. */
    if (pool->nodes[idx] == NODE_FREE) {
        pool->nodes[idx] = NODE_SPLIT;
        pool->split_count++;
        pool->nodes[2 * idx + 1] = NODE_FREE;
        pool->nodes[2 * idx + 2] = NODE_FREE;
    }

    /* Try left child */
    size_t half = (size_t)1 << (node_order - 1);
    size_t off = alloc_from(pool, 2 * idx + 1, depth + 1, req_order);
    if (off != (size_t)-1) {
        /* Update parent status */
        if (pool->nodes[2 * idx + 1] == NODE_FULL &&
            pool->nodes[2 * idx + 2] == NODE_FULL)
            pool->nodes[idx] = NODE_FULL;
        return off;
    }

    /* Try right child */
    off = alloc_from(pool, 2 * idx + 2, depth + 1, req_order);
    if (off != (size_t)-1) {
        off += half;
        if (pool->nodes[2 * idx + 1] == NODE_FULL &&
            pool->nodes[2 * idx + 2] == NODE_FULL)
            pool->nodes[idx] = NODE_FULL;
        return off;
    }

    return (size_t)-1;
}

/**
 * Free the block at offset `off` from node `idx` at depth `depth`.
 * Coalesces with buddy when both halves are free.
 */
static void free_at(buddy_pool_t *pool, size_t idx, uint32_t depth,
                    size_t off, uint32_t req_order)
{
    uint32_t node_order = pool->max_order - depth;

    if (node_order == req_order) {
        pool->nodes[idx] = NODE_FREE;
        pool->merge_count++;
        return;
    }

    size_t half = (size_t)1 << (node_order - 1);

    if (off < half) {
        free_at(pool, 2 * idx + 1, depth + 1, off, req_order);
    } else {
        free_at(pool, 2 * idx + 2, depth + 1, off - half, req_order);
    }

    /* Coalesce if both children are free */
    if (pool->nodes[2 * idx + 1] == NODE_FREE &&
        pool->nodes[2 * idx + 2] == NODE_FREE) {
        pool->nodes[idx] = NODE_FREE;
        pool->merge_count++;
    } else {
        pool->nodes[idx] = NODE_SPLIT;
    }
}

/**
 * Walk the tree to find the largest free block.
 */
static size_t find_largest_free(const buddy_pool_t *pool, size_t idx,
                                uint32_t depth)
{
    if (pool->nodes[idx] == NODE_FULL)
        return 0;
    if (pool->nodes[idx] == NODE_FREE)
        return (size_t)1 << (pool->max_order - depth);

    /* SPLIT */
    size_t left  = find_largest_free(pool, 2 * idx + 1, depth + 1);
    size_t right = find_largest_free(pool, 2 * idx + 2, depth + 1);
    return left > right ? left : right;
}

/**
 * Count free leaf-blocks.
 */
static void count_free_blocks(const buddy_pool_t *pool, size_t idx,
                              uint32_t depth, uint64_t *count)
{
    if (pool->nodes[idx] == NODE_FULL)
        return;
    if (pool->nodes[idx] == NODE_FREE) {
        (*count)++;
        return;
    }
    count_free_blocks(pool, 2 * idx + 1, depth + 1, count);
    count_free_blocks(pool, 2 * idx + 2, depth + 1, count);
}

/**
 * Reset all nodes to FREE recursively.
 */
static void reset_tree(buddy_pool_t *pool, size_t idx, uint32_t depth)
{
    if (depth > pool->max_order) return;
    pool->nodes[idx] = NODE_FREE;
    reset_tree(pool, 2 * idx + 1, depth + 1);
    reset_tree(pool, 2 * idx + 2, depth + 1);
}

/* ------------------------------------------------------------------ */
/* Offset ↔ pointer lookup                                            */
/* ------------------------------------------------------------------ */

/* Map a pointer back to its block order. We store it at a metadata
 * offset just before the user data: 2 bytes for the order. */
#define META_SIZE 16  /* 16 bytes before user data for metadata */

static inline void store_meta(void *ptr, uint32_t order)
{
    uint32_t *meta = (uint32_t *)((char *)ptr - META_SIZE);
    *meta = order;
    /* Sentinel so we can detect double-free */
    meta[1] = 0x42554459; /* "BUDY" */
}

static inline uint32_t load_meta_order(const void *ptr)
{
    const uint32_t *meta = (const uint32_t *)((const char *)ptr - META_SIZE);
    return *meta;
}

static inline int meta_is_valid(const void *ptr)
{
    const uint32_t *meta = (const uint32_t *)((const char *)ptr - META_SIZE);
    return meta[1] == 0x42554459;
}

static inline void clear_meta(void *ptr)
{
    uint32_t *meta = (uint32_t *)((char *)ptr - META_SIZE);
    meta[0] = 0;
    meta[1] = 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

buddy_pool_t *buddy_create(size_t size)
{
    if (size == 0) return NULL;

    /* Minimum effective size: 2 * META_SIZE + 16 so at least one
     * minimum allocation can be served. */
    if (size < (size_t)(1 << BUDDY_MIN_ORDER) * 2)
        size = (size_t)(1 << BUDDY_MIN_ORDER) * 2;

    /* Round capacity up to next power of two */
    size_t capacity = next_power_of_two(size);

    uint32_t max_order = order_of(capacity);
    if (max_order < BUDDY_MIN_ORDER)
        max_order = BUDDY_MIN_ORDER;

    /* Allocate the pool metadata */
    buddy_pool_t *pool = calloc(1, sizeof(buddy_pool_t));
    if (!pool) return NULL;

    /* mmap the backing region */
    void *base = mmap(NULL, capacity, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        free(pool);
        return NULL;
    }

    /* Allocate the node array */
    size_t nodes = total_nodes(max_order, BUDDY_MIN_ORDER);
    uint8_t *node_arr = calloc(nodes, sizeof(uint8_t));
    if (!node_arr) {
        munmap(base, capacity);
        free(pool);
        return NULL;
    }

    pool->base         = base;
    pool->capacity     = capacity;
    pool->min_order    = BUDDY_MIN_ORDER;
    pool->max_order    = max_order;
    pool->nodes        = node_arr;
    pool->node_count   = nodes;

    /* Root is free */
    pool->nodes[0] = NODE_FREE;

    return pool;
}

void buddy_destroy(buddy_pool_t *pool)
{
    if (!pool) return;
    if (pool->base)
        munmap(pool->base, pool->capacity);
    free(pool->nodes);
    free(pool);
}

void *buddy_alloc(buddy_pool_t *pool, size_t size)
{
    if (!pool || size == 0) return NULL;

    /* Each allocation needs META_SIZE bytes of overhead before the
     * user-visible pointer.  Round the total (size + META_SIZE) up
     * to the next power of two, but at least 2^min_order. */
    size_t total = size + META_SIZE;
    uint32_t req_order = order_of(total);
    if (req_order < pool->min_order)
        req_order = pool->min_order;

    if (req_order > pool->max_order)
        return NULL;

    size_t offset = alloc_from(pool, 0, 0, req_order);
    if (offset == (size_t)-1)
        return NULL;

    /* Track stats */
    size_t block_size = (size_t)1 << req_order;
    pool->total_allocated += block_size;
    pool->alloc_count++;
    if (pool->total_allocated > pool->peak_allocated)
        pool->peak_allocated = pool->total_allocated;

    /* User pointer starts after META_SIZE */
    void *user_ptr = (char *)pool->base + offset + META_SIZE;
    store_meta(user_ptr, req_order);

    return user_ptr;
}

void buddy_free(buddy_pool_t *pool, void *ptr)
{
    if (!pool || !ptr) return;

    /* Validate pointer is within our pool */
    if ((const char *)ptr < (const char *)pool->base ||
        (const char *)ptr >= (const char *)pool->base + pool->capacity)
        return;

    /* Check metadata sentinel to detect double-free / bad pointer */
    if (!meta_is_valid(ptr)) return;

    uint32_t req_order = load_meta_order(ptr);
    clear_meta(ptr);

    /* Calculate offset of the block start (before META_SIZE) */
    size_t offset = (size_t)((char *)ptr - META_SIZE - (char *)pool->base);

    free_at(pool, 0, 0, offset, req_order);

    size_t block_size = (size_t)1 << req_order;
    if (pool->total_allocated >= block_size)
        pool->total_allocated -= block_size;
    else
        pool->total_allocated = 0;

    pool->free_count++;
}

buddy_stats_t buddy_stats(const buddy_pool_t *pool)
{
    buddy_stats_t s;
    memset(&s, 0, sizeof(s));

    if (!pool) return s;

    /* Required metrics */
    s.total            = pool->capacity;
    s.used             = pool->total_allocated;
    s.free             = pool->capacity - pool->total_allocated;
    s.allocation_count = pool->alloc_count;
    s.free_count       = pool->free_count;

    /* Extended metrics */
    s.peak_allocated   = pool->peak_allocated;
    s.split_count      = pool->split_count;
    s.merge_count      = pool->merge_count;

    /* Walk tree for live metrics */
    uint64_t free_blks = 0;
    count_free_blocks(pool, 0, 0, &free_blks);
    s.free_blocks = free_blks;

    s.largest_free_block = find_largest_free(pool, 0, 0);

    /* fragmented_bytes: free space that is NOT in the largest block */
    if (s.free > s.largest_free_block)
        s.fragmented_bytes = s.free - s.largest_free_block;
    else
        s.fragmented_bytes = 0;

    /* Fragmentation ratio: 1 - (largest_free / total_free) */
    if (s.free > 0)
        s.fragmentation_ratio = 1.0 - ((double)s.largest_free_block /
                                        (double)s.free);
    else
        s.fragmentation_ratio = 0.0;

    return s;
}

void buddy_reset(buddy_pool_t *pool)
{
    if (!pool) return;

    /* Reinitialise the tree: mark everything free */
    reset_tree(pool, 0, 0);

    pool->total_allocated = 0;
    /* peak, alloc_count, free_count are NOT reset – they're lifetime */
}
