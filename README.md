# libeco ([中文](/README_ZH.md))

libeco is a lightweight **C++20 stackless coroutine library** with a **C-style API**.
It automatically schedules coroutines based on I/O events and is well suited to
resource-constrained embedded Linux systems.

The library itself does not depend on C++ runtime libraries such as `libstdc++` or
`libc++`; it only depends on the C library and Linux system interfaces. With C++20
`co_await`, you can write asynchronous tasks as sequential code without manually
organizing callbacks.

## Features

- **C++20 stackless coroutines**: Uses the coroutine mechanism provided by C++20.
  The compiler generates coroutine frames to hold the state needed to resume
  execution, without allocating a separate call stack for each coroutine.
- **Event-driven scheduling**: Waits for I/O events using epoll. When I/O cannot
  complete immediately, the current coroutine is automatically suspended and
  resumed by the scheduler once I/O is ready. Other coroutines can run in the meantime.
- **Persistent I/O watches**: A watch is registered with epoll once when created
  and reused across multiple reads, writes, and waits. Its creator explicitly
  destroys it, avoiding registration and removal on every wait.
- **Minimal dependencies**: Uses no STL containers, iostream, exceptions, or RTTI.
  The library itself requires no C++ runtime library.
- **Coroutine synchronization**: Supports condition notifications and mutexes for
  coroutines running on the same thread. Mutex ownership is handed off in FIFO
  order, and unlocking is explicit.
- **Process execution**: Supports command argument lists, separate reads from
  stdout and stderr, asynchronous waits for process exit, and signal delivery.
  Works with kernels older than Linux 5.10 and does not require users to block
  SIGCHLD beforehand.
- **Timeout support**: I/O, condition waits, mutex waits, and process exit waits
  all support timeouts in seconds, including fractional values. Coroutines can
  also yield execution by sleeping.

Currently, only Linux is supported. Building requires GCC or Clang with C++20
coroutine support and the corresponding `<coroutine>` header. The target device
does not need a C++ runtime library installed for libeco. Other C++ features used
by the application may introduce additional dependencies.

## Building

Requires CMake 3.12 or later.

```sh
mkdir -p build
(cd build && cmake .. -DCMAKE_BUILD_TYPE=Release)
cmake --build build
(cd build && ctest --output-on-failure)
```

The build produces both the shared library `libeco.so` and the static library
`libeco.a`. Use `-DECO_BUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF` to disable examples
and tests. When cross-compiling, specify the target platform's toolchain with
`-DCMAKE_TOOLCHAIN_FILE=...`.

To install:

```sh
cmake --build build --target install
```

## Basic Usage

The two coroutines below run in an interleaved fashion: while one sleeps, the
other can continue running. Save the code as `hello.cpp`:

```cpp
#include <eco.h>
#include <stdio.h>

static co_task worker(const char *name, double interval)
{
    for (int i = 0; i < 3; ++i) {
        printf("%s: %d\n", name, i);
        co_await co_sleep(interval);
    }
}

int main()
{
    co_spawn(worker("A", 0.1));
    co_spawn(worker("B", 0.2));

    co_run();

    return 0;
}
```

Compile with a C++ compiler and link with a C compiler to avoid linking a C++
runtime library by default:

```sh
g++ -std=c++20 -fno-exceptions -fno-rtti -Iinclude -c hello.cpp -o hello.o
gcc hello.o -Lbuild -Wl,-rpath,"$PWD/build" -leco -o hello
./hello
```

Existing C++ projects can also link against libeco normally and continue using
exceptions and RTTI. The library's build options do not force applications to
disable these features. If an uncaught exception escapes a `co_task` coroutine
body, the process terminates.

Scheduling is cooperative: blocking system calls and lengthy computations block
the current scheduler thread. Each thread has its own scheduler, and nested calls
to `co_run()` are not supported.

For parameters, return values, and error details, see the API comments in the
[public headers](include/).

## AI Coding Skill

The repository provides an English-language [libeco skill](skills/libeco/README.md)
to help write, review, and debug libeco programs. It covers public API contracts,
resource lifecycles, and build examples. Follow its installation instructions to
copy the entire `skills/libeco/` directory into your own skills directory, then
invoke it in Codex with `$libeco`.
