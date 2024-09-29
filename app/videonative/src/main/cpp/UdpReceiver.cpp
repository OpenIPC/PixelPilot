//
// Created by gaeta on 2024-04-01.
//


#include "UdpReceiver.h"
#include <arpa/inet.h>
#include <utility>
#include <vector>
#include <sstream>
#include <array>

#include "AndroidThreadPrioValues.hpp"
#include "helper/NDKThreadHelper.hpp"
#include "helper/AndroidLogger.hpp"
#include "helper/StringHelper.hpp"

UDPReceiver::UDPReceiver(JavaVM *javaVm, int port, std::string name, int CPUPriority,
                         DATA_CALLBACK onDataReceivedCallback, size_t WANTED_RCVBUF_SIZE) :
        mPort(port), mName(std::move(name)), WANTED_RCVBUF_SIZE(WANTED_RCVBUF_SIZE),
        mCPUPriority(CPUPriority), onDataReceivedCallback(std::move(onDataReceivedCallback)),
        javaVm(javaVm) {
}

void UDPReceiver::registerOnSourceIPFound(SOURCE_IP_CALLBACK onSourceIP1) {
    this->onSourceIP = std::move(onSourceIP1);
}

long UDPReceiver::getNReceivedBytes() const {
    return nReceivedBytes;
}

std::string UDPReceiver::getSourceIPAddress() const {
    return senderIP;
}

void UDPReceiver::startReceiving() {
    receiving = true;
    mUDPReceiverThread = std::make_unique<std::thread>([this] { this->receiveFromUDPLoop(); });
#ifdef __ANDROID__
    NDKThreadHelper::setName(mUDPReceiverThread->native_handle(), mName.c_str());
#endif
}

void UDPReceiver::stopReceiving() {
    receiving = false;
    //this stops the recvfrom even if in blocking mode
    shutdown(mSocket, SHUT_RD);
    if (mUDPReceiverThread->joinable()) {
        mUDPReceiverThread->join();
    }
    mUDPReceiverThread.reset();
}

void UDPReceiver::receiveFromUDPLoop() {
    mSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (mSocket == -1) {
        MLOGD << "Error creating socket";
        return;
    }
    int enable = 1;
    if (setsockopt(mSocket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        MLOGD << "Error setting reuse";
    }
    int recvBufferSize = 0;
    socklen_t len = sizeof(recvBufferSize);
    getsockopt(mSocket, SOL_SOCKET, SO_RCVBUF, &recvBufferSize, &len);
    MLOGD << "Default socket recv buffer is " << StringHelper::memorySizeReadable(recvBufferSize);

    if (WANTED_RCVBUF_SIZE > recvBufferSize) {
        recvBufferSize = WANTED_RCVBUF_SIZE;
        if (setsockopt(mSocket, SOL_SOCKET, SO_RCVBUF, &WANTED_RCVBUF_SIZE, len)) {
            MLOGD << "Cannot increase buffer size to "
                  << StringHelper::memorySizeReadable(WANTED_RCVBUF_SIZE);
        }
        getsockopt(mSocket, SOL_SOCKET, SO_RCVBUF, &recvBufferSize, &len);
        MLOGD << "Wanted " << StringHelper::memorySizeReadable(WANTED_RCVBUF_SIZE) << " Set "
              << StringHelper::memorySizeReadable(recvBufferSize);
    }
    if (javaVm != nullptr) {
#ifdef __ANDROID__
        NDKThreadHelper::setProcessThreadPriorityAttachDetach(javaVm, mCPUPriority, mName.c_str());
#endif
    }
    struct sockaddr_in myaddr;
    memset((uint8_t *) &myaddr, 0, sizeof(myaddr));
    myaddr.sin_family = AF_INET;
    myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    myaddr.sin_port = htons(mPort);
    if (bind(mSocket, (struct sockaddr *) &myaddr, sizeof(myaddr)) == -1) {
        MLOGE << "Error binding Port; " << mPort;
        return;
    }
    //wrap into unique pointer to avoid running out of stack
    const auto buff = std::make_unique<std::array<uint8_t, UDP_PACKET_MAX_SIZE>>();

    MLOGD << "Listening on " << INADDR_ANY << ":" << mPort;

    sockaddr_in source;
    socklen_t sourceLen = sizeof(sockaddr_in);

    while (receiving) {
        //TODO investigate: does a big buffer size create latency with MSG_WAITALL ?
        //I do not think so. recvfrom should return as soon as new data arrived,not when the buffer is full
        //But with a bigger buffer we do not loose packets when the receiver thread cannot keep up for a short amount of time
        // MSG_WAITALL does not wait until we have __n data, but a new UDP packet (that can be smaller than __n)
        const ssize_t message_length = recvfrom(mSocket, buff->data(), UDP_PACKET_MAX_SIZE, 0,
                                                (sockaddr *) &source, &sourceLen);
        //ssize_t message_length = recv(mSocket, buff, (size_t) mBuffsize, MSG_WAITALL);
        if (message_length > 0) { //else -1 was returned;timeout/No data received
            onDataReceivedCallback(buff->data(), (size_t) message_length);

            nReceivedBytes += message_length;
            //The source ip stuff
            const char *p = inet_ntoa(source.sin_addr);
            std::string s1 = std::string(p);
            //MLOGE << "received " << message_length <<  " bytes from " << s1;
            if (senderIP != s1) {
                senderIP = s1;
            }
            if (onSourceIP != nullptr) {
                onSourceIP(p);
            }
        } else {
            if (errno != EWOULDBLOCK) {
                MLOGE << "Error on recvfrom. errno=" << errno << " " << strerror(errno);
            }
        }
    }
    close(mSocket);
}

int UDPReceiver::getPort() const {
    return mPort;
}

