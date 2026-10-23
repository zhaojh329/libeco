---
name: libeco
description: Write, review, and debug C++20 code using libeco's co_task, persistent I/O watches, timers, coroutine synchronization, and child-process APIs on Linux. Use for libeco integration and API questions; not for lua-eco, Tencent libco, or unrelated coroutine libraries.
---

# libeco

Use libeco's public API to implement cooperative C++20 coroutines on Linux.
The API has C-style names and error reporting, but requires C++, not C.

## Choose the relevant reference

- Read [Public API](references/api.md) for signatures, return values, ownership,
  timeout behavior, and process constraints. Read the sections needed by the task.
- Read [Building and examples](references/building.md) for CMake/pkg-config
  integration, a complete program, and builds without a C++ runtime dependency.
- [Installation](README.md) describes installing this skill, not the library.

These references describe libeco 0.1.0. When a project uses another version,
prefer its public headers (`eco.h` and `eco/*.h`) over this snapshot. Use the
installed headers if no checkout is available. Do not assume the skill's parent
folder is a source checkout. The skill requires no MCP server or other skills.

## Runtime model

- Return `co_task` from coroutine entry points and schedule them with `co_spawn()`.
  Tasks are lazy: execution starts under `co_run()`. A named task must be moved
  into `co_spawn()`; ownership transfers even when spawning fails.
- `co_task` is not an awaitable result, future, or join handle. Communicate results
  through application-owned state whose lifetime covers the task.
- Each thread has its own scheduler. Await libeco operations from coroutines
  running under that thread's `co_run()`. Do not nest `co_run()`.
- Scheduling is cooperative. Blocking calls and CPU loops block the scheduler;
  `co_sleep(0)` yields. An operation that completes immediately need not yield.
- Use supported libeco awaitables. Arbitrary suspension without a registered wait
  can make `co_run()` fail with `EPROTO`. Do not manipulate awaiter internals,
  coroutine handles, or the private scheduler to build an application.
- A fatal scheduler error destroys pending coroutine frames. It does not assume
  ownership of application descriptors, buffers, watches, or process objects.
  Keep external owners alive through `co_run()` and clean up on both outcomes.
  A resource guard inside a lazy coroutine has not been constructed before the
  coroutine first runs; account for that when passing owned resources to tasks.

## Apply the API contracts

Use persistent watches rather than passing raw descriptors to `co_read()` or
`co_write()`. Create descriptors as nonblocking before making a watch; the library
checks their flags without changing them. Keep the watch, descriptor, and buffers
alive until operations finish or their awaiting frames are destroyed. Destroy a
caller-owned watch successfully before closing or reusing its descriptor.

Check immediate API calls and awaited results separately. Most operations report
`-1` and `errno`; creation returns `nullptr`. A successful `co_run()` does not mean
an application operation succeeded. Capture `errno` before another operation or
suspension if it is needed later; it is thread-local, not coroutine-local.

Choose timeouts from the application's requirements. Most waits accept `-1`
(indefinite), `0` (no waiting), or a positive finite number of seconds. A timeout
does not imply rollback: a failed write may already have sent bytes, and a timed
out process wait does not terminate the child.

Preserve application signal policy. `co_write()` follows `write()`'s SIGPIPE
semantics. Process APIs have their own SIGCHLD ownership requirements; read the
process section before using them.

## Writing and reviewing code

Match calls to the public API reference and the target version's headers. Do not
invent networking, cancellation, or task-joining APIs from similarly named
libraries. For accept/connect or other descriptor operations, combine nonblocking
Linux calls with `co_wait_fd()` and check the operation's actual result after
readiness; readiness alone does not prove success.

For reviews, focus on concrete failures: blocking the loop, cross-thread object
use, borrowed-resource destruction, buffers expiring during suspension, partial
writes being retried from the start, and child pipes not being drained. Report
trigger conditions and consequences. Normal POSIX behavior is not itself a defect.

Validate generated programs with the application's toolchain where available.
For library changes, use the repository's existing CTest suite and focused tests
for changed behavior. State when a compiler, target platform, or runtime test was
unavailable rather than treating compilation as runtime verification.
