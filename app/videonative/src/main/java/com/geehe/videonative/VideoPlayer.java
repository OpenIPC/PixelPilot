package com.geehe.videonative;

import android.content.Context;
import android.graphics.SurfaceTexture;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;

import java.util.Timer;
import java.util.TimerTask;


/**
 * All the receiving and decoding is done in .cpp
 * Tied to the lifetime of the java instance a .cpp instance is created
 * And the native functions talk to the native instance
 */
public class VideoPlayer implements IVideoParamsChanged {
    private static final String TAG = "com.geehe.fpvue";

    //All the native binding(s)
    static {
        System.loadLibrary("VideoNative");
    }

    private final long nativeVideoPlayer;
    private final Context context;
    private IVideoParamsChanged mVideoParamsChanged;
    // This timer is used to then 'call back' the IVideoParamsChanged
    private Timer timer;

    // Setup as much as possible without creating the decoder
    public VideoPlayer(final AppCompatActivity parent) {
        this.context = parent;
        nativeVideoPlayer = nativeInitialize(context);
    }

    public static native long nativeInitialize(Context context);

    public static native void nativeFinalize(long nativeVideoPlayer);

    public static native void nativeStart(long nativeInstance, Context context);

    public static native void nativeStop(long nativeInstance, Context context);

    public static native void nativeSetVideoSurface(long nativeInstance, Surface surface);

    public static native void nativeStartDvr(long nativeInstance, int fd, int fmp4_enabled);

    public static native void nativeStopDvr(long nativeInstance);

    public static native boolean nativeIsRecording(long nativeInstance);

    //get members or other information. Some might be only usable in between (nativeStart <-> nativeStop)
    public static native String getVideoInfoString(long nativeInstance);

    public static native boolean anyVideoDataReceived(long nativeInstance);

    public static native boolean anyVideoBytesParsedSinceLastCall(long nativeInstance);

    public static native boolean receivingVideoButCannotParse(long nativeInstance);

    // TODO: Use message queue from cpp for performance#
    // This initiates a 'call back' for the IVideoParams
    public static native <T extends IVideoParamsChanged> void nativeCallBack(T t, long nativeInstance);

    public static void verifyApplicationThread() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            Log.w(TAG, "Player is accessed on the wrong thread.");
        }
    }

    public void setIVideoParamsChanged(final IVideoParamsChanged iVideoParamsChanged) {
        mVideoParamsChanged = iVideoParamsChanged;
    }

    private void setVideoSurface(final @Nullable Surface surface) {
        verifyApplicationThread();
        nativeSetVideoSurface(nativeVideoPlayer, surface);
    }

    public synchronized void start() {
        verifyApplicationThread();
        nativeStart(nativeVideoPlayer, context);
        //The timer initiates the callback(s), but if no data has changed they are not called (and the timer does almost no work)
        //TODO: proper queue, but how to do synchronization in java ndk ?!
        timer = new Timer();
        timer.schedule(new TimerTask() {
            @Override
            public void run() {
                nativeCallBack(VideoPlayer.this, nativeVideoPlayer);
            }
        }, 0, 200);
    }

    public synchronized void stop() {
        if (timer == null) {
            return;
        }
        verifyApplicationThread();
        timer.cancel();
        timer.purge();
        nativeStop(nativeVideoPlayer, context);
        timer = null;
    }

    public boolean isRunning() {
        return timer != null;
    }

    public void startDvr(int fd, boolean enabled_fmp4) {
        nativeStartDvr(nativeVideoPlayer, fd, enabled_fmp4 ? 1 : 0);
    }

    public void stopDvr() {
        nativeStopDvr(nativeVideoPlayer);
    }

    /**
     * Depending on the selected Settings, this starts either
     * a) Receiving RTP over UDP
     * b) Receiving RAW over UDP
     * c) Receiving Data from a resource file (Assets)
     * d) Receiving Data from a file in the phone file system
     * e) and more
     */
    public void addAndStartDecoderReceiver(Surface surface) {
        setVideoSurface(surface);
    }

    /**
     * Stop the Receiver
     * Stop the Decoder
     * Free resources
     */
    public void stopAndRemoveReceiverDecoder() {
        stop();
        setVideoSurface(null);
    }

    /**
     * Configure for use with SurfaceHolder from a SurfaceVew
     * The callback will handle the lifecycle of the Video player
     *
     * @return Callback that should be added to SurfaceView.Holder
     */
    public SurfaceHolder.Callback configure1() {
        return new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                addAndStartDecoderReceiver(holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {

            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                stopAndRemoveReceiverDecoder();
            }
        };
    }

    /**
     * Configure for use with VideoSurfaceHolder (OpenGL)
     * The callback will handle the lifecycle of the video player
     *
     * @return Callback that should be added to VideoSurfaceHolder
     */
    public ISurfaceTextureAvailable configure2() {
        return new ISurfaceTextureAvailable() {
            @Override
            public void surfaceTextureCreated(SurfaceTexture surfaceTexture, Surface surface) {
                addAndStartDecoderReceiver(surface);
            }

            @Override
            public void surfaceTextureDestroyed() {
                stopAndRemoveReceiverDecoder();
            }
        };
    }

    public long getNativeInstance() {
        return nativeVideoPlayer;
    }

    // called by native code via NDK
    @Override
    @SuppressWarnings({"UnusedDeclaration"})
    public void onVideoRatioChanged(int videoW, int videoH) {
        if (mVideoParamsChanged != null) {
            mVideoParamsChanged.onVideoRatioChanged(videoW, videoH);
        }
        System.out.println("Video W and H" + videoW + "," + videoH);
    }

    // called by native code via NDK
    @Override
    public void onDecodingInfoChanged(DecodingInfo decodingInfo) {
        if (mVideoParamsChanged != null) {
            mVideoParamsChanged.onDecodingInfoChanged(decodingInfo);
        }
        //Log.d(TAG,"onDecodingInfoChanged"+decodingInfo.toString());
    }

    @Override
    protected void finalize() throws Throwable {
        try {
            nativeFinalize(nativeVideoPlayer);
        } finally {
            super.finalize();
        }
    }

}

