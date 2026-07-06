/*
 * Microbenchmark for the unique table (llmsset).
 *
 * Phase A: parallel insert of N unique nodes into a pre-sized table (raw insert throughput)
 * Phase B: parallel lookup of the same N nodes (hit throughput)
 * Phase C: same N inserts into a table starting at 2^20 buckets, growing on demand
 *          (amortized cost of dynamic growth, per-grow latency reported)
 *
 * Usage: bench_table [workers] [n_millions]
 */

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

#include <sylvan_int.h>
#include "getrss.h"

static double
wctime()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1E-9 * ts.tv_nsec;
}

static inline uint64_t
splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EB;
    return x ^ (x >> 31);
}

/* insert or look up keys [first, first+count); returns number of failures (table full) */
TASK_3(uint64_t, bench_range, uint64_t, first, uint64_t, count, uint64_t*, created_count)
{
    if (count > 4096) {
        SPAWN(bench_range, first, count/2, created_count);
        uint64_t right = CALL(bench_range, first + count/2, count - count/2, created_count);
        return right + SYNC(bench_range);
    }
    uint64_t failed = 0, created_here = 0;
    for (uint64_t i = first; i < first + count; i++) {
        int created;
        /* arbitrary unique 16-byte keys; the table stores them verbatim */
        uint64_t a = splitmix64(2*i), b = splitmix64(2*i+1);
        if (llmsset_lookup(nodes, a, b, &created) == 0) failed++;
        else if (created) created_here++;
    }
    if (created_here) __sync_fetch_and_add(created_count, created_here);
    return failed;
}

/* grow the table like garbage collection does (world already stopped here) */
static void
grow_table(size_t new_size)
{
    double t1 = wctime();
    llmsset_set_size(nodes, new_size);
    llmsset_clear_hashes(nodes);
    if (llmsset_rehash(nodes) != 0) {
        fprintf(stderr, "rehash failed after grow!\n");
        exit(1);
    }
    printf("    grow to 2^%d: %.1f ms\n", __builtin_ctzll(new_size), 1000*(wctime()-t1));
}

int
main(int argc, char** argv)
{
    int workers = argc > 1 ? atoi(argv[1]) : 4;
    uint64_t n = (argc > 2 ? (uint64_t)atoll(argv[2]) : 16) * 1000000;

    lace_start(workers, 1000000);

    /* Phase A+B: pre-sized table, load factor <= 50% */
    size_t presize = 1;
    while (presize < 2*n) presize *= 2;
    nodes = llmsset_create(presize, presize);

    uint64_t created = 0;
    double t1 = wctime();
    uint64_t failed = RUN(bench_range, 0, n, &created);
    double ta = wctime() - t1;
    printf("A: insert %" PRIu64 "M unique into 2^%d table: %.3f s (%.1f Mops/s), %" PRIu64 " created, %" PRIu64 " failed\n",
           n/1000000, __builtin_ctzll(presize), ta, n/ta/1e6, created, failed);

    created = 0;
    t1 = wctime();
    failed = RUN(bench_range, 0, n, &created);
    double tb = wctime() - t1;
    printf("B: lookup %" PRIu64 "M existing:            %.3f s (%.1f Mops/s), %" PRIu64 " created, %" PRIu64 " failed\n",
           n/1000000, tb, n/tb/1e6, created, failed);
    llmsset_free(nodes);

    /* Phase C: start small, grow on demand up to max */
    size_t max_size = presize * 4;
    nodes = llmsset_create(1 << 20, max_size);
    created = 0;
    uint64_t done = 0;
    t1 = wctime();
    while (done < n) {
        /* insert in slices; slice fits current table to limit wasted retries */
        uint64_t slice = llmsset_get_size(nodes) / 4;
        if (slice > n - done) slice = n - done;
        failed = RUN(bench_range, done, slice, &created);
        if (failed) grow_table(llmsset_get_size(nodes) * 2);
        else done += slice;
    }
    double tc = wctime() - t1;
    printf("C: insert %" PRIu64 "M growing 2^20 -> 2^%d:  %.3f s (%.1f Mops/s), %" PRIu64 " created\n",
           n/1000000, __builtin_ctzll(llmsset_get_size(nodes)), tc, n/tc/1e6, created);
    printf("   phase C / phase A slowdown: %.2fx\n", tc/ta);
    printf("peak RSS: %zu MB\n", getPeakRSS() / (1024*1024));

    llmsset_free(nodes);
    lace_stop();
    return 0;
}
