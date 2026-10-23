/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */

#include <sys/eventfd.h>
#include <sys/wait.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <sched.h>
#include <spawn.h>
#include <stdlib.h>
#include <unistd.h>

#include "internal.h"

struct co_process {
    struct list_head node;
    pid_t pid;
    int status;
    int error;
    bool reaped;
    struct co_context *waiter;
    struct co_io_watch *output[2];

    int reap();
    int spawn(const char *file, const char *const argv[], unsigned flags, sigset_t mask);
};

/* Only the owner accesses the registry. Atomic acquisition rejects other threads
 * without imposing a lock or a C++ runtime dependency on event handling. */
static co_loop *process_owner;
static_assert(__atomic_always_lock_free(sizeof(decltype(process_owner)), nullptr));
static struct list_head processes;
static struct co_io_watch *signal_watch;
static struct sigaction previous_sigchld_action;
static bool sigchld_handler_installed;
static bool restore_sigchld_block;
static int child_exit_fd = -1;
static unsigned active_signal_handlers;
static_assert(__atomic_always_lock_free(sizeof(child_exit_fd), nullptr));
static_assert(__atomic_always_lock_free(sizeof(active_signal_handlers), nullptr));

static void notify_child_exit(int)
{
    int saved_errno = errno;

    /* Sequential consistency pairs publication/removal of the fd with the
     * reader count: close cannot miss a handler which has loaded the old fd. */
    __atomic_add_fetch(&active_signal_handlers, 1, __ATOMIC_SEQ_CST);
    int fd = __atomic_load_n(&child_exit_fd, __ATOMIC_SEQ_CST);

    if (fd >= 0) {
        uint64_t value = 1;
        ssize_t result;

        do {
            result = write(fd, &value, sizeof(value));
        } while (result < 0 && errno == EINTR);

        /* EAGAIN means the eventfd already holds a pending notification. */
    }

    __atomic_sub_fetch(&active_signal_handlers, 1, __ATOMIC_SEQ_CST);
    errno = saved_errno;
}

