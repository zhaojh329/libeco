/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Linker wrapping is invisible to LTO: keep injection state observable across
 * calls the compiler still sees as ordinary libc allocation/syscalls. */
static volatile int fail_malloc;
static volatile int fail_calloc;
static void *last_allocation;
static bool allocation_freed;
static volatile int fail_realloc;
static volatile int fail_ctl;
static volatile int fail_wait;
static volatile int interrupted;
static volatile bool track_clock;
static volatile int clock_calls;
static volatile int fail_clock_call;
static volatile int expire_clock_call;
static volatile bool interrupt_write;
static volatile bool partial_write;

extern "C" {
void *__real_malloc(size_t size);
void __real_free(void *pointer);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
int __real_epoll_ctl(int epfd, int operation, int fd, struct epoll_event *event);
int __real_epoll_wait(int epfd, struct epoll_event *events, int count, int timeout);
int __real_clock_gettime(clockid_t clock, struct timespec *time);
ssize_t __real_write(int fd, const void *buffer, size_t size);

int __wrap_clock_gettime(clockid_t clock, struct timespec *time)
{
    if (!track_clock)
        return __real_clock_gettime(clock, time);

    clock_calls = clock_calls + 1;
    if (clock_calls == fail_clock_call) {
        errno = EIO;
        return -1;
    }

    int result = __real_clock_gettime(clock, time);

    if (result == 0 && clock_calls == expire_clock_call)
        time->tv_sec += 2;

    return result;
}

ssize_t __wrap_write(int fd, const void *buffer, size_t size)
{
    if (interrupt_write) {
        interrupt_write = false;
        errno = EINTR;
        return -1;
    }

    if (partial_write && size > 1) {
        partial_write = false;
        size = 1;
    }

    return __real_write(fd, buffer, size);
}

void *__wrap_malloc(size_t size)
{
    if (fail_malloc) {
        fail_malloc = 0;
        errno = ENOMEM;
        return nullptr;
    }

    last_allocation = __real_malloc(size);
    allocation_freed = false;

    return last_allocation;
}

void __wrap_free(void *pointer)
{
    if (pointer && pointer == last_allocation)
        allocation_freed = true;

    __real_free(pointer);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_calloc) {
        fail_calloc = 0;
        errno = ENOMEM;
        return NULL;
    }

    return __real_calloc(count, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (fail_realloc) {
        fail_realloc = 0;
        errno = ENOMEM;
        return NULL;
    }

    return __real_realloc(ptr, size);
}

int __wrap_epoll_ctl(int epfd, int operation, int fd, struct epoll_event *event)
{
    if (fail_ctl == operation) {
        fail_ctl = 0;
        errno = ENOSPC;
        return -1;
    }

    return __real_epoll_ctl(epfd, operation, fd, event);
}

int __wrap_epoll_wait(int epfd, struct epoll_event *events, int count, int timeout)
{
    if (fail_wait) {
        errno = fail_wait;
        fail_wait = 0;
        return -1;
    }

    if (!interrupted) {
        interrupted = 1;
        errno = EINTR;
        return -1;
    }

    return __real_epoll_wait(epfd, events, count, timeout);
}

}

struct context {
    int fd;
    struct co_io_watch *watch;
    int expected;
};

static co_task failing_wait(struct context c)
{
    int result = co_await co_wait_fd(c.watch, CO_READ, 0.002);
    assert(result == -1 && errno == c.expected);
}

static co_task pending_wait(struct context c)
{
    co_await co_wait_fd(c.watch, CO_READ);
    assert(0);
}

static co_task failing_sleep(void)
{
    int result = co_await co_sleep(0.001);
    assert(result == -1 && errno == ENOMEM);
}

static co_task sleeping(void)
{
    int result = co_await co_sleep(0.001);
    assert(result == 0);
}

static co_task failing_cond(struct co_cond *cond)
{
    assert(co_await co_cond_wait(cond, 1) == -1 && errno == ENOMEM);
    assert(co_cond_destroy(cond) == 0);
}

static co_task immediate_io(struct co_io_watch *watch, int peer)
{
    char byte;

    fail_malloc = 1;
    fail_calloc = 1;
    assert(co_await co_read(watch, &byte, 1, 1) == 1 && byte == 'x');
    assert(co_await co_write(watch, "y", 1, 1) == 1);
    assert(read(peer, &byte, 1) == 1 && byte == 'y');

    assert(co_await co_read(watch, &byte, 1, 0) == -1 && errno == EAGAIN);

    assert(fail_malloc && fail_calloc);

    fail_malloc = 0;
    fail_calloc = 0;
}

static void immediate_allocation_check(struct co_io_watch *watch, int peer)
{
    assert(write(peer, "x", 1) == 1);

    assert(co_spawn(immediate_io(watch, peer)) == 0);
    assert(co_run() == 0);
}

static co_task pending_io(struct co_io_watch *watch)
{
    char byte;

    co_await co_read(watch, &byte, 1);
    assert(0);
}

