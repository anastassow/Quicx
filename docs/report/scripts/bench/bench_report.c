/*
bench_report — merges the raw histograms written by each producer process
and prints the combined latency distribution.

merging the histograms rather than averaging each process's percentiles
matters: an average of p99s is not the p99 of the whole run.
*/

#include "bench_common.h"

static void print_row(const char* label, double us) {
    if (us >= 1000.0) printf("  %-8s %10.2f ms\n", label, us / 1000.0);
    else              printf("  %-8s %10.1f us\n", label, us);
}

static void print_bar(uint64_t n, uint64_t max) {
    int width = max ? (int)((double)n / (double)max * 40.0) : 0;
    for (int i = 0; i < width; i++) putchar('#');
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: bench_report [--summary] FILE [FILE...]\n");
        return 1;
    }

    int summary_only = 0;

    Hist total;
    hist_init(&total);

    int loaded = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--summary")) { summary_only = 1; continue; }

        Hist h;
        if (hist_read(&h, argv[i]) != 0) {
            fprintf(stderr, "bench_report: cannot read %s\n", argv[i]);
            continue;
        }
        hist_merge(&total, &h);
        loaded++;
    }

    if (!loaded || total.count == 0) {
        fprintf(stderr, "bench_report: no samples\n");
        return 1;
    }

    if (summary_only) {
        printf("p50=%.1f p90=%.1f p99=%.1f p999=%.1f max=%.1f samples=%llu\n",
               hist_pct(&total, 50.0), hist_pct(&total, 90.0),
               hist_pct(&total, 99.0), hist_pct(&total, 99.9),
               (double)total.max_ns / 1000.0,
               (unsigned long long)total.count);
        return 0;
    }

    printf("\nend-to-end latency  (submit due -> DONE received)\n");
    printf("  samples  %10llu   from %d producer process%s\n",
           (unsigned long long)total.count, loaded, loaded == 1 ? "" : "es");
    printf("\n");

    print_row("min",   (double)total.min_ns / 1000.0);
    print_row("mean",  (double)total.sum_ns / (double)total.count / 1000.0);
    print_row("p50",   hist_pct(&total, 50.0));
    print_row("p90",   hist_pct(&total, 90.0));
    print_row("p99",   hist_pct(&total, 99.0));
    print_row("p99.9", hist_pct(&total, 99.9));
    print_row("max",   (double)total.max_ns / 1000.0);

    if (total.overflow) {
        printf("\n  %llu sample%s above 65ms landed in the overflow bucket;\n"
               "  percentiles at or beyond that point are lower bounds.\n",
               (unsigned long long)total.overflow,
               total.overflow == 1 ? "" : "s");
    }

    /* coarse shape of the distribution, one decade per row */
    printf("\ndistribution\n");
    static const struct { const char* label; uint32_t lo, hi; } bands[] = {
        { "  <10us", 0,     10    },
        { " 10-50us", 10,   50    },
        { "50-100us", 50,   100   },
        { "100-500us", 100, 500   },
        { "0.5-1ms", 500,   1000  },
        { "  1-5ms", 1000,  5000  },
        { " 5-25ms", 5000,  25000 },
        { "25-65ms", 25000, 65536 },
    };

    uint64_t counts[8] = {0};
    uint64_t maxc = 0;
    for (int b = 0; b < 8; b++) {
        for (uint32_t i = bands[b].lo; i < bands[b].hi && i < HIST_BUCKETS; i++)
            counts[b] += total.bucket[i];
        if (counts[b] > maxc) maxc = counts[b];
    }
    if (total.overflow > maxc) maxc = total.overflow;

    for (int b = 0; b < 8; b++) {
        printf("  %-10s %7.3f%%  ", bands[b].label,
               (double)counts[b] / (double)total.count * 100.0);
        print_bar(counts[b], maxc);
        printf("\n");
    }
    if (total.overflow) {
        printf("  %-10s %7.3f%%  ", "  >65ms",
               (double)total.overflow / (double)total.count * 100.0);
        print_bar(total.overflow, maxc);
        printf("\n");
    }
    printf("\n");

    return 0;
}