int co_process::reap()
{
    if (reaped)
        return 0;

    pid_t result;

    do {
        result = waitpid(pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);

    if (result == 0)
        return 0;

    if (result < 0 && errno != ECHILD)
        return -1;

    error = result < 0 ? ECHILD : 0;
    reaped = true;

    if (waiter && waiter->state == CO_WAITING)
        waiter->make_ready(error);

    return 0;
}

static void handle_child_exits(co_io_watch *watch, unsigned)
{
    uint64_t notifications;

    for (;;) {
        ssize_t size = read(watch->fd, &notifications, sizeof(notifications));

        if (size > 0)
            continue;

        if (size < 0 && errno == EINTR)
            continue;

        if (size < 0 && errno == EAGAIN)
            break;

        watch->owner->fatal = size < 0 ? errno : EIO;
        return;
    }

    /* Standard signals coalesce; inspect every owned PID on each notification. */
    struct list_head *node;

    list_for_each(node, &processes) {
        auto *process = list_entry(node, co_process, node);

        if (process->reap() < 0) {
            watch->owner->fatal = errno;
            return;
        }
    }
}

static void close_unused_child_exit_watch(void)
{
    if (!list_empty(&processes))
        return;

    if (sigchld_handler_installed) {
        sigaction(SIGCHLD, &previous_sigchld_action, nullptr);
        sigchld_handler_installed = false;
    }

    __atomic_store_n(&child_exit_fd, -1, __ATOMIC_SEQ_CST);

    while (__atomic_load_n(&active_signal_handlers, __ATOMIC_SEQ_CST))
        sched_yield();

    if (signal_watch) {
        signal_watch->close();
        signal_watch = nullptr;
    }

    if (restore_sigchld_block) {
        sigset_t children;

        sigemptyset(&children);
        sigaddset(&children, SIGCHLD);
        sigprocmask(SIG_BLOCK, &children, nullptr);
        restore_sigchld_block = false;
    }

    __atomic_store_n(&process_owner, nullptr, __ATOMIC_RELEASE);
}

static int init_child_exit_watch(void)
{
    auto *loop = co_loop::get();
    co_loop *expected = nullptr;

    if (!__atomic_compare_exchange_n(&process_owner, &expected, loop, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (expected == loop)
            return 0;

        errno = EBUSY;
        return -1;
    }

    INIT_LIST_HEAD(&processes);

    struct sigaction action;

    if (sigaction(SIGCHLD, nullptr, &action) < 0) {
        close_unused_child_exit_watch();
        return -1;
    }

    if (action.sa_handler != SIG_DFL || (action.sa_flags & SA_NOCLDWAIT)) {
        close_unused_child_exit_watch();
        errno = EBUSY;
        return -1;
    }

    previous_sigchld_action = action;

    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    if (fd < 0) {
        int error = errno;

        close_unused_child_exit_watch();
        errno = error;
        return -1;
    }

    signal_watch = co_io_watch::create(fd, CO_READ, handle_child_exits);

    if (!signal_watch) {
        int error = errno;

        close(fd);
        close_unused_child_exit_watch();
        errno = error;
        return -1;
    }

    __atomic_store_n(&child_exit_fd, fd, __ATOMIC_SEQ_CST);
    action = {};
    action.sa_handler = notify_child_exit;
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigfillset(&action.sa_mask);

    if (sigaction(SIGCHLD, &action, nullptr) < 0) {
        int error = errno;

        close_unused_child_exit_watch();
        errno = error;
        return -1;
    }

    sigchld_handler_installed = true;

    sigset_t children, old_mask;

    sigemptyset(&children);
    sigaddset(&children, SIGCHLD);

    if (sigprocmask(SIG_UNBLOCK, &children, &old_mask) < 0) {
        int error = errno;

        close_unused_child_exit_watch();
        errno = error;
        return -1;
    }

    restore_sigchld_block = sigismember(&old_mask, SIGCHLD);

    return 0;
}

struct spawn_setup {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    bool actions_ready = false;
    bool attributes_ready = false;
    int writers[2] = {-1, -1};

    co_io_watch *capture_pipe(unsigned index);

    ~spawn_setup()
    {
        for (int fd : writers) {
            if (fd >= 0)
                close(fd);
        }

        if (actions_ready)
            posix_spawn_file_actions_destroy(&actions);

        if (attributes_ready)
            posix_spawnattr_destroy(&attributes);
    }
};

co_io_watch *spawn_setup::capture_pipe(unsigned index)
{
    int fds[2];

    if (pipe2(fds, O_CLOEXEC) < 0)
        return nullptr;

    /* Keep spawn dup2/close actions correct even if standard fds were closed. */
    for (int &fd : fds) {
        if (fd > STDERR_FILENO)
            continue;

        int moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);

        if (moved < 0) {
            int error = errno;

            close(fds[0]);
            close(fds[1]);
            errno = error;
            return nullptr;
        }

        close(fd);
        fd = moved;
    }

    writers[index] = fds[1];

    /* Only the parent reads nonblocking; ordinary child programs may block. */
    int flags = fcntl(fds[0], F_GETFL);
    co_io_watch *watch = nullptr;

    if (flags >= 0 && fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) == 0)
        watch = co_io_watch_create(fds[0], CO_READ);

    if (!watch) {
        int error = errno;

        close(fds[0]);
        errno = error;
        return nullptr;
    }

    int error = posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO + index);

    if (!error)
        error = posix_spawn_file_actions_addclose(&actions, fds[0]);

    if (!error)
        error = posix_spawn_file_actions_addclose(&actions, fds[1]);

    if (error) {
        watch->close();
        errno = error;
        return nullptr;
    }

    return watch;
}

int co_process::spawn(const char *file, const char *const argv[],
                       unsigned flags, sigset_t mask)
{
    spawn_setup setup;
    int error = posix_spawn_file_actions_init(&setup.actions);

    if (error)
        return error;

    setup.actions_ready = true;
    error = posix_spawnattr_init(&setup.attributes);

    if (error)
        return error;

    setup.attributes_ready = true;
    sigdelset(&mask, SIGCHLD);
    error = posix_spawnattr_setsigmask(&setup.attributes, &mask);

    if (!error)
        error = posix_spawnattr_setflags(&setup.attributes, POSIX_SPAWN_SETSIGMASK);

    if (error)
        return error;

    for (unsigned i = 0; i < 2; ++i) {
        if (!(flags & (CO_PROCESS_STDOUT << i)))
            continue;

        output[i] = setup.capture_pipe(i);

        if (!output[i])
            return errno;
    }

    /* POSIX's argv parameter is not const-qualified, but spawn does not modify it. */
    return posix_spawnp(&pid, file, &setup.actions, &setup.attributes,
                        const_cast<char *const *>(argv), environ);
}

