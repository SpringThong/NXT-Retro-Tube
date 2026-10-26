/* Single instance + background listening, compatible with launch.sh:
 * a running hidden instance is brought back instead of starting a second app.
 * State file "<socket>.state": "VISIBLE <pid>" or "HIDDEN <pid>".
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int server = -1, lockfd = -1;
static char sock_path[108], state_path[128];
static bool attach_requested;

int Session_start(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *custom = getenv("NXT_TUBE_SOCKET");
    if (custom) snprintf(sock_path, sizeof(sock_path), "%s", custom);
    else snprintf(sock_path, sizeof(sock_path), "/tmp/nxt-retro-tube-%lu.sock", (unsigned long)getuid());
    snprintf(state_path, sizeof(state_path), "%s.state", sock_path);
    /* an instance already running? ask it to come back */
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
    if (fd >= 0 && !connect(fd, (struct sockaddr *)&a, sizeof(a))) {
        struct timeval tv = {3, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char reply[8] = {0};
        bool ok = send(fd, "ATTACH\n", 7, MSG_NOSIGNAL) == 7 && read(fd, reply, sizeof(reply) - 1) >= 3 && !strncmp(reply, "OK", 2);
        close(fd);
        if (ok) return 1;
    } else if (fd >= 0) close(fd);
    char lock[140]; snprintf(lock, sizeof(lock), "%s.lock", sock_path);
    lockfd = open(lock, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB)) { Log_msg("another instance holds the lock"); return -1; }
    unlink(sock_path);
    server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (server < 0 || bind(server, (struct sockaddr *)&a, sizeof(a)) || listen(server, 4)) { Log_msg("session socket: %s", strerror(errno)); if (server >= 0) close(server); server = -1; return 0; }
    Session_visible(true);
    return 0;
}
void Session_visible(bool shown) {
    char tmp[140]; snprintf(tmp, sizeof(tmp), "%s.tmp", state_path);
    FILE *f = fopen(tmp, "w");
    if (f) { fprintf(f, "%s %ld\n", shown ? "VISIBLE" : "HIDDEN", (long)getpid()); fclose(f); rename(tmp, state_path); }
}
bool Session_poll(void) {
    if (server < 0) return false;
    for (;;) {
        int c = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) break;
        struct timeval tv = {0, 200000}; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char req[32] = {0};
        if (read(c, req, sizeof(req) - 1) > 0 && !strncmp(req, "ATTACH", 6)) { attach_requested = true; send(c, "OK\n", 3, MSG_NOSIGNAL); }
        close(c);
    }
    bool r = attach_requested; attach_requested = false; return r;
}
void Session_quit(void) {
    if (server >= 0) { close(server); unlink(sock_path); }
    unlink(state_path);
    if (lockfd >= 0) close(lockfd);
}
