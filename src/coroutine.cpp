/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */

#include <stdlib.h>
#include <errno.h>

#include "internal.h"

void *co_task::promise_type::operator new(size_t size) noexcept
{
    if (size > SIZE_MAX - sizeof(struct co_context)) {
        errno = ENOMEM;
        return nullptr;
    }

    void *allocation = malloc(sizeof(struct co_context) + size);

    return allocation ? (char *)allocation + sizeof(struct co_context) : nullptr;
}

void co_task::promise_type::operator delete(void *pointer) noexcept
{
    free(co_context::from_frame(pointer));
}

co_task co_task::promise_type::get_return_object_on_allocation_failure() noexcept
{
    errno = ENOMEM;
    return {};
}

co_task co_task::promise_type::get_return_object() noexcept
{
    return co_task(std::coroutine_handle<promise_type>::from_promise(*this));
}

void co_task::promise_type::unhandled_exception() noexcept
{
    abort();
}
