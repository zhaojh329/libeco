/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco.h>

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

static int order[64];
static int order_count;

static double now(void)
{
    struct timespec ts;

    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static co_task worker(int id, int *completed)
{
    char buffer[32] = {};
    char *original = buffer;

    for (int iteration = 0; iteration < 5; ++iteration) {
        buffer[iteration] = (char)(id + iteration);
        order[order_count++] = id;

        co_await co_sleep(0);
        assert(original == buffer && buffer[iteration] == id + iteration);

        if (iteration % 2)
            co_await co_sleep(0);
    }

    ++*completed;
}

static co_task increment(int *count)
{
    ++*count;
    co_await co_sleep(0);
    ++*count;
}

static co_task operations(int *completed)
{
    assert(co_run() == -1 && errno == EBUSY);
    assert(co_await co_sleep(0) == 0);
    assert(co_await co_sleep(-1) == -1 && errno == EINVAL);
    assert(co_await co_sleep(NAN) == -1 && errno == EINVAL);
    assert(co_await co_sleep(INFINITY) == -1 && errno == EINVAL);

    double start = now();

    assert(co_await co_sleep(0.002) == 0 && now() - start >= 0.002);

    ++*completed;
}

/* Test-only probes verify frame destruction; the library does not own business resources. */
struct lifetime {
    int *destroyed;

    ~lifetime() { ++*destroyed; }
};

static co_task counted(lifetime value, int *entered)
{
    (void)value;
    ++*entered;
    co_await co_sleep(0.001);
}

static co_task unsupported_suspend(void)
{
    co_await std::suspend_always{};
}

static co_task parent(int *completed)
{
    assert(co_spawn(increment(completed)) == 0);
    co_return;
}

int main(void)
{
    int completed = 0;
    int destroyed = 0;
    int entered = 0;

    {
        auto unused = counted({&destroyed}, &entered);

        assert(co_run() == 0 && entered == 0);
    }

    /* The argument temporary and the copy stored in the frame are both destroyed. */
    assert(destroyed == 2);

    int moved_count = 0;

    {
        auto pending = increment(&moved_count);
        auto moved = static_cast<co_task &&>(pending);

        assert(co_spawn(static_cast<co_task &&>(moved)) == 0);
    }

    assert(co_run() == 0 && moved_count == 2);

    assert(co_spawn(worker(1, &completed)) == 0);
    assert(co_spawn(worker(2, &completed)) == 0);
    assert(co_spawn(operations(&completed)) == 0);
    assert(co_run() == 0);
    assert(completed == 3 && order_count == 10);

    for (int i = 0; i < order_count; ++i)
        assert(order[i] == i % 2 + 1);

    int counts[2] = {17, 0};

    assert(co_spawn(increment(&counts[1])) == 0);
    assert(co_spawn(increment(&counts[1])) == 0);
    assert(co_run() == 0);
    assert(counts[0] == 17 && counts[1] == 4);

    destroyed = 0;
    assert(co_spawn(counted({&destroyed}, &entered)) == 0);
    assert(co_run() == 0);
    assert(destroyed == 2 && entered == 1);

    destroyed = 0;
    entered = 0;
    assert(co_spawn(counted({&destroyed}, &entered)) == 0);
    assert(co_spawn(unsupported_suspend()) == 0);
    assert(co_run() == -1 && errno == EPROTO);
    assert(destroyed == 2 && entered == 1);

    assert(co_spawn(parent(&counts[1])) == 0);
    assert(co_run() == 0 && counts[1] == 6);

    puts("core: OK");

    return 0;
}
