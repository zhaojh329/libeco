/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#ifndef ECO_TIME_H
#define ECO_TIME_H

#include <coroutine>

struct co_sleep_awaitable {
    double seconds;
    struct co_context *context = nullptr;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) noexcept;
    int await_resume() const noexcept;
};

/**
 * @brief Suspend the current coroutine for a duration, or yield when it is zero.
 * @param seconds Finite, nonnegative duration in seconds.
 * @return An awaitable yielding 0 on success, or -1 with errno set on failure.
 *         EINVAL indicates an invalid duration; ENOMEM indicates timer allocation failure.
 * @pre Await the result from a coroutine running under co_run().
 */
co_sleep_awaitable co_sleep(double seconds);

#endif
