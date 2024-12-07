//
// Created by gaeta on 2024-04-01.
//

#ifndef FPVUE_UDPRECEIVER_H
#define FPVUE_UDPRECEIVER_H

#include <jni.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <thread>
// Starts a new thread that continuously checks for new data on UDP port

class UDPReceiver
{
  public:
    typedef std::function<void(const uint8_t[], size_t)> DATA_CALLBACK;
    typedef std::function<void(const std::string)>       SOURCE_IP_CALLBACK;

  public:
    /**
     * @param javaVm used to set thread priority (attach and then detach) for android,
       nullptr when priority doesn't matter/not using android
     * @param port : The port to listen on
     * @param CPUPriority: The priority the receiver thread will run with if javaVm!=nullptr
     * @param onDataReceivedCallback: called every time new data is received
     * @param WANTED_RCVBUF_SIZE: The buffer allocated by the OS might not be sufficient to buffer incoming data when
     receiving at a high data rate
     * If @param WANTED_RCVBUF_SIZE is bigger than the size allocated by the OS a bigger buffer is requested, but it is
     not
     * guaranteed that the size is actually increased. Use 0 to leave the buffer size untouched
     */
    UDPReceiver(
        JavaVM*       javaVm,
        int           port,
        std::string   name,
        int           CPUPriority,
        DATA_CALLBACK onDataReceivedCallback,
        size_t        WANTED_RCVBUF_SIZE = 0);

    /**
     * Register a callback that is called once and contains the IP address of the first received packet's sender
     */
    void registerOnSourceIPFound(SOURCE_IP_CALLBACK onSourceIP1);

    /**
     * Start receiver thread,which opens UDP port
     */
    void startReceiving();

    /**
     * Stop and join receiver thread, which closes port
     */
    void stopReceiving();

    // Get function(s) for private member variables
    long getNReceivedBytes() const;

    std::string getSourceIPAddress() const;

    int getPort() const;

  private:
    void receiveFromUDPLoop();

    const DATA_CALLBACK onDataReceivedCallback = nullptr;
    SOURCE_IP_CALLBACK  onSourceIP             = nullptr;
    const int           mPort;
    const int           mCPUPriority;
    // Hmm....
    const size_t      WANTED_RCVBUF_SIZE;
    const std::string mName;
    /// We need this reference to stop the receiving thread
    int                          mSocket        = 0;
    std::string                  senderIP       = "0.0.0.0";
    std::atomic<bool>            receiving      = false;
    std::atomic<long>            nReceivedBytes = 0;
    std::unique_ptr<std::thread> mUDPReceiverThread;
    // https://en.wikipedia.org/wiki/User_Datagram_Protocol
    // 65,507 bytes (65,535 − 8 byte UDP header − 20 byte IP header).
    static constexpr const size_t UDP_PACKET_MAX_SIZE = 65507;
    JavaVM*                       javaVm;
};

#endif  // FPVUE_UDPRECEIVER_H
