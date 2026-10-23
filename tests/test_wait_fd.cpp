/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco.h>

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static int completed;

static co_task validation(int fd, struct co_io_watch *watch)
{
    assert(co_await co_wait_fd(nullptr, CO_READ) == -1 && errno == EINVAL);
    assert(co_await co_wait_fd(watch, 0) == -1 && errno == EINVAL);
    assert(co_await co_wait_fd(watch, 4) == -1 && errno == EINVAL);
    assert(co_await co_wait_fd(watch, CO_READ, NAN) == -1 && errno == EINVAL);
    assert(co_await co_wait_fd(watch, CO_READ, -0.5) == -1 && errno == EINVAL);
    assert(co_await co_wait_fd(watch, CO_READ, INFINITY) == -1 && errno == EINVAL);

    assert(co_await co_wait_fd(watch, CO_WRITE, 0) == 0);
    assert(co_await co_wait_fd(watch, CO_READ | CO_WRITE, 0) == 0);
    assert(co_await co_wait_fd(watch, CO_READ, 0) == -1 && errno == EAGAIN);
    assert(co_await co_wait_fd(watch, CO_READ, 0.002) == -1 && errno == ETIMEDOUT);

    int closed = dup(fd);

    assert(closed >= 0);
    close(closed);
    assert(co_io_watch_create(closed, CO_READ) == nullptr && errno == EBADF);
}

static co_task reader(int fd, struct co_io_watch *watch)
{
    assert(co_await co_wait_fd(watch, CO_READ, 1) == 0);

    /* Readiness does not consume data: every waiter sees the same byte. */
    char byte;

    assert(recv(fd, &byte, 1, MSG_PEEK) == 1 && byte == 'x');
    assert(co_await co_wait_fd(watch, CO_READ, 0) == 0);

    ++completed;
}

static co_task sender(int fd)
{
    assert(co_await co_sleep(0.002) == 0);
    assert(write(fd, "x", 1) == 1);
}

static co_task eof(int fd, struct co_io_watch *watch)
{
    assert(co_await co_wait_fd(watch, CO_READ, 1) == 0);

    char byte;

    assert(read(fd, &byte, 1) == 0);
}

static co_task writer(int fd, struct co_io_watch *watch)
{
    assert(co_await co_wait_fd(watch, CO_WRITE, 0) == -1 && errno == EAGAIN);
    assert(co_await co_wait_fd(watch, CO_WRITE, 1) == 0);
    assert(write(fd, "y", 1) == 1);
}

static co_task drain(int fd)
{
    char buffer[4096];

    assert(co_await co_sleep(0.002) == 0);

    while (read(fd, buffer, sizeof(buffer)) > 0) {}

    assert(errno == EAGAIN);
}

int main(void)
{
    int fds[2];
    struct co_io_watch *watch;

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
    watch = co_io_watch_create(fds[0], CO_READ | CO_WRITE);
    assert(watch);
    assert(co_spawn(validation(fds[0], watch)) == 0);
    assert(co_run() == 0);

    assert(co_spawn(reader(fds[0], watch)) == 0);
    assert(co_spawn(reader(fds[0], watch)) == 0);
    assert(co_spawn(sender(fds[1])) == 0);
    assert(co_run() == 0 && completed == 2);

    char byte;

    assert(read(fds[0], &byte, 1) == 1);
    close(fds[1]);

    assert(co_spawn(eof(fds[0], watch)) == 0);
    assert(co_run() == 0);

    assert(co_io_watch_destroy(watch) == 0);
    close(fds[0]);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
    watch = co_io_watch_create(fds[0], CO_READ | CO_WRITE);
    assert(watch);

    char buffer[4096] = {};

    while (write(fds[0], buffer, sizeof(buffer)) > 0) {}

    assert(errno == EAGAIN);

    assert(co_spawn(writer(fds[0], watch)) == 0);
    assert(co_spawn(drain(fds[1])) == 0);
    assert(co_run() == 0);

    assert(co_io_watch_destroy(watch) == 0);
    close(fds[0]);
    close(fds[1]);

    puts("wait_fd: OK");

    return 0;
}
