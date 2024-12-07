//
// Created by gaeta on 2024-06-20.
//

#ifndef FPVUE_TIME_UTIL_H
#define FPVUE_TIME_UTIL_H

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/**
 * @return milliseconds
 */
uint64_t get_time_ms()
{
    struct timespec spec;
    if (clock_gettime(1, &spec) == -1)
    { /* 1 is CLOCK_MONOTONIC */
        abort();
    }
    return spec.tv_sec * 1000 + spec.tv_nsec / 1e6;
}

#endif  // FPVUE_TIME_UTIL_H
