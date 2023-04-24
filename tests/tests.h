#ifndef TESTS_H
#define TESTS_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>
#include <assert.h>
#include "../xv.h"

static bool nameless_tests = false;

static long nallocs = 0;
static long ntotalsize = 0;

static bool rand_alloc_fail = false;
// 1 in 10 chance malloc or realloc will fail.
static int rand_alloc_fail_odds = 10; 
static bool sysalloc = false;

static void *xmalloc(size_t size) {
    if (sysalloc) {
        return malloc(size);
    }
    if (rand_alloc_fail && rand()%rand_alloc_fail_odds == 0) {
        return NULL;
    }
    void *ptr = malloc(sizeof(size_t)+size);
    assert(ptr);
    nallocs++;
    *((size_t*)ptr) = size;
    ntotalsize += (long)*((size_t*)ptr);
    return (char*)ptr+sizeof(size_t);
}

static void *xrealloc(void *ptr, size_t size) {
    if (sysalloc) {
        return realloc(ptr, size);
    }
    if (!ptr) return xmalloc(size);
    if (rand_alloc_fail && rand()%rand_alloc_fail_odds == 0) {
        return NULL;
    }
    ptr = ((char*)ptr) - sizeof(size_t);
    ntotalsize -= (long)*((size_t*)ptr);
    ptr = realloc(ptr, sizeof(size_t)+size);
    assert(ptr);
    *((size_t*)ptr) = size;
    ntotalsize += (long)*((size_t*)ptr);
    return (char*)ptr+sizeof(size_t);
}

static void xfree(void *ptr) {
    if (sysalloc) {
        free(ptr);
        return;
    }
    if (!ptr) return;
    ptr = ((char*)ptr) - sizeof(size_t);
    ntotalsize -= (long)*((size_t*)ptr);
    nallocs--;
    free(ptr);
}

double now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec*1e9 + now.tv_nsec) / 1e9;
}

void seedrand(void) {
    uint64_t seed = 0;
    FILE *urandom = fopen("/dev/urandom", "r");
    assert(urandom);
    assert(fread(&seed, sizeof(uint64_t), 1, urandom));
    fclose(urandom);
    srand(seed);
}

#define cleanup() { \
    xv_cleanup(); \
    assert(nallocs == 0); \
    assert(ntotalsize == 0); \
    struct xv_memstats memstats = xv_memstats(); \
    assert(memstats.heap_size == 0); \
    assert(memstats.heap_allocs == 0); \
    assert(memstats.thread_total_size > 0); \
    assert(memstats.thread_allocs == 0); \
    assert(memstats.thread_size == 0); \
}

#define do_test(name) { \
    if (argc < 2 || strstr(#name, argv[1])) { \
        if (!nameless_tests) printf("%s\n", #name); \
        xv_set_allocator(xmalloc, xrealloc, xfree); \
        seedrand(); \
        name(); \
        cleanup(); \
    } \
}

#define do_chaos_test(name) { \
    if (argc < 2 || strstr(#name, argv[1])) { \
        if (!nameless_tests) printf("%s\n", #name); \
        rand_alloc_fail = true; \
        xv_set_allocator(xmalloc, xrealloc, xfree); \
        seedrand(); \
        name(); \
        cleanup(); \
        rand_alloc_fail = false; \
    } \
}

#define do_sysalloc_test(name) { \
    if (argc < 2 || strstr(#name, argv[1])) { \
        if (!nameless_tests) printf("%s\n", #name); \
        sysalloc = true; \
        xv_set_allocator(xmalloc, xrealloc, xfree); \
        seedrand(); \
        name(); \
        cleanup(); \
        sysalloc = false; \
    } \
}

#endif // TESTS_H
