#define _POSIX_C_SOURCE 199309L

#include "tdmm.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

//  helpers

static void section(const char *title) { printf("\n===== %s =====\n", title); }

static long ns_diff(struct timespec t0, struct timespec t1) {
    return (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
}

//  benchmark

// speed as function of size, log scale 1b to 8mb
static void run_benchmark(const char *name, alloc_strat_e pol) {
    section(name);
    printf("DEBUG: calling t_init\n"); fflush(stdout);
    t_init(pol);
    printf("DEBUG: t_init done, calling t_malloc(1)\n"); fflush(stdout);
    void *test = t_malloc(1);
    printf("DEBUG: t_malloc(1) = %p\n", test); fflush(stdout);
    t_free(test);
    printf("DEBUG: t_free done\n"); fflush(stdout);

    size_t sizes[] = {1, 4, 16, 64, 256, 1024, 4096,
                      16384, 65536, 262144, 1048576, 4194304, 8388608};
    int n = sizeof(sizes) / sizeof(sizes[0]);

    printf("%-12s %15s %15s\n", "Size (B)", "tmalloc (ns)", "tfree (ns)");
    printf("%-12s %15s %15s\n", "--------", "------------", "----------");

    for (int i = 0; i < n; i++) {
        t_init(pol); // fresh heap each iteration

        struct timespec t0, t1;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        void *p = t_malloc(sizes[i]);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long malloc_ns = ns_diff(t0, t1);

        if (p == NULL) {
            printf("%-12zu %15s %15s\n", sizes[i], "OOM", "OOM");
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &t0);
        t_free(p);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long free_ns = ns_diff(t0, t1);

        printf("%-12zu %15ld %15ld\n", sizes[i], malloc_ns, free_ns);
    }

    printf("\nFinal stats after benchmark:\n");
    printStats();
}

//  utilization over time

// alloc and free in pattern
static void utilization_over_time(const char *name, alloc_strat_e pol) {
    section(name);
    t_init(pol);

    printf("Step, Event, Size, Utilization%%\n");

    size_t sizes[] = {64, 128, 256, 512, 1024, 2048, 128, 64, 512, 256};
    int n = sizeof(sizes) / sizeof(sizes[0]);
    void *ptrs[10];

    // alloc all
    for (int i = 0; i < n; i++) {
        ptrs[i] = t_malloc(sizes[i]);
        printf("%d, alloc, %zu, ", i, sizes[i]);
        printStats();
    }

    // free every other
    for (int i = 0; i < n; i += 2) {
        t_free(ptrs[i]);
        ptrs[i] = NULL;
        printf("%d, free, %zu, ", n + i / 2, sizes[i]);
        printStats();
    }

    // alloc again into fragmented heap
    for (int i = 0; i < n; i += 2) {
        ptrs[i] = t_malloc(sizes[i] / 2);
        printf("%d, realloc, %zu, ", n + n / 2 + i / 2, sizes[i] / 2);
        printStats();
    }

    // free everything
    for (int i = 0; i < n; i++) {
        if (ptrs[i])
            t_free(ptrs[i]);
    }
}

//  overhead tracking

// header overhead vs user data over run
static void overhead_test(const char *name, alloc_strat_e pol) {
    section(name);
    t_init(pol);

    printf("%-10s %-12s %-12s %-12s\n", "Allocs", "User (B)", "Overhead (B)",
           "Ratio");
    printf("%-10s %-12s %-12s %-12s\n", "------", "--------", "------------",
           "-----");

    void *ptrs[20];
    size_t total_user = 0;

    for (int i = 0; i < 20; i++) {
        size_t sz = (i + 1) * 64;
        ptrs[i] = t_malloc(sz);
        total_user += sz;
        size_t overhead = (i + 1) * sizeof(sec);
        printf("%-10d %-12zu %-12zu %-12.4f\n", i + 1, total_user, overhead,
               (double)overhead / (total_user + overhead));
    }

    for (int i = 0; i < 20; i++)
        t_free(ptrs[i]);
}

//  correctness

static void correctness_tests() {
    t_init(FIRST_FIT);

    section("TEST 1: Single Allocation");
    void *p1 = t_malloc(100);
    printf("p1 = %p (expect non-null)\n", p1);
    printf("aligned to 4: %s\n", ((uintptr_t)p1 % 4 == 0) ? "YES" : "NO");
    printStats();

    section("TEST 2: Multiple Allocations");
    void *p2 = t_malloc(200);
    void *p3 = t_malloc(300);
    printf("p2 = %p, p3 = %p\n", p2, p3);
    printf("p2 < p3: %s\n", p2 < p3 ? "YES" : "NO"); // sanity check ordering
    printStats();

    section("TEST 3: Write and Read Back");
    memset(p1, 0xAB, 100);
    int ok = 1;
    for (int i = 0; i < 100; i++)
        if (((unsigned char *)p1)[i] != 0xAB) {
            ok = 0;
            break;
        }
    printf("Memory read/write intact: %s\n", ok ? "YES" : "NO");

    section("TEST 4: Free and Merge");
    t_free(p2);
    t_free(p3); // should merge with p2
    printf("Freed p2 and p3 (expect merge)\n");
    printStats();

    section("TEST 5: Free All (expect one big free block)");
    t_free(p1);
    printStats();

    section("TEST 6: Invalid Free (expect error, no crash)");
    int dummy = 42;
    t_free(&dummy);

    section("TEST 7: Double Free (expect error, no crash)");
    void *p4 = t_malloc(64);
    t_free(p4);
    t_free(p4);

    section("TEST 8: Zero Size Malloc");
    void *p5 = t_malloc(0);
    printf("t_malloc(0) = %p (expect null)\n", p5);

    section("TEST 9: BEST_FIT policy");
    t_init(BEST_FIT);
    void *b1 = t_malloc(512);
    void *b2 = t_malloc(128);
    void *b3 = t_malloc(256);
    t_free(b1);               // 512-byte hole
    t_free(b3);               // 256-byte hole
    void *b4 = t_malloc(200); // best fit = 256-byte hole
    printf("b4 should be near b3 region: b3=%p b4=%p\n", b3, b4);
    t_free(b2);
    t_free(b4);

    section("TEST 10: WORST_FIT policy");
    t_init(WORST_FIT);
    void *w1 = t_malloc(512);
    void *w2 = t_malloc(128);
    void *w3 = t_malloc(256);
    t_free(w1);               // 512-byte hole
    t_free(w3);               // 256-byte hole
    void *w4 = t_malloc(200); // worst fit = 512-byte hole
    printf("w4 should be near w1 region: w1=%p w4=%p\n", w1, w4);
    t_free(w2);
    t_free(w4);
}

//  main

int main() {
    correctness_tests();

    run_benchmark("BENCHMARK: FIRST_FIT", FIRST_FIT);
    run_benchmark("BENCHMARK: BEST_FIT", BEST_FIT);
    run_benchmark("BENCHMARK: WORST_FIT", WORST_FIT);

    utilization_over_time("UTILIZATION OVER TIME: FIRST_FIT", FIRST_FIT);
    utilization_over_time("UTILIZATION OVER TIME: BEST_FIT", BEST_FIT);
    utilization_over_time("UTILIZATION OVER TIME: WORST_FIT", WORST_FIT);

    overhead_test("OVERHEAD: FIRST_FIT", FIRST_FIT);
    overhead_test("OVERHEAD: BEST_FIT", BEST_FIT);
    overhead_test("OVERHEAD: WORST_FIT", WORST_FIT);

    return 0;
}