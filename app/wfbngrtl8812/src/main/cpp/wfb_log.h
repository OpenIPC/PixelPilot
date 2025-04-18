#pragma once

#include <android/log.h>
#include <cstdarg>

#define WFB_ERR(...)                                                                                                   \
    do {                                                                                                               \
        __android_log_print(ANDROID_LOG_ERROR, "wfb-ng", __VA_ARGS__);                                                 \
    } while (0)
#define WFB_INFO(...)                                                                                                  \
    do {                                                                                                               \
        (void)0; /*__android_log_print(ANDROID_LOG_INFO, "wfb-ng", __VA_ARGS__);*/                                     \
    } while (0)

#define WFB_DBG(...) (void(0))

#define ANDROID_IPC_MSG(...)                                                                                           \
    do {                                                                                                               \
        __android_log_print(ANDROID_LOG_INFO, "wfb-ng", __VA_ARGS__);                                                  \
    } while (0)
#define IPC_MSG(...)                                                                                                   \
    do {                                                                                                               \
        __android_log_print(ANDROID_LOG_INFO, "wfb-ng", __VA_ARGS__);                                                  \
    } while (0)
#define IPC_MSG_SEND() (void)0
