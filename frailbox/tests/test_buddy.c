/**
 * @file test_buddy.c
 * @brief Test suite for the buddy allocator.
 *
 * Compile:  gcc -Iinclude -o test_buddy tests/test_buddy.c src/buddy.c -Wall -Wextra -Werror
 * Run:      ./test_buddy
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <time.h>
#include <stdint.h>

#include "buddy.h"

/* ================================================================== */
/* MINIMAL TEST FRAMEWORK (copied from test_connector.c style)        */
/* ================================================================== */

#define MAX_TESTS 256

typedef struct {
    const char *name;
    int (*func)(void);
    int failed;
    double duration_ms;
} test_case_t;

static test_case_t tests[MAX_TESTS];
static int test_count = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static jmp_buf assert_jmp;
static int assert_failed = 0;
static char assert_msg[1024];

#define TEST(name) \
    static int test_##name(void); \
    __attribute__((constructor)) static void register_##name(void) { \
        if (test_count < MAX_TESTS) { \
            tests[test_count].name = #name; \
            tests[test_count].func = test_##name; \
            tests[test_count].failed = 0; \
            test_count++; \
        } \
    } \
    static int test_##name(void)

#define ASSERT(cond, msg, ...) do { \
    if (!(cond)) { \
        snprintf(assert_msg, sizeof(assert_msg), "ASSERT FAILED: " msg, ##__VA_ARGS__); \
        assert_failed = 1; \
        longjmp(assert_jmp, 1); \
    } \
} while(0)

#define ASSERT_EQ(a, b, msg, ...) ASSERT((a) == (b), "Expected " msg, ##__VA_ARGS__)
#define ASSERT_NULL(ptr) ASSERT((ptr) == NULL, "Expected NULL pointer")
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != NULL, "Expected non-NULL pointer")

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static int run_all_tests(void) {
    printf("\n");
    printf("============================================================\n");
    printf("  BUDDY ALLOCATOR TEST SUITE\n");
    printf("============================================================\n\n");

    for (int i = 0; i < test_count; i++) {
        test_case_t *test = &tests[i];
        printf("  [%3d/%3d] %-50s ", i + 1, test_count, test->name);

        double start = get_time_ms();
        assert_failed = 0;

        if (setjmp(assert_jmp) == 0) {
            int result = test->func();
            if (result == 0) {
                tests_passed++;
                double elapsed = get_time_ms() - start;
                printf("PASS (%.1fms)\n", elapsed);
            } else {
                tests_failed++;
                test->failed = 1;
                printf("FAIL (returned %d)\n", result);
            }
        } else {
            tests_failed++;
            test->failed = 1;
            double elapsed = get_time_ms() - start;
            printf("FAIL (%.1fms)\n", elapsed);
            printf("         %s\n", assert_msg);
        }
    }

    printf("\n");
    printf("============================================================\n");
    printf("  RESULTS: %d passed, %d failed out of %d\n",
           tests_passed, tests_failed, test_count);
    printf("============================================================\n\n");

    return tests_failed;
}

/* ================================================================== */
/* TESTS                                                               */
/* ================================================================== */

TEST(test_create_destroy)
{
    buddy_pool_t *pool = buddy_create(1024 * 1024);
    ASSERT_NOT_NULL(pool);
    buddy_destroy(pool);
    return 0;
}

TEST(test_create_zero)
{
    buddy_pool_t *pool = buddy_create(0);
    ASSERT_NULL(pool);
    return 0;
}

TEST(test_alloc_basic)
{
    buddy_pool_t *pool = buddy_create(1024 * 1024);
    ASSERT_NOT_NULL(pool);

    void *ptr = buddy_alloc(pool, 128);
    ASSERT_NOT_NULL(ptr);

    buddy_stats_t stats = buddy_stats(pool);
    ASSERT(stats.used > 0, "Should have allocated bytes");
    ASSERT_EQ(stats.allocation_count, (uint64_t)1, "Should have 1 alloc");

    buddy_free(pool, ptr);
    buddy_destroy(pool);
    return 0;
}

TEST(test_alloc_zero_size)
{
    buddy_pool_t *pool = buddy_create(1024 * 1024);
    ASSERT_NOT_NULL(pool);

    void *ptr = buddy_alloc(pool, 0);
    ASSERT_NULL(ptr);

    buddy_destroy(pool);
    return 0;
}

TEST(test_alloc_multiple)
{
    buddy_pool_t *pool = buddy_create(1024 * 1024);
    ASSERT_NOT_NULL(pool);

    void *ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = buddy_alloc(pool, 256);
        ASSERT_NOT_NULL(ptrs[i]);
    }

    buddy_stats_t stats = buddy_stats(pool);
    ASSERT_EQ(stats.allocation_count, (uint64_t)10, "Should have 10 allocs");

    for (int i = 0; i < 10; i++) {
        buddy_free(pool, ptrs[i]);
    }

    stats = buddy_stats(pool);
    ASSERT_EQ(stats.free_count, (uint64_t)10, "Should have 10 frees");
    ASSERT_EQ(stats.used, (uint64_t)0, "Should be fully freed");

    buddy_destroy(pool);
    return 0;
}

