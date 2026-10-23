# libeco

libeco 是一个轻量级的 **C++20 无栈协程库**，提供 **C 风格的 API**，
基于 I/O 事件自动调度协程，适合资源受限的嵌入式 Linux 系统。

库本身不依赖 `libstdc++`、`libc++` 等 C++ 运行库，只依赖 C 库和 Linux 系统接口。
通过 C++20 的 `co_await`，可以用顺序执行的代码编写异步任务，无需手动组织回调。

## 特性

- **C++20 无栈协程**：基于 C++20 语言提供的协程机制，由编译器生成协程帧以保存
  恢复执行所需的状态，无需为每个协程分配独立的调用栈。
- **事件驱动**：基于 epoll 等待 I/O 事件。I/O 无法立即完成时自动挂起当前协程，
  I/O 就绪后由调度器恢复执行，等待期间可运行其他协程。
- **持久 I/O 监听**：watch 创建时向 epoll 注册一次，多次读写和等待复用同一监听，
  由创建者显式销毁，无需每次等待都注册和删除。
- **轻量依赖**：不使用 STL 容器、iostream、异常或 RTTI，库本身无需 C++ 运行库。
- **协程同步**：支持条件通知和同线程协程互斥锁；互斥锁按 FIFO 顺序交接，使用显式解锁。
- **进程执行**：支持命令参数列表、分别读取 stdout/stderr、异步等待退出及发送信号，
  兼容 Linux 5.10 以下内核，无需用户预先屏蔽 SIGCHLD。
- **超时支持**：I/O、条件等待、互斥锁等待和进程退出等待均支持以秒为单位的超时，
  可使用小数。协程也可通过休眠让出执行。

目前仅支持 Linux。编译时需要支持 C++20 协程的 GCC/Clang 及配套的 `<coroutine>`
头文件；运行设备无需为 libeco 安装 C++ 运行库。应用自身使用的其他 C++ 功能可能
引入额外依赖。

## 构建

需要 CMake 3.12 或更新版本。

```sh
mkdir -p build
(cd build && cmake .. -DCMAKE_BUILD_TYPE=Release)
cmake --build build
(cd build && ctest --output-on-failure)
```

构建同时生成动态库 `libeco.so` 和静态库 `libeco.a`。可使用
`-DECO_BUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF` 关闭示例和测试。
交叉编译时，通过 `-DCMAKE_TOOLCHAIN_FILE=...` 指定目标平台工具链。

安装：

```sh
cmake --build build --target install
```

## 基本用法

下面的两个协程会交替执行；一个协程休眠时，另一个仍可运行。
将代码保存为 `hello.cpp`：

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

使用 C++ 编译器编译、C 编译器链接，可避免默认链接 C++ 运行库：

```sh
g++ -std=c++20 -fno-exceptions -fno-rtti -Iinclude -c hello.cpp -o hello.o
gcc hello.o -Lbuild -Wl,-rpath,"$PWD/build" -leco -o hello
./hello
```

已有 C++ 项目也可以正常链接 libeco，继续使用异常和 RTTI。库本身的编译选项不会强制
应用关闭这些功能；若异常未被捕获并传播出 `co_task` 协程体，进程会终止。

调度是协作式的，阻塞系统调用和长时间计算会阻塞当前调度线程。每个线程拥有独立的
调度器，`co_run()` 不支持嵌套调用。

各接口的参数、返回值及错误说明见[公开头文件](include/)中的 API 注释。

## AI 编程技能

仓库提供英文 [libeco skill](skills/libeco/README.md)，用于辅助编写、审查和调试
libeco 程序，包含公开 API 约定、资源生命周期和构建示例。可按其中的安装说明将
`skills/libeco/` 整个目录复制到自己的技能目录，在 Codex 中通过 `$libeco` 调用。
