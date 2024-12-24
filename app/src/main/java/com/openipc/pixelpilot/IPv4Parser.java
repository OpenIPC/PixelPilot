package com.openipc.pixelpilot;

import android.util.Log;

import java.net.InetAddress;
import java.net.UnknownHostException;

public class IPv4Parser {

    private static final String TAG = "IPv4Parser";

    /**
     * Parse and log basic IPv4 header fields from the given byte array.
     *
     * @param packet Raw buffer containing the IPv4 packet.
     * @param length Number of valid bytes in 'packet'.
     */
    public static boolean parseIPv4Header(byte[] packet, int length) {
        // Minimum IPv4 header is 20 bytes
        if (length < 20) {
            Log.i(TAG, "Packet too short to be an IPv4 header (need at least 20 bytes).");
            return false;
        }

        // packet[0]: version (4 bits) + IHL (4 bits)
        // Higher 4 bits = version, lower 4 bits = IHL (in 32-bit words)
        int versionIHL = packet[0] & 0xFF;
        int version = (versionIHL >> 4) & 0x0F;
        int ihl = versionIHL & 0x0F;

        if (version != 4) {
            Log.i(TAG, "Not an IPv4 packet. Version = " + version);
            return false;
        }

        if (ihl < 5) {
            Log.i(TAG, "Invalid IHL (got " + ihl + ", must be >= 5).");
            return false;
        }

        // DSCP/ECN (old TOS) is packet[1]
        int tos = packet[1] & 0xFF;

        // Total length is packet[2..3]
        int totalLength = ((packet[2] & 0xFF) << 8) | (packet[3] & 0xFF);

        // Identification is packet[4..5]
        int identification = ((packet[4] & 0xFF) << 8) | (packet[5] & 0xFF);

        // Flags + Fragment Offset is packet[6..7]
        int flagsFragOffset = ((packet[6] & 0xFF) << 8) | (packet[7] & 0xFF);

        // Time to Live is packet[8]
        int ttl = packet[8] & 0xFF;

        // Protocol is packet[9] (e.g., 6 = TCP, 17 = UDP)
        int protocol = packet[9] & 0xFF;

        // Header checksum is packet[10..11]
        int headerChecksum = ((packet[10] & 0xFF) << 8) | (packet[11] & 0xFF);

        // Source IP is packet[12..15]
        byte[] srcBytes = new byte[] {
                packet[12], packet[13], packet[14], packet[15]
        };

        // Destination IP is packet[16..19]
        byte[] dstBytes = new byte[] {
                packet[16], packet[17], packet[18], packet[19]
        };

        String srcAddr = bytesToIP(srcBytes);
        String dstAddr = bytesToIP(dstBytes);

        // Log all fields:
        Log.i(TAG, "=== IPv4 Header ===");
        Log.i(TAG, " Version            : " + version);
        Log.i(TAG, " IHL (in 32-bit wds): " + ihl + "  => " + (ihl * 4) + " bytes");
        Log.i(TAG, " TOS (DSCP/ECN)     : 0x" + Integer.toHexString(tos));
        Log.i(TAG, " Total Length       : " + totalLength);
        Log.i(TAG, " Identification     : 0x" + Integer.toHexString(identification));
        Log.i(TAG, " Flags/Frag Offset  : 0x" + Integer.toHexString(flagsFragOffset));
        Log.i(TAG, " TTL                : " + ttl);
        Log.i(TAG, " Protocol           : " + protocol);
        Log.i(TAG, " Header Checksum    : 0x" + Integer.toHexString(headerChecksum));
        Log.i(TAG, " Source IP          : " + srcAddr);
        Log.i(TAG, " Destination IP     : " + dstAddr);
        Log.i(TAG, "====================");


        int headerLength = ihl * 4;
        if (length < headerLength) {
            Log.i(TAG, "Packet too short to include the complete IPv4 header.");
            return false;
        }


        if (length > headerLength) {
            if (protocol == 17) { // Protocol 17 is UDP
                parseUDPHeader(packet, headerLength, length - headerLength);
            } else {
                Log.i(TAG, "Not a UDP packet. Protocol = " + protocol);
            }
        }

        return true;
    }
    private static boolean parseUDPHeader(byte[] packet, int offset, int length) {
        if (length < 8) {
            Log.i(TAG, "Packet too short to be a UDP header (need at least 8 bytes).");
            return false;
        }

        int sourcePort = ((packet[offset] & 0xFF) << 8) | (packet[offset + 1] & 0xFF);
        int destPort = ((packet[offset + 2] & 0xFF) << 8) | (packet[offset + 3] & 0xFF);
        int udpLength = ((packet[offset + 4] & 0xFF) << 8) | (packet[offset + 5] & 0xFF);
        int checksum = ((packet[offset + 6] & 0xFF) << 8) | (packet[offset + 7] & 0xFF);

        Log.i(TAG, "=== UDP Header ===");
        Log.i(TAG, " Source Port        : " + sourcePort);
        Log.i(TAG, " Destination Port   : " + destPort);
        Log.i(TAG, " Length             : " + udpLength);
        Log.i(TAG, " Checksum           : 0x" + Integer.toHexString(checksum));
        Log.i(TAG, "====================");
        return true;
    }


    /**
     * Convert 4-byte array (big-endian IP) to dotted-decimal string.
     * In Java, we can use InetAddress.getByAddress(...) + getHostAddress().
     */
    private static String bytesToIP(byte[] ipBytes) {
        try {
            InetAddress addr = InetAddress.getByAddress(ipBytes);
            return addr.getHostAddress();  // e.g. "192.168.1.10"
        } catch (UnknownHostException e) {
            return "Invalid IP";
        }
    }
}
