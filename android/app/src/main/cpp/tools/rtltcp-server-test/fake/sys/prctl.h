// Host stand-in for Linux <sys/prctl.h>: thread naming is a no-op off Android.
#pragma once
#define PR_SET_NAME 15
static inline int prctl(int, ...) { return 0; }
