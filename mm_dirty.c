#include "mig_mon.h"
#include <signal.h>

#define N_1M (1024 * 1024)

struct thread_info {
    unsigned char *buf;
    unsigned long pages;
};

static void prefault_range(unsigned char *buf, unsigned long pages)
{
    unsigned long index = 0;

    while (index < pages) {
        *(buf) = 1;
        buf = (unsigned char *)((unsigned long)buf + page_size);

        /* Each 1GB for 4K page size, print a dot */
        if (++index % (256 * 1024) == 0) {
            printf(".");
            fflush(stdout);
        }
    }
}

static void *prefault_thread(void *data)
{
    struct thread_info *info = data;

    prefault_range(info->buf, info->pages);

    return NULL;
}

static void prefault_memory(unsigned char *buf, unsigned long pages)
{
    unsigned long each = pages / n_cpus;
    unsigned long left = pages % n_cpus;
    pthread_t *threads = calloc(n_cpus, sizeof(pthread_t));
    struct thread_info *infos = calloc(n_cpus, sizeof(struct thread_info));
    int i, ret;

    assert(threads);

    for (i = 0; i < n_cpus; i++) {
        struct thread_info *info = infos + i;
        pthread_t *thread = threads + i;

        info->buf = buf + each * page_size * i;
        info->pages = each;
        ret = pthread_create(thread, NULL, prefault_thread, info);
        assert(ret == 0);
    }

    if (left) {
        prefault_range(buf + each * page_size * n_cpus, left);
    }

    for (i = 0; i < n_cpus; i++) {
        ret = pthread_join(threads[i], NULL);
        assert(ret == 0);
    }
    printf("done\n");
}

static bool record_latencies;
/* 1us, 2us, 4us, ..., 1024us, 2048us, ..., 1048576us */
#define  BUCKET_SIZE  21
static uint64_t ts_bucket[BUCKET_SIZE];

static void mm_dirty_sig_handler(int sig)
{
    if (record_latencies) {
        unsigned int i, n = BUCKET_SIZE;

        puts("\nMemory Latencies:\n");
        for (i = 0; i < n; i++) {
            printf("%12u (us): %lu\n", 1<<i, ts_bucket[i]);
        }
        puts("");
    }
    exit(0);
}

