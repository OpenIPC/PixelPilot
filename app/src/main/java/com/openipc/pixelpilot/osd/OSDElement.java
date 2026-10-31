package com.openipc.pixelpilot.osd;

import android.view.View;

public class OSDElement {
    public String name;
    public MovableLayout layout;   // null for a fixed item
    public View fixedView;         // fixed item, e.g. the centre dot
    public boolean defaultEnabled;

    public OSDElement(String n, MovableLayout l) {
        name = n;
        layout = l;
        fixedView = null;
        defaultEnabled = false;
    }

    public OSDElement(String n, View fixed, boolean enabledByDefault) {
        name = n;
        layout = null;
        fixedView = fixed;
        defaultEnabled = enabledByDefault;
    }

    public View view() {
        return layout != null ? layout : fixedView;
    }

    public String prefName() {
        return String.format("%d", name.hashCode());
    }
}
