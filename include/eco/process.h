/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#ifndef ECO_PROCESS_H
#define ECO_PROCESS_H

#include <coroutine>
#include <sys/wait.h>

struct co_process;
struct co_io_watch;

enum co_process_flags {
    CO_PROCESS_STDOUT = 1u << 0,
    CO_PROCESS_STDERR = 1u << 1,
};

/**
 * @brief Start a program with a null-terminated argument list, searching PATH.
 * @param file Program name or path; shell syntax is not interpreted.
 * @param argv Arguments including argv[0], valid until this call returns.
 * @param flags Capture stdout and/or stderr; other standard streams are inherited.
 * @return Caller-owned process, or nullptr with errno set on failure.
 * @note Installs a SIGCHLD handler and unblocks SIGCHLD on the owning thread.
 *       Destroying the last process restores the previous disposition and the
 *       owner's original SIGCHLD blocked state. Other threads need no setup.
 *       SIG_IGN, SA_NOCLDWAIT, or an existing handler conflicts and returns EBUSY.
 * @pre Do not change the SIGCHLD disposition, block it on the owning thread,
 *      consume it elsewhere, or reap owned children while process objects exist.
 * @note All live process objects belong to one scheduler thread; another thread
 *       cannot start processes until all objects are destroyed (EBUSY).
 *       Creation is synchronous; exit waiting and captured reads are asynchronous.
 *       The child inherits cwd/environment and other signal settings, but SIGCHLD
 *       is unblocked in the child. Internal descriptors are close-on-exec.
 *       Exits are collected while the owner's co_run() services events, or by
 *       wait/kill/destroy. Keep running that loop or explicitly collect children.
 *       Do not exit the owning thread with live process objects.
 */
co_process *co_process_start(const char *file, const char *const argv[], unsigned flags = 0);

/**
 * @brief Borrow a captured stream's read watch; nullptr if not captured.
 * @note Only use on the owning thread. Read using co_read(); never destroy the
 *       watch or close its descriptor yourself. Drain stdout and stderr
 *       concurrently to avoid a full pipe blocking the child. Exit and EOF are
 *       independent: buffered output, or descendants holding writers, may remain.
 */
co_io_watch *co_process_stdout(co_process *process);
co_io_watch *co_process_stderr(co_process *process);

struct co_process_wait_awaitable {
    co_process_wait_awaitable(co_process *process, int *status, double timeout) noexcept
        : process(process), status(status), timeout(timeout) {}

    co_process_wait_awaitable(const co_process_wait_awaitable &) = delete;
    co_process_wait_awaitable &operator=(const co_process_wait_awaitable &) = delete;

    ~co_process_wait_awaitable();

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle) noexcept;
    int await_resume() noexcept;

private:
    co_process *process;
    int *status;
    double timeout;
    struct co_context *context = nullptr;
    bool borrowed = false;
};

/**
 * @brief Wait for a child exit and write its raw wait status to a nonnull status.
 * @return Awaitable yielding 0 on success, or -1 with errno set. Status is only
 *         written on success; use WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG.
 *         Nonzero child exit codes are successful waits, not API failures.
 * @param timeout -1 for unlimited wait, 0 for a check (EAGAIN if still running),
 *                or a positive finite duration in seconds (ETIMEDOUT on expiry).
 * @note Await under the owning thread's co_run(). One active wait per process
 *       (EBUSY otherwise). Completed status is cached for subsequent waits.
 *       Timeout or destruction of the awaiting coroutine does not kill the child
 *       or release the process; its external owner must finish cleanup.
 *       ECHILD means another consumer or automatic reaping took the exit status.
 */
co_process_wait_awaitable co_process_wait(co_process *process, int *status, double timeout = -1);

/**
 * @brief Send a signal to the direct child, without waiting or killing descendants.
 * @return 0 on success, -1 with errno set; ESRCH if already collected.
 */
int co_process_kill(co_process *process, int signal);

/**
 * @brief Release an exited process and close captured streams, discarding unread data.
 * @return 0 on success, or -1 with errno set. EBUSY leaves ownership unchanged
 *         while the child is running or a stream operation/wait is still active.
 * @note Never blocks for exit. Also permits cleanup after ECHILD. All operations
 *       on a process must run on its owning thread. Borrowed watches become invalid.
 */
int co_process_destroy(co_process *process);

#endif
