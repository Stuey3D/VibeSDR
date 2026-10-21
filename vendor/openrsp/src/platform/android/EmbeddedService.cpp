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

// Android: the service core (sdrplay_apiService worker) embedded in the app process.
// There is no daemon on Android; sdrplay_android_service_start() does what
// src/service/main.cpp does on Linux, on a thread of the app, and the client half of
// libsdrplay_api reaches it through the in-process named objects (AndroidShm.h).
#include "sdrplay_android.h"
#include "AndroidShm.h"

#include "platform/LogSink.h"
#include "platform/Thread.h"
#include "platform/Usb.h"
#include "service/Firmware.h"
#include "service/DeviceChannel.h"
#include "service/ServiceCore.h"

#include "core/dsp/StageTimes.h"
#include "core/ipc/Layouts.h"
#include "core/ipc/Names.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace sdrplay;

struct Embedded {
    std::mutex m; // serialises start/stop
    std::unique_ptr<platform::LogSink> sink;
    std::unique_ptr<platform::UsbEnumerator> usb;
    std::unique_ptr<service::ServiceCore> core;
    std::unique_ptr<platform::Thread> worker;
};

Embedded &embedded()
{
    static Embedded *e = new Embedded(); // never destroyed: apps are killed, not exited
    return *e;
}

void teardown(Embedded &e)
{
    e.worker.reset();
    e.core.reset(); // every device handle goes before libusb_exit
    e.usb.reset();
    e.sink.reset();
}
} // namespace

extern "C" SDRPLAY_ANDROID_API int sdrplay_android_service_start(const char *firmwareDir)
{
    Embedded &e = embedded();
    std::lock_guard<std::mutex> lk(e.m);
    if (e.core) return 0;
    if (platform::android_shm::remoteResolverSet()) return -1; // attached to OpenRSP (service mode)
    e.sink = platform::createSystemLogSink();
    e.usb = platform::createUsbEnumerator(); // null = no USB: GetDevices reports 0 devices
    service::ServiceConfig cfg;
    cfg.logSink = e.sink.get();
    cfg.usb = e.usb.get();
    cfg.firmwareDir = (firmwareDir && *firmwareDir) ? firmwareDir : service::kDefaultFirmwareDir;
    service::setForceBulkTransfers(true); // phones drop isochronous packets
    e.core = std::make_unique<service::ServiceCore>(cfg);
    if (!e.core->start()) {
        teardown(e);
        return -1;
    }
    service::ServiceCore *core = e.core.get();
    e.worker = platform::Thread::start([core] { core->run(nullptr); }, "sdrSrvWorker");
    if (!e.worker) {
        core->requestStop();
        core->run(nullptr); // normal shutdown path, on this thread
        teardown(e);
        return -1;
    }
    return 0;
}

extern "C" SDRPLAY_ANDROID_API int sdrplay_android_service_stop(void)
{
    Embedded &e = embedded();
    std::lock_guard<std::mutex> lk(e.m);
    if (!e.core) return -1;
    e.core->requestStop();
    e.worker->join();
    teardown(e);
    return 0;
}

extern "C" SDRPLAY_ANDROID_API int sdrplay_android_service_running(void)
{
    Embedded &e = embedded();
    std::lock_guard<std::mutex> lk(e.m);
    return e.core ? 1 : 0;
}

// ------------------------------------------------------------------ status (read-only)
// sdrplay_android_status_json: a read-only snapshot of the embedded service for the OpenRSP
// app's Status panel. Android-only (this file is compiled only by the Android build); it
// reads the service's device table under its own lock (try-lock, never blocks the service
// for long) and the per-device shared-memory objects (CmdMap = the client-visible device
// params, DataMap = the stream ring headers) through read-only mappings. Nothing is written
// and no client <-> service message is sent.
namespace {

void jappend(std::string &out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void jappend(std::string &out, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) out.append(buf, static_cast<std::size_t>(n) < sizeof buf ? static_cast<std::size_t>(n) : sizeof buf - 1);
}

std::string jstr(const char *s, std::size_t max)
{
    std::string r = "\"";
    for (std::size_t i = 0; i < max && s[i]; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\') {
            r += '\\';
            r += static_cast<char>(c);
        } else if (c < 0x20 || c >= 0x7f) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            r += b;
        } else {
            r += static_cast<char>(c);
        }
    }
    return r + "\"";
}

// Read-only private copy of the first `size` bytes of a registry object; false if absent.
bool readObject(const std::string &name, void *out, std::size_t size)
{
    int err = 0;
    const int fd = sdrplay::platform::android_shm::registryOpen(name, size, &err);
    if (fd < 0) return false;
    void *p = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    std::memcpy(out, p, size);
    munmap(p, size);
    return true;
}