static co_task io_deadline_checks(struct co_io_watch *watch, int peer)
{
    char buffer[2];

    track_clock = true;
    clock_calls = 0;
    fail_clock_call = 1;
    assert(co_await co_read(watch, buffer, 1, 1) == -1 && errno == EIO);
    assert(clock_calls == 1);

    fail_clock_call = 0;
    assert(write(peer, "x", 1) == 1);
    clock_calls = 0;
    assert(co_await co_read(watch, buffer, 1, 1) == 1 && buffer[0] == 'x');
    assert(clock_calls == 1);

    clock_calls = 0;
    assert(co_await co_write(watch, "x", 1, 1) == 1);
    assert(clock_calls == 1);
    assert(read(peer, buffer, sizeof(buffer)) == 1 && buffer[0] == 'x');

    /* An interrupted syscall must check time again before retrying. */
    clock_calls = 0;
    fail_clock_call = 2;
    interrupt_write = true;
    assert(co_await co_write(watch, "x", 1, 1) == -1 && errno == EIO);
    assert(clock_calls == 2 && !interrupt_write);
    assert(read(peer, buffer, sizeof(buffer)) == -1 && errno == EAGAIN);

    /* Partial progress must not reset or bypass the original deadline. */
    clock_calls = 0;
    fail_clock_call = 0;
    expire_clock_call = 2;
    partial_write = true;
    assert(co_await co_write(watch, "yz", 2, 1) == -1 && errno == ETIMEDOUT);
    assert(clock_calls == 2 && !partial_write);
    assert(read(peer, buffer, sizeof(buffer)) == 1 && buffer[0] == 'y');

    expire_clock_call = 0;
    track_clock = false;
}

static co_task waiting_without_frame(struct co_io_watch *watch)
{
    char byte;

    assert(co_await co_read(watch, &byte, 1, 0.002) == -1 && errno == ETIMEDOUT);

    assert(fail_malloc);

    fail_malloc = 0;
}

static co_task waiting_write_without_frame(struct co_io_watch *watch)
{
    fail_malloc = 1;
    assert(co_await co_write(watch, "z", 1, 1) == 1);

    assert(fail_malloc);

    fail_malloc = 0;
}

static co_task drain_later(int fd)
{
    char buffer[4096];

    assert(co_await co_sleep(0.001) == 0);

    while (read(fd, buffer, sizeof(buffer)) > 0) {}

    assert(errno == EAGAIN);
}

static void waiting_write_check(int writer, int reader, struct co_io_watch *watch)
{
    char buffer[4096] = {};

    while (write(writer, buffer, sizeof(buffer)) > 0) {}

    assert(errno == EAGAIN);

    assert(co_spawn(waiting_write_without_frame(watch)) == 0);
    assert(co_spawn(drain_later(reader)) == 0);
    assert(co_run() == 0);
    assert(read(reader, buffer, sizeof(buffer)) == 1 && buffer[0] == 'z');
}

int main(void)
{
    int fds[2];
    struct context initial = {};

    fail_calloc = 1;
    assert(co_cond_create() == nullptr && errno == ENOMEM);

    struct co_cond *cond = co_cond_create();

    assert(cond);
    assert(co_spawn(failing_cond(cond)) == 0);
    fail_realloc = 1;
    assert(co_run() == 0);

    fail_malloc = 1;
    assert(co_spawn(sleeping()) == -1 && errno == ENOMEM);
    assert(co_run() == 0);

    /* Spawning an already allocated frame must not need a second allocation. */
    auto pending = sleeping();
    fail_malloc = 1;
    fail_calloc = 1;
    assert(co_spawn(static_cast<co_task &&>(pending)) == 0);

    assert(fail_malloc && fail_calloc);

    fail_malloc = 0;
    fail_calloc = 0;
    assert(co_run() == 0);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
    initial.fd = fds[0];

    fail_calloc = 1;
    assert(co_io_watch_create(initial.fd, CO_READ) == nullptr && errno == ENOMEM);
    fail_ctl = EPOLL_CTL_ADD;
    assert(co_io_watch_create(initial.fd, CO_READ) == nullptr && errno == ENOSPC);

    initial.watch = co_io_watch_create(initial.fd, CO_READ | CO_WRITE);
    assert(initial.watch);

    immediate_allocation_check(initial.watch, fds[1]);
    assert(co_spawn(io_deadline_checks(initial.watch, fds[1])) == 0);
    assert(co_run() == 0);

    waiting_write_check(fds[0], fds[1], initial.watch);
    assert(co_spawn(waiting_without_frame(initial.watch)) == 0);
    fail_malloc = 1;
    assert(co_run() == 0);

    initial.expected = ENOMEM;
    assert(co_spawn(failing_wait(initial)) == 0);
    fail_realloc = 1;
    assert(co_run() == 0);

    assert(co_spawn(failing_sleep()) == 0);
    fail_realloc = 1;
    assert(co_run() == 0);

    assert(co_spawn(pending_wait(initial)) == 0);
    fail_wait = EIO;
    assert(co_run() == -1 && errno == EIO);
    assert(allocation_freed);
    assert(fcntl(fds[0], F_GETFD) >= 0);

    assert(co_spawn(pending_io(initial.watch)) == 0);
    fail_wait = EIO;
    assert(co_run() == -1 && errno == EIO);
    assert(allocation_freed);

    initial.expected = ETIMEDOUT;
    assert(co_spawn(failing_wait(initial)) == 0);
    assert(co_run() == 0);

    fail_ctl = EPOLL_CTL_DEL;
    assert(co_io_watch_destroy(initial.watch) == -1 && errno == ENOSPC);
    assert(co_io_watch_destroy(initial.watch) == 0);

    assert(co_spawn(sleeping()) == 0);
    assert(co_run() == 0 && interrupted);

    close(fds[0]); close(fds[1]);

    puts("faults: OK");

    return 0;
}
