#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stddef.h>
#include <time.h>

typedef struct sec {
    size_t size;
    int free;
    struct sec *n;
    struct sec *p;
} sec;

typedef enum policy { FIRST, BEST, WORST } policy;

// allocator functions
int tinit(size_t size, policy pol);
void *tmalloc(size_t size);
void tfree(void *ptr);
void printFr();
void printStats();
void benchmark();


static void section(const char *title) {
    printf("\n===== %s =====\n", title);
}

// benchmark sweep
static void run_benchmark(const char *name, policy pol) {
    printf("\n--- Benchmark: %s ---\n", name);
    // large allocation
    tinit(32 * 1024 * 1024, pol);
    benchmark();
    printStats();
}

// correctness (first fit)

static void correctness_tests() {
    tinit(1024 * 1024, FIRST);

    section("TEST 1: Initialization");
    printFr();

    section("TEST 2: Single Allocation");
    void *p1 = tmalloc(100);
    printf("Allocated p1 = %p\n", p1);
    printFr();
    printStats();

    section("TEST 3: Multiple Allocations");
    void *p2 = tmalloc(200);
    void *p3 = tmalloc(300);
    printf("Allocated p2 = %p\n", p2);
    printf("Allocated p3 = %p\n", p3);
    printFr();
    printStats();

    section("TEST 4: Free One Block");
    tfree(p2);
    printf("Freed p2\n");
    printFr();
    printStats();

    section("TEST 5: Free Adjacent Block (tests merge)");
    tfree(p3);
    printf("Freed p3 (should merge with p2 block)\n");
    printFr();
    printStats();

    section("TEST 6: Free All");
    tfree(p1);
    printf("Freed p1 (should merge everything into one big block)\n");
    printFr();
    printStats();

    section("TEST 7: Invalid Free (should print error, not crash)");
    int dummy = 42;
    tfree(&dummy); // not from allocator

    section("TEST 8: Double Free (should print error, not crash)");
    void *p6 = tmalloc(64);
    tfree(p6);
    tfree(p6); // already freed

    section("TEST 9: BEST policy");
    // re-init
    tinit(1024 * 1024, BEST);
    void *b1 = tmalloc(512);
    void *b2 = tmalloc(128);
    void *b3 = tmalloc(256);
    tfree(b1); // free 512-byte hole
    tfree(b3); // free 256-byte hole
    // should pick the 256-byte
    void *b4 = tmalloc(200);
    printf("BEST: b4=%p (should be closer to b3 than b1)\n", b4);
    printFr();
    tfree(b2);
    tfree(b4);

    section("TEST 10: WORST policy");
    tinit(1024 * 1024, WORST);
    void *w1 = tmalloc(512);
    void *w2 = tmalloc(128);
    void *w3 = tmalloc(256);
    tfree(w1); // free 512-byte hole
    tfree(w3); // free 256-byte hole
    // should pick the 512-byte
    void *w4 = tmalloc(200);
    printf("WORST: w4=%p (should be closer to w1 than w3)\n", w4);
    printFr();
    tfree(w2);
    tfree(w4);

    section("CORRECTNESS TESTS COMPLETE");
}

int main() {
    correctness_tests();

    // benchmark under all three policies
    run_benchmark("FIRST fit", FIRST);
    run_benchmark("BEST fit",  BEST);
    run_benchmark("WORST fit", WORST);

    return 0;
}