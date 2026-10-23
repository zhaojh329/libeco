/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */

#include <sys/epoll.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <new>

#include "internal.h"

static thread_local struct co_loop local_loop;

struct co_loop *co_loop::get()
{
    struct co_loop *loop = &local_loop;

    if (!loop->initialized) {
        INIT_LIST_HEAD(&loop->tasks);
        INIT_LIST_HEAD(&loop->ready);
        INIT_LIST_HEAD(&loop->retired);
        loop->epoll_fd = -1;
        loop->initialized = true;
    }

    return loop;
}

int co_spawn(co_task coroutine)
{
    struct co_loop *loop = co_loop::get();
    struct co_context *task;

    if (!coroutine.handle) {
        errno = ENOMEM;
        return -1;
    }

    task = new (co_context::from_frame(coroutine.handle.address())) co_context{};

    INIT_LIST_HEAD(&task->all);
    INIT_LIST_HEAD(&task->queue);
    task->timer_index = co_context::no_timer;
    task->resume_data = coroutine.handle.address();
    coroutine.handle = nullptr;
    task->state = CO_READY;

    list_add_tail(&task->all, &loop->tasks);
    list_add_tail(&task->queue, &loop->ready);

    return 0;
}

void co_context::detach()
{
    co_loop::get()->timer_remove(this);

    switch (wait_kind) {
    case co_wait_kind::none:
        break;
    case co_wait_kind::io:
    case co_wait_kind::mutex:
        list_del(&queue);
        break;
    case co_wait_kind::cond:
        cond_wait->cond->detach(this);
        break;
    }

    watch = nullptr;
    wait_kind = co_wait_kind::none;
}

void co_context::make_ready(int error)
{
    assert(state == CO_WAITING);

    detach();
    this->error = error;
    state = CO_READY;
    list_add_tail(&queue, &co_loop::get()->ready);
}

int co_context::suspend(struct co_io_watch *watch, unsigned interests)
{
    if (watch)
        watch->attach(this, interests);

    if (co_loop::get()->timer_add(this) < 0) {
        int error = errno;

        detach();
        this->error = error;
        return -1;
    }

    state = CO_WAITING;

    return 0;
}

void co_context::destroy()
{
    detach();

    if (!list_empty(&queue))
        list_del(&queue);

    list_del(&all);
    std::coroutine_handle<>::from_address(this + 1).destroy();
}

void co_context::resume()
{
    if (resume_io) {
        resume_io = false;
        co_io_awaitable::resume(resume_data);
    } else {
        std::coroutine_handle<>::from_address(resume_data).resume();
    }
}

int co_run(void)
{
    struct co_loop *loop = co_loop::get();
    struct list_head *node, *next;
    int error;

    if (loop->running) {
        errno = EBUSY;
        return -1;
    }

    if (list_empty(&loop->tasks))
        return 0;

    loop->running = true;
    loop->fatal = 0;

    if (loop->epoll_fd < 0)
        loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    if (loop->epoll_fd < 0)
        loop->fatal = errno;

    while (!loop->fatal && !list_empty(&loop->tasks)) {
        /* Bound work even when callbacks spawn more tasks or continuously yield. */
        for (unsigned budget = 0; budget < 64 && !loop->fatal && !list_empty(&loop->ready); ++budget) {
            struct co_context *task = list_entry(loop->ready.next, struct co_context, queue);

            list_del(&task->queue);
            task->state = CO_RUNNING;
            loop->current = task;

            task->resume();

            loop->current = nullptr;

            if (std::coroutine_handle<>::from_address(task + 1).done())
                task->destroy();
            else if (task->state == CO_RUNNING)
                loop->fatal = EPROTO;

            if (loop->fatal)
                break;
        }

        if (loop->fatal || list_empty(&loop->tasks))
            break;

        loop->expire_timers();

        int timeout = list_empty(&loop->ready) ? loop->timer_timeout_ms() : 0;

        if (!loop->fatal && loop->poll_io(timeout) < 0)
            loop->fatal = errno;

        loop->expire_timers();
    }

    error = loop->fatal;

    list_for_each_safe(node, next, &loop->tasks)
        list_entry(node, struct co_context, all)->destroy();

    loop->reclaim_watches();

    if (!loop->watch_count && loop->epoll_fd >= 0) {
        close(loop->epoll_fd);
        loop->epoll_fd = -1;
    }

    free(loop->timers);
    loop->timers = NULL;
    loop->timer_count = loop->timer_capacity = 0;
    loop->running = false;
    loop->fatal = 0;

    if (error) {
        errno = error;
        return -1;
    }

    return 0;
}
