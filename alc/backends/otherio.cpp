/**
 * OpenAL cross platform audio library
 * Copyright (C) 2024 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include "otherio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <memory.h>

#include <wtypes.h>
#include <cguid.h>
#include <devpropdef.h>
#include <mmreg.h>
#include <propsys.h>
#include <propkey.h>
#include <devpkey.h>
#ifndef _WAVEFORMATEXTENSIBLE_
#include <ks.h>
#include <ksmedia.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "alstring.h"
#include "althrd_setname.h"
#include "comptr.h"
#include "core/converter.h"
#include "core/device.h"
#include "core/helpers.h"
#include "core/logging.h"

namespace {

using namespace std::string_view_literals;
using std::chrono::nanoseconds;
using std::chrono::milliseconds;
using std::chrono::seconds;


enum class MsgType {
    OpenDevice,
    ResetDevice,
    StartDevice,
    StopDevice,
    CloseDevice,

    QuitThread
};

constexpr const char *GetMessageTypeName(MsgType type) noexcept
{
    switch(type)
    {
    case MsgType::OpenDevice: return "Open Device";
    case MsgType::ResetDevice: return "Reset Device";
    case MsgType::StartDevice: return "Start Device";
    case MsgType::StopDevice: return "Stop Device";
    case MsgType::CloseDevice: return "Close Device";
    case MsgType::QuitThread: break;
    }
    return "";
}


/* Proxy interface used by the message handler, to ensure COM objects are used
 * on a thread where COM is initialized.
 */
struct OtherIOProxy {
    OtherIOProxy() = default;
    OtherIOProxy(const OtherIOProxy&) = delete;
    OtherIOProxy(OtherIOProxy&&) = delete;
    virtual ~OtherIOProxy() = default;

    void operator=(const OtherIOProxy&) = delete;
    void operator=(OtherIOProxy&&) = delete;

    virtual HRESULT openProxy(std::string_view name) = 0;
    virtual void closeProxy() = 0;

    virtual HRESULT resetProxy() = 0;
    virtual HRESULT startProxy() = 0;
    virtual void stopProxy() = 0;

    struct Msg {
        MsgType mType;
        OtherIOProxy *mProxy;
        std::string_view mParam;
        std::promise<HRESULT> mPromise;

        explicit operator bool() const noexcept { return mType != MsgType::QuitThread; }
    };
    static inline std::deque<Msg> mMsgQueue;
    static inline std::mutex mMsgQueueLock;
    static inline std::condition_variable mMsgQueueCond;

    auto pushMessage(MsgType type, std::string_view param) -> std::future<HRESULT>
    {
        auto promise = std::promise<HRESULT>{};
        auto future = std::future<HRESULT>{promise.get_future()};
        {
            auto msglock = std::lock_guard{mMsgQueueLock};
            mMsgQueue.emplace_back(Msg{type, this, param, std::move(promise)});
        }
        mMsgQueueCond.notify_one();
        return future;
    }

    static auto popMessage() -> Msg
    {
        auto lock = std::unique_lock{mMsgQueueLock};
        mMsgQueueCond.wait(lock, []{return !mMsgQueue.empty();});
        auto msg = Msg{std::move(mMsgQueue.front())};
        mMsgQueue.pop_front();
        return msg;
    }

    static void messageHandler(std::promise<HRESULT> *promise);
};

void OtherIOProxy::messageHandler(std::promise<HRESULT> *promise)
{
    TRACE("Starting COM message thread\n");

    auto com = ComWrapper{COINIT_MULTITHREADED};
    if(!com)
    {
        WARN("Failed to initialize COM: 0x%08lx\n", com.status());
        promise->set_value(com.status());
        return;
    }

    auto hr = HRESULT{S_OK};
    promise->set_value(hr);
    promise = nullptr;

    TRACE("Starting message loop\n");
    while(Msg msg{popMessage()})
    {
        TRACE("Got message \"%s\" (0x%04x, this=%p, param=\"%.*s\")\n",
            GetMessageTypeName(msg.mType), static_cast<uint>(msg.mType),
            static_cast<void*>(msg.mProxy), al::sizei(msg.mParam), msg.mParam.data());

        switch(msg.mType)
        {
        case MsgType::OpenDevice:
            hr = msg.mProxy->openProxy(msg.mParam);
            msg.mPromise.set_value(hr);
            continue;

        case MsgType::ResetDevice:
            hr = msg.mProxy->resetProxy();
            msg.mPromise.set_value(hr);
            continue;

        case MsgType::StartDevice:
            hr = msg.mProxy->startProxy();
            msg.mPromise.set_value(hr);
            continue;

        case MsgType::StopDevice:
            msg.mProxy->stopProxy();
            msg.mPromise.set_value(S_OK);
            continue;

        case MsgType::CloseDevice:
            msg.mProxy->closeProxy();
            msg.mPromise.set_value(S_OK);
            continue;

        case MsgType::QuitThread:
            break;
        }
        ERR("Unexpected message: %u\n", static_cast<uint>(msg.mType));
        msg.mPromise.set_value(E_FAIL);
    }
    TRACE("Message loop finished\n");
}


struct OtherIOPlayback final : public BackendBase, OtherIOProxy {
    OtherIOPlayback(DeviceBase *device) noexcept : BackendBase{device} { }
    ~OtherIOPlayback() final;

