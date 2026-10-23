/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

static const char *program = "/proc/self/exe";
static const size_t burst_size = 512 * 1024;

static void write_all(int fd, const void *data, size_t size)
{
    auto *bytes = static_cast<const char *>(data);

    while (size) {
        ssize_t n = write(fd, bytes, size);

        if (n < 0 && errno == EINTR)
            continue;

        assert(n > 0);
        bytes += n;
        size -= n;
    }
}

static int child(int argc, char **argv)
{
    sigset_t mask;

    assert(sigprocmask(SIG_BLOCK, nullptr, &mask) == 0);
    assert(!sigismember(&mask, SIGCHLD));

    if (!strcmp(argv[2], "args")) {
        for (int i = 3; i < argc; ++i) {
            write_all(STDOUT_FILENO, argv[i], strlen(argv[i]));
            write_all(STDOUT_FILENO, "|", 1);
        }

        write_all(STDERR_FILENO, "error output", 12);
        return 7;
    }

    if (!strcmp(argv[2], "burst")) {
        assert(!(fcntl(STDOUT_FILENO, F_GETFL) & O_NONBLOCK));
        assert(!(fcntl(STDERR_FILENO, F_GETFL) & O_NONBLOCK));

        char output[4096], error[4096];

        memset(output, 'o', sizeof(output));
        memset(error, 'e', sizeof(error));

        for (size_t i = 0; i < burst_size; i += sizeof(output)) {
            write_all(STDOUT_FILENO, output, sizeof(output));
            write_all(STDERR_FILENO, error, sizeof(error));
        }

        return 0;
    }

    if (!strcmp(argv[2], "hold")) {
        for (;;)
            pause();
    }

    if (!strcmp(argv[2], "delayed")) {
        struct timespec delay = {0, 30000000};

        nanosleep(&delay, nullptr);
        return 23;
    }

    if (!strcmp(argv[2], "descendant")) {
        pid_t pid = fork();

        assert(pid >= 0);

        if (pid == 0) {
            struct timespec delay = {0, 30000000};

            nanosleep(&delay, nullptr);
            write_all(STDOUT_FILENO, "tail", 4);
            _exit(19);
        }

        return 11;
    }

    return 0;
}

static int fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    int count = 0;

    assert(directory);

    while (auto *entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            ++count;
    }

    closedir(directory);

    return count;
}

static co_process *start(const char *mode, unsigned flags = 0)
{
    const char *argv[] = {"test_process", "--child", mode, nullptr};
    auto *process = co_process_start(program, argv, flags);

    assert(process);

    return process;
}

static co_task wait_exit(co_process *process, int expected)
{
    int status = -1;

    assert(co_await co_process_wait(process, &status, 3) == 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == expected);
    assert(co_process_kill(process, 0) == -1 && errno == ESRCH);

    int cached = -1;

    assert(co_await co_process_wait(process, &cached, 0) == 0);
    assert(cached == status);
}

static co_task read_text(co_io_watch *watch, char *buffer, size_t capacity)
{
    size_t used = 0;

    for (;;) {
        assert(used < capacity - 1);

        ssize_t count = co_await co_read(watch, buffer + used, capacity - used - 1, 3);

        assert(count >= 0);

        if (!count)
            break;

        used += count;
    }

    buffer[used] = '\0';
}

static co_task drain(co_io_watch *watch, char expected, size_t *total)
{
    char buffer[4096];

    for (;;) {
        ssize_t count = co_await co_read(watch, buffer, sizeof(buffer), 3);

        assert(count >= 0);

        if (!count)
            break;

        for (ssize_t i = 0; i < count; ++i)
            assert(buffer[i] == expected);

        *total += count;
    }
}

static co_task tick(bool *ran)
{
    assert(co_await co_sleep(0.001) == 0);
    *ran = true;
}

