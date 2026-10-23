/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

static bool worker_ready;
static bool send_signal;
static bool pause_write;
static bool write_entered;
static bool release_write;
static bool closing;
static int notification_fd;

extern "C" ssize_t __real_write(int fd, const void *data, size_t size);

extern "C" ssize_t __wrap_write(int fd, const void *data, size_t size)
{
    if (__atomic_load_n(&pause_write, __ATOMIC_SEQ_CST)) {
        notification_fd = fd;
        __atomic_store_n(&write_entered, true, __ATOMIC_SEQ_CST);

        while (!__atomic_load_n(&release_write, __ATOMIC_SEQ_CST)) {
        }
    }

    return __real_write(fd, data, size);
}

static void *existing_worker(void *)
{
    sigset_t mask;

    assert(pthread_sigmask(SIG_BLOCK, nullptr, &mask) == 0);
    assert(!sigismember(&mask, SIGCHLD));
    __atomic_store_n(&worker_ready, true, __ATOMIC_SEQ_CST);

    while (!__atomic_load_n(&send_signal, __ATOMIC_SEQ_CST))
        sched_yield();

    errno = EDOM;
    assert(raise(SIGCHLD) == 0);
    assert(errno == EDOM);

    return nullptr;
}

static void *release_handler(void *)
{
    while (!__atomic_load_n(&closing, __ATOMIC_SEQ_CST))
        sched_yield();

    /* Hold a handler inside write while the owner tries to close its eventfd. */
    struct timespec delay = {0, 20000000};

    nanosleep(&delay, nullptr);
    assert(fcntl(notification_fd, F_GETFD) >= 0);
    __atomic_store_n(&release_write, true, __ATOMIC_SEQ_CST);

    return nullptr;
}

static co_task wait_exit(co_process *process)
{
    int status;

    assert(co_await co_process_wait(process, &status, 3) == 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void custom_handler(int)
{
}

int main(void)
{
    sigset_t children;

    sigemptyset(&children);
    sigaddset(&children, SIGCHLD);
    assert(pthread_sigmask(SIG_UNBLOCK, &children, nullptr) == 0);

    pthread_t worker;

    assert(pthread_create(&worker, nullptr, existing_worker, nullptr) == 0);

    while (!__atomic_load_n(&worker_ready, __ATOMIC_SEQ_CST))
        sched_yield();

    /* First use after an unmasked worker exists; it stays alive throughout. */
    const char *args[] = {"sh", "-c", "sleep 0.01; exit 0", nullptr};
    co_process *process = nullptr;

    for (int i = 0; i < 20; ++i) {
        process = co_process_start("/bin/sh", args);
        assert(process);
        assert(co_spawn(wait_exit(process)) == 0);
        assert(co_run() == 0);

        if (i != 19)
            assert(co_process_destroy(process) == 0);
    }

    __atomic_store_n(&pause_write, true, __ATOMIC_SEQ_CST);
    __atomic_store_n(&send_signal, true, __ATOMIC_SEQ_CST);

    while (!__atomic_load_n(&write_entered, __ATOMIC_SEQ_CST))
        sched_yield();

    pthread_t releaser;

    assert(pthread_create(&releaser, nullptr, release_handler, nullptr) == 0);
    __atomic_store_n(&closing, true, __ATOMIC_SEQ_CST);
    assert(co_process_destroy(process) == 0);
    assert(pthread_join(worker, nullptr) == 0);
    assert(pthread_join(releaser, nullptr) == 0);
    assert(fcntl(notification_fd, F_GETFD) == -1 && errno == EBADF);
    __atomic_store_n(&pause_write, false, __ATOMIC_SEQ_CST);

    /* A reused descriptor must not receive writes from the old handler. */
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    uint64_t value;

    assert(fd >= 0);
    assert(raise(SIGCHLD) == 0);
    assert(read(fd, &value, sizeof(value)) == -1 && errno == EAGAIN);
    close(fd);

    struct sigaction action = {}, current;

    assert(sigaction(SIGCHLD, nullptr, &current) == 0);
    assert(current.sa_handler == SIG_DFL);
    action.sa_handler = custom_handler;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGCHLD, &action, nullptr) == 0);
    assert(co_process_start("/bin/sh", args) == nullptr && errno == EBUSY);
    assert(sigaction(SIGCHLD, nullptr, &current) == 0);
    assert(current.sa_handler == custom_handler);
    action.sa_handler = SIG_DFL;
    assert(sigaction(SIGCHLD, &action, nullptr) == 0);

    puts("process signals: existing threads, in-flight handler close, conflicts OK");

    return 0;
}
