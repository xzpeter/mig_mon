#include "mig_mon.h"

#define  MAGIC_SEND_PAGE          (0x123)      /* For sending page */
#define  MAGIC_REQ_PAGE           (0x124)      /* For requesting page */
#define  MAGIC_HANDSHAKE          (0x125)      /* For src->dst handshake */

/* These emulates QEMU */
#define  DEF_IO_BUF_SIZE    32768
#define  MAX_IOV_SIZE       64

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
