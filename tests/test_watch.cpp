/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile unsigned additions;
static volatile unsigned modifications;
static volatile unsigned deletions;
static volatile unsigned polls;

extern "C" {
int __real_epoll_ctl(int epfd, int operation, int fd, struct epoll_event *event);
int __real_epoll_wait(int epfd, struct epoll_event *events, int count, int timeout);

int __wrap_epoll_ctl(int epfd, int operation, int fd, struct epoll_event *event)
{
    int result = __real_epoll_ctl(epfd, operation, fd, event);

    if (result == 0) {
        switch (operation) {
        case EPOLL_CTL_ADD:
            additions = additions + 1;
            break;
        case EPOLL_CTL_MOD:
            modifications = modifications + 1;
            break;
        case EPOLL_CTL_DEL:
            deletions = deletions + 1;
            break;
        }
    }

    return result;
}

int __wrap_epoll_wait(int epfd, struct epoll_event *events, int count, int timeout)
{
    polls = polls + 1;

    return __real_epoll_wait(epfd, events, count, timeout);
}
}

static co_task ping(struct co_io_watch *watch)
{
    char byte;

    for (int i = 0; i < 200; ++i) {
        assert(co_await co_write(watch, "x", 1, 1) == 1);
        assert(co_await co_read(watch, &byte, 1, 1) == 1 && byte == 'x');
    }
}

static co_task pong(struct co_io_watch *watch)
{
    char byte;

    for (int i = 0; i < 200; ++i) {
        assert(co_await co_read(watch, &byte, 1, 1) == 1 && byte == 'x');
        assert(co_await co_write(watch, &byte, 1, 1) == 1);
    }
}

static co_task idle(void)
{
    assert(co_await co_sleep(0.003) == 0);
}

static co_task ready_waiter(struct co_io_watch *watch)
{
    assert(co_await co_wait_fd(watch, CO_READ, 1) == 0);

    /* The other waiter is ready but still borrows the watch until it resumes. */
    assert(co_io_watch_destroy(watch) == -1 && errno == EBUSY);
}

static co_task consume(struct co_io_watch *watch)
{
    char byte;

    assert(co_await co_read(watch, &byte, 1, 1) == 1 && byte == 'x');
}

static co_task notify(struct co_io_watch *watch, int peer)
{
    assert(co_await co_sleep(0.001) == 0);
    assert(co_io_watch_destroy(watch) == -1 && errno == EBUSY);
    assert(write(peer, "x", 1) == 1);
}

static co_task already_readable(struct co_io_watch *watch, int fd)
{
    /* Let epoll consume the edge before registering an actual waiter. */
    assert(co_await co_sleep(0.003) == 0);
    assert(co_await co_wait_fd(watch, CO_READ, 1) == 0);
    assert(co_await co_wait_fd(watch, CO_READ, 1) == 0);

    char byte;

    assert(read(fd, &byte, 1) == 1 && byte == 'x');
    assert(co_await co_wait_fd(watch, CO_READ, 0.002) == -1 && errno == ETIMEDOUT);
}

static co_task wrong_direction(struct co_io_watch *watch)
{
    assert(co_await co_write(watch, "x", 1) == -1 && errno == EINVAL);
    assert(co_await co_wait_fd(watch, CO_WRITE) == -1 && errno == EINVAL);
}

static co_task pending_read(struct co_io_watch *watch)
{
    char byte;

    co_await co_read(watch, &byte, 1);
    assert(false);
}

static co_task unsupported_suspend(void)
{
    co_await std::suspend_always{};
}

struct watch_owner {
    struct co_io_watch *watch;
    bool *destroyed;

    ~watch_owner()
    {
        assert(co_io_watch_destroy(watch) == 0);
        *destroyed = true;
    }
};

static co_task owned_wait(int fd, bool *destroyed)
{
    watch_owner owner{co_io_watch_create(fd, CO_READ), destroyed};
    char byte;

    assert(owner.watch);
    co_await co_read(owner.watch, &byte, 1);
    assert(false);
}

int main(void)
{
    int fds[2];

    assert(co_io_watch_create(-1, CO_READ) == nullptr && errno == EBADF);
    assert(co_io_watch_destroy(nullptr) == -1 && errno == EINVAL);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    int flags = fcntl(fds[0], F_GETFL);

    assert(flags >= 0 && !(flags & O_NONBLOCK));
    assert(co_io_watch_create(fds[0], CO_READ) == nullptr && errno == EINVAL);
    assert(fcntl(fds[0], F_GETFL) == flags);
    assert(additions == 0);

    close(fds[0]);
    close(fds[1]);
    assert(co_io_watch_create(fds[0], CO_READ) == nullptr && errno == EBADF);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
    assert(co_io_watch_create(fds[0], 0) == nullptr && errno == EINVAL);
    assert(co_io_watch_create(fds[0], 4) == nullptr && errno == EINVAL);

    auto *a = co_io_watch_create(fds[0], CO_READ | CO_WRITE);
    auto *b = co_io_watch_create(fds[1], CO_READ | CO_WRITE);

    assert(a && b && additions == 2);
    assert(co_io_watch_create(fds[0], CO_READ) == nullptr && errno == EEXIST);
    assert(co_spawn(ping(a)) == 0);
    assert(co_spawn(pong(b)) == 0);
    assert(co_run() == 0);
    assert(additions == 2 && modifications == 0 && deletions == 0);

    unsigned initial_polls = polls;

    assert(co_spawn(idle()) == 0);
    assert(co_run() == 0);
    assert(polls - initial_polls <= 8);

    assert(co_spawn(ready_waiter(a)) == 0);
    assert(co_spawn(consume(a)) == 0);
    assert(co_spawn(notify(a, fds[1])) == 0);
    assert(co_run() == 0);

    assert(write(fds[1], "x", 1) == 1);
    assert(co_spawn(already_readable(a, fds[0])) == 0);
    assert(co_run() == 0);

    assert(co_spawn(pending_read(a)) == 0);
    assert(co_spawn(unsupported_suspend()) == 0);
    assert(co_run() == -1 && errno == EPROTO);
    assert(additions == 2 && modifications == 0 && deletions == 0);

    assert(co_io_watch_destroy(a) == 0);
    assert(co_io_watch_destroy(b) == 0);
    assert(deletions == 2);
    assert(fcntl(fds[0], F_GETFD) >= 0 && fcntl(fds[1], F_GETFD) >= 0);

    a = co_io_watch_create(fds[0], CO_READ);
    assert(a);
    assert(co_spawn(wrong_direction(a)) == 0);
    assert(co_run() == 0);
    assert(co_io_watch_destroy(a) == 0);

    bool destroyed = false;

    assert(co_spawn(owned_wait(fds[0], &destroyed)) == 0);
    assert(co_spawn(unsupported_suspend()) == 0);
    assert(co_run() == -1 && errno == EPROTO);
    assert(destroyed);

    close(fds[0]);
    close(fds[1]);

    puts("watch: 200 round trips, 2 ADD, 0 MOD, 2 explicit DEL; lifecycle OK");

    return 0;
}
