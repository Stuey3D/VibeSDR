// Host stand-in for <android/log.h>: the server's log lines go to stderr.
#pragma once
#include <cstdio>
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
#define __android_log_print(prio, tag, ...) (std::fprintf(stderr, "[%s] ", tag), std::fprintf(stderr, __VA_ARGS__), std::fprintf(stderr, "\n"))