static co_task wait_and_kill(co_process *process)
{
    int status = 123;

    assert(co_await co_process_wait(process, &status, 0) == -1 && errno == EAGAIN);
    assert(status == 123);
    assert(co_await co_process_wait(process, &status, 0.002) == -1 && errno == ETIMEDOUT);
    assert(status == 123);
    assert(co_process_destroy(process) == -1 && errno == EBUSY);
    assert(co_process_kill(process, 0) == 0);
    assert(co_process_kill(process, SIGKILL) == 0);
    assert(co_await co_process_wait(process, &status, 3) == 0);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

static co_task pending(co_process *process)
{
    int status;

    co_await co_process_wait(process, &status);
    assert(false);
}

static co_task competing(co_process *process)
{
    int status = 123;

    assert(co_await co_process_wait(process, &status) == -1 && errno == EBUSY);
    assert(status == 123);
    assert(co_process_destroy(process) == -1 && errno == EBUSY);
}

static co_task unsupported_suspend(void)
{
    co_await std::suspend_always{};
}

static co_task pending_read(co_io_watch *watch)
{
    char byte;

    co_await co_read(watch, &byte, 1);
    assert(false);
}

static co_task invalid_waits(co_process *process)
{
    int status = 123;

    assert(co_await co_process_wait(nullptr, &status) == -1 && errno == EINVAL);
    assert(co_await co_process_wait(process, nullptr) == -1 && errno == EINVAL);
    assert(co_await co_process_wait(process, &status, -2) == -1 && errno == EINVAL);
    assert(co_await co_process_wait(process, &status, INFINITY) == -1 && errno == EINVAL);
    assert(co_await co_process_wait(process, &status, NAN) == -1 && errno == EINVAL);
    assert(status == 123);
}

static co_task read_after_exit(co_process *process)
{
    int status;
    char buffer[32];
    size_t used = 0;

    assert(co_await co_process_wait(process, &status, 3) == 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 7);

    for (;;) {
        ssize_t count = co_await co_read(co_process_stdout(process), buffer + used,
                                         sizeof(buffer) - used - 1, 3);

        assert(count >= 0);

        if (!count)
            break;

        used += count;
    }

    buffer[used] = '\0';
    assert(!strcmp(buffer, "buffered|"));
}

static co_task borrowed_output(co_process *process)
{
    int status;
    char byte;

    assert(co_await co_process_wait(process, &status, 3) == 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 11);

    /* A descendant keeps the output pipe open after the direct child exits. */
    auto read = co_read(co_process_stdout(process), &byte, 1, 3);
    bool ready = read.await_ready();

    assert(co_process_destroy(process) == -1 && errno == EBUSY);

    if (ready) {
        assert(read.await_resume() == 1 && byte == 't');
    }
}

static void *wrong_thread(void *pointer)
{
    auto *process = static_cast<co_process *>(pointer);
    const char *argv[] = {"test_process", "--child", "exit", nullptr};

    assert(co_process_start(program, argv) == nullptr && errno == EBUSY);
    assert(co_process_stdout(process) == nullptr && errno == EINVAL);
    assert(co_process_stderr(process) == nullptr && errno == EINVAL);
    assert(co_process_kill(process, 0) == -1 && errno == EINVAL);
    assert(co_process_destroy(process) == -1 && errno == EINVAL);

    return nullptr;
}

static co_task externally_reaped(co_process *process)
{
    int status = 123;

    assert(co_await co_process_wait(process, &status, 3) == -1 && errno == ECHILD);
    assert(status == 123);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--child"))
        return child(argc, argv);

    sigset_t children, original;
    struct sigaction original_action, action = {};

    sigemptyset(&children);
    sigaddset(&children, SIGCHLD);
    assert(sigprocmask(SIG_UNBLOCK, &children, &original) == 0);
    action.sa_handler = SIG_DFL;
    assert(sigaction(SIGCHLD, &action, &original_action) == 0);

    const char *exit_args[] = {"test_process", "--child", "exit", nullptr};
    int initial_fds = fd_count();

    /* Default signal settings work without any application preparation. */
    auto *initial = co_process_start(program, exit_args);

    assert(initial);
    assert(co_spawn(wait_exit(initial, 0)) == 0);
    assert(co_run() == 0);
    assert(co_process_destroy(initial) == 0);

    sigset_t current_mask;
    struct sigaction current_action;

    assert(sigprocmask(SIG_BLOCK, nullptr, &current_mask) == 0);
    assert(!sigismember(&current_mask, SIGCHLD));
    assert(sigaction(SIGCHLD, nullptr, &current_action) == 0);
    assert(current_action.sa_handler == SIG_DFL);

    /* A previously blocked signal stays blocked after the last process closes. */
    assert(sigprocmask(SIG_BLOCK, &children, nullptr) == 0);
    initial = co_process_start(program, exit_args);
    assert(initial);
    assert(sigprocmask(SIG_BLOCK, nullptr, &current_mask) == 0);
    assert(!sigismember(&current_mask, SIGCHLD));
    assert(co_spawn(wait_exit(initial, 0)) == 0);
    assert(co_run() == 0);
    assert(co_process_destroy(initial) == 0);
    assert(sigprocmask(SIG_BLOCK, nullptr, &current_mask) == 0);
    assert(sigismember(&current_mask, SIGCHLD));
    assert(sigprocmask(SIG_UNBLOCK, &children, nullptr) == 0);
    assert(co_process_start(nullptr, exit_args) == nullptr && errno == EINVAL);
    assert(co_process_start("", exit_args) == nullptr && errno == EINVAL);
    assert(co_process_start(program, nullptr) == nullptr && errno == EINVAL);
    assert(co_process_start(program, exit_args, 4) == nullptr && errno == EINVAL);
    assert(co_process_destroy(nullptr) == -1 && errno == EINVAL);
    assert(co_process_kill(nullptr, 0) == -1 && errno == EINVAL);

    action.sa_handler = SIG_IGN;
    assert(sigaction(SIGCHLD, &action, nullptr) == 0);
    assert(co_process_start(program, exit_args) == nullptr && errno == EBUSY);
    assert(sigaction(SIGCHLD, nullptr, &current_action) == 0);
    assert(current_action.sa_handler == SIG_IGN);
    action.sa_handler = SIG_DFL;
    action.sa_flags = SA_NOCLDWAIT;
    assert(sigaction(SIGCHLD, &action, nullptr) == 0);
    assert(co_process_start(program, exit_args) == nullptr && errno == EBUSY);
    assert(sigaction(SIGCHLD, nullptr, &current_action) == 0);
    assert(current_action.sa_flags & SA_NOCLDWAIT);
    action.sa_flags = 0;
    assert(sigaction(SIGCHLD, &action, nullptr) == 0);

    assert(co_process_start("/libeco/no-such-program", exit_args,
                            CO_PROCESS_STDOUT | CO_PROCESS_STDERR) == nullptr && errno == ENOENT);
    assert(fd_count() == initial_fds);

    const char *args[] = {"test_process", "--child", "args", "a b", "", "$(literal);*", nullptr};
    auto *process = co_process_start(program, args, CO_PROCESS_STDOUT | CO_PROCESS_STDERR);
    char output[128] = {}, error[128] = {};

    assert(process);
    assert(co_spawn(read_text(co_process_stdout(process), output, sizeof(output))) == 0);
    assert(co_spawn(read_text(co_process_stderr(process), error, sizeof(error))) == 0);
    assert(co_spawn(wait_exit(process, 7)) == 0);
    assert(co_run() == 0);
    assert(!strcmp(output, "a b||$(literal);*|"));
    assert(!strcmp(error, "error output"));
    assert(co_process_destroy(process) == 0);

    process = start("burst", CO_PROCESS_STDOUT | CO_PROCESS_STDERR);
    size_t out_size = 0, err_size = 0;
    bool ticked = false;

    assert(co_spawn(drain(co_process_stdout(process), 'o', &out_size)) == 0);
    assert(co_spawn(drain(co_process_stderr(process), 'e', &err_size)) == 0);
    assert(co_spawn(wait_exit(process, 0)) == 0);
    assert(co_spawn(tick(&ticked)) == 0);
    assert(co_run() == 0);
    assert(out_size == burst_size && err_size == burst_size && ticked);
    assert(co_process_destroy(process) == 0);

    process = start("hold");
    assert(!co_process_stdout(process) && !co_process_stderr(process));
    assert(co_spawn(invalid_waits(process)) == 0);
    assert(co_spawn(wait_and_kill(process)) == 0);
    assert(co_run() == 0);
    assert(co_process_destroy(process) == 0);

    process = start("hold", CO_PROCESS_STDOUT | CO_PROCESS_STDERR);
    pthread_t thread;

    assert(pthread_create(&thread, nullptr, wrong_thread, process) == 0);
    assert(pthread_join(thread, nullptr) == 0);
    assert(co_spawn(pending(process)) == 0);
    assert(co_spawn(competing(process)) == 0);
    assert(co_spawn(pending_read(co_process_stdout(process))) == 0);
    assert(co_spawn(pending_read(co_process_stderr(process))) == 0);
    assert(co_spawn(unsupported_suspend()) == 0);
    assert(co_run() == -1 && errno == EPROTO);
    assert(co_spawn(wait_and_kill(process)) == 0);
    assert(co_run() == 0);
    assert(co_process_destroy(process) == 0);

    /* Coalesced exits, including unrelated children, must all be handled. */
    pid_t unrelated = fork();

    assert(unrelated >= 0);

    if (!unrelated)
        _exit(29);

    co_process *many[24];

    for (auto &p : many)
        p = start("exit");

    for (auto *p : many)
        assert(co_spawn(wait_exit(p, 0)) == 0);

    assert(co_run() == 0);

    for (auto *p : many)
        assert(co_process_destroy(p) == 0);

    int status;

    assert(waitpid(unrelated, &status, 0) == unrelated);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 29);

    /* The event dispatcher collects exits even with no explicit process waiter. */
    process = start("exit");
    struct timespec delay = {0, 30000000};

    nanosleep(&delay, nullptr);
    assert(co_spawn(tick(&ticked)) == 0);
    assert(co_run() == 0);
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    assert(co_spawn(wait_exit(process, 0)) == 0);
    assert(co_run() == 0);
    assert(co_process_destroy(process) == 0);

    const char *buffered[] = {"sh", "-c", "printf 'buffered|'; exit 7", nullptr};

    process = co_process_start("sh", buffered, CO_PROCESS_STDOUT);
    assert(process);
    assert(co_spawn(read_after_exit(process)) == 0);
    assert(co_run() == 0);
    assert(co_process_destroy(process) == 0);

    assert(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    process = start("descendant", CO_PROCESS_STDOUT);
    assert(co_spawn(borrowed_output(process)) == 0);
    assert(co_run() == 0);
    assert(co_spawn(read_text(co_process_stdout(process), output, sizeof(output))) == 0);
    assert(co_run() == 0);
    assert(!strcmp(output, "tail") || !strcmp(output, "ail"));
    assert(co_process_destroy(process) == 0);
    assert(waitpid(-1, &status, 0) > 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 19);
    assert(prctl(PR_SET_CHILD_SUBREAPER, 0) == 0);

    /* Test documented ECHILD recovery if another component steals a child. */
    process = start("delayed");
    assert(waitpid(-1, &status, 0) > 0);
    assert(co_spawn(externally_reaped(process)) == 0);
    assert(co_run() == 0);
    assert(co_process_destroy(process) == 0);

    /* Low-numbered pipe descriptors must not break dup2 actions or leak writers. */
    int saved_out = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    int saved_err = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);

    assert(saved_out >= 0 && saved_err >= 0);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    process = co_process_start(program, args, CO_PROCESS_STDOUT | CO_PROCESS_STDERR);
    assert(process);
    assert(co_spawn(read_text(co_process_stdout(process), output, sizeof(output))) == 0);
    assert(co_spawn(read_text(co_process_stderr(process), error, sizeof(error))) == 0);
    assert(co_spawn(wait_exit(process, 7)) == 0);
    assert(co_run() == 0);
    assert(!strcmp(output, "a b||$(literal);*|"));
    assert(!strcmp(error, "error output"));
    assert(co_process_destroy(process) == 0);

    /* Internal descriptors may temporarily occupy the closed standard slots. */
    assert(dup2(saved_out, STDOUT_FILENO) == STDOUT_FILENO);
    assert(dup2(saved_err, STDERR_FILENO) == STDERR_FILENO);
    close(saved_out);
    close(saved_err);

    assert(fd_count() == initial_fds);
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    assert(sigprocmask(SIG_SETMASK, &original, nullptr) == 0);
    assert(sigaction(SIGCHLD, &original_action, nullptr) == 0);

    puts("process: arguments, streams, exits, timeout, cancellation, ownership OK");

    return 0;
}
