#include "mig_mon.h"

const char *prog_name = NULL;
long n_cpus;
short mig_mon_port = MIG_MON_PORT;
/*
 * huge_page_size stands for the real page size we used.  page_size will always
 * be the smallest page size of the system, as that's the size that guest
 * hypervisor will track dirty.
 */
long page_size, huge_page_size;
const char *pattern_str[PATTERN_NUM] = { "sequential", "random", "once" };

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
                args.mm_size = parse_size_to_mega(optarg);
                break;
            case 'r':
                args.dirty_rate = parse_size_to_mega(optarg);
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
