/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct context {
    int fd;
    struct co_io_watch *watch;
    unsigned *completed;
};

static co_task read_timeout(struct context c)
{
    char byte = 0;

    ssize_t bytes = co_await co_read(c.watch, &byte, 1, 0.002);

    assert(bytes == -1 && errno == ETIMEDOUT);

    /* Repeat a read wait while a writer remains on the same watch. */
    bytes = co_await co_read(c.watch, &byte, 1, 1);
    assert(bytes == 1 && byte == 'x');

    ++*c.completed;
}

static co_task write_waiter(struct context c)
{
    int result = co_await co_write(c.watch, "y", 1, 1);

    assert(result == 1);

    ++*c.completed;
}

static co_task drain_and_reply(struct context c)
{
    char buffer[4096] = {};

    assert(co_await co_sleep(0.01) == 0);
    ssize_t count;

    do {
        count = read(c.fd, buffer, sizeof(buffer));
    } while (count > 0);

    assert(count == -1 && errno == EAGAIN);
    assert(write(c.fd, "x", 1) == 1);
}

static co_task competing_reader(struct context c)
{
    char byte = 0;

    ssize_t bytes = co_await co_read(c.watch, &byte, 1, 0.02);

    if (bytes == 1) {
        assert(byte == 'x');
        *c.completed += 1;
    } else {
        assert(bytes == -1 && errno == ETIMEDOUT);
        *c.completed += 10;
    }
}

static co_task send_byte(struct context c)
{
    assert(co_await co_sleep(0.001) == 0);
    assert(write(c.fd, "x", 1) == 1);
}

int main(void)
{
    int fds[2];
    int size = 4096;
    char buffer[4096] = {};
    ssize_t count;
    unsigned completed = 0;
    struct context a = {};

    a.completed = &completed;

    struct context b = a;

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
    assert(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);

    do {
        count = write(fds[0], buffer, sizeof(buffer));
    } while (count > 0);

    assert(count == -1 && errno == EAGAIN);

    a.fd = fds[0]; b.fd = fds[1];
    a.watch = co_io_watch_create(a.fd, CO_READ | CO_WRITE);
    assert(a.watch);

    assert(co_spawn(read_timeout(a)) == 0);
    assert(co_spawn(write_waiter(a)) == 0);
    assert(co_spawn(drain_and_reply(b)) == 0);
    assert(co_run() == 0 && completed == 2);

    assert(co_io_watch_destroy(a.watch) == 0);
    close(fds[0]); close(fds[1]);

    /* Both readers wake for one byte. The loser must wait again with its
     * original deadline, without dangling event pointers or duplicate wakeups. */
    completed = 0;
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);

    a.fd = fds[0]; b.fd = fds[1];
    a.watch = co_io_watch_create(a.fd, CO_READ | CO_WRITE);
    assert(a.watch);

    assert(co_spawn(competing_reader(a)) == 0);
    assert(co_spawn(competing_reader(a)) == 0);
    assert(co_spawn(send_byte(b)) == 0);
    assert(co_run() == 0 && completed == 11);

    assert(co_io_watch_destroy(a.watch) == 0);
    close(fds[0]); close(fds[1]);

    puts("events: OK");

    return 0;
}