struct SlotCopy {
    bool used = false, selected = false;
    char serNo[64] = {};
    unsigned hwVer = 0, pid = 0, rev = 0, valid = 0, tuner = 0, rspDuoMode = 0, bus = 0, port = 0;
};

void appendDevice(std::string &j, unsigned slot, const SlotCopy &s)
{
    namespace ipc = sdrplay::ipc;
    const std::string prefix = ipc::devicePrefix(slot);
    jappend(j, "{\"slot\":%u,\"serial\":%s,\"hwVer\":%u,\"pid\":%u,\"rev\":%u,\"valid\":%u,\"tuner\":%u,"
               "\"rspDuoMode\":%u,\"bus\":%u,\"port\":%u,\"selected\":%s,\"prefix\":%s",
            slot, jstr(s.serNo, sizeof s.serNo).c_str(), s.hwVer, s.pid, s.rev, s.valid, s.tuner, s.rspDuoMode, s.bus,
            s.port, s.selected ? "true" : "false", jstr(prefix.c_str(), prefix.size()).c_str());
    static ipc::CmdMap cmd; // guarded by the caller's status mutex (2 KB, off the stack)
    if (s.selected &&
        readObject(ipc::deviceObjectName(prefix, ipc::Side::Master, ipc::Channel::Cmd, ipc::Kind::Map), &cmd,
                   sizeof cmd)) {
        const bool chB = s.hwVer == SDRPLAY_RSPduo_ID && s.tuner == sdrplay_api_Tuner_B;
        const sdrplay_api_RxChannelParamsT &rx = chB ? cmd.rxChannelB : cmd.rxChannelA;
        const sdrplay_api_DevParamsT &dp = cmd.devParams;
        const sdrplay_api_TunerParamsT &tp = rx.tunerParams;
        const sdrplay_api_ControlParamsT &cp = rx.ctrlParams;
        unsigned biasT = 0, rfNotch = 0, dabNotch = 0, amNotch = 0;
        switch (s.hwVer) {
        case SDRPLAY_RSP1A_ID:
        case SDRPLAY_RSP1B_ID:
            biasT = rx.rsp1aTunerParams.biasTEnable;
            rfNotch = dp.rsp1aParams.rfNotchEnable;
            dabNotch = dp.rsp1aParams.rfDabNotchEnable;
            break;
        case SDRPLAY_RSP2_ID:
            biasT = rx.rsp2TunerParams.biasTEnable;
            rfNotch = rx.rsp2TunerParams.rfNotchEnable;
            break;
        case SDRPLAY_RSPduo_ID:
            biasT = rx.rspDuoTunerParams.biasTEnable;
            rfNotch = rx.rspDuoTunerParams.rfNotchEnable;
            dabNotch = rx.rspDuoTunerParams.rfDabNotchEnable;
            amNotch = rx.rspDuoTunerParams.tuner1AmNotchEnable;
            break;
        case SDRPLAY_RSPdx_ID:
        case SDRPLAY_RSPdxR2_ID:
            biasT = dp.rspDxParams.biasTEnable;
            rfNotch = dp.rspDxParams.rfNotchEnable;
            dabNotch = dp.rspDxParams.rfDabNotchEnable;
            break;
        default:
            break;
        }
        jappend(j, ",\"params\":{\"channel\":\"%s\",\"fsHz\":%.1f,\"ppm\":%.3f,\"transferMode\":%u,"
                   "\"decimationEnable\":%u,\"decimationFactor\":%u,\"ifType\":%d,\"bwType\":%d,\"loMode\":%d,"
                   "\"rfHz\":%.1f,\"lnaState\":%u,\"gRdB\":%d,\"minGr\":%d,\"gainCurr\":%.2f,\"agc\":%d,"
                   "\"agcSetPoint\":%d,\"dcEnable\":%u,\"iqEnable\":%u,\"biasT\":%u,\"rfNotch\":%u,\"dabNotch\":%u,"
                   "\"amNotch\":%u}",
                chB ? "B" : "A", dp.fsFreq.fsHz, dp.ppm, static_cast<unsigned>(dp.mode),
                static_cast<unsigned>(cp.decimation.enable), static_cast<unsigned>(cp.decimation.decimationFactor),
                static_cast<int>(tp.ifType), static_cast<int>(tp.bwType), static_cast<int>(tp.loMode), tp.rfFreq.rfHz,
                static_cast<unsigned>(tp.gain.LNAstate), tp.gain.gRdB, static_cast<int>(tp.gain.minGr),
                static_cast<double>(tp.gain.gainVals.curr), static_cast<int>(cp.agc.enable), cp.agc.setPoint_dBfs,
                static_cast<unsigned>(cp.dcOffset.DCenable), static_cast<unsigned>(cp.dcOffset.IQenable), biasT,
                rfNotch, dabNotch, amNotch);
    }
    // Stream ring: only each ipc::DataSlot's bufIdx and paramsA are read (the newest slot wins).
    if (s.selected) {
        int err = 0;
        const std::string dn = ipc::deviceObjectName(prefix, ipc::Side::Master, ipc::Channel::Data, ipc::Kind::Map);
        const int fd = sdrplay::platform::android_shm::registryOpen(dn, sizeof(ipc::DataMap), &err);
        if (fd >= 0) {
            void *p = mmap(nullptr, sizeof(ipc::DataMap), PROT_READ, MAP_SHARED, fd, 0);
            close(fd);
            if (p != MAP_FAILED) {
                const auto *m = static_cast<const ipc::DataMap *>(p);
                std::uint32_t best = 0;
                sdrplay_api_StreamCbParamsT last{};
                for (unsigned k = 0; k < ipc::kDataRingSlots; ++k) {
                    std::uint32_t b;
                    std::memcpy(&b, &m->slot[k].bufIdx, sizeof b);
                    if (b >= best && b != 0) {
                        best = b;
                        std::memcpy(&last, &m->slot[k].paramsA, sizeof last);
                    }
                }
                munmap(p, sizeof(ipc::DataMap));
                jappend(j, ",\"stream\":{\"bufIdx\":%u,\"firstSampleNum\":%u,\"numSamples\":%u}", best,
                        last.firstSampleNum, last.numSamples);
            }
        }
    }
    j += '}';
}

} // namespace

