package com.geehe.videonative;

import android.graphics.SurfaceTexture;
import android.view.Surface;

public interface ISurfaceTextureAvailable {
    // Called when the SurfaceTexture (and Surface) was created on the OpenGL Thread
    // However, the callback is run on the UI Thread to ease synchronization
    void surfaceTextureCreated(final SurfaceTexture surfaceTexture, final Surface surface);

    // Called when the SurfaceTexture (and Surface) are about to be deleted. After this call
    // returns, both SurfaceTexture and Surface are invalid and must not be used anymore
    void surfaceTextureDestroyed();
}

