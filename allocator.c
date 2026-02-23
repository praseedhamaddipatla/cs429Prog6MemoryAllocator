#define _POSIX_C_SOURCE 199309L

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef struct sec {
    size_t size;
    int free;
    struct sec *n; // next
    struct sec *p; // prev
} sec;

typedef enum policy { FIRST, BEST, WORST } policy;

sec *frH = NULL;    // free head
sec *allocH = NULL; // allocated head

void *mStart = NULL; // memory start
size_t mSize = 0;    // memory size

policy currPol;

size_t totMap = 0;
size_t totAlloc = 0;
size_t totOh = 0;
double utilSum = 0;
size_t utilCount = 0;

// init globals
int tinit(size_t size, policy pol) {
    size_t pgSize = getpagesize();
    mSize = ((size + pgSize - 1) / pgSize) * pgSize;

    mStart = mmap(NULL, mSize, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    if (mStart == MAP_FAILED) {
        fprintf(stderr, "mmap failed");
        exit(1);
    }

    // init free block
    frH = (sec *)mStart;
    frH->free = 1;
    frH->n = NULL;
    frH->p = NULL;
    frH->size = mSize - sizeof(sec);

    allocH = NULL;
    currPol = pol;
    totMap = mSize;

    totAlloc = 0;
    totOh = 0;
    utilSum = 0;
    utilCount = 0;

    return 0;
}

// helper for removing block
void detach(sec **head, sec *s) {
    if (!s || !head)
        return;

    if (s->p)
        s->p->n = s->n;
    else
        *head = s->n;

    if (s->n)
        s->n->p = s->p;

    s->n = NULL;
    s->p = NULL;
}

// helper for inserting block
void insert(sec **head, sec *s) {
    if (s == NULL || head == NULL)
        return;
    s->n = *head;
    s->p = NULL;

    if (*head != NULL) {
        (*head)->p = s;
    }

    *head = s;
}

size_t align(size_t size) {
    if (size % 4 == 0) {
        return size;
    }
    return ((size + 3) / 4) * 4;
}

sec *findFirst(size_t size) {
    sec *search = frH;
    while (search != NULL) {
        if (search->size >= size) {
            return search;
        }
        search = search->n;
    }
    fprintf(stderr, "free block not found");
    return NULL;
}

sec *findBest(size_t size) {
    sec *search = frH;
    sec *best = NULL;
    size_t diff = SIZE_MAX;

    while (search != NULL) {
        if (search->size >= size) {

            size_t curr_diff = search->size - size;

            if (best == NULL || curr_diff < diff) {
                best = search;
                diff = curr_diff;
            }
        }
        search = search->n;
    }

    if (best == NULL) {
        fprintf(stderr, "free block not found");
    }

    return best;
}

sec *findWorst(size_t size) {
    sec *search = frH;
    sec *worst = NULL;
    size_t diff = 0;

    while (search != NULL) {

        if (search->size >= size) {

            size_t curr_diff = search->size - size;

            if (worst == NULL || curr_diff > diff) {
                worst = search;
                diff = curr_diff;
            }
        }

        search = search->n;
    }

    if (worst == NULL) {
        fprintf(stderr, "free block not found");
    }

    return worst;
}

void split(sec *s, size_t size) {
    size_t aligned = align(size);

    if (s->size >= aligned + sizeof(sec) + 4) {
        sec *new = (sec *)((char *)s + sizeof(sec) + aligned);

        new->size = s->size - aligned - sizeof(sec);
        new->free = 1;
        new->n = s->n;
        new->p = s;

        if (s->n != NULL)
            s->n->p = new;

        s->n = new;
        s->size = aligned;
    }
}

void printStats() {
    printf("Mapped: %zu\n", totMap);
    printf("Allocated: %zu\n", totAlloc);
    printf("Utilization: %.2f%%\n", 100.0 * totAlloc / totMap);
    printf("Overhead: %zu\n", totOh);
    if (utilCount > 0)
        printf("Avg Utilization: %.4f%%\n", 100.0 * utilSum / utilCount);
}

void *tmalloc(size_t size) {
    size_t aligned = align(size);
    sec *found = NULL;

    // choose policy
    if (currPol == FIRST) {
        found = findFirst(aligned);
    } else if (currPol == BEST) {
        found = findBest(aligned);
    } else if (currPol == WORST) {
        found = findWorst(aligned);
    } else {
        fprintf(stderr, "policy undefined");
        exit(1);
    }
    if (found == NULL) {
        fprintf(stderr, "tmalloc failed");
        exit(1);
    }
    split(found, aligned);

    // mark allocated
    found->free = 0;

    detach(&frH, found);
    insert(&allocH, found);

    totAlloc += found->size;
    totOh += sizeof(sec);
    utilSum += (double)totAlloc / totMap;
    utilCount++;

    // return pointer
    return (void *)(found + 1);

}

void merge(sec *s) {
    // merge next
    sec *next = (sec *)((char *)s + sizeof(sec) + s->size);
    if ((char *)next < (char *)mStart + mSize && next->free) {
        s->size += sizeof(sec) + next->size;
        detach(&frH, next); // detach next
    }

    // -merge prev
    sec *curr = frH;
    while (curr != NULL) {
        if (curr != s && (char *)curr + sizeof(sec) + curr->size == (char *)s) {
            curr->size += sizeof(sec) + s->size;
            detach(&frH, s);
            break;
        }
        curr = curr->n;
    }
}

void tfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    sec *block = (sec *)ptr - 1;

    // validate pointer exists
    sec *check = allocH;
    while (check != NULL && check != block)
        check = check->n;
    if (check == NULL) {
        fprintf(stderr, "tfree: invalid or already-freed pointer\n");
        return;
    }

    block->free = 1;
    detach(&allocH, block);
    insert(&frH, block);

    totAlloc -= block->size;
    totOh -= sizeof(sec);
    utilSum += (double)totAlloc / totMap;
    utilCount++;

    merge(block);
    printStats();
}

void benchmark() {
    size_t sizes[] = {1,     4,     16,     64,      256,     1024,   4096,
                      16384, 65536, 262144, 1048576, 4194304, 8388608};
    int n = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < n; i++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        void *p = tmalloc(sizes[i]);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ns =
            (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
        printf("tmalloc(%7zu): %ld ns\n", sizes[i], ns);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        tfree(p);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
        printf(" tfree(%7zu): %ld ns\n", sizes[i], ns);
    }
}

// debug--remove later
void printFr() {
    sec *curr = frH;
    while (curr) {
        printf("Section at %p with size %zu and free state %d\n", curr,
               curr->size, curr->free);
        curr = curr->n;
    }
}