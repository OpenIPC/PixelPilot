package com.openipc.pixelpilot;

import android.content.Context;
import android.content.res.XmlResourceParser;

import org.xmlpull.v1.XmlPullParserException;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

public class UsbDeviceFilter {
    public int vendorId;
    public int productId;

    public UsbDeviceFilter(int vid, int pid) {
        vendorId = vid;
        productId = pid;
    }

    public static List<UsbDeviceFilter> parseXml(Context context, int resourceId)
            throws XmlPullParserException, IOException {
        List<UsbDeviceFilter> devices = new ArrayList<>();
        XmlResourceParser parser = context.getResources().getXml(resourceId);

        int eventType = parser.getEventType();
        while (eventType != XmlResourceParser.END_DOCUMENT) {
            if (eventType == XmlResourceParser.START_TAG) {
                String tag = parser.getName();
                if (tag.equals("usb-device")) {
                    String vendorIdString = parser.getAttributeValue(null, "vendor-id");
                    String productIdString = parser.getAttributeValue(null, "product-id");

                    int vendorId = Integer.parseInt(vendorIdString, 16);
                    int productId = Integer.parseInt(productIdString, 16);

                    UsbDeviceFilter device = new UsbDeviceFilter(vendorId, productId);
                    devices.add(device);
                }
            }
            eventType = parser.next();
        }
        return devices;
    }
}
