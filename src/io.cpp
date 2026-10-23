/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */

#include <sys/epoll.h>
#include <stdlib.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <poll.h>

#include "internal.h"
#include "eco/io.h"

static unsigned epoll_mask(unsigned interests)
{
    unsigned mask = 0;

    if (interests & CO_READ)
        mask |= EPOLLIN | EPOLLRDHUP;

    if (interests & CO_WRITE)
        mask |= EPOLLOUT;

    return mask;
}

struct co_io_watch *co_io_watch::create(int fd, unsigned events,
    void (*callback)(struct co_io_watch *, unsigned))
{
    struct co_loop *loop = co_loop::get();

    if (fd < 0) {
        errno = EBADF;
        return nullptr;
    }

    if (!events || (events & ~(CO_READ | CO_WRITE))) {
        errno = EINVAL;
        return nullptr;
    }

    int flags = fcntl(fd, F_GETFL);

    if (flags < 0)
        return nullptr;

    if (!(flags & O_NONBLOCK)) {
        errno = EINVAL;
        return nullptr;
    }

    auto *watch = (struct co_io_watch *)calloc(1, sizeof(struct co_io_watch));
    if (!watch)
        return nullptr;

    bool opened = loop->epoll_fd < 0;

    if (opened)
        loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    struct epoll_event event = {};

    /* Callback sources remain level-triggered so failed reads can be retried. */
    event.events = epoll_mask(events);

    if (!callback)
        event.events |= EPOLLET;

    event.data.ptr = watch;

    if (loop->epoll_fd < 0 || epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        int error = errno;

        if (opened && loop->epoll_fd >= 0) {
            ::close(loop->epoll_fd);
            loop->epoll_fd = -1;
        }

        free(watch);
        errno = error;
        return nullptr;
    }

    watch->has_callback = callback != nullptr;

    if (callback)
        watch->callback = callback;
    else
        INIT_LIST_HEAD(&watch->waiters);

    watch->owner = loop;
    watch->fd = fd;
    watch->events = events;
    ++loop->watch_count;

    return watch;
}

struct co_io_watch *co_io_watch_create(int fd, unsigned events)
{
    return co_io_watch::create(fd, events, nullptr);
}

void co_io_watch::retire()
{
    struct co_loop *loop = owner;

    dead = true;
    --loop->watch_count;
    list_add_tail(&retired, &loop->retired);

    if (!loop->running) {
        loop->reclaim_watches();

        if (!loop->watch_count) {
            ::close(loop->epoll_fd);
            loop->epoll_fd = -1;
        }
    }
}

int co_io_watch_destroy(struct co_io_watch *watch)
{
    struct co_loop *loop = co_loop::get();

    if (!watch || watch->dead || watch->owner != loop) {
        errno = EINVAL;
        return -1;
    }

    if (watch->users) {
        errno = EBUSY;
        return -1;
    }

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, watch->fd, nullptr) < 0)
        return -1;

    watch->retire();

    return 0;
}

void co_io_watch::close()
{
    assert(!dead && !users && owner == co_loop::get());

    /* Closing the only copy removes it from epoll even if DEL would fail. */
    ::close(fd);
    retire();
}

int co_io_watch::acquire(unsigned interests)
{
    if (dead || owner != co_loop::get() || has_callback ||
        !interests || (interests & ~events)) {
        errno = EINVAL;
        return -1;
    }

    if (users == max_users) {
        errno = EOVERFLOW;
        return -1;
    }

    ++users;

    return 0;
}

void co_io_watch::release()
{
    assert(users);
    --users;
}

void co_io_watch::attach(struct co_context *task, unsigned interests)
{
    assert(!has_callback);

    assert(task->wait_kind == co_wait_kind::none);

    task->watch = this;
    task->wait_kind = co_wait_kind::io;
    task->interests = interests;
    list_add_tail(&task->queue, &waiters);
}

