/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#ifndef ECO_COROUTINE_H
#define ECO_COROUTINE_H

#include <stddef.h>

#include <coroutine>
#include <utility>

/* A lazy coroutine. co_spawn transfers ownership to the scheduler. */
struct co_task {
    struct promise_type;

    co_task() noexcept = default;

    co_task(co_task &&other) noexcept : handle(std::exchange(other.handle, nullptr)) {}

    co_task(const co_task &) = delete;
    co_task &operator=(const co_task &) = delete;

    ~co_task() { if (handle) handle.destroy(); }

private:
    explicit co_task(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}

    std::coroutine_handle<promise_type> handle{};

    friend int co_spawn(co_task task);
};

/* Coroutine promise protocol; use the functions in eco.h. */
struct co_task::promise_type {
    static void *operator new(size_t size) noexcept;
    static void operator delete(void *pointer) noexcept;
    static co_task get_return_object_on_allocation_failure() noexcept;
    co_task get_return_object() noexcept;

    std::suspend_always initial_suspend() noexcept { return {}; }

    std::suspend_always final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    /* An exception escaping the coroutine body terminates the process via abort(). */
    void unhandled_exception() noexcept;
};

#endif
