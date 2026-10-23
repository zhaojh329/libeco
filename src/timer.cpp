/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <math.h>

#include <utility>

#include "internal.h"

/* CLOCK_MONOTONIC is available on the supported Linux runtime. */
double co_now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        co_loop::get()->fatal = errno;
        return 0;
    }

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

void co_loop::timer_swap(size_t a, size_t b)
{
    std::swap(timers[a], timers[b]);

    timers[a]->timer_index = a;
    timers[b]->timer_index = b;
}

void co_loop::timer_up(size_t index)
{
    while (index > 0) {
        size_t parent = (index - 1) / 2;

        if (timers[parent]->deadline <= timers[index]->deadline)
            break;

        timer_swap(parent, index);
        index = parent;
    }
}

int co_loop::timer_add(struct co_context *task)
{
    if (task->deadline < 0)
        return 0;

    if (timer_count >= co_context::no_timer) {
        errno = ENOMEM;
        return -1;
    }

    if (timer_count == timer_capacity) {
        size_t capacity = timer_capacity ? timer_capacity * 2 : 16;
        struct co_context **timers;

        if (capacity > co_context::no_timer)
            capacity = co_context::no_timer;

        if (capacity < timer_capacity || capacity > SIZE_MAX / sizeof(*timers)) {
            errno = ENOMEM;
            return -1;
        }

        timers = (struct co_context **)realloc(this->timers, capacity * sizeof(*timers));
        if (!timers)
            return -1;

        this->timers = timers;
        timer_capacity = capacity;
    }

    task->timer_index = timer_count;
    timers[timer_count++] = task;
    timer_up(task->timer_index);

    return 0;
}

void co_loop::timer_remove(struct co_context *task)
{
    size_t index = task->timer_index;

    if (index == co_context::no_timer)
        return;

    task->timer_index = co_context::no_timer;
    --timer_count;

    if (index == timer_count)
        return;

    timers[index] = timers[timer_count];
    timers[index]->timer_index = index;

    if (index > 0 && timers[index]->deadline < timers[(index - 1) / 2]->deadline) {
        timer_up(index);
        return;
    }

    while (index < timer_count / 2) {
        size_t child = index * 2 + 1;

        if (child + 1 < timer_count &&
            timers[child + 1]->deadline < timers[child]->deadline)
            ++child;

        if (timers[index]->deadline <= timers[child]->deadline)
            break;

        timer_swap(index, child);
        index = child;
    }
}

int co_loop::timer_timeout_ms()
{
    double milliseconds;

    if (!timer_count)
        return -1;

    milliseconds = (timers[0]->deadline - co_now()) * 1000.0;
    if (milliseconds <= 0)
        return 0;

    if (milliseconds >= INT_MAX)
        return INT_MAX;

    /* Round up so sub-millisecond waits do not spin. */
    int timeout = (int)milliseconds;

    return timeout + (milliseconds > timeout);
}

void co_loop::expire_timers()
{
    if (!timer_count)
        return;

    double now = co_now();

    while (timer_count && timers[0]->deadline <= now) {
        struct co_context *task = timers[0];

        task->make_ready(task->timeout_is_error ? ETIMEDOUT : 0);
    }
}

bool co_sleep_awaitable::await_suspend(std::coroutine_handle<> handle) noexcept
{
    context = co_loop::get()->current;
    assert(context && context->state == CO_RUNNING);

    context->resume_data = handle.address();
    context->error = 0;
    context->deadline = -1;
    context->timeout_is_error = false;

    if (!isfinite(seconds) || seconds < 0) {
        context->error = EINVAL;
        return false;
    }

    if (seconds == 0) {
        context->state = CO_WAITING;
        context->make_ready(0);

        return true;
    }

    context->deadline = co_now() + seconds;

    return context->suspend(nullptr, 0) == 0;
}

int co_sleep_awaitable::await_resume() const noexcept
{
    if (context->error) {
        errno = context->error;
        return -1;
    }

    return 0;
}

co_sleep_awaitable co_sleep(double seconds)
{
    return { seconds };
}
