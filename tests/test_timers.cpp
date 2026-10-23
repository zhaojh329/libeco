/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include "internal.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec ts;

    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);

    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static co_task sleeper(int id, int *completed)
{
    double delay = (1 + id % 17) * 0.001;
    double start = now();

    assert(co_await co_sleep(delay) == 0);
    assert(now() - start >= delay);

    ++*completed;
}

static co_task reader(struct co_io_watch *watch, int *completed)
{
    int result = co_await co_wait_fd(watch, CO_READ, 0.03);

    assert(result == 0 || (result == -1 && errno == ETIMEDOUT));

    ++*completed;
}

static co_task send(int fd)
{
    assert(co_await co_sleep(0.002) == 0);
    assert(write(fd, "x", 1) == 1);
}

static co_task spinner(int *completed, int target)
{
    while (*completed < target)
        assert(co_await co_sleep(0) == 0);
}

static void timer_limit()
{
    co_loop loop{};
    co_context task{};

    task.timer_index = co_context::no_timer;
    task.state = CO_WAITING;
    task.interests = CO_READ | CO_WRITE;
    task.timeout_is_error = true;
    task.wait_kind = co_wait_kind::io;
    task.resume_io = true;
    loop.timer_count = co_context::no_timer;

    assert(loop.timer_add(&task) == -1 && errno == ENOMEM);
    assert(task.timer_index == co_context::no_timer);
    assert(loop.timer_count == co_context::no_timer && !loop.timers);
    loop.timer_remove(&task);
    assert(loop.timer_count == co_context::no_timer);

    loop.timer_count = 0;
    assert(loop.timer_add(&task) == 0);
    assert(task.timer_index == 0 && loop.timer_count == 1);
    loop.timer_remove(&task);
    assert(task.timer_index == co_context::no_timer && loop.timer_count == 0);
    assert(task.state == CO_WAITING && task.interests == (CO_READ | CO_WRITE));
    assert(task.timeout_is_error && task.wait_kind == co_wait_kind::io && task.resume_io);

    free(loop.timers);
}

int main(void)
{
    int completed = 0;
    int fds[2];

    timer_limit();

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);

    auto *watch = co_io_watch_create(fds[0], CO_READ);
    assert(watch);

    for (int i = 0; i < 256; ++i) {
        assert(co_spawn(sleeper(i, &completed)) == 0);
        assert(co_spawn(reader(watch, &completed)) == 0);
    }

    assert(co_spawn(send(fds[1])) == 0);
    assert(co_spawn(spinner(&completed, 512)) == 0);
    assert(co_run() == 0 && completed == 512);

    assert(co_io_watch_destroy(watch) == 0);
    close(fds[0]);
    close(fds[1]);

    puts("timers: OK");

    return 0;
}
