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

    assert(ret == size);
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

    assert(ret == size);
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
