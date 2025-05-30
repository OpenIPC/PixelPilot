package com.openipc.wfbngrtl8812;

import android.content.Context;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.util.Log;

import androidx.annotation.Keep;
import androidx.appcompat.app.AppCompatActivity;

import java.util.HashMap;
import java.util.Map;
import java.util.Timer;
import java.util.TimerTask;

@Keep
public class WfbNgLink implements WfbNGStatsChanged {

    // Set FEC thresholds for switching levels (Java wrapper and JNI)
    public static native void nativeSetFecThresholds(long nativeInstance, int lostTo5, int recTo4, int recTo3, int recTo2, int recTo1);

    public void setFecThresholds(int lostTo5, int recTo4, int recTo3, int recTo2, int recTo1) {
        nativeSetFecThresholds(nativeWfbngLink, lostTo5, recTo4, recTo3, recTo2, recTo1);
    }
    public static String TAG = "pixelpilot";

    // Load the native library on application startup.
    static {
        System.loadLibrary("WfbngRtl8812");
    }

    private final long nativeWfbngLink;
    private final Timer timer;
    private final Context context;
    Map<UsbDevice, Thread> linkThreads = new HashMap<>();
    Map<UsbDevice, UsbDeviceConnection> linkConns = new HashMap<>();
    private WfbNGStatsChanged statsChanged;

    // Native method declarations.
    public static native long nativeInitialize(Context context);
    public static native void nativeRun(long nativeInstance, Context context, int wifiChannel, int bandWidth, int fd);
    public static native void nativeStop(long nativeInstance, Context context, int fd);
    public static native void nativeRefreshKey(long nativeInstance);
    public static native <T extends WfbNGStatsChanged> void nativeCallBack(T t, long nativeInstance);
    public static native void nativeStartAdaptivelink(long nativeInstance);
    public static native void nativeSetAdaptiveLinkEnabled(long nativeInstance, boolean enabled);
    public static native void nativeSetTxPower(long nativeInstance, int power);
    public static native void nativeSetUseFec(long nativeInstance, int use);

    public WfbNgLink(final AppCompatActivity parent) {
        this.context = parent;
        nativeWfbngLink = nativeInitialize(context);
        timer = new Timer();
        timer.schedule(new TimerTask() {
            @Override
            public void run() {
                nativeCallBack(WfbNgLink.this, nativeWfbngLink);
            }
        }, 0, 300);
    }

    public boolean isRunning() {
        return !linkThreads.isEmpty();
    }

    public void refreshKey() {
        nativeRefreshKey(nativeWfbngLink);
    }

    // Instance wrapper for nativeSetAdaptiveLinkEnabled.
    public void nativeSetAdaptiveLinkEnabled(boolean state) {
        nativeSetAdaptiveLinkEnabled(nativeWfbngLink, state);
    }

    // Instance wrapper for nativeSetTxPower.
    public void nativeSetTxPower(int power) {
        nativeSetTxPower(nativeWfbngLink, power);
    }

    public void nativeSetUseFec(int use) {
        nativeSetUseFec(nativeWfbngLink, use);
    }

    public synchronized void start(int wifiChannel, int bandWidth, UsbDevice usbDevice) {
        Log.d(TAG, "wfb-ng monitoring on " + usbDevice.getDeviceName() + " using wifi channel " + wifiChannel);
        UsbManager usbManager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        UsbDeviceConnection usbDeviceConnection = usbManager.openDevice(usbDevice);
        int fd = usbDeviceConnection.getFileDescriptor();
        Thread t = new Thread(() -> nativeRun(nativeWfbngLink, context, wifiChannel, bandWidth, fd));
        t.setName("wfb-" + usbDevice.getDeviceName().split("/dev/bus/usb/")[1]);
        linkThreads.put(usbDevice, t);
        linkConns.put(usbDevice, usbDeviceConnection);
        linkThreads.get(usbDevice).start();
        Log.d(TAG, "wfb-ng thread on " + usbDevice.getDeviceName() + " started.");
    }

    public synchronized void stopAll() throws InterruptedException {
        for (Map.Entry<UsbDevice, UsbDeviceConnection> entry : linkConns.entrySet()) {
            nativeStop(nativeWfbngLink, context, entry.getValue().getFileDescriptor());
        }
        for (Map.Entry<UsbDevice, UsbDeviceConnection> entry : linkConns.entrySet()) {
            Thread t = linkThreads.get(entry.getKey());
            if (t != null) {
                t.join();
            }
            Log.d(TAG, "wfb-ng thread on " + entry.getKey().getDeviceName() + " done.");
        }
        linkThreads.clear();
    }

    public synchronized void stop(UsbDevice dev) throws InterruptedException {
        UsbDeviceConnection conn = linkConns.get(dev);
        if (conn == null) {
            return;
        }
        int fd = conn.getFileDescriptor();
        nativeStop(nativeWfbngLink, context, fd);
        Thread t = linkThreads.get(dev);
        if (t != null) {
            t.join();
        }
        linkThreads.remove(dev);
    }

    public void SetWfbNGStatsChanged(final WfbNGStatsChanged callback) {
        statsChanged = callback;
    }

    // Called by native code via NDK.
    @Override
    public void onWfbNgStatsChanged(WfbNGStats stats) {
        if (statsChanged != null) {
            statsChanged.onWfbNgStatsChanged(stats);
        }
    }
}
