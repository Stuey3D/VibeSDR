// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the OpenRSP authors
//
// This file is part of OpenRSP.
//
// OpenRSP is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// OpenRSP is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with OpenRSP. If
// not, see <https://www.gnu.org/licenses/>.

// Windows service host: process entry, ServiceMain and the service control handler.
//
//  - The service runs as SERVICE_WIN32_OWN_PROCESS under the SCM name SDRplayAPIService. If
//    StartServiceCtrlDispatcherW fails (e.g. started from a console), the process exits with
//    GetLastError().
//  - ServiceMain reports START_PENDING, then RUNNING (accepting STOP only), runs the worker and
//    reports STOPPED with exit code 0.
//  - STOP while RUNNING reports STOP_PENDING and requests the worker to stop; every other
//    control code is answered NO_ERROR and otherwise ignored.
//  - Suspend / resume notifications go to the handler set with windows::setPowerEventHandler.
//  - "--console" (also "-console", "/console") runs the same body in the foreground with
//    Ctrl+C / Ctrl+Break / close / shutdown as the stop request, for Wine and debugging.
#include "platform/ServiceHost.h"
#include "platform/windows/WinCommon.h"
#include "platform/windows/WindowsService.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace sdrplay {
namespace platform {

namespace {
std::atomic<bool> g_stop{false};
std::atomic<bool> g_asService{false};
const std::function<int(const StopSignal &)> *g_body = nullptr;
int g_exitCode = 0;

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
std::mutex g_statusMutex;

std::mutex g_powerMutex;
std::function<void(bool)> g_powerHandler;

// Checkpoint of the current pending state; 0 outside START_PENDING / STOP_PENDING.
DWORD g_checkPoint = 0;

class FlagStop final : public StopSignal {
public:
    bool stopRequested() const override { return g_stop.load(); }
};

void setStatus(DWORD state, DWORD accepted, DWORD exitCode)
{
    std::lock_guard<std::mutex> lk(g_statusMutex);
    const bool pending = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING;
    g_checkPoint = pending ? g_checkPoint + 1 : 0;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = accepted;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwCheckPoint = g_checkPoint;
    g_status.dwWaitHint = 0;
    if (g_statusHandle) SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI ctrlHandler(DWORD control, DWORD, LPVOID, LPVOID)
{
    if (control == SERVICE_CONTROL_STOP) {
        bool running;
        {
            std::lock_guard<std::mutex> lk(g_statusMutex);
            running = g_status.dwCurrentState == SERVICE_RUNNING;
        }
        if (running) {
            setStatus(SERVICE_STOP_PENDING, 0, NO_ERROR);
            g_stop = true;
        }
    }
    return NO_ERROR;
}

// ---- RegisterSuspendResumeNotification (user32, Windows 8+), resolved at run time ----
// MinGW-w64 declares neither DEVICE_NOTIFY_CALLBACK nor DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS;
// NotifySubscribeParams has the layout of the latter.
#ifndef DEVICE_NOTIFY_CALLBACK
#define DEVICE_NOTIFY_CALLBACK 2
#endif
using PowerCallback = ULONG(CALLBACK *)(PVOID context, ULONG type, PVOID setting);
struct NotifySubscribeParams {
    PowerCallback callback;
    PVOID context;
};

ULONG CALLBACK powerCallback(PVOID, ULONG type, PVOID)
{
    std::function<void(bool)> h;
    {
        std::lock_guard<std::mutex> lk(g_powerMutex);
        h = g_powerHandler;
    }
    if (!h) return 0;
    if (type == PBT_APMSUSPEND) h(true);
    else if (type == PBT_APMRESUMESUSPEND) h(false);
    return 0;
}

class PowerRegistration {
public:
    PowerRegistration()
    {
        user32_ = LoadLibraryW(L"user32");
        if (!user32_) return;
        using RegFn = HANDLE(WINAPI *)(HANDLE, DWORD);
        auto reg = reinterpret_cast<RegFn>(
            reinterpret_cast<void (*)()>(GetProcAddress(user32_, "RegisterSuspendResumeNotification")));
        if (!reg) return;
        params_.callback = powerCallback;
        params_.context = nullptr;
        handle_ = reg(reinterpret_cast<HANDLE>(&params_), DEVICE_NOTIFY_CALLBACK);
    }
    ~PowerRegistration()
    {
        if (user32_ && handle_) {
            using UnregFn = BOOL(WINAPI *)(HANDLE);
            auto unreg = reinterpret_cast<UnregFn>(
                reinterpret_cast<void (*)()>(GetProcAddress(user32_, "UnregisterSuspendResumeNotification")));
            if (unreg) unreg(handle_);
        }
        if (user32_) FreeLibrary(user32_);
    }
    PowerRegistration(const PowerRegistration &) = delete;
    PowerRegistration &operator=(const PowerRegistration &) = delete;

private:
    HMODULE user32_ = nullptr;
    HANDLE handle_ = nullptr;
    NotifySubscribeParams params_{};
};

int runBody()
{
    PowerRegistration power;
    FlagStop stop;
    return (*g_body)(stop);
}

void WINAPI serviceMain(DWORD, LPWSTR *)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(windows::kServiceTableName, ctrlHandler, nullptr);
    if (!g_statusHandle) return;
    setStatus(SERVICE_START_PENDING, 0, NO_ERROR);
    setStatus(SERVICE_RUNNING, SERVICE_ACCEPT_STOP, NO_ERROR);
    g_exitCode = runBody();
    // The service always stops with exit code 0, whatever the worker returned.
    setStatus(SERVICE_STOPPED, 0, NO_ERROR);
}

BOOL WINAPI consoleHandler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop = true;
        return TRUE;
    default:
        return FALSE;
    }
}

bool consoleRequested()
{
    for (int i = 1; i < __argc; ++i) {
        const char *a = __argv ? __argv[i] : nullptr;
        if (a && (std::strcmp(a, "--console") == 0 || std::strcmp(a, "-console") == 0 ||
                  std::strcmp(a, "/console") == 0))
            return true;
    }
    return false;
}
} // namespace

namespace windows {
void setPowerEventHandler(std::function<void(bool suspend)> handler)
{
    std::lock_guard<std::mutex> lk(g_powerMutex);
    g_powerHandler = std::move(handler);
}

bool runningAsService() { return g_asService.load(); }
} // namespace windows

int runServiceHost(const std::function<int(const StopSignal &)> &body)
{
    g_body = &body;
    g_stop = false;
    if (consoleRequested()) {
        SetConsoleCtrlHandler(consoleHandler, TRUE);
        const int rc = runBody();
        SetConsoleCtrlHandler(consoleHandler, FALSE);
        return rc;
    }
    // The table name is writable in the API's signature.
    static wchar_t name[sizeof windows::kServiceTableName / sizeof(wchar_t)];
    std::memcpy(name, windows::kServiceTableName, sizeof name);
    SERVICE_TABLE_ENTRYW table[] = {{name, serviceMain}, {nullptr, nullptr}};
    g_asService = true;
    if (!StartServiceCtrlDispatcherW(table)) {
        g_asService = false;
        return static_cast<int>(GetLastError());
    }
    return g_exitCode;
}

std::string executableDirectory()
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
        if (n == 0) return std::string();
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    const std::size_t slash = buf.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::string();
    return win::toUtf8(buf.substr(0, slash).c_str());
}

} // namespace platform
} // namespace sdrplay
