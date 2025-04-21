#include "UdsReceiver.h"

#include <array>
#include <cstring>
#include "helper/AndroidLogger.hpp"
#include "helper/NDKThreadHelper.hpp"
#include "helper/StringHelper.hpp"

namespace
{
constexpr size_t MAX_PKT = 3700;  // safe MTU‑sized buffer
}

UDSReceiver::UDSReceiver(JavaVM* jvm, std::string path, std::string name, int prio, DATA_CALLBACK cb, size_t wanted)
    : mSocketPath(std::move(path)),
      mName(std::move(name)),
      WANTED_RCVBUF_SIZE(wanted),
      mCPUPriority(prio),
      onData(std::move(cb)),
      javaVm(jvm)
{
}

void UDSReceiver::startReceiving()
{
    receiving = true;
    mThread   = std::make_unique<std::thread>([this] { receiveLoop(); });
#ifdef __ANDROID__
    NDKThreadHelper::setName(mThread->native_handle(), mName.c_str());
#endif
}

void UDSReceiver::stopReceiving()
{
    receiving = false;
    if (mSocket != -1) shutdown(mSocket, SHUT_RD);
    if (mThread && mThread->joinable()) mThread->join();
    mThread.reset();
}

void UDSReceiver::receiveLoop()
{
    mSocket = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (mSocket == -1)
    {
        MLOGE << "socket(AF_UNIX) failed: " << strerror(errno);
        return;
    }

    // upscale recv buf (same logic as UDP)
    int       cur = 0;
    socklen_t len = sizeof(cur);
    getsockopt(mSocket, SOL_SOCKET, SO_RCVBUF, &cur, &len);
    if (WANTED_RCVBUF_SIZE > static_cast<size_t>(cur))
    {
        setsockopt(mSocket, SOL_SOCKET, SO_RCVBUF, &WANTED_RCVBUF_SIZE, len);
        getsockopt(mSocket, SOL_SOCKET, SO_RCVBUF, &cur, &len);
        MLOGD << "UDS recvbuf set to " << StringHelper::memorySizeReadable(cur);
    }

    // ---- bind to either abstract or filesystem Unix‑domain socket ----
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    socklen_t addrlen;

    if (!mSocketPath.empty() && mSocketPath[0] == '\0')
    {
        // abstract namespace: copy full blob (leading '\0' + name)
        memcpy(addr.sun_path, mSocketPath.data(), mSocketPath.size());
        // length = offsetof + full name length
        addrlen = offsetof(sockaddr_un, sun_path) + static_cast<socklen_t>(mSocketPath.size());
    }
    else
    {
        // filesystem socket: unlink old, strncpy, bind whole struct
        std::strncpy(addr.sun_path, mSocketPath.c_str(), sizeof(addr.sun_path) - 1);
        unlink(mSocketPath.c_str());
        addrlen = sizeof(addr);
    }

    if (bind(mSocket, reinterpret_cast<sockaddr*>(&addr), addrlen) == -1)
    {
        // show "@abstract:name" if abstract, else full path
        std::string what = (mSocketPath[0] == '\0') ? "@abstract:" + mSocketPath.substr(1) : mSocketPath;
        MLOGE << "bind(" << what << ") failed: " << strerror(errno);
        close(mSocket);
        mSocket = -1;
        return;
    }

#ifdef __ANDROID__
    if (javaVm) NDKThreadHelper::setProcessThreadPriorityAttachDetach(javaVm, mCPUPriority, mName.c_str());
#endif

    const auto buf = std::make_unique<std::array<uint8_t, MAX_PKT>>();
    MLOGD << "UDS listening on '" << mSocketPath << '\'';

    sockaddr_un peer{};
    socklen_t   peerLen = sizeof(peer);

    while (receiving)
    {
        ssize_t n = recvfrom(mSocket, buf->data(), buf->size(), 0, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (n > 0)
        {
            onData(buf->data(), static_cast<size_t>(n));
            nReceivedBytes += n;

            if (peer.sun_path[0] != '\0')
            {
                std::string p(peer.sun_path);
                if (p != senderPath)
                {
                    senderPath = p;
                    if (onSource) onSource(senderPath.c_str());
                }
            }
        }
        else if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            MLOGE << "recvfrom error: " << strerror(errno);
        }
    }

    close(mSocket);
    unlink(mSocketPath.c_str());
    mSocket = -1;
}