void co_loop::reclaim_watches()
{
    struct list_head *node, *next;

    list_for_each_safe(node, next, &retired) {
        struct co_io_watch *watch = list_entry(node, struct co_io_watch, retired);

        list_del(node);
        free(watch);
    }
}

int co_loop::poll_io(int timeout)
{
    if (!watch_count && timeout == 0) {
        reclaim_watches();
        return 0;
    }

    struct epoll_event events[64];
    int count = epoll_wait(epoll_fd, events, 64, timeout);

    if (count < 0) {
        if (errno == EINTR)
            return 0;

        return -1;
    }

    for (int i = 0; i < count; ++i) {
        struct co_io_watch *watch = (struct co_io_watch *)events[i].data.ptr;
        struct list_head *node, *next;
        unsigned mask = events[i].events;

        if (watch->dead)
            continue;

        if (watch->has_callback) {
            watch->callback(watch, mask);
            continue;
        }

        list_for_each_safe(node, next, &watch->waiters) {
            struct co_context *task = list_entry(node, struct co_context, queue);
            bool readable = (task->interests & CO_READ) && (mask & (EPOLLIN | EPOLLRDHUP));
            bool writable = (task->interests & CO_WRITE) && (mask & EPOLLOUT);

            if (readable || writable || (mask & (EPOLLERR | EPOLLHUP)))
                task->make_ready(0);
        }
    }

    reclaim_watches();

    return 0;
}

bool co_fd_awaitable::await_suspend(std::coroutine_handle<> handle) noexcept
{
    context = co_loop::get()->current;
    assert(context && context->state == CO_RUNNING);

    context->resume_data = handle.address();
    context->error = 0;
    context->deadline = -1;
    context->timeout_is_error = true;

    if (!watch || !isfinite(timeout) || (timeout < 0 && timeout != -1)) {
        context->error = EINVAL;
        return false;
    }

    if (watch->acquire(events) < 0) {
        context->error = errno;
        return false;
    }

    borrowed = true;
    deadline = timeout > 0 ? co_now() + timeout : -1;
    context->deadline = deadline;

    /* An earlier edge may have been consumed while no coroutine was waiting. */
    struct pollfd pfd = {};

    pfd.fd = watch->fd;
    pfd.events = ((events & CO_READ) ? POLLIN : 0) | ((events & CO_WRITE) ? POLLOUT : 0);

    int result;

    do {
        result = poll(&pfd, 1, 0);
    } while (result < 0 && errno == EINTR);

    if (result < 0)
        context->error = errno;
    else if (pfd.revents & POLLNVAL)
        context->error = EBADF;
    else if (result == 0) {
        if (timeout != 0)
            return context->suspend(watch, events) == 0;

        context->error = EAGAIN;
    }

    return false;
}

co_fd_awaitable::~co_fd_awaitable()
{
    if (borrowed)
        watch->release();
}

int co_fd_awaitable::await_resume() noexcept
{
    if (borrowed) {
        watch->release();
        borrowed = false;
    }

    if (context->error) {
        errno = context->error;
        return -1;
    }

    if (deadline > 0 && co_now() >= deadline) {
        errno = ETIMEDOUT;
        return -1;
    }

    return 0;
}

co_fd_awaitable co_wait_fd(struct co_io_watch *watch, unsigned events, double timeout)
{
    return { watch, events, timeout };
}

static double monotonic_now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return -1;

    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int prepare_deadline(double timeout, double *deadline, double *now)
{
    if (!isfinite(timeout) || (timeout < 0 && timeout != -1)) {
        errno = EINVAL;
        return -1;
    }

    *now = timeout > 0 ? monotonic_now() : 0;

    if (*now < 0)
        return -1;

    *deadline = timeout > 0 ? *now + timeout : timeout;

    return 0;
}

static int check_buffer(const void *buf, size_t len)
{
    if (len > (size_t)SSIZE_MAX) {
        errno = EINVAL;
        return -1;
    }

    if (!buf && len) {
        errno = EFAULT;
        return -1;
    }

    return 0;
}

