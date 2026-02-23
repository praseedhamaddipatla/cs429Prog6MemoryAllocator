#include "tdmm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

sec *frH = NULL;    // free head
sec *allocH = NULL; // allocated head

void *mStart = NULL; // memory start
size_t mSize = 0;    // memory size

alloc_strat_e currPol;

size_t totMap = 0;
size_t totAlloc = 0;
size_t totOh = 0;
double utilSum = 0;
size_t utilCount = 0;

// get ptr to tag at end of block
size_t *tag(sec *s) {
    return (size_t *)((char *)(s + 1) + s->size - sizeof(size_t));
}

// call when size or free changes
void setTag(sec *s) {
    *tag(s) = s->size;
}

// init globals
void t_init(alloc_strat_e pol) {

	frH = NULL;
    allocH = NULL;

    if (mStart != NULL && mStart != MAP_FAILED) {
        munmap(mStart, mSize);
    }

    // null ptrs before remapping
    mStart = NULL;
    frH = NULL;
    allocH = NULL;
    mSize = 0;

    size_t pgSize = getpagesize();
    size_t requested = 64 * 1024 * 1024; //64 mb but ASK
    mSize = ((requested + pgSize - 1) / pgSize) * pgSize;

    mStart = mmap(NULL, mSize, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    if (mStart == MAP_FAILED) {
        fprintf(stderr, "mmap failed\n");
        exit(1);
    }

	//init free region
    frH = (sec *)mStart;
    frH->free = 1;
    frH->n = NULL;
    frH->p = NULL;
    frH->size = mSize - sizeof(sec);
    setTag(frH);

    allocH = NULL;
    currPol = pol;
    totMap = mSize;
    totAlloc = 0;
    totOh = 0;
    utilSum = 0;
    utilCount = 0;
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

// helper for inserting block sorted by addr
void insert(sec **head, sec *s) {
    if (!s || !head) return;

    // first node with addr > s
    sec *curr = *head;
    sec *prev = NULL;
    while (curr && curr < s) {
        prev = curr;
        curr = curr->n;
    }

    // insert between prev and curr
    s->n = curr;
    s->p = prev;
    if (curr) curr->p = s;
    if (prev) prev->n = s; else *head = s;
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

    if (s->size >= aligned + sizeof(sec) + sizeof(size_t) + 4) {
        sec *new = (sec *)((char *)s + sizeof(sec) + aligned);

        new->size = s->size - aligned - sizeof(sec);
        new->free = 1;
        new->n = NULL;
        new->p = NULL;

        s->size = aligned;

        setTag(s);
        setTag(new);

        // insert remaining into free
        insert(&frH, new);
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

void *t_malloc(size_t size) {
	if(size==0) return NULL;
    size_t aligned = align(size);
    sec *found = NULL;

    // choose policy
    if (currPol == FIRST_FIT) {
        found = findFirst(aligned);
    } else if (currPol == BEST_FIT) {
        found = findBest(aligned);
    } else if (currPol == WORST_FIT) {
        found = findWorst(aligned);
    } else {
        fprintf(stderr, "policy undefined");
        exit(1);
    }
    if (found == NULL) {
        fprintf(stderr, "t_malloc failed");
        return NULL;
    }
    split(found, aligned);

    // mark allocated
    found->free = 0;
    setTag(found);

    detach(&frH, found);
    insert(&allocH, found);

    totAlloc += found->size;
    totOh += sizeof(sec);
    utilSum += (double)totAlloc / totMap;
    utilCount++;

    // return ptr
    return (void *)(found + 1);
}

// consolidate adj free blocks
void merge(sec *s) {
    // merge next
    sec *next = (sec *)((char *)(s + 1) + s->size);
    if ((char *)next < (char *)mStart + mSize && next->free) {
        s->size += sizeof(sec) + next->size;
        detach(&frH, next);
        setTag(s);
    }

    // merge prev using tag
    if ((char *)s - sizeof(size_t) >= (char *)mStart + sizeof(sec)) {
        size_t *prevTag = (size_t *)s - 1;
        size_t prevSize = *prevTag;

        // sanity check
        if (prevSize == 0 || prevSize > mSize) return;

        sec *prev = (sec *)((char *)s - prevSize - sizeof(sec));

        // bounds check prev header
        if ((char *)prev < (char *)mStart || (char *)prev >= (char *)mStart + mSize) return;

        // verify size match
        if (prev->size != prevSize) return;

        if (prev->free) {
            prev->size += sizeof(sec) + s->size;
            detach(&frH, s);
            setTag(prev);
        }
    }
}

void t_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    sec *block = (sec *)ptr - 1;

    // validate ptr exists
    sec *check = allocH;
    while (check != NULL && check != block)
        check = check->n;
    if (check == NULL) {
        fprintf(stderr, "t_free: invalid or already-freed pointer\n");
        return;
    }

    block->free = 1;
    setTag(block);
    detach(&allocH, block);
    insert(&frH, block);

    totAlloc -= block->size;
    totOh -= sizeof(sec);
    utilSum += (double)totAlloc / totMap;
    utilCount++;

    merge(block);
    printStats();
}

void t_gcollect(void) {
    // use addr of local
    volatile uintptr_t here = 0;
    uintptr_t scanLo = ((uintptr_t)&here) & ~(sizeof(void *) - 1);
    uintptr_t scanHi = scanLo + 65536;

    // sanity check
    if (scanHi < scanLo || scanHi - scanLo > 1024 * 1024) return;

    sec *block = allocH;
    while (block) {
        sec *next = block->n;
        void *userPtr = (void *)(block + 1);

        int referenced = 0;
        for (uintptr_t addr = scanLo; addr + sizeof(void *) <= scanHi; addr += sizeof(void *)) {
            // skip my heap region
            if (addr >= (uintptr_t)mStart && addr < (uintptr_t)mStart + mSize)
                continue;
            void *candidate = *(void **)addr;
            if (candidate == userPtr) {
                referenced = 1;
                break;
            }
        }

        if (!referenced)
            t_free(userPtr);

        block = next;
    }
}