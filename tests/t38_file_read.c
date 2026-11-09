/* em_read on a regular file (offloaded on epoll, IORING_OP_READ on uring) while a ping-pong
 * pair keeps running; content, EOF, errors, and that other fd kinds still read normally. */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "util.h"

#define SIZE (1 << 20)

static int sv[2];
static volatile int done;
static long rounds;

static int pinger(void *arg)
{
    (void)arg;
    char c = 'p';
    while (!done) {
        assert(em_send(sv[0], &c, 1) == 1);
        assert(em_recv(sv[0], &c, 1) == 1);
        rounds++;
    }
    assert(em_shutdown(sv[0], SHUT_WR) == 0);
    return 0;
}

static int ponger(void *arg)
{
    (void)arg;
    char c;
    for (;;) {
        ssize_t n = em_recv(sv[1], &c, 1);
        if (n == 0) return 0;
        assert(n == 1);
        assert(em_send(sv[1], &c, 1) == 1);
    }
}

static unsigned char byte_at(size_t i) { return (unsigned char)(i * 31 + (i >> 8)); }

static int reader(void *arg)
{
    int fd = (int)(intptr_t)arg;
    static unsigned char buf[64 * 1024];
    size_t off = 0;
    for (;;) {
        ssize_t n = em_read(fd, buf, sizeof buf);
        assert(n >= 0);
        if (n == 0) break;
        for (ssize_t i = 0; i < n; i++) assert(buf[i] == byte_at(off + (size_t)i));
        off += (size_t)n;
    }
    assert(off == SIZE);
    assert(em_read(fd, buf, sizeof buf) == 0); /* EOF stays EOF */
    done = 1;
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    char path[] = "/tmp/em_t38_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unlink(path);
    static unsigned char data[SIZE];
    for (size_t i = 0; i < SIZE; i++) data[i] = byte_at(i);
    assert(write(fd, data, SIZE) == SIZE);
    assert(lseek(fd, 0, SEEK_SET) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, reader, (void *)(intptr_t)fd) == 0);
    assert(em_spawn(&s, pinger, NULL) == 0);
    assert(em_spawn(&s, ponger, NULL) == 0);
    assert(em_scope_close(&s) == 0);
    em_close(sv[0]);
    em_close(sv[1]);

    /* a pipe read after the file: not mistaken for a file */
    int p[2];
    assert(pipe(p) == 0);
    assert(write(p[1], "xyz", 3) == 3);
    char b[8];
    assert(em_read(p[0], b, sizeof b) == 3 && memcmp(b, "xyz", 3) == 0);
    em_close(p[0]);
    em_close(p[1]);

    /* before the call: cancel point */
    assert(lseek(fd, 0, SEEK_SET) == 0);
    em_scope d;
    em_scope_open(&d, em_now() - 1);
    assert(em_read(fd, b, sizeof b) == -ETIMEDOUT);
    em_scope_close(&d);
    assert(em_read(fd, b, sizeof b) == 8);

    /* the fd number is reused by another kind of file: em_close reset the slot */
    em_close(fd);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(write(sv[1], "ab", 2) == 2);
    assert(em_read(sv[0], b, sizeof b) == 2);
    em_close(sv[0]);
    em_close(sv[1]);
    assert(em_read(-1, b, sizeof b) < 0);
    printf("file read (%s): 1 MiB intact, %ld ping-pong rounds meanwhile\n", em_backend(), rounds);
    return 0;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    return 0;
}
