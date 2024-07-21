#include "mig_mon.h"

/*
 * State machine for the event handler. It just starts from 0 until
 * RUNNING.
 */
enum event_state {
    /* Idle, waiting for first time triggering event */
    STATE_WAIT_FIRST_TRIGGER = 0,
    /* Got first event, waiting for the 2nd one */
    STATE_WAIT_SECOND_TRIGGER = 1,
    /* Normal running state */
    STATE_RUNNING = 2,
    STATE_MAX
};

static void write_spike_log(int fd, uint64_t delay)
{
    char spike_buf[1024] = {0};
    int str_len = -1;
    str_len = snprintf(spike_buf, sizeof(spike_buf) - 1,
                       "%"PRIu64",%"PRIu64"\n", get_timestamp(), delay);
    spike_buf[sizeof(spike_buf) - 1] = 0x00;
    write(fd, spike_buf, str_len);
    /* not flushed to make it fast */
}

static int socket_set_timeout(int sock, int timeout_ms)
{
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };

    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                      (void *)&tv, sizeof(tv));
}

/*
 * This is a state machine to handle the incoming event. Return code
 * is the state before calling this handler.
 */
static enum event_state handle_event(int spike_fd)
{
    /* Internal static variables */
    static enum event_state state = STATE_WAIT_FIRST_TRIGGER;
    static uint64_t last = 0, max_delay = 0;
    /*
     * this will store the 1st and 2nd UDP packet latency, as a
     * baseline of latency values (this is very, very possibly the
     * value that you provided as interval when you start the
     * client). This is used to define spikes, using formular:
     *
     *         spike_throttle = first_latency * 2
     */
    static uint64_t first_latency = 0, spike_throttle = 0;

    /* Temp variables */
    uint64_t cur = 0, delay = 0;
    enum event_state old_state = state;

    cur = get_msec();

    if (last) {
        /*
         * If this is not exactly the first event we got, we calculate
         * the delay.
         */
        delay = cur - last;
    }

    switch (state) {
    case STATE_WAIT_FIRST_TRIGGER:
        assert(last == 0);
        assert(max_delay == 0);
        /*
         * We need to do nothing here, just to init the "last", which
         * will be done after the switch().
         */
        state++;
        break;

    case STATE_WAIT_SECOND_TRIGGER:
        /*
         * if this is _exactly_ the 2nd packet we got, we need to note
         * this down as a baseline.
         */
        assert(first_latency == 0);
        first_latency = delay;
        printf("1st and 2nd packet latency: %"PRIu64" (ms)\n", first_latency);
        spike_throttle = delay * 2;
        printf("Setting spike throttle to: %"PRIu64" (ms)\n", spike_throttle);
        if (spike_fd != -1) {
            printf("Updating spike log initial timestamp\n");
            /* this -1 is meaningless, shows the init timestamp only. */
            write_spike_log(spike_fd, -1);
        }
        state++;
        break;

    case STATE_RUNNING:
        if (delay > max_delay) {
            max_delay = delay;
        }
        /*
         * if we specified spike_log, we need to log spikes into that
         * file.
         */
        if (spike_fd != -1 && delay >= spike_throttle) {
            write_spike_log(spike_fd, delay);
        }
        printf("\r                                                       ");
        printf("\r[%"PRIu64"] max_delay: %"PRIu64" (ms), cur: %"PRIu64" (ms)", cur,
               max_delay, delay);
        fflush(stdout);
        break;

    default:
        printf("Unknown state: %d\n", state);
        exit(1);
        break;
    }

    /* update LAST */
    last = cur;

    return old_state;
}

static int spike_log_open(const char *spike_log)
{
    int spike_fd = -1;

    if (spike_log) {
        spike_fd = open(spike_log, O_WRONLY | O_CREAT, 0644);
        if (spike_fd == -1) {
            perror("failed to open spike log");
            /* Silently disable spike log */
        } else {
            ftruncate(spike_fd, 0);
        }
    }

    return spike_fd;
}

int mon_server_callback(int sock, int spike_fd)
{
    static in_addr_t target = -1;
    int ret;
    char buf[BUF_LEN];
    struct sockaddr_in clnt_addr = {};
    socklen_t addr_len = sizeof(clnt_addr);

    ret = recvfrom(sock, buf, BUF_LEN, 0, (struct sockaddr *)&clnt_addr,
                   &addr_len);
    if (ret == -1) {
        perror("recvfrom() error");
        return -1;
    }

    if (target == -1) {
        /* this is the first packet we recved. we should init the
           environment and remember the target client we are monitoring
           for this round. */
        printf("setting monitor target to client '%s'\n",
               inet_ntoa(clnt_addr.sin_addr));
        target = clnt_addr.sin_addr.s_addr;
        /* Should be the first time calling */
        assert(handle_event(spike_fd) == STATE_WAIT_FIRST_TRIGGER);
        return 0;
    }

#if MIG_MON_SINGLE_CLIENT
    /* this is not the first packet we received, we will only monitor
       the target client, and disgard all the other packets recved. */
    if (clnt_addr.sin_addr.s_addr != target) {
        printf("\nWARNING: another client (%s:%d) is connecting...\n",
               inet_ntoa(clnt_addr.sin_addr),
               ntohs(clnt_addr.sin_port));
        /* disgard it! */
        return 0;
    }
#endif

    handle_event(spike_fd);

    return 0;
}