static int check_deadline(double deadline, double now)
{
    if (deadline <= 0)
        return 0;

    if (now < 0)
        now = monotonic_now();

    if (now < 0)
        return -1;

    if (now >= deadline) {
        errno = ETIMEDOUT;
        return -1;
    }

    return 0;
}

bool co_io_awaitable::try_complete(double now) noexcept
{
    for (;;) {
        if (state.kind == operation::write && state.offset == state.length) {
            state.result = (ssize_t)state.offset;
            return true;
        }

        if (check_deadline(state.deadline, now) < 0)
            break;

        /* Reuse the initial timestamp only once; retries need a fresh check. */
        now = -1;

        ssize_t count;

        switch (state.kind) {
        case operation::read:
            count = read(state.watch->fd, state.buffer, state.length);
            break;
        case operation::write:
            count = write(state.watch->fd, (const char *)state.buffer + state.offset,
                          state.length - state.offset);
            break;
        default:
            errno = EINVAL;
            count = -1;
            break;
        }

        if (count >= 0) {
            if (state.kind != operation::write) {
                state.result = count;
                return true;
            }

            if (count == 0) {
                errno = EIO;
                break;
            }

            state.offset += (size_t)count;
            continue;
        }

        if (errno == EINTR)
            continue;

        if ((errno == EAGAIN || errno == EWOULDBLOCK) && state.deadline != 0)
            return false;

        break;
    }

    state.result = -1;
    state.error = errno;
    return true;
}

bool co_io_awaitable::await_ready() noexcept
{
    if (state.kind == operation::invalid || !state.watch) {
        state.error = EINVAL;
        return true;
    }

    unsigned interests = state.kind == operation::write ? CO_WRITE : CO_READ;

    if (state.watch->acquire(interests) < 0) {
        state.error = errno;
        return true;
    }

    state.borrowed = true;

    double now;

    if (prepare_deadline(state.deadline, &state.deadline, &now) < 0) {
        state.error = errno;
        return true;
    }

    if (check_buffer(state.buffer, state.length) < 0) {
        state.error = errno;
        return true;
    }

    return try_complete(now);
}

bool co_io_awaitable::suspend() noexcept
{
    struct co_context *context = state.context;
    unsigned interests = state.kind == operation::write ? CO_WRITE : CO_READ;

    context->error = 0;
    context->deadline = state.deadline > 0 ? state.deadline : -1;
    context->timeout_is_error = true;

    if (context->suspend(state.watch, interests) < 0) {
        state.error = context->error;
        return false;
    }

    context->resume_data = this;
    context->resume_io = true;

    return true;
}

bool co_io_awaitable::await_suspend(std::coroutine_handle<> parent) noexcept
{
    state.parent = parent;
    state.context = co_loop::get()->current;
    assert(state.context && state.context->state == CO_RUNNING);

    return suspend();
}

void co_io_awaitable::resume(void *operation) noexcept
{
    auto *io = (co_io_awaitable *)operation;

    if (io->state.context->error)
        io->state.error = io->state.context->error;
    else if (!io->try_complete() && io->suspend())
        return;

    io->state.context->resume_data = io->state.parent.address();
    io->state.parent.resume();
}

co_io_awaitable::~co_io_awaitable()
{
    if (state.borrowed)
        state.watch->release();
}

ssize_t co_io_awaitable::await_resume() noexcept
{
    if (state.borrowed) {
        state.watch->release();
        state.borrowed = false;
    }

    if (state.error) {
        errno = state.error;
        return -1;
    }

    return state.result;
}

co_io_awaitable co_read(struct co_io_watch *watch, void *buf, size_t len, double timeout)
{
    return {co_io_awaitable::operation::read, watch, buf, len, timeout};
}

co_io_awaitable co_write(struct co_io_watch *watch, const void *buf, size_t len, double timeout)
{
    return {co_io_awaitable::operation::write, watch, const_cast<void *>(buf), len, timeout};
}