co_process *co_process_start(const char *file, const char *const argv[], unsigned flags)
{
    if (!file || !*file || !argv || !argv[0] ||
        (flags & ~(CO_PROCESS_STDOUT | CO_PROCESS_STDERR))) {
        errno = EINVAL;
        return nullptr;
    }

    sigset_t mask;

    if (sigprocmask(SIG_BLOCK, nullptr, &mask) < 0)
        return nullptr;

    auto *process = static_cast<co_process *>(calloc(1, sizeof(co_process)));

    if (!process)
        return nullptr;

    if (init_child_exit_watch() < 0) {
        int error = errno;

        free(process);
        errno = error;
        return nullptr;
    }

    int error = process->spawn(file, argv, flags, mask);

    if (error) {
        for (auto *watch : process->output) {
            if (watch)
                watch->close();
        }

        free(process);
        close_unused_child_exit_watch();
        errno = error;
        return nullptr;
    }

    /* All fallible setup precedes spawn: a successful child always has an owner. */
    list_add_tail(&process->node, &processes);

    return process;
}

co_io_watch *co_process_stdout(co_process *process)
{
    if (!process || __atomic_load_n(&process_owner, __ATOMIC_ACQUIRE) != co_loop::get()) {
        errno = EINVAL;
        return nullptr;
    }

    return process->output[0];
}

co_io_watch *co_process_stderr(co_process *process)
{
    if (!process || __atomic_load_n(&process_owner, __ATOMIC_ACQUIRE) != co_loop::get()) {
        errno = EINVAL;
        return nullptr;
    }

    return process->output[1];
}

int co_process_kill(co_process *process, int signal)
{
    if (!process || __atomic_load_n(&process_owner, __ATOMIC_ACQUIRE) != co_loop::get()) {
        errno = EINVAL;
        return -1;
    }

    if (process->reap() < 0)
        return -1;

    if (process->reaped) {
        errno = ESRCH;
        return -1;
    }

    return kill(process->pid, signal);
}

int co_process_destroy(co_process *process)
{
    if (!process || __atomic_load_n(&process_owner, __ATOMIC_ACQUIRE) != co_loop::get()) {
        errno = EINVAL;
        return -1;
    }

    if (process->waiter || (process->output[0] && process->output[0]->users) ||
        (process->output[1] && process->output[1]->users)) {
        errno = EBUSY;
        return -1;
    }

    if (process->reap() < 0)
        return -1;

    if (!process->reaped) {
        errno = EBUSY;
        return -1;
    }

    for (auto *watch : process->output) {
        if (watch)
            watch->close();
    }

    list_del(&process->node);
    free(process);
    close_unused_child_exit_watch();

    return 0;
}

bool co_process_wait_awaitable::await_suspend(std::coroutine_handle<> handle) noexcept
{
    auto *loop = co_loop::get();

    context = loop->current;
    assert(context && context->state == CO_RUNNING);

    context->resume_data = handle.address();
    context->error = 0;
    context->deadline = -1;
    context->timeout_is_error = true;

    if (!process || !status || __atomic_load_n(&process_owner, __ATOMIC_ACQUIRE) != loop ||
        !isfinite(timeout) || (timeout < 0 && timeout != -1)) {
        context->error = EINVAL;
        return false;
    }

    if (process->waiter) {
        context->error = EBUSY;
        return false;
    }

    process->waiter = context;
    borrowed = true;

    if (process->reap() < 0) {
        context->error = errno;
        return false;
    }

    if (process->reaped) {
        context->error = process->error;
        return false;
    }

    if (timeout == 0) {
        context->error = EAGAIN;
        return false;
    }

    if (timeout > 0)
        context->deadline = co_now() + timeout;

    return context->suspend(nullptr, 0) == 0;
}

co_process_wait_awaitable::~co_process_wait_awaitable()
{
    if (borrowed)
        process->waiter = nullptr;
}

int co_process_wait_awaitable::await_resume() noexcept
{
    if (borrowed) {
        process->waiter = nullptr;
        borrowed = false;
    }

    if (context->error) {
        errno = context->error;
        return -1;
    }

    *status = process->status;

    return 0;
}

co_process_wait_awaitable co_process_wait(co_process *process, int *status, double timeout)
{
    return {process, status, timeout};
}
