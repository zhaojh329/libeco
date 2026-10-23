/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco/io.h>
#include <eco.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#define TRANSFER_SIZE (512 * 1024)

struct channel {
    int fd;
    struct co_io_watch *watch;
    char *payload;
    int *completed;
};

static struct co_io_watch *evaluated_watch(struct co_io_watch *watch, int *evaluations)
{
    ++*evaluations;
    return watch;
}

static co_task sender(struct channel c)
{
    int evaluations = 0;

    ssize_t bytes = co_await co_write(evaluated_watch(c.watch, &evaluations), c.payload, TRANSFER_SIZE, 2);

    assert(bytes == TRANSFER_SIZE && evaluations == 1);

    ++*c.completed;
    assert(shutdown(c.fd, SHUT_WR) == 0);
}

static co_task receiver(struct channel c)
{
    size_t offset = 0;
    char buffer[4096] = {};

    assert(co_await co_sleep(0.002) == 0);
    while (offset < TRANSFER_SIZE) {
        ssize_t bytes = co_await co_read(c.watch, buffer, sizeof(buffer), 2);

        assert(bytes > 0);
        assert(memcmp(buffer, c.payload + offset, (size_t)bytes) == 0);
        offset += (size_t)bytes;
    }

    ssize_t bytes = co_await co_read(c.watch, buffer, sizeof(buffer), 2);

    assert(bytes == 0);

    ++*c.completed;
}

static co_task timeout_reader(struct channel c)
{
    char buffer[4096] = {};
    int evaluations = 0;

    ssize_t bytes = co_await co_read(c.watch, buffer, 1, 0);

    assert(bytes == -1 && errno == EAGAIN);

    bytes = co_await co_read(evaluated_watch(c.watch, &evaluations), buffer, 1, 0.003);
    assert(bytes == -1 && errno == ETIMEDOUT && evaluations == 1);

    bytes = co_await co_read(nullptr, buffer, 1, -1);
    assert(bytes == -1 && errno == EINVAL);

    bytes = co_await co_read(c.watch, NULL, 1, -1);
    assert(bytes == -1 && errno == EFAULT);

    bytes = co_await co_read(c.watch, buffer, 1, NAN);
    assert(bytes == -1 && errno == EINVAL);

    bytes = co_await co_read(c.watch, buffer, 1, -0.5);
    assert(bytes == -1 && errno == EINVAL);

    bytes = co_await co_write(c.watch, NULL, 0, -1);
    assert(bytes == 0);

    ++*c.completed;
}

static co_task partial_timeout(struct channel c)
{
    ssize_t bytes = co_await co_write(c.watch, c.payload, TRANSFER_SIZE, 0.003);

    assert(bytes == -1 && errno == ETIMEDOUT);
}

static co_task broken_writer(struct co_io_watch *watch)
{
    ssize_t bytes = co_await co_write(watch, "x", 1);

    assert(bytes == -1 && errno == EPIPE);
}

static co_task close_peer(int fd)
{
    assert(co_await co_sleep(0) == 0);
    assert(close(fd) == 0);
}

static void pair(int fds[2])
{
    int buffer = 4096;

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
    assert(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) == 0);
}

static co_task lazy_operations(struct co_io_watch *watch, int peer)
{
    char byte = 0;

    assert(write(peer, "r", 1) == 1);
    auto pending_read = co_read(watch, &byte, 1, 0.001);
    assert(byte == 0);

    assert(co_await co_sleep(0.004) == 0);

    auto moved_read = static_cast<co_io_awaitable &&>(pending_read);
    assert(co_await moved_read == 1 && byte == 'r');

    {
        auto unused = co_write(watch, "discard", 7);
        (void)unused;
    }

    auto pending_write = co_write(watch, "w", 1);
    assert(read(peer, &byte, 1) == -1 && errno == EAGAIN);

    auto moved_write = static_cast<co_io_awaitable &&>(pending_write);
    assert(co_await moved_write == 1);
    assert(read(peer, &byte, 1) == 1 && byte == 'w');

    /* Moving a not-yet-awaited operation must also work on the waiting path. */
    auto pending_timeout = co_read(watch, &byte, 1, 0.002);
    auto moved_timeout = static_cast<co_io_awaitable &&>(pending_timeout);

    assert(co_await moved_timeout == -1 && errno == ETIMEDOUT);
}