extern "C" SDRPLAY_ANDROID_API int sdrplay_android_status_json(char *buf, int size)
{
    static std::mutex statusMtx;
    std::lock_guard<std::mutex> slk(statusMtx);
    std::string j = "{";
    Embedded &e = embedded();
    std::unique_lock<std::mutex> lk(e.m);
    if (!e.core) {
        j += "\"running\":false,\"devices\":[]}";
    } else {
        service::Enumerator &en = e.core->enumerator();
        SlotCopy slots[ipc::kMaxDeviceSlots];
        bool locked = false;
        for (int tries = 0; tries < 10 && !locked; ++tries) { // never wait on a firmware upload
            locked = en.mutex().try_lock();
            if (!locked) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        int numDevices = -1;
        if (locked) {
            numDevices = en.numDevices();
            for (unsigned i = 0; i < ipc::kMaxDeviceSlots; ++i) {
                const service::DeviceSlot &s = en.slot(static_cast<int>(i));
                SlotCopy &c = slots[i];
                c.used = !s.rec.empty();
                c.selected = static_cast<bool>(s.dev);
                if (!c.used && !c.selected) continue;
                std::memcpy(c.serNo, s.rec.serNo, sizeof c.serNo);
                c.serNo[sizeof c.serNo - 1] = '\0';
                c.hwVer = s.rec.hwVer;
                c.pid = s.rec.pid;
                c.rev = s.rec.rev;
                c.valid = s.rec.valid;
                c.tuner = s.rec.tuner;
                c.rspDuoMode = s.rec.rspDuoMode;
                c.bus = s.rec.bus;
                c.port = s.rec.port;
            }
            en.mutex().unlock();
        }
        jappend(j, "\"running\":true,\"tableBusy\":%s,\"numDevices\":%d,", locked ? "false" : "true", numDevices);
        // stream-path stage totals (ns since start, all streams); reading them keeps the timing armed
        const dsp::stages::Snapshot st = dsp::stages::read();
        j += "\"stages\":{";
        for (unsigned i = 0; i < dsp::stages::kCount; ++i)
            jappend(j, "\"%s\":%llu,", dsp::stages::key(static_cast<dsp::stages::Stage>(i)),
                    static_cast<unsigned long long>(st.ns[i]));
        jappend(j, "\"blocks\":%llu},\"devices\":[", static_cast<unsigned long long>(st.blocks));
        bool first = true;
        for (unsigned i = 0; i < ipc::kMaxDeviceSlots; ++i) {
            if (!slots[i].used && !slots[i].selected) continue;
            if (!first) j += ',';
            first = false;
            appendDevice(j, i, slots[i]);
        }
        j += "]}";
    }
    lk.unlock();
    const int need = static_cast<int>(j.size()) + 1;
    if (buf && size > 0) {
        const std::size_t n = static_cast<std::size_t>(need <= size ? need - 1 : size - 1);
        std::memcpy(buf, j.data(), n);
        buf[n] = '\0';
    }
    return need;
}
