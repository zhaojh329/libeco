/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jianhui Zhao <zhaojh329@gmail.com>
 */
#include <eco.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static size_t resident_bytes(void)
{
    FILE *file = fopen("/proc/self/statm", "r");
    unsigned long virtual_pages, resident_pages;
    long page_size = sysconf(_SC_PAGESIZE);

    if (!file || page_size <= 0) {
        perror("read RSS");
        exit(1);
    }

    if (fscanf(file, "%lu %lu", &virtual_pages, &resident_pages) != 2) {
        fprintf(stderr, "cannot parse /proc/self/statm\n");
        exit(1);
    }

    fclose(file);

    return resident_pages * (size_t)page_size;
}

static co_task suspended(bool *released)
{
    while (!*released)
        co_await co_sleep(0);
}

static co_task measure(bool *released, size_t tasks, size_t baseline)
{
    size_t rss = resident_bytes();
    size_t increase = rss > baseline ? rss - baseline : 0;

    printf("tasks=%zu baseline_RSS=%zu suspended_RSS=%zu increase=%zu bytes\n",
           tasks, baseline, rss, increase);
    printf("RSS_increase/task=%.1f bytes (frames, scheduler and allocator included)\n",
           (double)increase / tasks);

    *released = true;

    co_return;
}

int main(int argc, char **argv)
{
    size_t tasks = 100000;
    char *end;

    if (argc > 1) {
        errno = 0;

        unsigned long long parsed = strtoull(argv[1], &end, 10);

        if (errno || *end || argv[1][0] == '-' || !parsed || parsed > SIZE_MAX)
            return 1;

        tasks = (size_t)parsed;
    }

    if (argc > 2)
        return 1;

    bool released = false;

    size_t baseline = resident_bytes();

    for (size_t i = 0; i < tasks; ++i) {
        if (co_spawn(suspended(&released)) < 0) {
            perror("spawn");
            return 1;
        }
    }

    if (co_spawn(measure(&released, tasks, baseline)) < 0 ||
        co_run() < 0) {
        perror("run");
        return 1;
    }

    return 0;
}