/* This is actually a udp ECHO server. */
int mon_server_rr_callback(int sock, int spike_fd)
{
    int ret;
    char buf[BUF_LEN];
    struct sockaddr_in clnt_addr = {};
    socklen_t addr_len = sizeof(clnt_addr);
    uint64_t cur;

    ret = recvfrom(sock, buf, BUF_LEN, 0, (struct sockaddr *)&clnt_addr,
                   &addr_len);
    if (ret == -1) {
        perror("recvfrom() error");
        return -1;
    }

    ret = sendto(sock, buf, ret, 0, (struct sockaddr *)&clnt_addr,
                 addr_len);
    if (ret == -1) {
        perror("sendto() error");
        return -1;
    }

    cur = get_msec();

    printf("\r                                                  ");
    printf("\r[%"PRIu64"] responding to client", cur);
    fflush(stdout);

    return 0;
}

/*
 * spike_log is the file path to store spikes. Spikes will be
 * stored in the form like (for each line):
 *
 * A,B
 *
 * Here, A is the timestamp in seconds. B is the latency value in
 * ms.
 */
int mon_server(const char *spike_log, mon_server_cbk server_callback)
{
    int sock = 0;
    int ret = 0;
    struct sockaddr_in svr_addr = {};
    int spike_fd = spike_log_open(spike_log);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket() creation failed");
        return -1;
    }

    svr_addr.sin_family = AF_INET;
    svr_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    svr_addr.sin_port = mig_mon_port;

    ret = bind(sock, (struct sockaddr *)&svr_addr, sizeof(svr_addr));
    if (ret == -1) {
        perror("bind() failed");
        return -1;
    }

    printf("listening on UDP port %d...\n", mig_mon_port);
#if MIG_MON_SINGLE_CLIENT
    printf("allowing single client only.\n");
#else
    printf("allowing multiple clients.\n");
#endif

    while (1) {
        ret = server_callback(sock, spike_fd);
        if (ret) {
            break;
        }
    }

    return ret;
}

int mon_client_callback(int sock, int spike_fd, int interval_ms)
{
    int ret;
    uint64_t cur;
    char buf[BUF_LEN] = "echo";
    int msg_len = strlen(buf);
    int int_us = interval_ms * 1000;

    ret = sendto(sock, buf, msg_len, 0, NULL, 0);
    if (ret == -1) {
        perror("sendto() failed");
        return -1;
    } else if (ret != msg_len) {
        printf("sendto() returned %d?\n", ret);
        return -1;
    }
    cur = get_msec();
    printf("\r                                                  ");
    printf("\r[%"PRIu64"] sending packet to server", cur);
    fflush(stdout);
    usleep(int_us);

    return 0;
}

int mon_client_rr_callback(int sock, int spike_fd, int interval_ms)
{
    int ret;
    uint64_t cur;
    char buf[BUF_LEN] = "echo";
    int msg_len = strlen(buf);
    static int init = 0;
    static uint64_t last = 0;

    if (!init) {
        printf("Setting socket recv timeout to %d (ms)\n",
               interval_ms);
        socket_set_timeout(sock, interval_ms);
        init = 1;
    }

    cur = get_msec();

    if (last) {
        /*
         * This is not the first packet, we need to wait until we
         * reaches the interval.
         */
        int64_t delta = last + interval_ms - cur;
        if (delta > 0) {
            usleep(delta * 1000);
        }
    }

    last = get_msec();

    ret = sendto(sock, buf, msg_len, 0, NULL, 0);
    if (ret == -1) {
        perror("sendto() failed");
        return -1;
    } else if (ret != msg_len) {
        printf("sendto() returned %d?\n", ret);
        return -1;
    }

    ret = recvfrom(sock, buf, msg_len, 0, NULL, 0);
    if (ret == -1) {
        if (errno == ECONNREFUSED || errno == EAGAIN) {
            /*
             * This is when server is down, e.g., due to migration. So
             * this is okay.
             */
            return 0;
        } else {
            printf("recvfrom() ERRNO: %d\n", errno);
        }
    } else if (ret != msg_len) {
        printf("recvfrom() returned %d?\n", ret);
        return -1;
    }

    handle_event(spike_fd);

    return 0;
}

int mon_client(const char *server_ip, int interval_ms,
               const char *spike_log, mon_client_cbk client_callback)
{
    int ret = -1;
    int sock = 0;
    struct sockaddr_in addr;
    int spike_fd = spike_log_open(spike_log);

    bzero(&addr, sizeof(addr));

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) {
        perror("socket() failed");
        return -1;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = mig_mon_port;
    if (inet_aton(server_ip, &addr.sin_addr) != 1) {
        printf("server ip '%s' invalid\n", server_ip);
        ret = -1;
        goto close_sock;
    }

    ret = connect(sock, (const struct sockaddr *)&addr, sizeof(addr));
    if (ret) {
        perror("connect() failed");
        goto close_sock;
    }

    while (1) {
        ret = client_callback(sock, spike_fd, interval_ms);
        if (ret) {
            break;
        }
    }

close_sock:
    close(sock);
    return ret;
}

void usage_downtime_short(void)
{
    puts("");
    printf("       %s server [spike_log]\n", prog_name);
    printf("       %s client server_ip [interval_ms]\n", prog_name);
    printf("       %s server_rr\n", prog_name);
    printf("       %s client_rr server_ip [interval_ms [spike_log]]\n", prog_name);
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
