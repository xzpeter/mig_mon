#ifndef __UTILS_H__
#define __UTILS_H__

#include <stdint.h>
#include <pthread.h>

void fd_write(int fd, void *buffer, size_t size);
void fd_read(int fd, void *buffer, size_t size);
void socket_set_fast_reuse(int fd);
void pthread_set_name(pthread_t thread, const char *name);
unsigned long parse_size_to_mega(const char *str);

static inline uint64_t get_usec(void)
{
    uint64_t val = 0;
    struct timespec t;
    int ret = clock_gettime(CLOCK_MONOTONIC, &t);
    if (ret == -1) {
        perror("clock_gettime() failed");
        /* should never happen */
        exit(-1);
    }
    val = t.tv_nsec / 1000;     /* ns -> us */
    val += t.tv_sec * 1000000;  /* s -> us */
    return val;
}

static inline uint64_t get_msec(void)
{
    return get_usec() / 1000;
}

static inline uint64_t get_timestamp(void)
{
    return (uint64_t)time(NULL);
}

#endif
