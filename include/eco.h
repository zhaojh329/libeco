/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#ifndef LIBECO_H
#define LIBECO_H

#include "eco/coroutine.h" // IWYU pragma: export
#include "eco/time.h" // IWYU pragma: export
#include "eco/cond.h" // IWYU pragma: export
#include "eco/mutex.h" // IWYU pragma: export
#include "eco/io.h" // IWYU pragma: export
#include "eco/process.h" // IWYU pragma: export

/**
 * @brief Schedule a coroutine on the current thread's scheduler.
 * @param task Coroutine whose ownership is transferred to the scheduler.
 * @return 0 on success, or -1 with errno set to ENOMEM if task is empty.
 * @note Consumes task even on failure. Execution begins when co_run() schedules it.
 *       Application buffers and file descriptors remain owned by the caller.
 */
int co_spawn(co_task task);

/**
 * @brief Run the current thread's scheduler until all tasks finish or a fatal error occurs.
 * @return 0 on completion, including when no tasks are queued; -1 with errno set
 *         on failure. EBUSY indicates a nested call; EPROTO indicates that a task
 *         suspended without registering a supported wait. System errors propagate.
 * @note Fatal scheduler errors destroy all pending coroutine frames. A rejected
 *       nested call does not stop the active scheduler or destroy its tasks.
 *       I/O watches remain owned by their creators after co_run() returns.
 */
int co_run(void);

#endif