int main(void)
{
    int fds[2];
    int completed = 0;
    char *payload = (char *)malloc(TRANSFER_SIZE);
    struct channel a = {};

    a.payload = payload;
    a.completed = &completed;

    struct channel b = a;

    assert(payload);
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);

    for (size_t i = 0; i < TRANSFER_SIZE; ++i)
        payload[i] = (char)(i * 31);

    pair(fds);
    a.watch = co_io_watch_create(fds[0], CO_READ | CO_WRITE);
    b.watch = co_io_watch_create(fds[1], CO_READ | CO_WRITE);
    assert(a.watch && b.watch);

    assert(co_spawn(lazy_operations(a.watch, fds[1])) == 0);
    assert(co_run() == 0);

    assert(co_io_watch_destroy(a.watch) == 0);
    assert(co_io_watch_destroy(b.watch) == 0);
    close(fds[0]); close(fds[1]);

    pair(fds);
    a.watch = co_io_watch_create(fds[0], CO_READ | CO_WRITE);
    b.watch = co_io_watch_create(fds[1], CO_READ | CO_WRITE);
    assert(a.watch && b.watch);
    a.fd = fds[0]; b.fd = fds[1];

    assert(co_spawn(sender(a)) == 0);
    assert(co_spawn(receiver(b)) == 0);
    assert(co_run() == 0 && completed == 2);

    assert(co_io_watch_destroy(a.watch) == 0);
    assert(co_io_watch_destroy(b.watch) == 0);
    close(fds[0]); close(fds[1]);

    pair(fds);
    a.watch = co_io_watch_create(fds[0], CO_READ | CO_WRITE);
    b.watch = co_io_watch_create(fds[1], CO_READ | CO_WRITE);
    assert(a.watch && b.watch);
    a.fd = fds[0]; b.fd = fds[1];

    assert(co_spawn(timeout_reader(a)) == 0);
    assert(co_run() == 0 && completed == 3);

    assert(co_io_watch_destroy(a.watch) == 0);
    assert(co_io_watch_destroy(b.watch) == 0);
    close(fds[0]); close(fds[1]);

    pair(fds);
    a.watch = co_io_watch_create(fds[0], CO_READ | CO_WRITE);
    b.watch = co_io_watch_create(fds[1], CO_READ | CO_WRITE);
    assert(a.watch && b.watch);
    a.fd = fds[0];

    assert(co_spawn(partial_timeout(a)) == 0);
    assert(co_run() == 0);

    char drain[16384];
    ssize_t written = read(fds[1], drain, sizeof(drain));

    assert(written > 0 && written < TRANSFER_SIZE);

    assert(co_io_watch_destroy(a.watch) == 0);
    assert(co_io_watch_destroy(b.watch) == 0);
    close(fds[0]); close(fds[1]);

    /* The application ignores SIGPIPE so broken streams report EPIPE. */
    pair(fds);
    a.watch = co_io_watch_create(fds[0], CO_WRITE);
    assert(a.watch);
    assert(close(fds[1]) == 0);

    assert(co_spawn(broken_writer(a.watch)) == 0);
    assert(co_run() == 0);
    assert(co_io_watch_destroy(a.watch) == 0);
    assert(close(fds[0]) == 0);

    for (bool pending : {false, true}) {
        assert(pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
        a.watch = co_io_watch_create(fds[1], CO_WRITE);
        assert(a.watch);

        if (pending) {
            char buffer[4096] = {};

            while (write(fds[1], buffer, sizeof(buffer)) > 0) {}
            assert(errno == EAGAIN);
        } else {
            assert(close(fds[0]) == 0);
        }

        assert(co_spawn(broken_writer(a.watch)) == 0);

        if (pending)
            assert(co_spawn(close_peer(fds[0])) == 0);

        assert(co_run() == 0);
        assert(co_io_watch_destroy(a.watch) == 0);
        assert(close(fds[1]) == 0);
    }

    free(payload);

    puts("io: OK");

    return 0;
}
