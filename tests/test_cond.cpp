/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco.h>

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>

static int order[3];
static int completed;

static co_task waiter(struct co_cond *cond, int id, double timeout = -1)
{
    assert(co_await co_cond_wait(cond, timeout) == 0);
    order[completed++] = id;
}

static co_task notify(struct co_cond *cond)
{
    assert(co_cond_destroy(cond) == -1 && errno == EBUSY);

    assert(co_cond_signal(cond) == 0);
    assert(completed == 0); /* Notification must not resume inline. */
    assert(co_await co_sleep(0) == 0);
    assert(completed == 1 && order[0] == 0);

    assert(co_cond_broadcast(cond) == 0);
    assert(completed == 1);

    /* Awakened tasks must not dereference the destroyed condition. */
    assert(co_cond_destroy(cond) == 0);
}

static co_task timeout_waiter(struct co_cond *cond)
{
    assert(co_await co_cond_wait(nullptr) == -1 && errno == EINVAL);
    assert(co_await co_cond_wait(cond, NAN) == -1 && errno == EINVAL);
    assert(co_await co_cond_wait(cond, INFINITY) == -1 && errno == EINVAL);
    assert(co_await co_cond_wait(cond, -0.5) == -1 && errno == EINVAL);
    assert(co_await co_cond_wait(cond, 0) == -1 && errno == EAGAIN);
    assert(co_await co_cond_wait(cond, 0.002) == -1 && errno == ETIMEDOUT);
}

static co_task timed_out_middle(struct co_cond *cond)
{
    assert(co_await co_cond_wait(cond, 0.002) == -1 && errno == ETIMEDOUT);

    assert(co_cond_broadcast(cond) == 0);
    assert(co_cond_destroy(cond) == 0);
}

static co_task unsupported(void)
{
    co_await std::suspend_always{};
}

int main(void)
{
    assert(co_cond_destroy(nullptr) == -1 && errno == EINVAL);
    assert(co_cond_signal(nullptr) == -1 && errno == EINVAL);
    assert(co_cond_broadcast(nullptr) == -1 && errno == EINVAL);

    struct co_cond *cond = co_cond_create();

    assert(cond);

    assert(co_cond_signal(cond) == 0);
    assert(co_cond_broadcast(cond) == 0);
    assert(co_spawn(timeout_waiter(cond)) == 0);
    assert(co_run() == 0); /* Earlier notifications were not retained. */
    assert(co_spawn(waiter(cond, 0)) == 0);
    assert(co_spawn(waiter(cond, 1, 1)) == 0);
    assert(co_spawn(waiter(cond, 2, 1)) == 0);
    assert(co_spawn(notify(cond)) == 0);
    assert(co_run() == 0 && completed == 3);
    assert(order[1] == 1 && order[2] == 2);

    completed = 0;
    cond = co_cond_create();
    assert(cond);

    assert(co_spawn(waiter(cond, 0)) == 0);
    assert(co_spawn(timed_out_middle(cond)) == 0);
    assert(co_spawn(waiter(cond, 1)) == 0);
    assert(co_run() == 0 && completed == 2);
    assert(order[0] == 0 && order[1] == 1);

    cond = co_cond_create();
    assert(cond);
    assert(co_spawn(waiter(cond, 0, 1)) == 0);
    assert(co_spawn(unsupported()) == 0);
    assert(co_run() == -1 && errno == EPROTO);
    assert(co_cond_destroy(cond) == 0);

    puts("cond: OK");

    return 0;
}
