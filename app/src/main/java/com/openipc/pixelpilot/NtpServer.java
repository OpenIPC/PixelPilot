package com.openipc.pixelpilot;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.SocketException;
import java.util.Arrays;

public class NtpServer extends Thread {
    private boolean running = true;
    private DatagramSocket socket;
    private final int port = 8988;

    public void stopServer() {
        running = false;
        if (socket != null && !socket.isClosed()) {
            socket.close();
        }
    }

    @Override
    public void run() {
        try {
            socket = new DatagramSocket(port);
            byte[] buffer = new byte[48];
            System.out.println("NTP server started on port：" + port);

            while (running) {
                DatagramPacket request = new DatagramPacket(buffer, buffer.length);
                socket.receive(request);

                byte[] requestData = Arrays.copyOf(request.getData(), request.getLength());
                byte[] responseData = createNtpResponse(requestData);

                DatagramPacket response = new DatagramPacket(
                        responseData, responseData.length,
                        request.getAddress(), request.getPort()
                );

                socket.send(response);
                System.out.println("Response NTP request from: " + request.getAddress());
            }

        } catch (SocketException e) {
            if (running) e.printStackTrace();
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    private byte[] createNtpResponse(byte[] requestData) {
        byte[] response = new byte[48];
        long currentTime = System.currentTimeMillis();

        // NTP timestamp since 1900
        long ntpTime = (currentTime / 1000L) + 2208988800L;
        long fractional = ((currentTime % 1000L) * 0x100000000L) / 1000L;

        // LI = 0, VN = 4, Mode = 4 (server)
        response[0] = 0b00100100;

        response[1] = 1; // stratum
        response[2] = 0; // poll
        response[3] = (byte) 0xEC; // precision

        // Root Delay and Root Dispersion
        Arrays.fill(response, 4, 12, (byte) 0);

        // Reference ID (e.g., "LOCL")
        response[12] = 'L';
        response[13] = 'O';
        response[14] = 'C';
        response[15] = 'L';

        // Reference Timestamp
        writeTimestamp(response, 16, ntpTime, fractional);

        // Originate Timestamp: from client's Transmit Timestamp
        System.arraycopy(requestData, 40, response, 24, 8);

        // Receive Timestamp
        writeTimestamp(response, 32, ntpTime, fractional);

        // Transmit Timestamp
        writeTimestamp(response, 40, ntpTime, fractional);

        return response;
    }

    private void writeTimestamp(byte[] buffer, int offset, long seconds, long fraction) {
        buffer[offset]     = (byte) (seconds >> 24);
        buffer[offset + 1] = (byte) (seconds >> 16);
        buffer[offset + 2] = (byte) (seconds >> 8);
        buffer[offset + 3] = (byte) (seconds);

        buffer[offset + 4] = (byte) (fraction >> 24);
        buffer[offset + 5] = (byte) (fraction >> 16);
        buffer[offset + 6] = (byte) (fraction >> 8);
        buffer[offset + 7] = (byte) (fraction);
    }
}
