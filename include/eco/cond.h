/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#ifndef ECO_COND_H
#define ECO_COND_H

#include <coroutine>

struct co_cond;

struct co_cond_awaitable {
    struct co_cond *cond;
    double timeout;
    struct co_context *context = nullptr;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) noexcept;
    int await_resume() const noexcept;
};

/**
 * @brief Create a condition with an empty wait queue.
 * @return A condition owned by the caller, or nullptr with errno set to ENOMEM.
 * @note Release the condition with co_cond_destroy(). All accesses must be
 *       serialized; while waiters exist, use their scheduler thread.
 */
struct co_cond *co_cond_create(void);

/**
 * @brief Destroy a condition that has no queued waiters.
 * @param cond Condition to destroy. Must not be nullptr.
 * @return 0 on success, or -1 with errno set to EINVAL for nullptr or EBUSY
 *         while waiters remain queued. On failure, the condition is unchanged.
 * @note Serialize access with all other operations on cond. Notified waiters
 *       are already detached, so cond may be destroyed before they resume.
 */
int co_cond_destroy(struct co_cond *cond);

/**
 * @brief Wait for a condition notification, optionally with a timeout.
 * @param cond Condition to wait on. Must remain alive while the waiter is queued.
 * @param timeout Timeout in seconds: -1 waits indefinitely, 0 returns immediately,
 *                and a positive finite value limits the wait.
 * @return An awaitable yielding 0 on notification, or -1 with errno set:
 *         EINVAL for nullptr, an invalid timeout, or a different active scheduler;
 *         EAGAIN for zero timeout; ETIMEDOUT on expiry; ENOMEM on allocation failure.
 * @pre Await the result from a coroutine running under co_run(). Serialize access
 *      to cond and use the same scheduler thread as any existing waiters.
 * @note Notifications are not retained. Check the application predicate in a loop.
 */
co_cond_awaitable co_cond_wait(struct co_cond *cond, double timeout = -1);

/**
 * @brief Make the oldest queued waiter ready to resume.
 * @param cond Condition to notify. Must not be nullptr.
 * @return 0 on success, including when no waiters exist; -1 with errno set to
 *         EINVAL for nullptr or a different active scheduler.
 * @pre Serialize access to cond; while waiters exist, use their scheduler thread.
 * @note The waiter resumes through the scheduler, not inside this call.
 *       A notification with no waiters is discarded.
 */
int co_cond_signal(struct co_cond *cond);

/**
 * @brief Make all queued waiters ready to resume in FIFO order.
 * @param cond Condition to notify. Must not be nullptr.
 * @return 0 on success, including when no waiters exist; -1 with errno set to
 *         EINVAL for nullptr or a different active scheduler.
 * @pre Serialize access to cond; while waiters exist, use their scheduler thread.
 * @note Waiters resume through the scheduler, not inside this call.
 *       Notifications are not retained for future waiters.
 */
int co_cond_broadcast(struct co_cond *cond);

#endif
