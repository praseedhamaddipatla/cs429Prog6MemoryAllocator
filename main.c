#define _POSIX_C_SOURCE 199309L

#include "tdmm.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// calc diff in nanoseconds between two timestamps 
static long nsDiff(struct timespec t0, struct timespec t1) {
    return (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
}

// benchmark malloc/free speed across log-scale sizes 
static void runBench(const char *name, alloc_strat_e pol) {
    printf("\n===== %s =====\n", name);
    printf("DEBUG: calling t_init\n"); fflush(stdout);
    t_init(pol);
    printf("DEBUG: t_init done, calling t_malloc(1)\n"); fflush(stdout);
    void *test = t_malloc(1);
    printf("DEBUG: t_malloc(1) = %p\n", test); fflush(stdout);
    t_free(test);
    printf("DEBUG: t_free done\n"); fflush(stdout);

    // sizes range from 1b to 8mb 
    size_t sizes[] = {1, 4, 16, 64, 256, 1024, 4096,
                      16384, 65536, 262144, 1048576, 4194304, 8388608};
    int n = sizeof(sizes) / sizeof(sizes[0]);

    printf("%-12s %15s %15s\n", "Size (B)", "tmalloc (ns)", "tfree (ns)");
    printf("%-12s %15s %15s\n", "--------", "------------", "----------");

    for (int i = 0; i < n; i++) {
        t_init(pol); // fresh heap each iter

        struct timespec t0, t1;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        void *p = t_malloc(sizes[i]);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long mallocNs = nsDiff(t0, t1);

        if (p == NULL) {
            printf("%-12zu %15s %15s\n", sizes[i], "OOM", "OOM");
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &t0);
        t_free(p);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long freeNs = nsDiff(t0, t1);

        printf("%-12zu %15ld %15ld\n", sizes[i], mallocNs, freeNs);
    }

    printf("\nFinal stats after benchmark:\n");
    printStats();
}

// alloc then free in a pattern to track utilization over time 
static void utilTest(const char *name, alloc_strat_e pol) {
    printf("\n===== %s =====\n", name);
    t_init(pol);

    printf("Step, Event, Size, Utilization%%\n");

    size_t sizes[] = {64, 128, 256, 512, 1024, 2048, 128, 64, 512, 256};
    int n = sizeof(sizes) / sizeof(sizes[0]);
    void *ptrs[10];

    // alloc all blocks
    for (int i = 0; i < n; i++) {
        ptrs[i] = t_malloc(sizes[i]);
        printf("%d, alloc, %zu, ", i, sizes[i]);
        printStats();
    }

    // free every other block to create fragmentation
    for (int i = 0; i < n; i += 2) {
        t_free(ptrs[i]);
        ptrs[i] = NULL;
        printf("%d, free, %zu, ", n + i / 2, sizes[i]);
        printStats();
    }

    // realloc into fragmented heap w/ half sizes to test fit into holes 
    for (int i = 0; i < n; i += 2) {
        ptrs[i] = t_malloc(sizes[i] / 2);
        printf("%d, realloc, %zu, ", n + n / 2 + i / 2, sizes[i] / 2);
        printStats();
    }

    // free everything remaining
    for (int i = 0; i < n; i++) {
        if (ptrs[i])
            t_free(ptrs[i]);
    }
}

// track header overhead vs user data ratio as allocs grow 
static void overheadTest(const char *name, alloc_strat_e pol) {
    printf("\n===== %s =====\n", name);
    t_init(pol);

    printf("%-10s %-12s %-12s %-12s\n", "Allocs", "User (B)", "Overhead (B)", "Ratio");
    printf("%-10s %-12s %-12s %-12s\n", "------", "--------", "------------", "-----");

    void *ptrs[20];
    size_t totUser = 0;

    for (int i = 0; i < 20; i++) {
        size_t sz = (i + 1) * 64;
        ptrs[i] = t_malloc(sz);
        totUser += sz;
        // overhead = num headers * size of one header (sec struct) 
        size_t overhead = (i + 1) * sizeof(sec);
        printf("%-10d %-12zu %-12zu %-12.4f\n", i + 1, totUser, overhead,
               (double)overhead / (totUser + overhead));
    }

    for (int i = 0; i < 20; i++)
        t_free(ptrs[i]);
}

static void correctnessTests() {
    t_init(FIRST_FIT);

    printf("\n===== TEST 1: Single Allocation =====\n");
    void *p1 = t_malloc(100);
    printf("p1 = %p (expect non-null)\n", p1);
    // check 4-byte alignment 
    printf("aligned to 4: %s\n", ((uintptr_t)p1 % 4 == 0) ? "YES" : "NO");
    printStats();

    printf("\n===== TEST 2: Multiple Allocations =====\n");
    void *p2 = t_malloc(200);
    void *p3 = t_malloc(300);
    printf("p2 = %p, p3 = %p\n", p2, p3);
    printf("p2 < p3: %s\n", p2 < p3 ? "YES" : "NO");
    printStats();

    printf("\n===== TEST 3: Write and Read Back =====\n");
    memset(p1, 0xAB, 100);
    int ok = 1;
    for (int i = 0; i < 100; i++)
        if (((unsigned char *)p1)[i] != 0xAB) { ok = 0; break; }
    printf("Memory read/write intact: %s\n", ok ? "YES" : "NO");

    printf("\n===== TEST 4: Free and Merge =====\n");
    t_free(p2);
    t_free(p3); // p2+p3 should coalesce into one block
    printf("Freed p2 and p3 (expect merge)\n");
    printStats();

    printf("\n===== TEST 5: Free All (expect one big free block) =====\n");
    t_free(p1);
    printStats();

    printf("\n===== TEST 6: Invalid Free (expect error, no crash) =====\n");
    int dummy = 42;
    t_free(&dummy);

    printf("\n===== TEST 7: Double Free (expect error, no crash) =====\n");
    void *p4 = t_malloc(64);
    t_free(p4);
    t_free(p4); // second free should fail gracefully

    printf("\n===== TEST 8: Zero Size Malloc =====\n");
    void *p5 = t_malloc(0);
    printf("t_malloc(0) = %p (expect null)\n", p5);

    printf("\n===== TEST 9: BEST_FIT policy =====\n");
    t_init(BEST_FIT);
    void *b1 = t_malloc(512);
    void *b2 = t_malloc(128);
    void *b3 = t_malloc(256);
    t_free(b1); // leaves 512b hole
    t_free(b3); // leaves 256b hole
    // best fit picks smallest hole that fits, so 200b -> 256b hole 
    void *b4 = t_malloc(200);
    printf("b4 should be near b3 region: b3=%p b4=%p\n", b3, b4);
    t_free(b2);
    t_free(b4);

    printf("\n===== TEST 10: WORST_FIT policy =====\n");
    t_init(WORST_FIT);
    void *w1 = t_malloc(512);
    void *w2 = t_malloc(128);
    void *w3 = t_malloc(256);
    t_free(w1); // leaves 512b hole
    t_free(w3); // leaves 256b hole
    // worst fit picks largest hole, so 200b -> 512b hole 
    void *w4 = t_malloc(200);
    printf("w4 should be near w1 region: w1=%p w4=%p\n", w1, w4);
    t_free(w2);
    t_free(w4);
}

int main() {
    correctnessTests();

    runBench("BENCHMARK: FIRST_FIT", FIRST_FIT);
    runBench("BENCHMARK: BEST_FIT", BEST_FIT);
    runBench("BENCHMARK: WORST_FIT", WORST_FIT);

    utilTest("UTILIZATION OVER TIME: FIRST_FIT", FIRST_FIT);
    utilTest("UTILIZATION OVER TIME: BEST_FIT", BEST_FIT);
    utilTest("UTILIZATION OVER TIME: WORST_FIT", WORST_FIT);

    overheadTest("OVERHEAD: FIRST_FIT", FIRST_FIT);
    overheadTest("OVERHEAD: BEST_FIT", BEST_FIT);
    overheadTest("OVERHEAD: WORST_FIT", WORST_FIT);

    return 0;
}