    void mixerProc();

    void open(std::string_view name) final;
    auto openProxy(std::string_view name) -> HRESULT final;
    void closeProxy() final;
    auto reset() -> bool final;
    auto resetProxy() -> HRESULT final;
    void start() final;
    auto startProxy() -> HRESULT final;
    void stop() final;
    void stopProxy() final;

    HRESULT mOpenStatus{E_FAIL};

    std::atomic<bool> mKillNow{true};
    std::thread mThread;
};

OtherIOPlayback::~OtherIOPlayback()
{
    if(SUCCEEDED(mOpenStatus))
        pushMessage(MsgType::CloseDevice, {}).wait();
}

void OtherIOPlayback::mixerProc()
{
    const auto restTime = milliseconds{mDevice->UpdateSize*1000/mDevice->Frequency / 2};

    SetRTPriority();
    althrd_setname(GetMixerThreadName());

    auto done = int64_t{0};
    auto start = std::chrono::steady_clock::now();
    while(!mKillNow.load(std::memory_order_acquire)
        && mDevice->Connected.load(std::memory_order_acquire))
    {
        auto now = std::chrono::steady_clock::now();

        /* This converts from nanoseconds to nanosamples, then to samples. */
        auto avail = int64_t{std::chrono::duration_cast<seconds>((now-start) * mDevice->Frequency).count()};
        if(avail-done < mDevice->UpdateSize)
        {
            std::this_thread::sleep_for(restTime);
            continue;
        }
        while(avail-done >= mDevice->UpdateSize)
        {
            mDevice->renderSamples(nullptr, mDevice->UpdateSize, 0u);
            done += mDevice->UpdateSize;
        }

        if(done >= mDevice->Frequency)
        {
            auto s = seconds{done/mDevice->Frequency};
            start += s;
            done -= mDevice->Frequency*s.count();
        }
    }
}


void OtherIOPlayback::open(std::string_view name)
{
    if(name.empty())
        name = "OtherIO"sv;
    else if(name != "OtherIO"sv)
        throw al::backend_exception{al::backend_error::NoDevice, "Device name \"%.*s\" not found",
            al::sizei(name), name.data()};

    mOpenStatus = pushMessage(MsgType::OpenDevice, name).get();
    if(FAILED(mOpenStatus))
        throw al::backend_exception{al::backend_error::DeviceError, "Failed to open \"%.*s\"",
            al::sizei(name), name.data()};

    mDevice->DeviceName = name;
}

auto OtherIOPlayback::openProxy(std::string_view name [[maybe_unused]]) -> HRESULT
{
    return S_OK;
}

void OtherIOPlayback::closeProxy()
{
}

auto OtherIOPlayback::reset() -> bool
{
    return SUCCEEDED(pushMessage(MsgType::ResetDevice, {}).get());
}

auto OtherIOPlayback::resetProxy() -> HRESULT
{
    setDefaultWFXChannelOrder();
    return S_OK;
}

void OtherIOPlayback::start()
{
    auto hr = pushMessage(MsgType::StartDevice, {}).get();
    if(FAILED(hr))
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to start playback: 0x%08lx", hr};
}

auto OtherIOPlayback::startProxy() -> HRESULT
{
    try {
        mKillNow.store(false, std::memory_order_release);
        mThread = std::thread{std::mem_fn(&OtherIOPlayback::mixerProc), this};
        return S_OK;
    }
    catch(std::exception& e) {
        ERR("Failed to start mixing thread: %s", e.what());
    }
    return E_FAIL;
}

void OtherIOPlayback::stop()
{
    pushMessage(MsgType::StopDevice, {}).wait();
}

void OtherIOPlayback::stopProxy()
{
    if(mKillNow.exchange(true, std::memory_order_acq_rel) || !mThread.joinable())
        return;
    mThread.join();
}

} // namespace


auto OtherIOBackendFactory::init() -> bool
{
    static HRESULT InitResult{E_FAIL};
    if(FAILED(InitResult)) try
    {
        auto promise = std::promise<HRESULT>{};
        auto future = promise.get_future();

        std::thread{&OtherIOProxy::messageHandler, &promise}.detach();
        InitResult = future.get();
    }
    catch(...) {
    }

    return SUCCEEDED(InitResult);
}

auto OtherIOBackendFactory::querySupport(BackendType type) -> bool
{ return type == BackendType::Playback; }

auto OtherIOBackendFactory::enumerate(BackendType type) -> std::vector<std::string>
{
    std::vector<std::string> outnames;

    switch(type)
    {
    case BackendType::Playback:
        outnames.emplace_back("OtherIO"sv);
        break;

    case BackendType::Capture:
        break;
    }

    return outnames;
}

auto OtherIOBackendFactory::createBackend(DeviceBase *device, BackendType type) -> BackendPtr
{
    if(type == BackendType::Playback)
        return BackendPtr{new OtherIOPlayback{device}};
    return nullptr;
}

auto OtherIOBackendFactory::getFactory() -> BackendFactory&
{
    static auto factory = OtherIOBackendFactory{};
    return factory;
}

auto OtherIOBackendFactory::queryEventSupport(alc::EventType, BackendType) -> alc::EventSupport
{
    return alc::EventSupport::NoSupport;
}