TEST(test_free_null)
{
    buddy_pool_t *pool = buddy_create(1024 * 1024);
    ASSERT_NOT_NULL(pool);

    /* Should be safe no-op */
    buddy_free(pool, NULL);

    buddy_stats_t stats = buddy_stats(pool);
    ASSERT_EQ(stats.free_count, (uint64_t)0, "NULL free should not count");

    buddy_destroy(pool);
    return 0;
}

TEST(test_alloc_alignment)
{
    buddy_pool_t *pool = buddy_create(1024 * 1024);
    ASSERT_NOT_NULL(pool);

    void *ptr = buddy_alloc(pool, 50);
    ASSERT_NOT_NULL(ptr);

    /* Buddy allocator should return at least 16-byte aligned pointers */
    ASSERT(((uintptr_t)ptr & 0xF) == 0, "Pointer should be 16-byte aligned, got %p", ptr);

    buddy_free(pool, ptr);
    buddy_destroy(pool);
    return 0;
}

TEST(test_write_read_pattern)
{
    buddy_pool_t *pool = buddy_create(1024 * 1024);
    ASSERT_NOT_NULL(pool);

    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        unsigned char *ptr = buddy_alloc(pool, sizes[i]);
        ASSERT_NOT_NULL(ptr);

        /* Write a pattern */
        memset(ptr, (int)(i + 0x42), sizes[i]);

        /* Verify */
        for (size_t j = 0; j < sizes[i]; j++) {
            ASSERT(ptr[j] == (unsigned char)(i + 0x42),
                   "Pattern mismatch at byte %zu of size %zu", j, sizes[i]);
        }

        buddy_free(pool, ptr);
    }

    buddy_destroy(pool);
    return 0;
}

TEST(test_coalesce)
{
    buddy_pool_t *pool = buddy_create(1024 * 1024);
    ASSERT_NOT_NULL(pool);

    /* Allocate two adjacent blocks of the same size */
    void *p1 = buddy_alloc(pool, 256);
    void *p2 = buddy_alloc(pool, 256);
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);

    buddy_stats_t stats = buddy_stats(pool);
    uint64_t splits_before = stats.split_count;

    /* Free both – they should coalesce */
    buddy_free(pool, p1);
    buddy_free(pool, p2);

    stats = buddy_stats(pool);
    ASSERT(stats.merge_count > 0, "Should have performed merges");
    ASSERT_EQ(stats.used, (uint64_t)0, "All memory should be freed");

    /* After coalescing, a large allocation should succeed */
    void *p3 = buddy_alloc(pool, 512);
    ASSERT_NOT_NULL(p3);
    buddy_free(pool, p3);

    buddy_destroy(pool);
    return 0;
}

TEST(test_stats)
{
    buddy_pool_t *pool = buddy_create(4096);
    ASSERT_NOT_NULL(pool);

    buddy_stats_t stats = buddy_stats(pool);
    ASSERT(stats.total >= 4096, "Capacity should be >= 4096, got %lu",
           (unsigned long)stats.total);
    ASSERT_EQ(stats.used, (uint64_t)0, "Initially nothing allocated");
    ASSERT_EQ(stats.allocation_count, (uint64_t)0, "No allocs yet");
    ASSERT_EQ(stats.free_count, (uint64_t)0, "No frees yet");

    void *ptr = buddy_alloc(pool, 64);
    ASSERT_NOT_NULL(ptr);

    stats = buddy_stats(pool);
    ASSERT(stats.used >= 64, "Should have >= 64 bytes allocated");
    ASSERT(stats.peak_allocated >= 64, "Peak should be >= 64");
    ASSERT_EQ(stats.allocation_count, (uint64_t)1, "1 alloc");

    buddy_free(pool, ptr);
    buddy_destroy(pool);
    return 0;
}

TEST(test_fragmentation_ratio)
{
    buddy_pool_t *pool = buddy_create(1024 * 1024);
    ASSERT_NOT_NULL(pool);

    buddy_stats_t stats = buddy_stats(pool);
    /* Initially no fragmentation */
    ASSERT(stats.fragmentation_ratio < 0.001,
           "Initial fragmentation should be ~0, got %f", stats.fragmentation_ratio);

    void *ptr = buddy_alloc(pool, 16);
    ASSERT_NOT_NULL(ptr);

    stats = buddy_stats(pool);
    /* Should have some fragmentation now but not extreme */
    ASSERT(stats.fragmentation_ratio >= 0.0 && stats.fragmentation_ratio <= 1.0,
           "Fragmentation ratio should be [0,1], got %f", stats.fragmentation_ratio);

    buddy_free(pool, ptr);
    buddy_destroy(pool);
    return 0;
}

