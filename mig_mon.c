#include "mig_mon.h"

static const char *pattern_str[PATTERN_NUM] = { "sequential", "random", "once" };
static const char *prog_name = NULL;
static long n_cpus;
short mig_mon_port = MIG_MON_PORT;
/*
 * huge_page_size stands for the real page size we used.  page_size will always
 * be the smallest page size of the system, as that's the size that guest
 * hypervisor will track dirty.
 */
static long page_size, huge_page_size;

void usage_downtime(void)
{
    puts("");
    puts("======== VM Migration Downtime Measurement ========");
    puts("");
    puts("This is a program that could be used to measure");
    puts("VM migration down time. Please specify work mode.");
    puts("");
    puts("Example usage to measure guest server downtime (single way):");
    puts("");
    printf("1. [on guest]  start server using '%s server /tmp/spike.log'\n",
           prog_name);
    printf("   this will start server, log all spikes into spike.log.\n");
    printf("2. [on client] start client using '%s client GUEST_IP 50'\n",
           prog_name);
    printf("   this starts sending UDP packets to server, interval 50ms.\n");
    printf("3. trigger loop migration (e.g., 100 times)\n");
    printf("4. see the results on server side.\n");
    puts("");
    puts("Example usage to measure round-trip downtime:");
    puts("(This is preferred since it simulates a simplest server behavior)");
    puts("");
    printf("1. [on guest]  start server using '%s server_rr'\n",
           prog_name);
    printf("   this will start a UDP echo server.\n");
    printf("2. [on client] start client using '%s client GUEST_IP 50 spike.log'\n",
           prog_name);
    printf("   this starts sending UDP packets to server, then try to recv it.\n");
    printf("   the timeout of recv() will be 50ms.\n");
    printf("3. trigger loop migration (e.g., 100 times)\n");
    printf("4. see the results on client side.\n");
    puts("");
}

void usage_mm_dirty(void)
{
    puts("======== Memory Dirty Workload ========");
    puts("");
    puts("This sub-tool can also generate dirty memory workload in different ways.");
    puts("");
    puts("Example 1: generate 500MB/s random dirty workload upon 200GB memory using:");
    puts("");
    printf("  %s mm_dirty -m 200000 -r 500 -p random\n", prog_name);
    puts("");
    puts("Example 2: dirty 10GB memory then keep idle after dirtying:");
    puts("");
    printf("  %s mm_dirty -m 10000 -p once\n", prog_name);
    puts("");
}

void usage_vm(void)
{
    puts("======== Emulate VM Live Migrations ========");
    puts("");
    puts("This sub-tool can be used to emulate live migration TCP streams.");
    puts("");
    puts("There're two types of live migration: (1) precopy (2) postcopy.");
    puts("This tool can emulate (1) or (2) or (1+2) case by specifying");
    puts("different '-t' parameters.");
    puts("");
    puts("For precopy stream, it's the bandwidth that matters.  The bandwidth");
    puts("information will be dumped per-second on src VM.");
    puts("");
    puts("For postcopy stream, it's the latency that matters.  The average/maximum");
    puts("latency value of page requests will be dumped per-second on dst VM.");
    puts("");
    puts("Example:");
    puts("");
    puts("To start the (emulated) destination VM, one can run this on dest host:");
    puts("");
    printf("  %s vm -d\n", prog_name);
    puts("");
    puts("Then, to start a src VM emulation and start both live migration streams,");
    puts("one can run this command on src host:");
    puts("");
    printf("  %s vm -s -H $DEST_IP -t precopy -t postcopy\n", prog_name);
    puts("");
    puts("Specifying both '-t' will just enable both migration streams.");
    puts("");
}

void version(void)
{
    printf("Version: %s\n", MIG_MON_VERSION);
    puts("");
}

