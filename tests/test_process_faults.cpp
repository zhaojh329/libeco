/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

static volatile int fail_calloc;
static volatile int fail_pipe;
static volatile int fail_eventfd;
static volatile int fail_sigaction;
static volatile int fail_unblock;
static volatile int fail_add;
static volatile int fail_actions;
static volatile int fail_attributes;
static volatile int fail_dup;
static volatile int fail_close_action;
static volatile int fail_spawn;
static volatile int fail_realloc;
static volatile int fail_read;
static volatile int interrupt_waitpid;
static volatile unsigned spawn_calls;
static void *allocations[32];
static unsigned allocation_count;

static bool fail(volatile int &count)
{
    if (!count)
        return false;

    count = count - 1;

    if (count)
        return false;

    errno = ENOMEM;
    return true;
}

extern "C" {
void *__real_calloc(size_t count, size_t size);
void __real_free(void *pointer);
int __real_pipe2(int fds[2], int flags);
int __real_eventfd(unsigned initval, int flags);
int __real_sigaction(int signal, const struct sigaction *action, struct sigaction *old);
int __real_sigprocmask(int how, const sigset_t *mask, sigset_t *old);
int __real_epoll_ctl(int fd, int op, int target, struct epoll_event *event);
int __real_posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions);
int __real_posix_spawnattr_init(posix_spawnattr_t *attr);
int __real_posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int target);
int __real_posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions, int fd);
int __real_posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                       const posix_spawnattr_t *attr, char *const argv[], char *const envp[]);
void *__real_realloc(void *pointer, size_t size);
ssize_t __real_read(int fd, void *buffer, size_t size);
pid_t __real_waitpid(pid_t pid, int *status, int options);

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail(fail_calloc))
        return nullptr;

    void *pointer = __real_calloc(count, size);

    if (pointer) {
        assert(allocation_count < 32);
        allocations[allocation_count++] = pointer;
    }

    return pointer;
}

void __wrap_free(void *pointer)
{
    for (unsigned i = 0; i < allocation_count; ++i) {
        if (allocations[i] == pointer) {
            allocations[i] = allocations[--allocation_count];
            break;
        }
    }

    __real_free(pointer);
}

int __wrap_pipe2(int fds[2], int flags)
{
    return fail(fail_pipe) ? -1 : __real_pipe2(fds, flags);
}

int __wrap_eventfd(unsigned initval, int flags)
{
    return fail(fail_eventfd) ? -1 : __real_eventfd(initval, flags);
}

int __wrap_sigaction(int signal, const struct sigaction *action, struct sigaction *old)
{
    return fail(fail_sigaction) ? -1 : __real_sigaction(signal, action, old);
}

int __wrap_sigprocmask(int how, const sigset_t *mask, sigset_t *old)
{
    if (how == SIG_UNBLOCK && mask && fail(fail_unblock))
        return -1;

    return __real_sigprocmask(how, mask, old);
}

int __wrap_epoll_ctl(int fd, int op, int target, struct epoll_event *event)
{
    if (op == EPOLL_CTL_ADD && fail(fail_add))
        return -1;

    return __real_epoll_ctl(fd, op, target, event);
}

int __wrap_posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions)
{
    return fail(fail_actions) ? ENOMEM : __real_posix_spawn_file_actions_init(actions);
}

int __wrap_posix_spawnattr_init(posix_spawnattr_t *attr)
{
    return fail(fail_attributes) ? ENOMEM : __real_posix_spawnattr_init(attr);
}

int __wrap_posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int target)
{
    return fail(fail_dup) ? ENOMEM : __real_posix_spawn_file_actions_adddup2(actions, fd, target);
}

int __wrap_posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions, int fd)
{
    return fail(fail_close_action) ? ENOMEM : __real_posix_spawn_file_actions_addclose(actions, fd);
}

int __wrap_posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                       const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
    spawn_calls = spawn_calls + 1;

    return fail(fail_spawn) ? ENOMEM : __real_posix_spawnp(pid, file, actions, attr, argv, envp);
}

void *__wrap_realloc(void *pointer, size_t size)
{
    return fail(fail_realloc) ? nullptr : __real_realloc(pointer, size);
}

ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
    return fail(fail_read) ? -1 : __real_read(fd, buffer, size);
}