TEST(test_reset)
{
    buddy_pool_t *pool = buddy_create(1024 * 1024);
    ASSERT_NOT_NULL(pool);

    /* Allocate some blocks */
    void *ptrs[5];
    for (int i = 0; i < 5; i++) {
        ptrs[i] = buddy_alloc(pool, 512);
        ASSERT_NOT_NULL(ptrs[i]);
    }

    buddy_stats_t stats = buddy_stats(pool);
    ASSERT(stats.used > 0, "Should have allocations");

    /* Reset */
    buddy_reset(pool);

    stats = buddy_stats(pool);
    ASSERT_EQ(stats.used, (uint64_t)0, "After reset, nothing allocated");

    /* Should be able to allocate again */
    void *ptr = buddy_alloc(pool, 1024);
    ASSERT_NOT_NULL(ptr);

    buddy_free(pool, ptr);
    buddy_destroy(pool);
    return 0;
}

TEST(test_alloc_too_large)
{
    buddy_pool_t *pool = buddy_create(4096);
    ASSERT_NOT_NULL(pool);

    /* Try to allocate more than pool capacity */
    void *ptr = buddy_alloc(pool, 1024 * 1024 * 100);
    ASSERT_NULL(ptr);

    buddy_destroy(pool);
    return 0;
}

TEST(test_double_free_protection)
{
    buddy_pool_t *pool = buddy_create(1024 * 1024);
    ASSERT_NOT_NULL(pool);

    void *ptr = buddy_alloc(pool, 256);
    ASSERT_NOT_NULL(ptr);

    buddy_free(pool, ptr);
    /* Second free should be a no-op (not crash) */
    buddy_free(pool, ptr);

    buddy_stats_t stats = buddy_stats(pool);
    ASSERT_EQ(stats.free_count, (uint64_t)1, "Only first free should count");

    buddy_destroy(pool);
    return 0;
}

TEST(test_stress_alloc_free)
{
    buddy_pool_t *pool = buddy_create(2 * 1024 * 1024);
    ASSERT_NOT_NULL(pool);

    /* Stress test: many alloc/free cycles */
    for (int round = 0; round < 100; round++) {
        void *ptrs[32];
        for (int i = 0; i < 32; i++) {
            size_t sz = (size_t)(16 + (i * 7) % 4080);
            ptrs[i] = buddy_alloc(pool, sz);
            ASSERT_NOT_NULL(ptrs[i]);
            memset(ptrs[i], 0xAA, sz);
        }
        for (int i = 31; i >= 0; i--) {
            buddy_free(pool, ptrs[i]);
        }
    }

    buddy_stats_t stats = buddy_stats(pool);
    ASSERT_EQ(stats.used, (uint64_t)0, "All memory should be freed");
    ASSERT_EQ(stats.allocation_count, (uint64_t)3200, "Should have 3200 allocs");
    ASSERT_EQ(stats.free_count, (uint64_t)3200, "Should have 3200 frees");

    buddy_destroy(pool);
    return 0;
}

TEST(test_stats_null_pool)
{
    buddy_stats_t stats = buddy_stats(NULL);
    ASSERT_EQ(stats.total, (uint64_t)0, "NULL pool stats should be zero");
    return 0;
}

TEST(test_various_sizes)
{
    buddy_pool_t *pool = buddy_create(4 * 1024 * 1024);
    ASSERT_NOT_NULL(pool);

    /* Test various sizes */
    size_t sizes[] = {1, 2, 3, 7, 15, 16, 17, 31, 32, 33, 63, 64, 127, 128,
                      255, 256, 511, 512, 1023, 1024, 2048, 4096, 8192};
    size_t n = sizeof(sizes) / sizeof(sizes[0]);

    void *ptrs[sizeof(sizes)/sizeof(sizes[0])];
    for (size_t i = 0; i < n; i++) {
        ptrs[i] = buddy_alloc(pool, sizes[i]);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], (int)(i & 0xFF), sizes[i]);
    }

    /* Verify all data */
    for (size_t i = 0; i < n; i++) {
        unsigned char *p = (unsigned char *)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            ASSERT(p[j] == (unsigned char)(i & 0xFF),
                   "Data corruption at size=%zu byte=%zu", sizes[i], j);
        }
    }

    for (size_t i = 0; i < n; i++) {
        buddy_free(pool, ptrs[i]);
    }

    buddy_destroy(pool);
    return 0;
}

TEST(test_largest_free_block)
{
    buddy_pool_t *pool = buddy_create(1024 * 1024);
    ASSERT_NOT_NULL(pool);

    buddy_stats_t stats = buddy_stats(pool);
    /* Initially, the largest free block should be the entire pool capacity */
    ASSERT(stats.largest_free_block == stats.total,
           "Initially largest free should equal capacity: got %lu vs %lu",
           (unsigned long)stats.largest_free_block,
           (unsigned long)stats.total);

    void *ptr = buddy_alloc(pool, 256);
    ASSERT_NOT_NULL(ptr);

    stats = buddy_stats(pool);
    /* Largest free block should be less than total now */
    ASSERT(stats.largest_free_block < stats.total,
           "After alloc, largest free should be smaller");

    buddy_free(pool, ptr);
    buddy_destroy(pool);
    return 0;
}

/* ================================================================== */
/* MAIN                                                                */
/* ================================================================== */

int main(void) {
    return run_all_tests();
}
