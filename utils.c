#include "mig_mon.h"

/* Return 0 when succeed, 1 for retry, assert on error */
void fd_write(int fd, void *buffer, size_t size)
{
    int ret;

retry:
    ret = write(fd, buffer, size);
    if (ret < 0)
        ret = -errno;
    if (ret == -EAGAIN || ret == -EINTR)
        goto retry;

    assert((size_t)ret == size);
}

/* Return 0 when succeed, 1 for retry, assert on error */
void fd_read(int fd, void *buffer, size_t size)
{
    int ret;

retry:
    ret = read(fd, buffer, size);
    if (ret < 0)
        ret = -errno;
    if (ret == -EAGAIN || ret == -EINTR)
        goto retry;

    assert((size_t)ret == size);
}

void socket_set_fast_reuse(int fd)
{
    int val = 1, ret;

    ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                     (const char *)&val, sizeof(val));

    assert(ret == 0);
}

void pthread_set_name(pthread_t thread, const char *name)
{
#ifdef __linux__
    int ret = pthread_setname_np(thread, name);
    assert(ret == 0);
#endif
}

/* Parse some number like "2G", if no unit, using "M" by default. */
uint64_t parse_size_to_mega(const char *str)
{
    uint64_t value, n = 1;
    char *endptr;

    value = strtoul(str, &endptr, 10);
    if (value == 0 || endptr == NULL) {
        printf("Unknown size string: '%s'\n", str);
        exit(-1);
    }

    switch (*endptr) {
    case 't':
    case 'T':
        n *= 1024;
        /* fall through */
    case 'g':
    case 'G':
        n *= 1024;
        /* fall through */
    case 'm':
    case 'M':
    case '\0': /* This means, no unit, so MB by default */
        break;
    default:
        printf("Unknown unit '%c', try something else (MB/GB/...)\n", *endptr);
        exit(-1);
        break;
    }

    return value * n;
}
