package com.openipc.pixelpilot.osd;

public class OSDElement {
    public String name;
    public MovableLayout layout;

    public OSDElement(String n, MovableLayout l) {
        name = n;
        layout = l;
    }

    public String prefName() {
        return String.format("%d", name.hashCode());
    }
}
