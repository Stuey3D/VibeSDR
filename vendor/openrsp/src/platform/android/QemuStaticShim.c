/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the OpenRSP authors
 *
 * This file is part of OpenRSP.
 *
 * OpenRSP is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * OpenRSP is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with OpenRSP. If
 * not, see <https://www.gnu.org/licenses/>.
 */

/* TEST-ONLY stand-ins for libandroid/liblog in static executables run under qemu-user
 * (cmake -DSDRPLAY_ANDROID_QEMU_STATIC=ON; the NDK ships those libraries only as .so).
 * ASharedMemory_* -> memfd_create(2) + ftruncate (what libandroid itself does since Q);
 * __android_log_write -> stderr. Never part of an app build. */
#include <android/log.h>
#include <android/sharedmem.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int ASharedMemory_create(const char *name, size_t size)
{
    int fd = (int)syscall(__NR_memfd_create, name ? name : "shm", 1u /* MFD_CLOEXEC */);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

size_t ASharedMemory_getSize(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 ? (size_t)st.st_size : 0;
}

int __android_log_write(int prio, const char *tag, const char *text)
{
    return fprintf(stderr, "<%d>%s: %s\n", prio, tag ? tag : "", text ? text : "");
}
