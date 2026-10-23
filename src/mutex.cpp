/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>

#include "internal.h"

struct co_mutex {
    co_loop *owner;
    struct list_head waiters;
    size_t users;
    bool locked;
    bool reserved;

    void release() noexcept;
};

co_mutex *co_mutex_create(void)
{
    auto *mutex = static_cast<co_mutex *>(calloc(1, sizeof(co_mutex)));

    if (!mutex)
        return nullptr;

    mutex->owner = co_loop::get();
    INIT_LIST_HEAD(&mutex->waiters);

    return mutex;
}

int co_mutex_destroy(co_mutex *mutex)
{
    if (!mutex || mutex->owner != co_loop::get()) {
        errno = EINVAL;
        return -1;
    }

    if (mutex->locked || mutex->users) {
        errno = EBUSY;
        return -1;
    }

    free(mutex);

    return 0;
}

void co_mutex::release() noexcept
{
    reserved = false;

    if (!list_empty(&waiters)) {
        auto *task = list_entry(waiters.next, co_context, queue);
        auto *waiter = task->mutex_wait;

        assert(task->wait_kind == co_wait_kind::mutex && task->state == CO_WAITING);

        reserved = true;
        waiter->granted = true;
        waiter->context->make_ready(0);
        return;
    }

    locked = false;
}

int co_mutex_trylock(co_mutex *mutex)
{
    if (!mutex || mutex->owner != co_loop::get()) {
        errno = EINVAL;
        return -1;
    }

    if (mutex->locked) {
        errno = EBUSY;
        return -1;
    }

    mutex->locked = true;

    return 0;
}

int co_mutex_unlock(co_mutex *mutex)
{
    if (!mutex || mutex->owner != co_loop::get()) {
        errno = EINVAL;
        return -1;
    }

    if (!mutex->locked) {
        errno = EPERM;
        return -1;
    }

    if (mutex->reserved) {
        errno = EBUSY;
        return -1;
    }

    mutex->release();

    return 0;
}

bool co_mutex_lock_awaitable::await_suspend(std::coroutine_handle<> handle) noexcept
{
    auto *loop = co_loop::get();

    context = loop->current;
    assert(context && context->state == CO_RUNNING);

    context->resume_data = handle.address();
    context->error = 0;
    context->deadline = -1;
    context->timeout_is_error = true;

    if (!mutex || mutex->owner != loop || !isfinite(timeout) ||
        (timeout < 0 && timeout != -1)) {
        context->error = EINVAL;
        return false;
    }

    if (!mutex->locked) {
        mutex->locked = true;
        return false;
    }

    if (timeout == 0) {
        context->error = EAGAIN;
        return false;
    }

    if (timeout > 0)
        context->deadline = co_now() + timeout;

    assert(context->wait_kind == co_wait_kind::none);

    list_add_tail(&context->queue, &mutex->waiters);
    context->mutex_wait = this;
    context->wait_kind = co_wait_kind::mutex;
    borrowed = true;
    ++mutex->users;

    return context->suspend(nullptr, 0) == 0;
}

co_mutex_lock_awaitable::~co_mutex_lock_awaitable()
{
    if (!borrowed)
        return;

    if (granted)
        mutex->release();

    --mutex->users;
}

int co_mutex_lock_awaitable::await_resume() noexcept
{
    if (borrowed) {
        if (granted) {
            mutex->reserved = false;
            granted = false;
        }

        --mutex->users;
        borrowed = false;
    }

    if (context->error) {
        errno = context->error;
        return -1;
    }

    return 0;
}

co_mutex_lock_awaitable co_mutex_lock(co_mutex *mutex, double timeout)
{
    return {mutex, timeout};
}
