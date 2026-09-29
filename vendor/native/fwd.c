/* Static TCP forwarder: IPv4 0.0.0.0:LPORT -> IPv6 ::1:RPORT, select()-based relay.
 * Bridges docker's IPv4 proxy to the container's IPv6-only adbd. fwd [LPORT=5556] [RPORT=5555] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
            dprintf(2, "[fwd] relaying\n");
            char buf[65536];
            int mx = (cs > rs ? cs : rs) + 1;
            for (;;) {
                fd_set fds; FD_ZERO(&fds); FD_SET(cs, &fds); FD_SET(rs, &fds);
                if (select(mx, &fds, 0, 0, 0) < 0) { if (errno == EINTR) continue; break; }
                /* write_all: a blocking write can still return short (or EINTR) under load —
                 * treating that as fatal drops the whole adb session mid-stream. */
                if (FD_ISSET(cs, &fds)) { int n = read(cs, buf, sizeof buf); if (n <= 0) break;
                    int off = 0; while (off < n) { int w = write(rs, buf + off, n - off);
                        if (w < 0 && errno == EINTR) continue; if (w <= 0) goto done; off += w; } }
                if (FD_ISSET(rs, &fds)) { int n = read(rs, buf, sizeof buf); if (n <= 0) break;
                    int off = 0; while (off < n) { int w = write(cs, buf + off, n - off);
                        if (w < 0 && errno == EINTR) continue; if (w <= 0) goto done; off += w; } }
            }
        done:
            _exit(0);
        }
        close(cs);
    }
}
