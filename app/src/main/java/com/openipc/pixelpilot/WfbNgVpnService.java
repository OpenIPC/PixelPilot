package com.openipc.pixelpilot;

import android.net.VpnService;
import android.content.Intent;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

import android.util.Log;

import java.net.InetAddress;
import java.net.UnknownHostException;

public class WfbNgVpnService extends VpnService {
    private static final String TAG = "WfbNgVpnService";

    // The TUN interface descriptor
    private ParcelFileDescriptor vpnInterface = null;

    // Threads for bidirectional traffic
    private Thread udpToVpnThread;
    private Thread vpnToUdpThread;

    // Control flags
    private volatile boolean isRunning = false;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Log.i(TAG, "VPN Service started");

        // If already running, don't start again
        if (isRunning) {
            Log.w(TAG, "VPN Service is already running");
            return START_STICKY;
        }

        // Build the VPN interface (TUN) and set it up
        try {
            vpnInterface = establishVpnInterface();
            isRunning = true;
        } catch (Exception e) {
            Log.e(TAG, "Failed to establish VPN interface", e);
            stopSelf();
            return START_NOT_STICKY;
        }

        // Start the worker threads
        startVpnThreads(vpnInterface);

        return START_STICKY;
    }

    /**
     * Construct and return the VPN (TUN) interface using VpnService.Builder
     */
    private ParcelFileDescriptor establishVpnInterface() throws Exception {
        // Build a new VPN interface using the VpnService.Builder
        Builder builder = new Builder();

        // Give the interface a human-readable name
        builder.setSession("wfb-ng");

        // Set the interface address to 10.5.0.3/24
        // On many devices, addAddress requires prefix length instead of a netmask
        builder.addAddress("10.5.0.3", 24);

        // Route only 10.5.0.0/24 through this interface
        builder.addRoute("10.5.0.0", 24);


        // You can optionally set DNS servers if needed
        // builder.addDnsServer("8.8.8.8");

        // Build and return the file descriptor for the TUN interface
        ParcelFileDescriptor pfd = builder.establish();
        Log.i(TAG, "VPN interface (wfb-ng) established with IP 10.5.0.3/24");
        return pfd;
    }

    /**
     * Start the bidirectional traffic threads:
     *  1) A thread to read from local UDP port 8000 and inject into VPN
     *  2) A thread to read from VPN and send to local UDP port 8001
     */
    private void startVpnThreads(final ParcelFileDescriptor vpnInterfacePfd) {
        // Prepare input (read from VPN) and output (write to VPN) streams
        final FileInputStream vpnInput = new FileInputStream(vpnInterfacePfd.getFileDescriptor());
        final FileOutputStream vpnOutput = new FileOutputStream(vpnInterfacePfd.getFileDescriptor());

        // 1) Thread to capture rtl8812 → [ local UDP:8000 → TUN ]
        udpToVpnThread = new Thread(new Runnable() {
            @Override
            public void run() {
                Log.i(TAG, "UDP → VPN thread started");
                byte[] buffer = new byte[4024];

                try (DatagramSocket socket = new DatagramSocket(new InetSocketAddress(8000))) {
                    // Bind to local UDP port 8000 on all interfaces
                    socket.setReuseAddress(true);
                    //socket.bind(new InetSocketAddress(8000));

                    while (isRunning) {
                        // Read data from UDP into buffer
                        DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
                        socket.receive(packet);

                        // log packet data and lentgh

                        if (packet.getLength() < 1)
                            continue;

//                        Log.i(TAG, "UDP → VPN: " + packet.getData() + " " + packet.getLength());
//                        IPv4Parser.parseIPv4Header(packet.getData(), packet.getLength());

                        // Write to the VPN interface (TUN)
                        try{
                            vpnOutput.write(packet.getData(), 2, packet.getLength()-2);
                        } catch (IOException e) {
                            Log.e(TAG, "UDP → VPN thread error", e);
                        }
                    }
                } catch (IOException e) {
                    Log.e(TAG, "UDP → VPN thread error", e);
                }
                Log.i(TAG, "UDP → VPN thread stopped");
            }
        }, "UdpToVpnThread");

        // 2) Thread to capture [ TUN → local UDP:8001 ] → rtl8812
        vpnToUdpThread = new Thread(new Runnable() {
            @Override
            public void run() {
                Log.i(TAG, "VPN → UDP thread started");
                byte[] buffer = new byte[1024];

                try (DatagramSocket socket = new DatagramSocket()) {
                    socket.setReuseAddress(true);

                    while (isRunning) {
                        // Read from VPN interface
                        int length = vpnInput.read(buffer);
                        if (length == -1) {
                            // End of stream
                            break;
                        }

                        if (length == 0)
                            continue;
//                        Log.i(TAG, "VPN → UDP:  length: " + length);

                        // Prepend the packet size in network byte order
                        byte[] sizeBytes = new byte[2];
                        sizeBytes[0] = (byte) ((length >> 8) & 0xFF); // High byte
                        sizeBytes[1] = (byte) (length & 0xFF);         // Low byte

                        // Combine the size bytes and actual packet data
                        byte[] outputData = new byte[2 + length];
                        System.arraycopy(sizeBytes, 0, outputData, 0, 2);
                        System.arraycopy(buffer, 0, outputData, 2, length);


                        // Forward to UDP:8001 on localhost
                        DatagramPacket packet = new DatagramPacket(outputData, outputData.length,
                                new InetSocketAddress("127.0.0.1", 8001));
                        socket.send(packet);
                    }
                } catch (IOException e) {
                    Log.e(TAG, "VPN→UDP thread error", e);
                }
                Log.i(TAG, "VPN → UDP thread stopped");
            }
        }, "VpnToUdpThread");

        // Start the two threads
        udpToVpnThread.start();
        vpnToUdpThread.start();
        Log.i(TAG, "VPN threads started");
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.i(TAG, "VPN Service destroyed");

        // Stop threads
        isRunning = false;

        if (udpToVpnThread != null) {
            udpToVpnThread.interrupt();
        }
        if (vpnToUdpThread != null) {
            vpnToUdpThread.interrupt();
        }

        // Close the interface
        if (vpnInterface != null) {
            try {
                vpnInterface.close();
            } catch (IOException e) {
                Log.e(TAG, "Failed to close VPN interface", e);
            }
            vpnInterface = null;
        }
    }
}
