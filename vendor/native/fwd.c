/* Static TCP forwarder: IPv4 0.0.0.0:LPORT -> IPv6 ::1:RPORT, poll()-based relay.
 * Bridges docker's IPv4 proxy to the container's IPv6-only adbd. fwd [LPORT=5556] [RPORT=5555] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

struct flow {
    char data[65536];
    size_t offset, length;
    bool eof;
};

static bool retryable(void) {
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

static void relay(int client, int backend) {
    const int sockets[2] = {client, backend};
    struct flow flows[2] = {0};

    // ADB data and acknowledgements travel in both directions. Full output queues
    // apply backpressure only to their own incoming data.
    while (!flows[0].eof || !flows[1].eof) {
        struct pollfd poll_fds[2];
        for (int i = 0; i < 2; ++i) {
            struct flow *from = &flows[i];
            struct flow *to = &flows[1 - i];
            short events = 0;
            if (!from->eof && from->offset == from->length) events |= POLLIN;
            if (to->offset < to->length) events |= POLLOUT;
            poll_fds[i] = (struct pollfd){.fd = events ? sockets[i] : -1, .events = events};
        }
        if (poll(poll_fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if ((poll_fds[0].revents | poll_fds[1].revents) & (POLLERR | POLLNVAL)) return;

        for (int i = 0; i < 2; ++i) {
            struct flow *flow = &flows[i];
            const int peer = 1 - i;
            if (!flow->eof && flow->offset == flow->length &&
                (poll_fds[i].revents & (POLLIN | POLLHUP))) {
                ssize_t n = recv(sockets[i], flow->data, sizeof flow->data, MSG_DONTWAIT);
                if (n > 0) {
                    flow->offset = 0;
                    flow->length = (size_t)n;
                } else if (n == 0) {
                    flow->eof = true;
                    shutdown(sockets[peer], SHUT_WR);
                } else if (!retryable()) {
                    return;
                }
            }
            if (flow->offset < flow->length &&
                (poll_fds[peer].revents & (POLLOUT | POLLHUP))) {
                ssize_t n = send(sockets[peer], flow->data + flow->offset,
                                 flow->length - flow->offset, MSG_DONTWAIT);
                if (n > 0) {
                    flow->offset += (size_t)n;
                } else if (n == 0 || !retryable()) {
                    return;
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    int lport = argc > 1 ? atoi(argv[1]) : 5556;
    int rport = argc > 2 ? atoi(argv[2]) : 5555;
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(lport);
    /* Wait for the port rather than exiting on EADDRINUSE. A predecessor holding 5556 is the NORMAL
     * case at startup, not an error: this image restarts its framework during first boot, so the
     * init service's start trigger fires twice and the second start races the first instance's
     * teardown. Exiting immediately turned that race into a restart storm — measured at 20 failed
     * starts per boot under init's fixed 5s retry, which is exactly the noise that would bury the
     * one spontaneous exit worth reading (bd remora-yn4). ~30s of patience covers the handover and
     * still gives up loudly if something owns the port for good. */
    int bound = 0;
    for (int i = 0; i < 60; i++) {
        if (bind(ls, (struct sockaddr *)&a, sizeof a) == 0) { bound = 1; break; }
        if (errno != EADDRINUSE) break;
        if (i == 0) dprintf(2, "[fwd] %d in use — waiting for the previous owner\n", lport);
        usleep(500000);
    }
    if (!bound) { dprintf(2, "[fwd] bind: %s\n", strerror(errno)); return 1; }
    listen(ls, 32);
    dprintf(2, "[fwd] listening 0.0.0.0:%d -> ::1:%d\n", lport, rport);

    for (;;) {
        int cs = accept(ls, 0, 0);
        if (cs < 0) continue;
        if (fork() == 0) {
            close(ls);
            int rs = socket(AF_INET6, SOCK_STREAM, 0);
            struct sockaddr_in6 r; memset(&r, 0, sizeof r);
            r.sin6_family = AF_INET6; r.sin6_port = htons(rport);
            inet_pton(AF_INET6, "::1", &r.sin6_addr);
            if (connect(rs, (struct sockaddr *)&r, sizeof r) < 0) {
                dprintf(2, "[fwd] connect ::1:%d FAILED: %s\n", rport, strerror(errno));
                _exit(1);
            }
            // Short ADB packets carry interactive input, audio, and flow-control
            // acknowledgements. Flush them promptly on both TCP connections.
            if (setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) < 0)
                dprintf(2, "[fwd] TCP_NODELAY on client: %s\n", strerror(errno));
            if (setsockopt(rs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) < 0)
                dprintf(2, "[fwd] TCP_NODELAY on adbd: %s\n", strerror(errno));
            dprintf(2, "[fwd] relaying\n");
            relay(cs, rs);
            _exit(0);
        }
        close(cs);
    }
}
