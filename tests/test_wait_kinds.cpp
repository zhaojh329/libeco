/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static unsigned stage;
static constexpr unsigned rounds = 8;

static co_task alternate(co_cond *cond, co_mutex *mutex, co_io_watch *watch)
{
    /* Failed waits must not leave queue membership behind for the next kind. */
    assert(co_await co_cond_wait(cond, 0.001) == -1 && errno == ETIMEDOUT);
    assert(co_await co_mutex_lock(mutex, 0.001) == -1 && errno == ETIMEDOUT);

    char byte;

    assert(co_await co_read(watch, &byte, 1, 0.001) == -1 && errno == ETIMEDOUT);

    for (unsigned i = 0; i < rounds; ++i) {
        stage = i * 4 + 1;
        assert(co_await co_cond_wait(cond, 1) == 0);
        stage = i * 4 + 2;
        assert(co_await co_mutex_lock(mutex, 1) == 0);
        assert(co_mutex_unlock(mutex) == 0);
        stage = i * 4 + 3;
        assert(co_await co_read(watch, &byte, 1, 1) == 1 && byte == 'x');
        assert(co_await co_sleep(0) == 0);
        stage = i * 4 + 4;
        assert(co_await co_cond_wait(cond, 1) == 0);
    }
}

static co_task drive(co_cond *cond, co_mutex *mutex, int fd)
{
    for (unsigned i = 0; i < rounds; ++i) {
        while (stage != i * 4 + 1)
            assert(co_await co_sleep(0) == 0);

        assert(co_cond_signal(cond) == 0);

        while (stage != i * 4 + 2)
            assert(co_await co_sleep(0) == 0);

        assert(co_mutex_unlock(mutex) == 0);

        while (stage != i * 4 + 3)
            assert(co_await co_sleep(0) == 0);

        assert(write(fd, "x", 1) == 1);

        while (stage != i * 4 + 4)
            assert(co_await co_sleep(0) == 0);

        assert(co_cond_signal(cond) == 0);

        if (i + 1 < rounds)
            assert(co_mutex_trylock(mutex) == 0);
    }
}

int main(void)
{
    int fds[2];

    assert(pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);

    auto *watch = co_io_watch_create(fds[0], CO_READ);
    auto *cond = co_cond_create();
    auto *mutex = co_mutex_create();

    assert(watch && cond && mutex);
    assert(co_mutex_trylock(mutex) == 0);
    assert(co_spawn(alternate(cond, mutex, watch)) == 0);
    assert(co_spawn(drive(cond, mutex, fds[1])) == 0);
    assert(co_run() == 0);
    assert(stage == rounds * 4);
    assert(co_mutex_destroy(mutex) == 0);
    assert(co_cond_destroy(cond) == 0);
    assert(co_io_watch_destroy(watch) == 0);
    close(fds[0]);
    close(fds[1]);

    puts("wait kinds: I/O, condition, mutex and timer transitions OK");

    return 0;
}
