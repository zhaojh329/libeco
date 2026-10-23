# Building and examples

The library requires Linux, CMake 3.12 or newer, and a compiler with C++20
coroutines and the corresponding `<coroutine>` header. This skill does not install
libeco or a toolchain. Use the application's existing build system when present.

## Build a source checkout

Run from the libeco repository root, not the installed skill directory:

```sh
mkdir -p build
(cd build && cmake .. -DCMAKE_BUILD_TYPE=Release)
cmake --build build
(cd build && ctest --output-on-failure)
```

The outputs are `libeco.so` and `libeco.a`. Disable examples and tests for a
library-only build with `-DECO_BUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF`. For cross
compilation, pass the target's `-DCMAKE_TOOLCHAIN_FILE=...`; host test success
does not establish target compatibility.

Set `CMAKE_INSTALL_PREFIX` during configuration to choose the destination, then
run `cmake --build build --target install` when installation is requested.

## Consume an installed library with CMake

```cmake
cmake_minimum_required(VERSION 3.12)
project(libeco_app LANGUAGES CXX)

find_package(eco CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE eco::eco_shared)
```

Use `eco::eco_static` for the static library. Set `CMAKE_PREFIX_PATH` to the
installation prefix if needed. Exported targets supply include paths and the
C++20 requirement; the package name is `eco`, not `libeco`.

Normal C++ applications may retain exceptions, RTTI, and their existing runtime
libraries. libeco's own lack of a C++ runtime dependency does not remove an
application's dependencies.

## Complete program

Save as `main.cpp`. The result object lives through `co_run()`; the coroutine
reports its operation failure separately from scheduler failure.

```cpp
#include <eco.h>
#include <stdio.h>

static co_task worker(int *result)
{
    if (co_await co_sleep(0.001) < 0) {
        perror("sleep");
        *result = 1;
        co_return;
    }

    *result = 0;
}

int main()
{
    int result = 1;

    if (co_spawn(worker(&result)) < 0) {
        perror("spawn");
        return 1;
    }

    if (co_run() < 0) {
        perror("run");
        return 1;
    }

    return result;
}
```

## pkg-config and runtime-minimal applications

The pkg-config package is named `libeco`. Set `PKG_CONFIG_PATH` to the installed
pkg-config directory when using a custom prefix. To compile the program above
without implicitly linking a C++ runtime, compile with the C++ driver and link
with the C driver:

```sh
g++ $(pkg-config --cflags libeco) -fno-exceptions -fno-rtti -c main.cpp -o main.o
gcc main.o $(pkg-config --libs libeco) -o app
./app
```

For a custom shared-library prefix, configure the runtime loader search path
(e.g. an application RPATH) as well as the compile/link paths. Disabling exceptions
and RTTI does not make arbitrary STL or other C++ code runtime-independent.

For an explicit static link, use the installed archive path rather than assuming
`pkg-config --static --libs libeco` forces the linker to choose `libeco.a`.

For builds intended to avoid the C++ runtime, inspect the final executable and
shared library with `readelf -d` for `NEEDED` entries. Repository tests already
check this property for the library and representative callers.