void usage(void)
{
    puts("");
    puts("This tool is a toolset of VM live migration testing & debugging.");
    puts("For detailed usage, please try '-h/--help'.");
    puts("");
    puts("Usage:");
    printf("       %s [-h|--help]\tShow full help message\n", prog_name);
    puts("");
    printf("       %s server [spike_log]\n", prog_name);
    printf("       %s client server_ip [interval_ms]\n", prog_name);
    printf("       %s server_rr\n", prog_name);
    printf("       %s client_rr server_ip [interval_ms [spike_log]]\n", prog_name);
    puts("");
    printf("       %s mm_dirty [options...]\n", prog_name);
    printf("       \t -m: \tMemory size in MB (default: %d)\n", DEF_MM_DIRTY_SIZE);
    printf("       \t -r: \tDirty rate in MB/s (default: unlimited)\n");
    printf("       \t -p: \tWork pattern: \"sequential\", \"random\", or \"once\"\n");
    printf("       \t\t(default: \"%s\")\n", pattern_str[DEF_MM_DIRTY_PATTERN]);
    printf("       \t -P: \tPage size: \"2m\" or \"1g\" for huge pages\n");
    puts("");
    printf("       %s vm [options...]\n", prog_name);
    printf("       \t -d: \tEmulate a dst VM\n");
    printf("       \t -h: \tDump help message\n");
    printf("       \t -H: \tSpecify dst VM IP (required for -s)\n");
    printf("       \t -p: \tSpecify connect/listen port\n");
    printf("       \t -s: \tEmulate a src VM\n");
    printf("       \t -S: \tSpecify size of the VM (GB)\n");
    printf("       \t -t: \tSpecify tests (precopy, postcopy)\n");
    puts("");
}

dirty_pattern parse_dirty_pattern(const char *str)
{
    int i;

    for (i = 0; i < PATTERN_NUM; i++) {
        if (!strcmp(pattern_str[i], str)) {
            return i;
        }
    }

    fprintf(stderr, "Dirty pattern unknown: %s\n", str);
    exit(1);
}

int parse_huge_page_size(const char *size)
{
#ifdef __linux__
    if (!strcmp(size, "2m") || !strcmp(size, "2M")) {
        huge_page_size = 2UL << 20;
        return MAP_HUGETLB | MAP_HUGE_2MB;
    } else if (!strcmp(size, "1g") || !strcmp(size, "1G")) {
        huge_page_size = 1UL << 30;
        return MAP_HUGETLB | MAP_HUGE_1GB;
    } else if (!strcmp(size, "4k") || !strcmp(size, "4K")) {
        return 0;
    } else {
        printf("Unknown page size (%s), please specify 4K/2M/1G\n", size);
        exit(1);
    }
#else
    printf("Specify page size is not supported on non-Linux arch yet.\n");
    exit(1);
#endif
}

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

static void * prefault_thread(void *data)
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

int mon_mm_dirty(long mm_size, long dirty_rate, dirty_pattern pattern,
                 unsigned map_flags)
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
    unsigned long dirtied_mb = 0, mm_npages;
    float speed;
    int i;

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

#define  DEF_VM_SIZE              (1UL << 40)  /* 1TB */
#define  MAGIC_SEND_PAGE          (0x123)      /* For sending page */
#define  MAGIC_REQ_PAGE           (0x124)      /* For requesting page */
#define  MAGIC_HANDSHAKE          (0x125)      /* For src->dst handshake */

/* These emulates QEMU */
#define  DEF_IO_BUF_SIZE    32768
#define  MAX_IOV_SIZE       64

typedef enum {
    EMULATE_NONE = 0,
    EMULATE_SRC = 1,
    EMULATE_DST = 2,
    EMULATE_NUM,
} emulate_target;

/* If set, will generate precopy live migration stream */
#define  VM_TEST_PRECOPY     (1UL << 0)
/* If set, will generate postcopy page requests */
#define  VM_TEST_POSTCOPY    (1UL << 1)

typedef struct {
    int sock;
    emulate_target target;
    unsigned int tests;
    /* Whether we should quit */
    int quit;
    /* Guest memory size (emulated) */
    uint64_t vm_size;
    /*
     * Both the src/dst VMs have these threads, even if they do not mean the
     * same workload will be run, we share the fields.
     */
    pthread_t sender;
    pthread_t receiver;

    /*
     * Maintaining receiving sockets
     */
    /* Size = DEF_IO_BUF_SIZE */
    char *recv_buffer;
    /* Length of data consumed */
    int recv_cur;
    /* Length of data in recv_buffer */
    int recv_len;

    /*
     * When on src: used to emulate page req queue.
     * When on dst: used to notify when a page req is resolved.
     *
     * Data is page offset (u64), always.
     */
    int page_req_pipe[2];

    union {
        /* Only needed on src VM */
        struct {
            /* Size = MAX_IOV_SIZE * DEF_IO_BUF_SIZE */
            struct iovec *src_iov_buffer;
            /* Dest VM ip */
            const char *src_target_ip;
            /* Points to the current IOV being used */
            int src_cur;
            /* Length of current IOV that has been consumed */
            size_t src_cur_len;
        };
        /* Only needed on dst VM */
        struct {
            /* Current page to request */
            uint64_t dst_current_req;
        };
    };
} vm_args;