int mon_mm_dirty(mm_dirty_args *args)
{
    unsigned char *mm_ptr, *mm_buf, *mm_end;
    /*
     * Prefault with 1, to skip migration zero detection, so the next value to
     * set is 2.
     */
    unsigned char cur_val = 2;
    long pages_per_mb = N_1M / page_size;
    uint64_t time_iter, time_now;
    uint64_t sleep_ms = 0, elapsed_ms;
    uint64_t ts_start = 0, ts_lat;
    unsigned long dirtied_mb = 0, mm_npages;
    dirty_pattern pattern = args->pattern;
    unsigned int map_flags = args->map_flags;
    long dirty_rate = args->dirty_rate;
    long mm_size = args->mm_size;
    struct sigaction sigact = { 0 };
    float speed;
    int i;

    record_latencies = args->record_latencies;

    sigact.sa_handler = mm_dirty_sig_handler;
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGINT, &sigact, NULL);

    mm_buf = mmap(NULL, mm_size * N_1M, PROT_READ | PROT_WRITE,
                  map_flags, -1, 0);
    if (mm_buf == MAP_FAILED) {
        fprintf(stderr, "%s: mmap() failed\n", __func__);
        return -1;
    }

    printf("Binary version: \t%s\n", MIG_MON_VERSION);
    printf("Test memory size: \t%ld (MB)\n", mm_size);
    printf("Backend page size: \t%ld (Bytes)\n", huge_page_size);
    printf("Dirty step size: \t%ld (Bytes)\n", page_size);
    if (dirty_rate) {
        printf("Dirty memory rate: \t%ld (MB/s)\n", dirty_rate);
    } else {
        printf("Dirty memory rate: \tMaximum\n");
    }
    printf("Dirty pattern: \t\t%s\n", pattern_str[pattern]);
    printf("Recording latencies: \t%s\n", record_latencies ? "yes" : "no");

    mm_ptr = mm_buf;
    mm_end = mm_buf + mm_size * N_1M;
    mm_npages = (unsigned long) ((mm_end - mm_ptr) / page_size);
    time_iter = get_msec();

    puts("+------------------------+");
    puts("|   Prefault Memory      |");
    puts("+------------------------+");
    prefault_memory(mm_buf, mm_npages);

    if (pattern == PATTERN_ONCE) {
        puts("[Goes to sleep; please hit ctrl-c to stop this program]");
        while (1) {
            sleep(1000);
        }
    }

    puts("+------------------------+");
    puts("|   Start Dirty Memory   |");
    puts("+------------------------+");

    while (1) {
        /* Dirty in MB unit */
        for (i = 0; i < pages_per_mb; i++) {
            if (record_latencies)
                ts_start = get_usec();
            if (pattern == PATTERN_SEQ) {
                /* Validate memory if not the first round */
                unsigned char target = cur_val - 1;

                if (*mm_ptr != target) {
                    fprintf(stderr, "%s: detected corrupted memory (%d != %d)!\n",
                            __func__, *mm_ptr, target);
                    exit(-1);
                }
                *mm_ptr = cur_val;
                mm_ptr += page_size;
            } else if (pattern == PATTERN_RAND) {
                /* Write something to a random page upon the range */
                unsigned long rand = random() % mm_npages;

                *(mm_buf + rand * page_size) = cur_val++;
            } else {
                assert(0);
            }
            if (record_latencies) {
                unsigned int index;

                ts_lat = get_usec() - ts_start;
                /*
                 * This puts the latency value into the bucket with index.
                 * E.g., 3us will be put into <4us bucket (index=2).  So
                 * it's not really accurate but just to show a pattern of
                 * the latencies.
                 *
                 * If it goes over 1sec, always put into 1sec bucket.
                 */
                if (ts_lat == 0)
                    index = 0;
                else
                    index = 64 - __builtin_clzll(ts_lat);
                printf("latency: %lu, index: %d\n", ts_lat, index);
                if (index > (sizeof(ts_bucket) - 1))
                    index = (sizeof(ts_bucket) - 1);
                ts_bucket[index]++;
            }
        }
        if (pattern == PATTERN_SEQ && mm_ptr + N_1M > mm_end) {
            mm_ptr = mm_buf;
            cur_val++;
        }
        dirtied_mb++;
        if (dirty_rate && dirtied_mb >= dirty_rate) {
            /*
             * We have dirtied enough, wait for a while until we reach
             * the next second.
             */
            sleep_ms = 1000 - get_msec() + time_iter;
            if (sleep_ms > 0) {
                usleep(sleep_ms * 1000);
            }
            while (get_msec() - time_iter < 1000);
        }
        time_now = get_msec();
        elapsed_ms = time_now - time_iter;
        if (elapsed_ms >= 1000) {
            speed = 1.0 * dirtied_mb / elapsed_ms * 1000;
            printf("Dirty rate: %.0f (MB/s), duration: %"PRIu64" (ms), "
                   "load: %.2f%%\n", speed, elapsed_ms,
                   100.0 * (elapsed_ms - sleep_ms) / elapsed_ms);
            time_iter = time_now;
            sleep_ms = 0;
            dirtied_mb = 0;
        }
    }

    /* Never reached */
    return 0;
}

void usage_mm_dirty_short(void)
{
    puts("");
    printf("       %s mm_dirty [options...]\n", prog_name);
    printf("       \t -h: \tDump help message for mm_dirty sub-cmd\n");
    printf("       \t -m: \tMemory size in MB (default: %d)\n", DEF_MM_DIRTY_SIZE);
    printf("       \t -r: \tDirty rate in MB/s (default: unlimited)\n");
    printf("       \t -p: \tWork pattern: \"sequential\", \"random\", or \"once\"\n");
    printf("       \t\t(default: \"%s\")\n", pattern_str[DEF_MM_DIRTY_PATTERN]);
    printf("       \t -L: \tRecord and report memory access latencies\n");
    printf("       \t -P: \tPage size: \"2m\" or \"1g\" for huge pages\n");
}

void usage_mm_dirty(void)
{
    puts("");
    puts("Usage:");
    usage_mm_dirty_short();
    puts("");
    puts("======== Memory Dirty Workload ========");
    puts("");
    puts("This sub-tool can also generate dirty memory workload in different ways.");
    puts("");
    puts("Example 1: generate 100MB/s random dirty workload upon 500MB memory using:");
    puts("");
    printf("  %s mm_dirty -m 500M -r 100M -p random\n", prog_name);
    puts("");
    puts("Example 2: dirty 10GB memory then keep idle after dirtying:");
    puts("");
    printf("  %s mm_dirty -m 10G -p once\n", prog_name);
    puts("");
    puts("Example 3: dirty 1GB memory, record and report memory access latencies when quit:");
    puts("");
    printf("  %s mm_dirty -m 10G -L\n", prog_name);
    puts("");
}
