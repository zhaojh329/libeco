/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include "internal.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

static uint64_t first_received;
static uint64_t second_received;

static void first_ready(co_io_watch *watch, unsigned events)
{
    assert(events & EPOLLIN);
    assert(read(watch->fd, &first_received, sizeof(first_received)) == sizeof(first_received));
    watch->close();
}

static void second_ready(co_io_watch *watch, unsigned events)
{
    assert(events & EPOLLIN);
    assert(read(watch->fd, &second_received, sizeof(second_received)) == sizeof(second_received));
    watch->close();
}

static co_task read_pipe(co_io_watch *watch)
{
    char byte;

    assert(co_await co_read(watch, &byte, 1, 1) == 1 && byte == 'x');
    assert(co_await co_sleep(0.001) == 0);
}

int main(void)
{
    int a = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int b = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int pipe[2];

    assert(a >= 0 && b >= 0);
    assert(pipe2(pipe, O_CLOEXEC | O_NONBLOCK) == 0);

    auto *first = co_io_watch::create(a, CO_READ, first_ready);
    auto *second = co_io_watch::create(b, CO_READ, second_ready);
    auto *stream = co_io_watch_create(pipe[0], CO_READ);

    assert(first && second && stream);
    assert(first->acquire(CO_READ) == -1 && errno == EINVAL);

    stream->users = co_io_watch::max_users - 1;
    assert(stream->acquire(CO_READ) == 0);
    assert(stream->users == co_io_watch::max_users);
    assert(stream->acquire(CO_READ) == -1 && errno == EOVERFLOW);
    assert(stream->users == co_io_watch::max_users);
    assert(co_io_watch_destroy(stream) == -1 && errno == EBUSY);
    stream->release();
    assert(stream->users == co_io_watch::max_users - 1);
    assert(stream->events == CO_READ && !stream->dead && !stream->has_callback);
    stream->users = 0;

    uint64_t value = 7;

    assert(write(a, &value, sizeof(value)) == sizeof(value));
    value = 11;
    assert(write(b, &value, sizeof(value)) == sizeof(value));
    assert(write(pipe[1], "x", 1) == 1);
    assert(co_spawn(read_pipe(stream)) == 0);
    assert(co_run() == 0);
    assert(first_received == 7 && second_received == 11);
    assert(fcntl(a, F_GETFD) == -1 && errno == EBADF);
    assert(fcntl(b, F_GETFD) == -1 && errno == EBADF);
    assert(co_io_watch_destroy(stream) == 0);
    close(pipe[0]);
    close(pipe[1]);

    puts("sources: independent callbacks, ordinary I/O and callback teardown OK");

    return 0;
}
