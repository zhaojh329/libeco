# Public API (libeco 0.1.0)

Include `<eco.h>` for the public API. `<eco/version.h>` separately provides
`ECO_VERSION_MAJOR`, `ECO_VERSION_MINOR`, `ECO_VERSION_PATCH`, and
`ECO_VERSION_STRING`; it is generated during the library build.

Signatures below show public entry points. For awaitables, the result column is
the value of `co_await`, not the return value of constructing the awaitable.
Unless noted otherwise, failure is `-1` with `errno` set; object creation fails
with `nullptr`. Check errors before calling code that can overwrite `errno`.

## Scheduling and time

| Call | Result and contract |
| --- | --- |
| `int co_spawn(co_task task)` | Transfers ownership; `0` on success, `-1`/`ENOMEM` for an empty task. Starts lazily under `co_run()`. |
| `int co_run()` | `0` when all tasks finish, including an empty queue. `-1` on fatal errors; pending frames are destroyed. Nested calls return `EBUSY` without stopping the outer run. |
| `co_sleep_awaitable co_sleep(double seconds)` | Await yields `0` or `-1`. Finite nonnegative seconds only; zero yields, negative values are invalid. |

`co_task` supports move construction, not copying or move assignment. An unspawned
task destroys its frame when its wrapper dies. An exception escaping a task body
terminates the process. Public promise/awaiter structures support the coroutine
protocol; applications normally use the functions above rather than calling their
protocol methods directly.

## Persistent I/O

| Call | Result and contract |
| --- | --- |
| `co_io_watch *co_io_watch_create(int fd, unsigned events)` | Caller-owned watch, bound to the calling thread. `events` is a nonzero combination of `CO_READ` and `CO_WRITE`. |
| `int co_io_watch_destroy(co_io_watch *watch)` | Unregisters and frees the watch, but does not close `fd`. `EBUSY` while an operation still holds a reference; failure leaves ownership unchanged. |
| `co_fd_awaitable co_wait_fd(co_io_watch *watch, unsigned events, double timeout = -1)` | Await yields `0` on readiness, `-1` on failure. Requested events must be a nonzero subset of the watch's interests. |
| `co_io_awaitable co_read(co_io_watch *watch, void *buf, size_t len, double timeout = -1)` | Await yields `ssize_t`: bytes read (possibly fewer than `len`), `0` at EOF or for a zero-length read, `-1` on error. |
| `co_io_awaitable co_write(co_io_watch *watch, const void *buf, size_t len, double timeout = -1)` | Await yields `ssize_t`: `len` on success, `-1` on error. Writes the entire buffer when successful. |

- `fd` must already have `O_NONBLOCK` and support epoll. Regular files are not
  accepted merely because they have the flag. Duplicate registration fails with
  `EEXIST`; share one watch for reads and writes on a descriptor.
- Keep the descriptor open and nonblocking without reusing its number until the
  watch is destroyed. Operate on a watch only from its creating thread.
- Readiness is advisory. A hangup/error also wakes a waiter; retry the intended
  nonblocking operation to determine its result. Waiting does not consume data.
- A buffer may be null only for zero length; `len` must not exceed `SSIZE_MAX`.
  Keep write buffers unchanged until completion.
- Read/write operations and their timeout measurement start when awaited, not
  when the awaitable is constructed. Move an I/O awaitable only before awaiting.
- For I/O waits, `-1` means indefinite; `0` does not wait and can return `EAGAIN`;
  positive finite seconds bound the whole operation, with `ETIMEDOUT` on expiry.
- Write failure may follow a partial transfer whose byte count is not returned.
  Do not retry the whole buffer unless the application's protocol permits replay.
  The write follows normal `write()` SIGPIPE semantics.

## Mutexes

| Call | Result and contract |
| --- | --- |
| `co_mutex *co_mutex_create()` | Caller-owned unlocked mutex, bound to the creating thread. |
| `int co_mutex_destroy(co_mutex *mutex)` | Requires unlocked state and no outstanding wait operations; otherwise `EBUSY`. |
| `co_mutex_lock_awaitable co_mutex_lock(co_mutex *mutex, double timeout = -1)` | Await yields `0` on acquisition, `-1` on failure. FIFO handoff under contention. |
| `int co_mutex_trylock(co_mutex *mutex)` | Immediate `0` on acquisition or `-1`/`EBUSY` when locked. |
| `int co_mutex_unlock(co_mutex *mutex)` | Immediate release/handoff; waiters resume through the scheduler. |

