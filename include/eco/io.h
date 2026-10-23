/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#ifndef ECO_IO_H
#define ECO_IO_H

#include <sys/types.h>
#include <stddef.h>

#include <coroutine>

#define CO_READ 1u
#define CO_WRITE 2u

struct co_io_watch;

/**
 * @brief Create a persistent I/O watch owned by the calling thread.
 * @param fd Nonblocking descriptor supported by epoll, owned by the caller.
 * @param events Nonzero combination of CO_READ and CO_WRITE to monitor.
 * @return A watch owned by the caller, or nullptr with errno set on failure.
 *         EINVAL indicates invalid events or a descriptor without O_NONBLOCK;
 *         EBADF an invalid fd; EEXIST an already registered fd.
 *         Allocation, fcntl and epoll errors propagate.
 * @note Registers once, independently of individual waits and co_run() calls.
 *       Checks O_NONBLOCK without changing descriptor flags. Keep fd open and
 *       nonblocking, and do not reuse its number until the watch is destroyed.
 *       Share one watch for concurrent reads and writes on the same fd.
 */
struct co_io_watch *co_io_watch_create(int fd, unsigned events);

/**
 * @brief Destroy a watch and unregister its descriptor without closing it.
 * @param watch Watch created on the calling thread.
 * @return 0 on success, or -1 with errno set on failure. EBUSY indicates an
 *         awaited operation has not resumed or been destroyed yet; EINVAL an
 *         invalid watch or wrong thread. epoll errors propagate.
 * @note On failure the watch remains owned by the caller. The scheduler never
 *       destroys caller-owned watches, including after a fatal co_run() error.
 *       Do not use the watch after successful destruction.
 */
int co_io_watch_destroy(struct co_io_watch *watch);

struct co_fd_awaitable {
    co_fd_awaitable(struct co_io_watch *watch, unsigned events, double timeout) noexcept
        : watch(watch), timeout(timeout), events(events) {}

    co_fd_awaitable(const co_fd_awaitable &) = delete;
    co_fd_awaitable &operator=(const co_fd_awaitable &) = delete;

    ~co_fd_awaitable();

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) noexcept;
    int await_resume() noexcept;

private:
    struct co_io_watch *watch;
    double timeout;
    struct co_context *context = nullptr;
    double deadline = -1;
    unsigned events;
    bool borrowed = false;
};

/**
 * @brief Wait until a file descriptor is ready in any requested direction.
 * @param watch Watch created on the current thread.
 * @param events Nonzero subset of the watch's CO_READ and CO_WRITE interests.
 * @param timeout Timeout in seconds: -1 waits indefinitely, 0 checks immediately,
 *                and a positive finite value limits the wait.
 * @return An awaitable yielding 0 on readiness, or -1 with errno set on failure.
 *         EINVAL indicates an invalid watch, events or timeout; EBADF an invalid fd;
 *         EAGAIN no immediate readiness; ETIMEDOUT expiry;
 *         EOVERFLOW the watch's borrow limit is reached. System errors propagate.
 * @pre Await the result from a coroutine running under co_run(). Keep the watch
 *      and its descriptor valid until the wait completes.
 * @note Readiness is advisory and consumes no data. A hangup or descriptor error
 *       also wakes the waiter; the subsequent I/O operation determines the result.
 */
co_fd_awaitable co_wait_fd(struct co_io_watch *watch, unsigned events, double timeout = -1);

/**
 * @brief Awaitable for a POSIX I/O operation that starts when awaited.
 * @note I/O and timeout measurement begin when awaited. Operation state lives in
 *       the caller's coroutine frame; no child coroutine is created. Move only
 *       before awaiting the operation.
 */
struct co_io_awaitable {
    co_io_awaitable() noexcept = default;

    co_io_awaitable(co_io_awaitable &&other) noexcept
        : state(other.state) { other.state = {}; }