typedef struct {
    uint64_t magic;
    uint64_t page_index;
} page_header;

void *mmap_anon(size_t size)
{
    return mmap(NULL, size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/* Init vm_args shared fields for both src/dst */
void vm_args_init_shared(vm_args *args)
{
    char *buf;
    int ret;

    /* Setup receiver socket buffers on both src/dst */
    buf = mmap_anon(DEF_IO_BUF_SIZE);
    assert(buf != MAP_FAILED);

    args->recv_buffer = buf;
    args->recv_cur = 0;
    args->recv_len = 0;

    ret = pipe(args->page_req_pipe);
    assert(ret == 0);
}

void vm_args_init_src(vm_args *args)
{
    struct iovec *iov;
    int i, ret;

    vm_args_init_shared(args);

    /* Only the sender does bulk sending, set it up */
    iov = mmap_anon(sizeof(struct iovec) * MAX_IOV_SIZE);
    assert(iov != MAP_FAILED);

    for (i = 0; i < MAX_IOV_SIZE; i++) {
        iov[i].iov_len = DEF_IO_BUF_SIZE;
        iov[i].iov_base = mmap_anon(DEF_IO_BUF_SIZE);
        assert(iov[i].iov_base != MAP_FAILED);
    }

    if (args->tests & VM_TEST_PRECOPY) {
        /*
         * If src enabled precopy, making the page req channel non-block so
         * it can handle both precopy/postcopy.  Otherwise keep it blocking
         * so if we're only testing postcopy we don't eat up 100% core on
         * src host.
         */
        ret = fcntl(args->page_req_pipe[0], F_SETFL, O_NONBLOCK);
        assert(ret == 0);
    }

    args->src_iov_buffer = iov;
    args->src_cur = 0;
    args->src_cur_len = 0;
}

void vm_args_init_dst(vm_args *args)
{
    vm_args_init_shared(args);

    /* This means, "no request yet" */
    args->dst_current_req = (uint64_t)-1;
}

int sock_write_flush(vm_args *args)
{
    struct iovec *iov = args->src_iov_buffer;
    struct msghdr msg = { NULL };
    int ret;

    /* Limit src_cur IOV to only send partial of its buffer */
    iov[args->src_cur].iov_len = args->src_cur_len;

    msg.msg_iov = iov;
    msg.msg_iovlen = args->src_cur + 1;

retry:
    ret = sendmsg(args->sock, &msg, 0);
    if (ret < 0) {
        if (ret == -EAGAIN || ret == -EINTR)
            goto retry;
        printf("sendmsg() failed: %d\n", ret);
        return ret;
    }

    /* Recover the iov_len field of last IOV */
    iov[args->src_cur].iov_len = DEF_IO_BUF_SIZE;
    /* Free all the IOV buffers by reset the fields */
    args->src_cur = 0;
    args->src_cur_len = 0;

    if (ret == 0)
        return -1;

    return 0;
}

int sock_write(vm_args *args, void *buffer, uint64_t size)
{
    struct iovec *iov = args->src_iov_buffer;
    int ret;

    while (size) {
        size_t to_move;
        void *cur_ptr;

        assert(args->src_cur < MAX_IOV_SIZE);
        assert(args->src_cur_len < DEF_IO_BUF_SIZE);

        /* Every IOV is the same len */
        to_move = DEF_IO_BUF_SIZE - args->src_cur_len;
        to_move = MIN(to_move, size);

        cur_ptr = (void *)((uint64_t)(iov[args->src_cur].iov_base) +
                           args->src_cur_len);
        if (buffer) {
            memcpy(cur_ptr, buffer, to_move);
            buffer = (void *)((uint64_t)buffer + to_move);
        } else {
            bzero(cur_ptr, to_move);
        }

        args->src_cur_len += to_move;
        size -= to_move;

        if (args->src_cur_len >= DEF_IO_BUF_SIZE) {
            assert(args->src_cur_len == DEF_IO_BUF_SIZE);
            args->src_cur++;
            args->src_cur_len = 0;

            if (args->src_cur >= MAX_IOV_SIZE) {
                assert(args->src_cur == MAX_IOV_SIZE);
                /* Flush all the data in the iovec */
                ret = sock_write_flush(args);
                if (ret)
                    return ret;
            }
        }
    }

    return 0;
}

int sock_read_refill(vm_args *args)
{
    int ret;

    /* Make sure we've consumed all */
    assert(args->recv_cur == args->recv_len);
retry:
    ret = read(args->sock, args->recv_buffer, DEF_IO_BUF_SIZE);
    if (ret < 0)
        ret = -errno;
    if (ret == -EAGAIN || ret == -EINTR)
        goto retry;
    if (ret == -ECONNRESET || ret == 0) {
        printf("Connection reset\n");
        return -1;
    }
    if (ret < 0) {
        printf("%s: ret==%d\n", __func__, ret);
        return -1;
    }

    args->recv_len = ret;
    args->recv_cur = 0;

    return 0;
}

int sock_read(vm_args *args, void *buf, uint64_t size)
{
    int len;

    while (size) {
        /* Out of data in the buffer, refill */
        if (args->recv_cur >= args->recv_len) {
            assert(args->recv_cur == args->recv_len);
            len = sock_read_refill(args);
            if (len < 0)
                return len;
        }

        len = args->recv_len - args->recv_cur;
        len = MIN(len, size);

        if (buf) {
            memcpy(buf, &args->recv_buffer[args->recv_cur], len);
            buf += len;
        }

        args->recv_cur += len;
        size -= len;
    }

    return 0;
}

int vm_src_send_page(vm_args *args, uint64_t page)
{
    page_header header = {
        .magic = MAGIC_SEND_PAGE,
        .page_index = page,
    };
    int ret;

    /* Send header */
    ret = sock_write(args, &header, sizeof(header));
    if (ret)
        return ret;

    /* Send page (which is all zero..) */
    ret = sock_write(args, NULL, page_size);
    if (ret)
        return ret;

    return 0;
}

int vm_src_enable_postcopy_on_dst(vm_args *args)
{
    page_header header = { .magic = MAGIC_HANDSHAKE };

    if (sock_write(args, &header, sizeof(header)))
        return -1;
    if (sock_write_flush(args))
        return -1;
    return 0;
}

void *vm_src_sender_thread(void *opaque)
{
    vm_args *args = opaque;
    uint64_t index = 0, end = args->vm_size / page_size;
    uint64_t total = 0, last, cur, requested_page;
    int ret;

    /* Enable dst postcopy if necessary */
    if (args->tests & VM_TEST_POSTCOPY) {
        if (vm_src_enable_postcopy_on_dst(args))
            goto fail;
    }

    if (args->tests & VM_TEST_PRECOPY)
        printf("Starting PRECOPY streaming test...\n");

    last = get_msec();
    while (1) {
        /* If no precopy test, we don't need this sender */
        if (args->tests & VM_TEST_PRECOPY) {
            debug("sending page %"PRIu64"\n", index);
            ret = vm_src_send_page(args, index);
            if (ret)
                goto fail;
            total += sizeof(page_header) + page_size;

            /* Update index */
            index++;
            if (index >= end)
                index = 0;

            cur = get_msec();
            if (cur - last >= 1000) {
                printf("Speed: %"PRIu64" (MB/s)\n",
                        (total / (1UL << 20)) * 1000 / (cur - last));
                last = cur;
                total = 0;
            }
        }

        while (1) {
            /* Request pipe read side is non-blocking */
            debug("try reading page requests\n");
            ret = read(args->page_req_pipe[0], &requested_page,
                       sizeof(requested_page));
            if (ret < 0)
                ret = -errno;
            if (ret == 0 || ret == -EINTR || ret == -EAGAIN)
                break;
            assert(ret == sizeof(requested_page));
            debug("got request, sending page\n");
            ret = vm_src_send_page(args, requested_page);
            if (ret)
                goto fail;
            ret = sock_write_flush(args);
            if (ret)
                goto fail;
            total += sizeof(page_header) + page_size;
            /* See if there're more requests; normally none */
            continue;
        }
    }

    return NULL;

fail:
    return (void *)-1;
}

void *vm_src_receiver_thread(void *opaque)
{
    vm_args *args = opaque;
    page_header header = { 0 };

    while (1) {
        if (sock_read(args, &header, sizeof(header)))
            goto fail;
        debug("src vm recv request\n");
        if (header.magic != MAGIC_REQ_PAGE) {
            printf("Page request magic incorrect: %"PRIx64"\n", header.magic);
            goto fail;
        }
        /* Queue the page */
        fd_write(args->page_req_pipe[1], &header.page_index,
                 sizeof(header.page_index));
        debug("src vm page queued\n");
    }

    return NULL;

fail:
    return (void *)-1;
}

void vm_src_run(vm_args *args)
{
    int ret;

    printf("Connected to dst VM %s.\n", args->src_target_ip);

    ret = pthread_create(&args->sender, NULL,
                         vm_src_sender_thread, args);
    if (ret) {
        printf("Sender thread creation failed: %s\n", strerror(ret));
        return;
    }
    pthread_set_name(args->sender, "vm-src-sender");

    ret = pthread_create(&args->receiver, NULL,
                         vm_src_receiver_thread, args);
    if (ret) {
        printf("Receiver thread creation failed: %s\n", strerror(ret));
        return;
    }
    pthread_set_name(args->receiver, "vm-src-receiver");

    pthread_join(args->sender, NULL);
    pthread_join(args->receiver, NULL);

    close(args->sock);
    printf("Dropped connection to dst VM %s.\n", args->src_target_ip);
}

int mon_start_src(vm_args *args)
{
    int sock, ret;
    struct sockaddr_in server;

    vm_args_init_src(args);

    puts("Start emulation of src VM.");

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Could not create socket.");
        return -errno;
    }

    server.sin_family = AF_INET;
    server.sin_port = htons(mig_mon_port);
    if (inet_aton(args->src_target_ip, &server.sin_addr) != 1) {
        printf("Destination VM address '%s' invalid\n", args->src_target_ip);
        return -1;
    }

    ret = connect(sock, (struct sockaddr *)&server, sizeof(server));
    if (ret < 0) {
        perror("Could not connect to dst VM.");
        return -1;
    }

    args->sock = sock;
    vm_src_run(args);

    return 0;
}

void *vm_dst_sender_thread(void *opaque)
{
    vm_args *args = opaque;
    page_header header = { .magic = MAGIC_REQ_PAGE };
    uint64_t npages, page_index, last, cur, total, count, max_lat, now;

    /* If don't test postcopy, we don't really need this */
    if (!(args->tests & VM_TEST_POSTCOPY))
        return NULL;

    printf("Starting POSTCOPY request-response test...\n");

    npages = args->vm_size / page_size;
    total = count = max_lat = 0;
    last = get_usec();
    while (1) {
        args->dst_current_req = random() % npages;
        header.page_index = args->dst_current_req;

        cur = get_usec();
        debug("sending page req: sock=%d\n", args->sock);
        fd_write(args->sock, &header, sizeof(header));
        /* We send the request, wait for response */
        debug("reading pipe\n");
        fd_read(args->page_req_pipe[0], &page_index, sizeof(page_index));
        debug("reading pipe done\n");

        if (args->quit)
            break;

        if (page_index != header.page_index) {
            printf("%s: Incorrect page index received!\n", __func__);
            break;
        }

        now = get_usec();
        /* Measure the latency, record max */
        cur = now - cur;
        if (cur > max_lat)
            max_lat = cur;
        total += cur;
        count++;

        /* For each second */
        if (now - last >= 1000000) {
            printf("Latency: average %"PRIu64" (us), max: %"PRIu64" (us)\n",
                   total / count, max_lat);
            total = count = max_lat = 0;
            last = now;
        }
    }

    return NULL;
}

void vm_dst_start_sender(vm_args *args)
{
    int ret;

    ret = pthread_create(&args->sender, NULL,
                         vm_dst_sender_thread, args);
    assert(ret == 0);
    pthread_set_name(args->sender, "vm-dst-sender");
}

void vm_dst_kick_sender_quit(vm_args *args)
{
    uint64_t tmp = 0;

    /* To make sure sender thread quits... write anything to pipe */
    args->quit = 1;
    fd_write(args->page_req_pipe[1], &tmp, sizeof(uint64_t));
}

void *vm_dst_receiver_thread(void *opaque)
{
    vm_args *args = opaque;
    uint64_t end = args->vm_size / page_size;
    page_header header;
    int ret;

    while (1) {
        ret = sock_read(args, &header, sizeof(header));
        if (ret)
            goto out;

        switch (header.magic) {
        case MAGIC_HANDSHAKE:
            if (!(args->tests & VM_TEST_POSTCOPY)) {
                args->tests |= VM_TEST_POSTCOPY;
                vm_dst_start_sender(args);
            }
            continue;
        case MAGIC_SEND_PAGE:
            /* A common page received */
            break;
        default:
            printf("magic error: 0x%"PRIx64"\n", header.magic);
            goto out;
        }

        if (header.page_index >= end) {
            printf("page index overflow: 0x%"PRIx64"\n", header.page_index);
            goto out;
        }
        ret = sock_read(args, NULL, page_size);
        if (ret)
            goto out;
        debug("dst vm receiving page\n");

        /* Check if this is a postcopy request page */
        if (header.page_index == args->dst_current_req) {
            fd_write(args->page_req_pipe[1],
                     &args->dst_current_req, sizeof(uint64_t));
        }
    }
out:
    /* Remember to kick the sender thread to quit */
    vm_dst_kick_sender_quit(args);
    return NULL;
}

void vm_dst_run(vm_args *args, char *src_ip)
{
    int ret;

    vm_args_init_dst(args);

    printf("Connected from src VM %s.\n", src_ip);

    ret = pthread_create(&args->receiver, NULL,
                         vm_dst_receiver_thread, args);
    if (ret) {
        printf("Receiver thread creation failed: %s\n", strerror(ret));
        return;
    }
    pthread_set_name(args->receiver, "vm-dst-receiver");

    pthread_join(args->receiver, NULL);

    if (args->tests & VM_TEST_POSTCOPY)
        pthread_join(args->sender, NULL);

    close(args->sock);
    printf("Dropped connection from src VM %s.\n", src_ip);
}

int mon_start_dst(vm_args *args)
{
    struct sockaddr_in server, cli_addr;
    int sock, ret, new_sock, child;
    socklen_t client_len = sizeof(server);

    puts("Start emulation of dst VM.");

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Could not create socket.");
        return -errno;
    }

    memset((char *)&server, 0, sizeof(server));

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(mig_mon_port);

    ret = bind(sock, (struct sockaddr *)&server, sizeof(server));
    if (ret < 0) {
        perror("Could not bind");
        return -errno;
    }

    socket_set_fast_reuse(sock);
    listen(sock, 5);

    while (1) {
        new_sock = accept(sock, (struct sockaddr *)&cli_addr, &client_len);
        if (new_sock < 0) {
            perror("Could not accept client.");
            return -errno;
        }

        child = fork();
        if (child == 0) {
            args->sock = new_sock;
            vm_dst_run(args, strdup(inet_ntoa(cli_addr.sin_addr)));
            return 0;
        }

        close(new_sock);
    }

    return 0;
}