All operations belong on the creating thread, whose lifetime must cover the
mutex. This is not a thread synchronization primitive or a recursive mutex.
Successful acquisition requires explicit unlocking, including on coroutine exits
and failure paths. Use an application guard when scope-based release is needed.
Coroutine ownership is not checked: any code on the owning thread may unlock.

Lock timeouts use `-1`, `0`, or positive finite seconds. A contended zero-timeout
awaited lock reports `EAGAIN`, whereas `co_mutex_trylock()` reports `EBUSY`.
Unlocking an unlocked mutex reports `EPERM`; unlocking before an outstanding
handoff recipient resumes reports `EBUSY`.

## Conditions

| Call | Result and contract |
| --- | --- |
| `co_cond *co_cond_create()` | Caller-owned condition with an empty wait queue. |
| `int co_cond_destroy(co_cond *cond)` | Requires no queued waiters (`EBUSY` otherwise). |
| `co_cond_awaitable co_cond_wait(co_cond *cond, double timeout = -1)` | Await yields `0` on notification or `-1` on failure. No mutex argument. |
| `int co_cond_signal(co_cond *cond)` | Makes the oldest waiter ready; succeeds even if there are no waiters. |
| `int co_cond_broadcast(co_cond *cond)` | Makes all queued waiters ready in FIFO order. |

Notifications are not retained. Check the application's predicate in a loop;
this API does not atomically unlock/relock a mutex as a pthread condition does.
Serialize all accesses and use the waiters' scheduler thread while waiters exist.
Keep the condition alive while they are queued. Notified waiters are detached,
so the condition may be destroyed before those waiters resume.

Timeouts use `-1`, `0`, or positive finite seconds. Zero returns `EAGAIN`; expiry
returns `ETIMEDOUT`. Null conditions and conflicting active schedulers are errors.

## Child processes

| Call | Result and contract |
| --- | --- |
| `co_process *co_process_start(const char *file, const char *const argv[], unsigned flags = 0)` | Starts synchronously; returns a caller-owned process. Searches PATH, without shell expansion. |
| `co_io_watch *co_process_stdout(co_process *process)` | Borrowed read watch, or null when not captured. |
| `co_io_watch *co_process_stderr(co_process *process)` | Borrowed read watch, or null when not captured. |
| `co_process_wait_awaitable co_process_wait(co_process *process, int *status, double timeout = -1)` | Await yields `0` on successful collection, `-1` on failure. `status` must be nonnull and is written only on success. |
| `int co_process_kill(co_process *process, int signal)` | Signals the direct child only, without waiting; `ESRCH` if already collected. |
| `int co_process_destroy(co_process *process)` | Releases an exited process and captured streams. Never blocks for exit; running children or outstanding operations cause `EBUSY`. |

- Pass a null-terminated argument list including `argv[0]`. Capture flags are
  `CO_PROCESS_STDOUT` and `CO_PROCESS_STDERR`; other standard streams are inherited.
- Borrowed output watches belong to the process. Never destroy them or close
  their descriptors yourself. They become invalid when the process is destroyed.
- Drain captured stdout and stderr concurrently with each other and with exit
  waiting. Waiting for exit before reading can deadlock on a full pipe.
- Exit and output EOF are separate: buffered data or descendant writers may
  remain after exit. Plan stream lifetimes independently of exit observation.
- Decode the raw wait status using `WIFEXITED`/`WEXITSTATUS` or
  `WIFSIGNALED`/`WTERMSIG`. A nonzero child exit code is not a failed wait.
- Only one active wait per process is allowed (`EBUSY` otherwise); collected
  status is cached. Wait timeouts use `-1`, `0` (`EAGAIN` if running), or positive
  finite seconds (`ETIMEDOUT`). Timeout/coroutine destruction does not kill or
  release the child. Its external owner remains responsible for cleanup.
- All live process objects share one scheduler thread. Other threads cannot
  start processes until all objects are destroyed. Keep the owning thread alive.
- The library installs a SIGCHLD handler and unblocks SIGCHLD on the owner thread.
  An existing handler, `SIG_IGN`, or `SA_NOCLDWAIT` conflicts (`EBUSY`). While objects
  exist, do not change that disposition, block SIGCHLD on the owner thread,
  consume it elsewhere, or reap owned children elsewhere. The last destruction
  restores prior disposition and the owner's original blocked state.
- Collect exits by servicing the owner loop or calling wait/kill/destroy.
  `ECHILD` indicates another consumer or automatic reaping took the exit status;
  destruction still permits cleanup in that case.
