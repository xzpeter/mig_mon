#include "mig_mon.h"

static const char *prog_name = NULL;

long n_cpus;
short mig_mon_port = MIG_MON_PORT;
/*
 * huge_page_size stands for the real page size we used.  page_size will always
 * be the smallest page size of the system, as that's the size that guest
 * hypervisor will track dirty.
 */
long page_size, huge_page_size;
const char *pattern_str[PATTERN_NUM] = { "sequential", "random", "once" };

void usage_downtime_short(void)
{
    puts("");
    printf("       %s server [spike_log]\n", prog_name);
    printf("       %s client server_ip [interval_ms]\n", prog_name);
    printf("       %s server_rr\n", prog_name);
    printf("       %s client_rr server_ip [interval_ms [spike_log]]\n", prog_name);
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
    printf("       \t -P: \tPage size: \"2m\" or \"1g\" for huge pages\n");
}

void usage_vm_short(void)
{
    puts("");
    printf("       %s vm [options...]\n", prog_name);
    printf("       \t -d: \tEmulate a dst VM\n");
    printf("       \t -h: \tDump help message for vm sub-cmd\n");
    printf("       \t -H: \tSpecify dst VM IP (required for -s)\n");
    printf("       \t -p: \tSpecify connect/listen port\n");
    printf("       \t -s: \tEmulate a src VM\n");
    printf("       \t -S: \tSpecify size of the VM (GB)\n");
    printf("       \t -t: \tSpecify tests (precopy, postcopy)\n");
}

void usage_downtime(void)
{
    puts("");
    puts("Usage:");
    usage_downtime_short();
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
    puts("");
    puts("Usage:");
    usage_mm_dirty_short();
    puts("");
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
    puts("");
    puts("Usage:");
    usage_vm_short();
    puts("");
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
    puts("For detailed usage, please try '-h/--help' for each sub-command.");
    puts("");
    puts("Usage:");
    printf("       %s [-h|--help]\tShow full help message\n", prog_name);
    usage_downtime_short();
    usage_mm_dirty_short();
    usage_vm_short();
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

    /* Let's allow some short forms.. */
    if (!strcmp(str, "seq"))
        return PATTERN_SEQ;
    else if (!strcmp(str, "ran"))
        return PATTERN_RAND;

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
            usage_downtime();
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
            usage_downtime();
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
                usage_vm();
                return -1;
            }
        }

        ret = mon_vm(&args);
    } else if (!strcmp(work_mode, "mm_dirty")) {
        mm_dirty_args args = {
            .dirty_rate = 0,
            .mm_size = DEF_MM_DIRTY_SIZE,
            .pattern = DEF_MM_DIRTY_PATTERN,
            .map_flags = MAP_ANONYMOUS | MAP_PRIVATE,
        };
        int c;

        while ((c = getopt(argc-1, argv+1, "hm:p:P:r:")) != -1) {
            switch (c) {
            case 'm':
                args.mm_size = atol(optarg);
                break;
            case 'r':
                args.dirty_rate = atol(optarg);
                break;
            case 'p':
                args.pattern = parse_dirty_pattern(optarg);
                break;
            case 'P':
                args.map_flags |= parse_huge_page_size(optarg);
                break;
            case 'h':
            default:
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
            usage_mm_dirty();
            return -1;
        }

        ret = mon_mm_dirty(&args);
    } else {
        usage();
        return -1;
    }

    return ret;
}
