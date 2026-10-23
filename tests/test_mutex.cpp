/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include "internal.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static bool fail_calloc;
static bool fail_realloc;
static int completed;

extern "C" void *__real_calloc(size_t, size_t);
extern "C" void *__real_realloc(void *, size_t);

extern "C" void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_calloc) {
        fail_calloc = false;
        errno = ENOMEM;
        return nullptr;
    }

    return __real_calloc(count, size);
}

extern "C" void *__wrap_realloc(void *pointer, size_t size)
{
    if (fail_realloc) {
        fail_realloc = false;
        errno = ENOMEM;
        return nullptr;
    }

    return __real_realloc(pointer, size);
}

static co_task waiter(co_mutex *mutex, int id)
{
    assert(co_await co_mutex_lock(mutex) == 0);
    assert(completed++ == id);
    assert(co_mutex_trylock(mutex) == -1 && errno == EBUSY);
    assert(co_await co_sleep(0) == 0);
    assert(co_mutex_unlock(mutex) == 0);
}

static co_task release(co_mutex *mutex, bool abort = false)
{
    assert(co_mutex_destroy(mutex) == -1 && errno == EBUSY);
    assert(co_mutex_unlock(mutex) == 0);
    assert(completed == 0);
    assert(co_mutex_trylock(mutex) == -1 && errno == EBUSY);
    assert(co_mutex_unlock(mutex) == -1 && errno == EBUSY);

    if (abort)
        co_await std::suspend_always{};
}

static co_task pending(co_mutex *mutex)
{
    co_await co_mutex_lock(mutex);
    assert(false);
}

static co_task unsupported()
{
    co_await std::suspend_always{};
}

static co_task owner_exits(co_mutex *mutex)
{
    assert(co_await co_mutex_lock(mutex) == 0);
    co_await std::suspend_always{};
}

static co_task errors(co_mutex *mutex)
{
    assert(co_await co_mutex_lock(nullptr) == -1 && errno == EINVAL);
    assert(co_await co_mutex_lock(mutex, NAN) == -1 && errno == EINVAL);
    assert(co_await co_mutex_lock(mutex, INFINITY) == -1 && errno == EINVAL);
    assert(co_await co_mutex_lock(mutex, -2) == -1 && errno == EINVAL);
    assert(co_await co_mutex_lock(mutex, 0) == 0);
    assert(co_await co_mutex_lock(mutex, 0) == -1 && errno == EAGAIN);
    assert(co_await co_mutex_lock(mutex, 0.001) == -1 && errno == ETIMEDOUT);
    assert(co_mutex_unlock(mutex) == 0);
    assert(co_await co_mutex_lock(mutex) == 0);
    assert(co_mutex_unlock(mutex) == 0);
}

static co_task no_memory(co_mutex *mutex)
{
    fail_realloc = true;
    assert(co_await co_mutex_lock(mutex, 1) == -1 && errno == ENOMEM);
    assert(!fail_realloc);
    assert(co_mutex_unlock(mutex) == 0);
    assert(co_await co_mutex_lock(mutex) == 0);
    assert(co_mutex_unlock(mutex) == 0);
}

static co_task expires(co_mutex *mutex)
{
    assert(co_await co_mutex_lock(mutex, 0.001) == -1 && errno == ETIMEDOUT);
}

static co_task expire_before_resume(co_mutex *mutex, bool other_waiters)
{
    struct timespec delay = {0, 5000000};

    nanosleep(&delay, nullptr);
    co_loop::get()->expire_timers();
    assert(co_mutex_unlock(mutex) == 0);
    assert(co_mutex_destroy(mutex) == -1 && errno == EBUSY);

    if (!other_waiters) {
        assert(co_mutex_trylock(mutex) == 0);
        assert(co_mutex_unlock(mutex) == 0);
    }

    co_return;
}

static co_task wrong_thread_lock(co_mutex *mutex)
{
    assert(co_await co_mutex_lock(mutex) == -1 && errno == EINVAL);
}

