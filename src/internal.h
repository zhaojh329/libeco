/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#ifndef ECO_INTERNAL_H
#define ECO_INTERNAL_H

#include <stdint.h>

#include "list.h"
#include "eco.h"

enum co_state : unsigned char { CO_READY, CO_RUNNING, CO_WAITING };
enum class co_wait_kind : unsigned char { none, io, cond, mutex };

struct co_io_watch;

/* The context prefixes its coroutine frame in one allocation. */
struct alignas(max_align_t) co_context {
    static constexpr unsigned timer_index_bits = 24;
    static constexpr unsigned no_timer = (1u << timer_index_bits) - 1;

    struct list_head all;
    /* A task belongs to either the ready queue or a wait queue. */
    struct list_head queue;
    void *resume_data;

    union {
        struct co_io_watch *watch;
        struct co_cond_awaitable *cond_wait;
        struct co_mutex_lock_awaitable *mutex_wait;
    };

    double deadline;
    int error;

    unsigned timer_index : timer_index_bits;
    enum co_state state : 2;
    co_wait_kind wait_kind : 2;
    unsigned timeout_is_error : 1;
    unsigned interests : 2;
    unsigned resume_io : 1;

    static struct co_context *from_frame(void *frame)
    {
        return (struct co_context *)((char *)frame - sizeof(struct co_context));
    }

    void detach();
    void make_ready(int error);
    int suspend(struct co_io_watch *watch, unsigned interests);
    void destroy();
    void resume();
};

struct co_loop {
    bool initialized;
    bool running;
    struct co_context *current;
    int epoll_fd;
    int fatal;
    struct list_head tasks;
    struct list_head ready;
    struct list_head retired;
    size_t watch_count;
    struct co_context **timers;
    size_t timer_count;
    size_t timer_capacity;

    static struct co_loop *get();

    int timer_add(struct co_context *task);
    void timer_remove(struct co_context *task);
    int timer_timeout_ms();
    void expire_timers();
    int poll_io(int timeout);
    void reclaim_watches();

private:
    void timer_swap(size_t a, size_t b);
    void timer_up(size_t index);
};

struct co_io_watch {
    static constexpr unsigned users_bits = 28;
    static constexpr unsigned max_users = (1u << users_bits) - 1;

    /* Retirement reuses storage after the watch has no remaining users. */
    union {
        struct list_head waiters;
        void (*callback)(struct co_io_watch *, unsigned);
        struct list_head retired;
    };

    struct co_loop *owner;
    int fd;
    unsigned users : users_bits;
    unsigned events : 2;
    unsigned dead : 1;
    unsigned has_callback : 1;

    /* Internal event sources dispatch directly; ordinary watches wake waiters. */
    static struct co_io_watch *create(int fd, unsigned events,
        void (*callback)(struct co_io_watch *, unsigned));

    int acquire(unsigned interests);
    void release();
    void attach(struct co_context *task, unsigned interests);
    void retire();
    /* Close a library-owned, unborrowed descriptor and retire its watch. */
    void close();
};

struct co_cond {
    struct list_head waiters;
    struct co_loop *owner;

    void detach(struct co_context *task);
};

double co_now(void);

#endif