    co_io_awaitable(const co_io_awaitable &) = delete;
    co_io_awaitable &operator=(const co_io_awaitable &) = delete;

    ~co_io_awaitable();

    bool await_ready() noexcept;
    bool await_suspend(std::coroutine_handle<> parent) noexcept;
    ssize_t await_resume() noexcept;

private:
    enum class operation : unsigned char { invalid, read, write };

    struct operation_state {
        struct co_io_watch *watch = nullptr;
        void *buffer = nullptr;
        size_t length = 0;
        double deadline = -1;
        size_t offset = 0;
        ssize_t result = -1;
        std::coroutine_handle<> parent{};
        struct co_context *context = nullptr;
        int error = 0;
        operation kind = operation::invalid;
        bool borrowed = false;
    };

    co_io_awaitable(operation kind, struct co_io_watch *watch, void *buffer, size_t length,
                    double timeout) noexcept
        : state{
            .watch = watch,
            .buffer = buffer,
            .length = length,
            .deadline = timeout,
            .kind = kind
        } {}

    bool try_complete(double now = -1) noexcept;
    bool suspend() noexcept;
    static void resume(void *operation) noexcept;

    operation_state state{};

    friend struct co_context;
    friend co_io_awaitable co_read(struct co_io_watch *, void *, size_t, double);
    friend co_io_awaitable co_write(struct co_io_watch *, const void *, size_t, double);
};

/**
 * @brief Read up to len bytes from a nonblocking descriptor.
 * @param watch Watch with CO_READ enabled, owned by the caller.
 * @param[out] buf Destination buffer, which may be nullptr only when len is zero.
 * @param len Buffer capacity in bytes, no greater than SSIZE_MAX.
 * @param timeout Timeout in seconds: -1 waits indefinitely, 0 does not wait,
 *                and a positive finite value limits the whole operation.
 * @return An awaitable yielding the number of bytes read, which may be less than
 *         len; 0 on end of file or a zero-length read; -1 with errno set on failure.
 *         EINVAL indicates an invalid watch, direction, timeout or excessive len;
 *         EFAULT a null buffer with nonzero len; EAGAIN/EWOULDBLOCK no immediate
 *         data; ETIMEDOUT expiry; EOVERFLOW the watch's borrow limit is reached.
 *         Descriptor and scheduler errors propagate.
 * @pre Await on the watch's thread under co_run(). Keep the watch, its descriptor
 *      and buf valid until completion.
 */
co_io_awaitable co_read(struct co_io_watch *watch, void *buf, size_t len, double timeout = -1);

/**
 * @brief Write all len bytes to a nonblocking descriptor.
 * @param watch Watch with CO_WRITE enabled, owned by the caller.
 * @param buf Source buffer, which may be nullptr only when len is zero.
 * @param len Number of bytes to write, no greater than SSIZE_MAX.
 * @param timeout Timeout in seconds: -1 waits indefinitely, 0 does not wait,
 *                and a positive finite value limits the whole operation.
 * @return An awaitable yielding len on success, or -1 with errno set on failure.
 *         EINVAL indicates an invalid watch, direction, timeout or excessive len;
 *         EFAULT a null buffer with nonzero len; EAGAIN/EWOULDBLOCK an incomplete write;
 *         ETIMEDOUT expiry; EIO a write that made no progress;
 *         EOVERFLOW the watch's borrow limit is reached. Descriptor and
 *         scheduler errors propagate.
 * @pre Await on the watch's thread under co_run(). Keep the watch, its descriptor
 *      and buf valid, and do not modify the source data until completion.
 * @note Follows write()'s SIGPIPE behavior without changing the signal disposition.
 * @note Failure may occur after some bytes have been written; their count is not
 *       returned. The caller remains responsible for the descriptor and buffer.
 */
co_io_awaitable co_write(struct co_io_watch *watch, const void *buf, size_t len, double timeout = -1);

#endif
