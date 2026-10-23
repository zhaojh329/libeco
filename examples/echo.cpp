/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco.h>

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Application-owned resources are released even if co_run() destroys the task. */
struct connection {
    int fd;
    struct co_io_watch *watch;

    connection(int fd, unsigned events) : fd(fd), watch(co_io_watch_create(fd, events)) {}

    connection(const connection &) = delete;
    connection &operator=(const connection &) = delete;

    ~connection()
    {
        if (watch && co_io_watch_destroy(watch) < 0) {
            perror("destroy watch");
            return;
        }

        close(fd);
    }
};

static co_task handle_connection(int fd)
{
    connection io(fd, CO_READ | CO_WRITE);

    if (!io.watch) {
        perror("create connection watch");
        co_return;
    }

    char buffer[1024];

    for (;;) {
        ssize_t received = co_await co_read(io.watch, buffer, sizeof(buffer));

        if (received <= 0)
            break;

        if (co_await co_write(io.watch, buffer, (size_t)received) < 0)
            break;
    }
}

static co_task accept_connections(int fd)
{
    connection io(fd, CO_READ);

    if (!io.watch) {
        perror("create listener watch");
        co_return;
    }

    for (;;) {
        int accepted = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (accepted < 0) {
            if (errno == EINTR)
                continue;

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (co_await co_wait_fd(io.watch, CO_READ) < 0) {
                    perror("wait for connection");
                    break;
                }

                continue;
            }

            perror("accept");
            break;
        }

        if (co_spawn(handle_connection(accepted)) < 0) {
            perror("spawn connection");
            close(accepted);
        }
    }
}

static co_task tick(void)
{
    if (co_await co_sleep(1.0) < 0)
        perror("sleep");
    else
        puts("echo server is running");
}

int main(int argc, char **argv)
{
    char *end;
    long port = argc > 1 ? strtol(argv[1], &end, 10) : 9000;
    struct sockaddr_in address = {};
    int enabled = 1;
    int fd;

    if (argc > 2 || port <= 0 || port > 65535 || (argc > 1 && *end)) {
        fprintf(stderr, "usage: %s [port: 1..65535]\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)port);

    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return 1;
    }

    if (co_spawn(accept_connections(fd)) < 0) {
        perror("spawn server");
        close(fd);
        return 1;
    }

    if (co_spawn(tick()) < 0)
        perror("spawn ticker");

    printf("listening on 127.0.0.1:%ld\n", port);

    if (co_run() < 0) {
        perror("run");
        return 1;
    }

    return 0;
}
