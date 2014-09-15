#ifndef CONTEXT_HPP
#define CONTEXT_HPP

#include "sfp/serial_framing_protocol.h"
#include "util/callback.hpp"

#ifdef SFP_CONFIG_DEBUG
#include <string>
#endif

#ifdef SFP_CONFIG_THREADSAFE
#include <thread>
#include <mutex>
#endif

namespace sfp {

class Context {
public:
#ifdef SFP_CONFIG_DEBUG
    explicit Context (std::string debugName = { })
            : mDebugName(debugName) {
#else
    Context () {
#endif
        initialize();
    }

    void initialize () {
        sfpInit(&mContext);
        sfpSetDeliverCallback(&mContext, staticDeliver, this);
        sfpSetWriteCallback(&mContext, SFP_WRITE_ONE, (void*)staticWrite, this);

#ifdef SFP_CONFIG_THREADSAFE
        sfpSetLockCallback(&mContext, staticLock, this);
        sfpSetUnlockCallback(&mContext, staticUnlock, this);
#endif

#ifdef SFP_CONFIG_DEBUG
        sfpSetDebugName(&mContext, mDebugName.c_str());
#endif
    }

    // incoming octet slot
    void input (uint8_t octet) {
        auto rc = sfpDeliverOctet(&mContext, octet, nullptr, 0, nullptr);
        assert(-1 != rc);
    }

    // outgoing octet signal
    util::Signal<void(uint8_t)> output;

    // outgoing message slot
    void sendMessage (const uint8_t* buf, size_t len) {
        size_t outlen;
        sfpWritePacket(&mContext, buf, len, &outlen);
    }

    // incoming message signal
    util::Signal<void(const uint8_t*,size_t)> messageReceived;

    void connect () {
        sfpConnect(&mContext);
    }

    bool isConnected () {
        return sfpIsConnected(&mContext);
    }

private:
    static void staticDeliver (uint8_t* buf, size_t len, void* data) {
        static_cast<Context*>(data)->messageReceived(buf, len);
    }

    static int staticWrite (uint8_t octet, size_t* outlen, void* data) {
        if (outlen) { *outlen = 1; }
        static_cast<Context*>(data)->output(octet);
        return 0;
    }

#ifdef SFP_CONFIG_THREADSAFE
    static void staticLock (void* data) {
        static_cast<Context*>(data)->mTransmitterMutex.lock();
    }

    static void staticUnlock (void* data) {
        static_cast<Context*>(data)->mTransmitterMutex.unlock();
    }
#endif

    SFPcontext mContext;
#ifdef SFP_CONFIG_THREADSAFE
    std::mutex mTransmitterMutex;
#endif

#ifdef SFP_CONFIG_DEBUG
    std::string mDebugName;
#endif
};

} // namespace sfp

#endif