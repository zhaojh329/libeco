/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */

#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>

#include "internal.h"

struct co_cond *co_cond_create(void)
{
    struct co_cond *cond = (struct co_cond *)calloc(1, sizeof(struct co_cond));

    if (!cond)
        return nullptr;

    INIT_LIST_HEAD(&cond->waiters);

    return cond;
}

int co_cond_destroy(struct co_cond *cond)
{
    if (!cond) {
        errno = EINVAL;
        return -1;
    }

    if (!list_empty(&cond->waiters)) {
        errno = EBUSY;
        return -1;
    }

    free(cond);

    return 0;
}

void co_cond::detach(struct co_context *task)
{
    struct co_cond_awaitable *waiter = task->cond_wait;

    assert(task->wait_kind == co_wait_kind::cond && waiter);

    list_del(&task->queue);
    if (list_empty(&waiters))
        owner = nullptr;

    /* The notifier may destroy cond before this coroutine resumes. */
    waiter->cond = nullptr;
}

bool co_cond_awaitable::await_suspend(std::coroutine_handle<> handle) noexcept
{
    struct co_loop *loop = co_loop::get();

    context = loop->current;
    assert(context && context->state == CO_RUNNING);

    context->resume_data = handle.address();
    context->error = 0;
    context->deadline = -1;
    context->timeout_is_error = true;

    if (!cond || (cond->owner && cond->owner != loop) ||
        !isfinite(timeout) || (timeout < 0 && timeout != -1)) {
        context->error = EINVAL;
        return false;
    }

    if (timeout == 0) {
        context->error = EAGAIN;
        return false;
    }

    if (timeout > 0)
        context->deadline = co_now() + timeout;

    assert(context->wait_kind == co_wait_kind::none);

    cond->owner = loop;
    list_add_tail(&context->queue, &cond->waiters);
    context->cond_wait = this;
    context->wait_kind = co_wait_kind::cond;

    return context->suspend(nullptr, 0) == 0;
}

int co_cond_awaitable::await_resume() const noexcept
{
    if (context->error) {
        errno = context->error;
        return -1;
    }

    return 0;
}

co_cond_awaitable co_cond_wait(struct co_cond *cond, double timeout)
{
    return { cond, timeout };
}

int co_cond_signal(struct co_cond *cond)
{
    if (!cond || (cond->owner && cond->owner != co_loop::get())) {
        errno = EINVAL;
        return -1;
    }

    if (!list_empty(&cond->waiters)) {
        struct co_context *task = list_entry(cond->waiters.next, struct co_context, queue);

        task->make_ready(0);
    }

    return 0;
}

int co_cond_broadcast(struct co_cond *cond)
{
    if (!cond || (cond->owner && cond->owner != co_loop::get())) {
        errno = EINVAL;
        return -1;
    }

    while (!list_empty(&cond->waiters)) {
        struct co_context *task = list_entry(cond->waiters.next, struct co_context, queue);

        task->make_ready(0);
    }

    return 0;
}