pid_t __wrap_waitpid(pid_t pid, int *status, int options)
{
    if (interrupt_waitpid) {
        interrupt_waitpid = 0;
        errno = EINTR;
        return -1;
    }

    return __real_waitpid(pid, status, options);
}
}

static int fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    int count = 0;

    assert(directory);

    while (auto *entry = readdir(directory)) {
        if (entry->d_name[0] != '.')
            ++count;
    }

    closedir(directory);

    return count;
}

static co_task wait_success(co_process *process)
{
    int status;

    assert(co_await co_process_wait(process, &status, 3) == 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static co_task wait_pending(co_process *process)
{
    int status;

    co_await co_process_wait(process, &status);
    assert(false);
}

static co_task wait_no_memory(co_process *process)
{
    int status = 123;

    assert(co_await co_process_wait(process, &status, 1) == -1 && errno == ENOMEM);
    assert(status == 123);
    assert(co_process_kill(process, SIGKILL) == 0);
    assert(co_await co_process_wait(process, &status, 3) == 0);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

static co_task kill_child(co_process *process)
{
    assert(co_process_kill(process, SIGKILL) == 0);

    co_return;
}

static co_task wait_killed(co_process *process)
{
    int status;

    assert(co_await co_process_wait(process, &status, 3) == 0);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

int main(void)
{
    const char *argv[] = {"sh", "-c", "exit 0", nullptr};
    int initial_fds = fd_count();
    sigset_t initial_mask;

    assert(sigprocmask(SIG_BLOCK, nullptr, &initial_mask) == 0);

    struct injection {
        volatile int *counter;
        int calls;
    } injections[] = {
        {&fail_calloc, 4}, {&fail_eventfd, 1}, {&fail_add, 3},
        {&fail_sigaction, 2}, {&fail_unblock, 1},
        {&fail_actions, 1}, {&fail_attributes, 1}, {&fail_pipe, 2},
        {&fail_dup, 2}, {&fail_close_action, 4}, {&fail_spawn, 1},
    };

    for (const auto &injection : injections) {
        for (int n = 1; n <= injection.calls; ++n) {
            *injection.counter = n;
            spawn_calls = 0;
            assert(co_process_start("/bin/sh", argv, CO_PROCESS_STDOUT | CO_PROCESS_STDERR)
                   == nullptr && errno == ENOMEM);
            assert(*injection.counter == 0);
            assert(spawn_calls == (injection.counter == &fail_spawn ? 1u : 0u));
            assert(allocation_count == 0);
            assert(fd_count() == initial_fds);

            struct sigaction action;
            sigset_t mask;

            assert(sigaction(SIGCHLD, nullptr, &action) == 0);
            assert(action.sa_handler == SIG_DFL);
            assert(sigprocmask(SIG_BLOCK, nullptr, &mask) == 0);
            assert(sigismember(&mask, SIGCHLD) == sigismember(&initial_mask, SIGCHLD));

            int status;

            assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
        }
    }

    /* Failure creating a second process must preserve the existing registry. */
    auto *process = co_process_start("/bin/sh", argv);

    assert(process);
    fail_pipe = 2;
    assert(co_process_start("/bin/sh", argv, CO_PROCESS_STDOUT | CO_PROCESS_STDERR)
           == nullptr && errno == ENOMEM);
    interrupt_waitpid = 1;
    assert(co_spawn(wait_success(process)) == 0);
    assert(co_run() == 0 && !interrupt_waitpid);
    assert(co_process_destroy(process) == 0);

    const char *hold[] = {"sh", "-c", "exec sleep 30", nullptr};

    process = co_process_start("/bin/sh", hold);
    assert(process);
    assert(co_spawn(wait_no_memory(process)) == 0);
    fail_realloc = 1;
    assert(co_run() == 0 && !fail_realloc);
    assert(co_process_destroy(process) == 0);

    /* A broken signal read aborts the scheduler but leaves the child manageable. */
    process = co_process_start("/bin/sh", hold);
    assert(process);
    assert(co_spawn(wait_pending(process)) == 0);
    assert(co_spawn(kill_child(process)) == 0);
    fail_read = 1;
    assert(co_run() == -1 && errno == ENOMEM && !fail_read);
    assert(co_spawn(wait_killed(process)) == 0);
    assert(co_run() == 0);
    assert(co_process_destroy(process) == 0);
    assert(allocation_count == 0);
    assert(fd_count() == initial_fds);

    puts("process faults: startup rollback, ownership, allocation and event failures OK");

    return 0;
}
