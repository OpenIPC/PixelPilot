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
    public static String TAG = "pixelpilot";

    // Load the 'Rtl8812au_Wfbng' library on application startup.
    static {
        System.loadLibrary("WfbngRtl8812");
    }

    private final long nativeWfbngLink;
    private final Timer timer;
    private final Context context;
    Map<UsbDevice, Thread> linkThreads = new HashMap<>();
    Map<UsbDevice, UsbDeviceConnection> linkConns = new HashMap<>();
    private WfbNGStatsChanged statsChanged;

    public WfbNgLink(final AppCompatActivity parent) {
        this.context = parent;
        nativeWfbngLink = nativeInitialize(context);
        timer = new Timer();
        timer.schedule(new TimerTask() {
            @Override
            public void run() {
                nativeCallBack(WfbNgLink.this, nativeWfbngLink);
            }
        }, 0, 500);
    }

    // Native cpp methods.
    public static native long nativeInitialize(Context context);

    public static native void nativeRun(long nativeInstance, Context context, int wifiChannel, int fd);

    public static native void nativeStop(long nativeInstance, Context context, int fd);

    public static native void nativeRefreshKey(long nativeInstance);

    public static native <T extends WfbNGStatsChanged> void nativeCallBack(T t, long nativeInstance);

    public boolean isRunning() {
        return !linkThreads.isEmpty();
    }

    public void refreshKey() {
        nativeRefreshKey(nativeWfbngLink);
    }

    public synchronized void start(int wifiChannel, UsbDevice usbDevice) {
        Log.d(TAG, "wfb-ng monitoring on " + usbDevice.getDeviceName() + " using wifi channel " + wifiChannel);
        UsbManager usbManager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        UsbDeviceConnection usbDeviceConnection = usbManager.openDevice(usbDevice);
        int fd = usbDeviceConnection.getFileDescriptor();
        Thread t = new Thread(() -> nativeRun(nativeWfbngLink, context, wifiChannel, fd));
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

    // called by native code via NDK
    @Override
    public void onWfbNgStatsChanged(WfbNGStats stats) {
        if (statsChanged != null) {
            statsChanged.onWfbNgStatsChanged(stats);
        }
    }
}