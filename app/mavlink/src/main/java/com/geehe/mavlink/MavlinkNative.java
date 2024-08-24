package com.geehe.mavlink;

import android.content.Context;

public class MavlinkNative {

    // Used to load the 'mavlink' library on application startup.
    static {
        System.loadLibrary("mavlink");
    }

    public static native void nativeStart(Context context);

    public static native void nativeStop(Context context);

    // TODO: Use message queue from cpp for performance#
    // This initiates a 'call back' for the IVideoParams
    public static native <T extends MavlinkUpdate> void nativeCallBack(T t);
}