int mon_vm(vm_args *args)
{
    emulate_target target = args->target;
    int ret;

    /* Doing sanity check on the parameters */
    if (target == EMULATE_NONE) {
        printf("Please specify to emulate either src (-s) or dst (-d)\n");
        return -1;
    } else if (target == EMULATE_SRC) {
        if (!args->src_target_ip) {
            printf("Please specify dst VM address using '-H'.\n");
            return -1;
        }
    } else {
        /* EMULATE_DST */
        if (args->tests) {
            printf("precopy/postcopy need to be specified on src VM.\n");
            return -1;
        }
    }

    if (target == EMULATE_SRC)
        ret = mon_start_src(args);
    else
        ret = mon_start_dst(args);

    return ret;
}

unsigned int vm_test_parse(const char *name)
{
    if (!strcmp(name, "precopy"))
        return VM_TEST_PRECOPY;
    else if (!strcmp(name, "postcopy"))
        return VM_TEST_POSTCOPY;
    printf("Unknown vm test type: '%s'\n", name);
    exit(1);
}

int main(int argc, char *argv[])
{
    int ret = 0;
    int interval_ms = MIG_MON_INT_DEF;
    const char *work_mode = NULL;
    const char *server_ip = NULL;
    const char *spike_log = MIG_MON_SPIKE_LOG_DEF;

    n_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    page_size = huge_page_size = getpagesize();

    prog_name = argv[0];

    if (argc == 1) {
        usage();
        version();
        return -1;
    }

    srand(time(NULL));

    work_mode = argv[1];

    if (!strcmp(work_mode, "-h") || !strcmp(work_mode, "--help")) {
        usage();
        usage_downtime();
        usage_mm_dirty();
        usage_vm();
        return -1;
    } else if (!strcmp(work_mode, "-v") || !strcmp(work_mode, "--version")) {
        version();
        return -1;
    } else if (!strcmp(work_mode, "server")) {
        puts("starting server mode...");
        if (argc >= 3) {
            spike_log = argv[2];
        }
        ret = mon_server(spike_log, mon_server_callback);
    } else if (!strcmp(work_mode, "client")) {
        if (argc < 3) {
            usage();
            return -1;
        }
        server_ip = argv[2];
        if (argc >= 4) {
            interval_ms = strtol(argv[3], NULL, 10);
        }
        puts("starting client mode...");
        printf("server ip: %s, interval: %d (ms)\n", server_ip, interval_ms);
        ret = mon_client(server_ip, interval_ms, NULL, mon_client_callback);
    } else if (!strcmp(work_mode, "server_rr")) {
        printf("starting server_rr...\n");
        ret = mon_server(NULL, mon_server_rr_callback);
    } else if (!strcmp(work_mode, "client_rr")) {
        if (argc < 3) {
            usage();
            return -1;
        }
        server_ip = argv[2];
        if (argc >= 4) {
            interval_ms = strtol(argv[3], NULL, 10);
        }
        if (argc >= 5) {
            spike_log = argv[4];
        }
        ret = mon_client(server_ip, interval_ms, spike_log,
                         mon_client_rr_callback);
    } else if (!strcmp(work_mode, "vm")) {
        vm_args args = {
            .target = EMULATE_NONE,
            .vm_size = DEF_VM_SIZE,
        };
        int c;

        while ((c = getopt(argc-1, argv+1, "dhp:st:H:S:")) != -1) {
            switch (c) {
            case 'd':
                args.target = EMULATE_DST;
                break;
            case 'p':
                mig_mon_port = atoi(optarg);
                break;
            case 's':
                args.target = EMULATE_SRC;
                break;
            case 't':
                args.tests |= vm_test_parse(optarg);
                break;
            case 'H':
                args.src_target_ip = strdup(optarg);
                break;
            case 'S':
                args.vm_size = atoi(optarg) * (1UL << 30);
                break;
            case 'h':
            default:
                usage();
                usage_vm();
                return -1;
            }
        }

        ret = mon_vm(&args);
    } else if (!strcmp(work_mode, "mm_dirty")) {
        long dirty_rate = 0, mm_size = DEF_MM_DIRTY_SIZE;
        dirty_pattern pattern = DEF_MM_DIRTY_PATTERN;
        int map_flags = MAP_ANONYMOUS | MAP_PRIVATE;
        int c;

        while ((c = getopt(argc-1, argv+1, "hm:p:P:r:")) != -1) {
            switch (c) {
            case 'm':
                mm_size = atol(optarg);
                break;
            case 'r':
                dirty_rate = atol(optarg);
                break;
            case 'p':
                pattern = parse_dirty_pattern(optarg);
                break;
            case 'P':
                map_flags |= parse_huge_page_size(optarg);
                break;
            case 'h':
            default:
                usage();
                usage_mm_dirty();
                return -1;
            }
        }

        /*
         * We should have consumed all parameters.  This will dump an error if
         * the user used the old mig_mon mm_dirty parameters.
         */
        if (optind != argc-1) {
            printf("Unknown extra parameters detected.\n");
            usage();
            return -1;
        }

        ret = mon_mm_dirty(mm_size, dirty_rate, pattern, map_flags);
    } else {
        usage();
        return -1;
    }

    return ret;
}
