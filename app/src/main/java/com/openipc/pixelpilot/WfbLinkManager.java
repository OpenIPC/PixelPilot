package com.openipc.pixelpilot;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.util.Log;
import android.view.View;

import com.openipc.pixelpilot.databinding.ActivityVideoBinding;
import com.openipc.wfbngrtl8812.WfbNgLink;

import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;

public class WfbLinkManager extends BroadcastReceiver {
    public static final String ACTION_USB_PERMISSION = "com.openipc.pixelpilot.USB_PERMISSION";
    private static final String TAG = "pixelpilot";
    static Map<String, UsbDevice> activeWifiAdapters = new HashMap<>();
    private final WfbNgLink wfbLink;
    private final ActivityVideoBinding binding;
    private final Context context;
    private int wifiChannel;

    public WfbLinkManager(Context context, ActivityVideoBinding binding, WfbNgLink wfbNgLink) {
        this.binding = binding;
        this.context = context;
        this.wfbLink = wfbNgLink;
    }

    public void refreshKey() {
        wfbLink.refreshKey();
    }

    public void setChannel(int channel) {
        wifiChannel = channel;
    }

    @Override
    public synchronized void onReceive(Context context, Intent intent) {
        UsbDevice dev = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
        if (android.hardware.usb.UsbManager.ACTION_USB_DEVICE_DETACHED.equals(intent.getAction())) {
            if (dev == null) {
                return;
            }
            Log.d(TAG, "usb device detached: " + dev.getVendorId() + "/" + dev.getProductId());
            refreshAdapters();
        } else if (android.hardware.usb.UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(intent.getAction())) {
            if (dev == null) {
                return;
            }
            Log.d(TAG, "usb device attached: " + dev.getVendorId() + "/" + dev.getProductId());
            // No need to refresh since this should trigger a call to VideoActivity.onReceive();
        } else if (ACTION_USB_PERMISSION.equals(intent.getAction())) {
            Log.d(TAG, "Permission handled");
        }
    }

    public Map<String, UsbDevice> getAttachedAdapters() {
        android.hardware.usb.UsbManager manager =
                (android.hardware.usb.UsbManager) context.getSystemService(Context.USB_SERVICE);

        List<UsbDeviceFilter> filters;
        try {
            filters = UsbDeviceFilter.parseXml(context, R.xml.usb_device_filter);
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }

        Map<String, UsbDevice> res = new HashMap<>();
        for (UsbDevice dev : manager.getDeviceList().values()) {
            boolean allowed = false;
            for (UsbDeviceFilter filter : filters) {
                if (filter.productId == dev.getProductId() && filter.vendorId == dev.getVendorId()) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                continue;
            }
            res.put(dev.getDeviceName(), dev);
        }
        return res;
    }

    public synchronized void refreshAdapters() {
        Map<String, UsbDevice> attachedAdapters = getAttachedAdapters();

        boolean missingPermissions = false;
        android.hardware.usb.UsbManager usbManager =
                (android.hardware.usb.UsbManager) context.getSystemService(Context.USB_SERVICE);
        for (Map.Entry<String, UsbDevice> entry : attachedAdapters.entrySet()) {
            if (!usbManager.hasPermission(entry.getValue())) {
                binding.tvMessage.setVisibility(View.VISIBLE);
                binding.tvMessage.setText("No permission for wifi adapter(s) " + entry.getValue().getDeviceName());
                PendingIntent pendingIntent = PendingIntent.getBroadcast(context, 0,
                        new Intent(WfbLinkManager.ACTION_USB_PERMISSION), PendingIntent.FLAG_IMMUTABLE);
                usbManager.requestPermission(entry.getValue(), pendingIntent);
                missingPermissions = true;
            }
        }
        if (missingPermissions) {
            return;
        }


        // Stops newly detached adapters.
        Iterator<Map.Entry<String, UsbDevice>> iterator = activeWifiAdapters.entrySet().iterator();
        while (iterator.hasNext()) {
            Map.Entry<String, UsbDevice> entry = iterator.next();
            if (attachedAdapters.containsKey(entry.getKey())) {
                continue;
            }
            stopAdapter(entry.getValue());
            iterator.remove();
        }

        // Starts newly attached adapters.
        for (Map.Entry<String, UsbDevice> entry : attachedAdapters.entrySet()) {
            if (activeWifiAdapters.containsKey(entry.getKey())) {
                continue;
            }
            startAdapter(entry.getValue());
            activeWifiAdapters.put(entry.getKey(), entry.getValue());
        }

        if (activeWifiAdapters.isEmpty()) {
            String text = "No compatible wifi adapter found.";
            String wifi = VideoActivity.wirelessInfo();
            if (wifi != null) {
                text += "\n(udp://" + wifi + ":5600)";
            }
            binding.tvMessage.setVisibility(View.VISIBLE);
            binding.tvMessage.setText(text);
        }
    }

    public synchronized void stopAdapters() {
        try {
            wfbLink.stopAll();
            activeWifiAdapters.clear();
        } catch (InterruptedException ignored) {
        }
    }

    public synchronized void stopAdapter(UsbDevice dev) {
        try {
            wfbLink.stop(dev);
        } catch (InterruptedException e) {
            e.printStackTrace();
        }
    }

    public synchronized void startAdapters(int channel) {
        wifiChannel = channel;
        if (wfbLink.isRunning()) {
            return;
        }
        for (Map.Entry<String, UsbDevice> entry : activeWifiAdapters.entrySet()) {
            if (!startAdapter(entry.getValue())) {
                break;
            }
        }
    }

    public synchronized boolean startAdapter(UsbDevice dev) {
        binding.tvMessage.setVisibility(View.VISIBLE);
        String name = String.format("%04X", dev.getVendorId()) + ":" + String.format("%04X", dev.getProductId());
        if (binding.tvMessage.getText().toString().startsWith("Starting")
                && !binding.tvMessage.getText().toString().endsWith(name)) {
            binding.tvMessage.setText(binding.tvMessage.getText() + ", " + name);
        } else {
            binding.tvMessage.setText("Starting wfb-ng on channel " + wifiChannel + " with " + name);
        }
        wfbLink.start(wifiChannel, dev);
        return true;
    }
}
