/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#ifndef ECO_MUTEX_H
#define ECO_MUTEX_H

#include <coroutine>

struct co_mutex;

struct co_mutex_lock_awaitable {
    co_mutex_lock_awaitable(co_mutex *mutex, double timeout) noexcept
        : mutex(mutex), timeout(timeout) {}

    co_mutex_lock_awaitable(const co_mutex_lock_awaitable &) = delete;
    co_mutex_lock_awaitable &operator=(const co_mutex_lock_awaitable &) = delete;
    ~co_mutex_lock_awaitable();

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle) noexcept;
    int await_resume() noexcept;

private:
    friend struct co_mutex;

    co_mutex *mutex;
    double timeout;
    struct co_context *context = nullptr;
    bool borrowed = false;
    bool granted = false;
};

/**
 * @brief Create an unlocked mutex bound to the calling thread.
 * @return Caller-owned mutex, or nullptr with errno set to ENOMEM.
 * @note All operations must run on this thread, whose lifetime must cover the
 *       mutex. No cross-thread synchronization or recursive ownership is provided.
 */
co_mutex *co_mutex_create(void);

/**
 * @brief Destroy an unlocked mutex with no outstanding waiting operations.
 * @return 0 on success; -1 with EINVAL for nullptr/wrong thread, or EBUSY while
 *         locked or referenced by an operation which has not resumed or been destroyed.
 */
int co_mutex_destroy(co_mutex *mutex);

/**
 * @brief Acquire the mutex, suspending only the coroutine on contention, in FIFO order.
 * @param timeout -1 waits indefinitely, 0 checks immediately, positive finite
 *                values limit the wait in seconds.
 * @return Awaitable yielding 0 on acquisition, or -1 with errno: EINVAL for an
 *         invalid mutex/thread/timeout, EAGAIN if busy with zero timeout,
 *         ETIMEDOUT on expiry, or ENOMEM if timer allocation fails.
 * @pre Await from a coroutine under the creating thread's co_run().
 * @note Successful acquisition requires explicit unlock, even if the coroutine
 *       later exits or is destroyed. Repeated acquisition is ordinary contention.
 *       Cancellation before acquisition returns relinquishes any reserved grant.
 */
co_mutex_lock_awaitable co_mutex_lock(co_mutex *mutex, double timeout = -1);

/**
 * @brief Acquire immediately, including from ordinary code on the creating thread.
 * @return 0 on success; -1 with EINVAL for nullptr/wrong thread, EBUSY if locked.
 */
int co_mutex_trylock(co_mutex *mutex);

/**
 * @brief Release a successfully acquired lock or hand it to the oldest valid waiter.
 * @return 0 on success; -1 with EINVAL for nullptr/wrong thread, EPERM if unlocked,
 *         or EBUSY while a previous unlock's recipient has not resumed yet.
 * @note Any code on the creating thread may unlock; coroutine ownership is not
 *       checked. Waiters resume through the scheduler, never inside this call.
 */
int co_mutex_unlock(co_mutex *mutex);

#endif
