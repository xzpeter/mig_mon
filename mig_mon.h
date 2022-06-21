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

#ifdef __linux__
#include <linux/mman.h>
#endif

#include "version.h"
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

#endif