static void *wrong_thread(void *arg)
{
    auto *mutex = static_cast<co_mutex *>(arg);

    assert(co_mutex_trylock(mutex) == -1 && errno == EINVAL);
    assert(co_mutex_unlock(mutex) == -1 && errno == EINVAL);
    assert(co_mutex_destroy(mutex) == -1 && errno == EINVAL);
    assert(co_spawn(wrong_thread_lock(mutex)) == 0);
    assert(co_run() == 0);

    return nullptr;
}

int main(void)
{
    assert(co_mutex_destroy(nullptr) == -1 && errno == EINVAL);
    assert(co_mutex_unlock(nullptr) == -1 && errno == EINVAL);
    assert(co_mutex_trylock(nullptr) == -1 && errno == EINVAL);
    fail_calloc = true;
    assert(co_mutex_create() == nullptr && errno == ENOMEM);

    auto *mutex = co_mutex_create();

    assert(mutex);
    assert(co_mutex_unlock(mutex) == -1 && errno == EPERM);
    assert(co_spawn(errors(mutex)) == 0);
    assert(co_run() == 0);

    pthread_t thread;

    assert(pthread_create(&thread, nullptr, wrong_thread, mutex) == 0);
    assert(pthread_join(thread, nullptr) == 0);
    assert(co_mutex_trylock(mutex) == 0);
    assert(co_spawn(waiter(mutex, 0)) == 0);
    assert(co_spawn(waiter(mutex, 1)) == 0);
    assert(co_spawn(waiter(mutex, 2)) == 0);
    assert(co_spawn(release(mutex)) == 0);
    assert(co_run() == 0 && completed == 3);

    assert(co_mutex_trylock(mutex) == 0);
    assert(co_spawn(no_memory(mutex)) == 0);
    assert(co_run() == 0);

    /* Expired waits retain a reference until they resume. */
    assert(co_mutex_trylock(mutex) == 0);
    assert(co_spawn(expires(mutex)) == 0);
    assert(co_spawn(expire_before_resume(mutex, false)) == 0);
    assert(co_run() == 0);

    completed = 0;
    assert(co_mutex_trylock(mutex) == 0);
    assert(co_spawn(expires(mutex)) == 0);
    assert(co_spawn(waiter(mutex, 0)) == 0);
    assert(co_spawn(waiter(mutex, 1)) == 0);
    assert(co_spawn(expire_before_resume(mutex, true)) == 0);
    assert(co_run() == 0 && completed == 2);

    /* Cancellation while queued leaves explicit ownership intact. */
    assert(co_mutex_trylock(mutex) == 0);
    assert(co_spawn(pending(mutex)) == 0);
    assert(co_spawn(unsupported()) == 0);
    assert(co_run() == -1 && errno == EPROTO);
    assert(co_mutex_destroy(mutex) == -1 && errno == EBUSY);
    assert(co_mutex_unlock(mutex) == 0);

    /* Cancel a reserved grant, including its handoff to another cancelled task. */
    completed = 0;
    assert(co_mutex_trylock(mutex) == 0);
    assert(co_spawn(pending(mutex)) == 0);
    assert(co_spawn(pending(mutex)) == 0);
    assert(co_spawn(release(mutex, true)) == 0);
    assert(co_run() == -1 && errno == EPROTO);
    assert(co_mutex_trylock(mutex) == 0);
    assert(co_mutex_unlock(mutex) == 0);

    assert(co_spawn(owner_exits(mutex)) == 0);
    assert(co_run() == -1 && errno == EPROTO);
    assert(co_mutex_destroy(mutex) == -1 && errno == EBUSY);
    assert(co_mutex_unlock(mutex) == 0);
    assert(co_mutex_destroy(mutex) == 0);

    puts("mutex: FIFO, timeout, cancellation, handoff, ownership and rollback OK");

    return 0;
}
