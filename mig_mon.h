#ifndef __MIG_MON_H__
#define __MIG_MON_H__

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>

#ifdef __linux__
#include <linux/mman.h>
#endif

#include "utils.h"

#define  MAX(a, b)  ((a > b) ? (a) : (b))
#define  MIN(a, b)  ((a < b) ? (a) : (b))

#ifdef DEBUG
#define  debug(...)  printf(__VA_ARGS__)
#else
#define  debug(...)
#endif

typedef enum {
    PATTERN_SEQ = 0,
    PATTERN_RAND = 1,
    PATTERN_ONCE = 2,
    PATTERN_NUM,
} dirty_pattern;

/* whether allow client change its IP */
#define  MIG_MON_SINGLE_CLIENT       (0)
#define  MIG_MON_PORT                (12323)
#define  MIG_MON_INT_DEF             (1000)
#define  BUF_LEN                     (1024)
#define  MIG_MON_SPIKE_LOG_DEF       ("/tmp/spike.log")
#define  DEF_MM_DIRTY_SIZE           (512)
#define  DEF_MM_DIRTY_PATTERN        PATTERN_SEQ

/******************
 * For mig_mon.c  *
 ******************/
extern short mig_mon_port;
extern long n_cpus;
extern long page_size, huge_page_size;
extern const char *pattern_str[PATTERN_NUM];
extern const char *prog_name;

/******************
 * For downtime.c *
 ******************/

/* Mig_mon callbacks. Return 0 for continue, non-zero for errors. */
typedef int (*mon_server_cbk)(int sock, int spike_fd);
typedef int (*mon_client_cbk)(int sock, int spike_fd, int interval_ms);

int mon_server_callback(int sock, int spike_fd);
int mon_server_rr_callback(int sock, int spike_fd);
int mon_server(const char *spike_log, mon_server_cbk server_callback);
int mon_client_callback(int sock, int spike_fd, int interval_ms);
int mon_client_rr_callback(int sock, int spike_fd, int interval_ms);
int mon_client(const char *server_ip, int interval_ms,
               const char *spike_log, mon_client_cbk client_callback);
void usage_downtime_short(void);
void usage_downtime(void);

/******************
 * For mm_dirty.c *
 ******************/
typedef struct {
    /* Size of the memory to test on */
    uint64_t mm_size;
    /* Dirty rate (in MB/s) */
    uint64_t dirty_rate;
    /* mmap() flags to pass over */
    unsigned int map_flags;
    /* Dirty pattern */
    dirty_pattern pattern;
    /* Whether we're recording the memory access latencies */
    bool record_latencies;
} mm_dirty_args;
int mon_mm_dirty(mm_dirty_args *args);
void usage_mm_dirty_short(void);
void usage_mm_dirty(void);

/**************
 * For vm.c   *
 **************/

/* If set, will generate precopy live migration stream */
#define  VM_TEST_PRECOPY     (1UL << 0)
/* If set, will generate postcopy page requests */
#define  VM_TEST_POSTCOPY    (1UL << 1)

#define  DEF_VM_SIZE              (1UL << 40)  /* 1TB */

typedef enum {
    EMULATE_NONE = 0,
    EMULATE_SRC = 1,
    EMULATE_DST = 2,
    EMULATE_NUM,
} emulate_target;

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

int mon_vm(vm_args *args);
void usage_vm(void);
void usage_vm_short(void);

#endif
