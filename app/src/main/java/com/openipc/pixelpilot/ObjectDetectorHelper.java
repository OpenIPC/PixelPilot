package com.openipc.pixelpilot;

import android.content.Context;
import android.graphics.Bitmap;
import android.os.SystemClock;
import android.util.Log;

import com.google.mediapipe.framework.image.BitmapImageBuilder;
import com.google.mediapipe.framework.image.MPImage;
import com.google.mediapipe.tasks.core.BaseOptions;
import com.google.mediapipe.tasks.core.Delegate;
import com.google.mediapipe.tasks.vision.core.RunningMode;
import com.google.mediapipe.tasks.vision.objectdetector.ObjectDetector;
import com.google.mediapipe.tasks.vision.objectdetector.ObjectDetectorResult;

import java.util.Collections;
import java.util.List;

public class ObjectDetectorHelper {
    private static final String TAG = "ObjectDetectorHelper";
    
    public static final int DELEGATE_CPU = 0;
    public static final int DELEGATE_GPU = 1;
    
    public static final int MODEL_EFFICIENTDETV0 = 0;
    public static final int MODEL_EFFICIENTDETV2 = 1;
    
    private final Context context;
    private final float threshold;
    private final int maxResults;
    private final int currentDelegate;
    private final int currentModel;
    
    private ObjectDetector objectDetector;

    public ObjectDetectorHelper(Context context, float threshold, int maxResults, int delegate, int model) {
        this.context = context;
        this.threshold = threshold;
        this.maxResults = maxResults;
        this.currentDelegate = delegate;
        this.currentModel = model;
        setupObjectDetector();
    }

    private void setupObjectDetector() {
        BaseOptions.Builder baseOptionsBuilder = BaseOptions.builder();
        if (currentDelegate == DELEGATE_GPU) {
            baseOptionsBuilder.setDelegate(Delegate.GPU);
        } else {
            baseOptionsBuilder.setDelegate(Delegate.CPU);
        }

        String modelName = "efficientdet-lite0.tflite";
        if (currentModel == MODEL_EFFICIENTDETV2) {
            modelName = "efficientdet-lite2.tflite";
        }
        baseOptionsBuilder.setModelAssetPath(modelName);

        try {
            ObjectDetector.ObjectDetectorOptions options = ObjectDetector.ObjectDetectorOptions.builder()
                    .setBaseOptions(baseOptionsBuilder.build())
                    .setScoreThreshold(threshold)
                    .setMaxResults(maxResults)
                    .setRunningMode(RunningMode.IMAGE)
                    .build();
            objectDetector = ObjectDetector.createFromOptions(context, options);
        } catch (Exception e) {
            Log.e(TAG, "Failed to load model: " + e.getMessage(), e);
        }
    }

    public ResultBundle detectImage(Bitmap image) {
        if (objectDetector == null) return null;
        long startTime = SystemClock.uptimeMillis();
        MPImage mpImage = new BitmapImageBuilder(image).build();
        try {
            ObjectDetectorResult detectionResult = objectDetector.detect(mpImage);
            long inferenceTime = SystemClock.uptimeMillis() - startTime;
            return new ResultBundle(Collections.singletonList(detectionResult), inferenceTime, image.getHeight(), image.getWidth());
        } catch (Exception e) {
            Log.e(TAG, "Detection failed: " + e.getMessage(), e);
            return null;
        }
    }

    public void clear() {
        if (objectDetector != null) {
            objectDetector.close();
            objectDetector = null;
        }
    }

    public static class ResultBundle {
        public final List<ObjectDetectorResult> results;
        public final long inferenceTime;
        public final int inputImageHeight;
        public final int inputImageWidth;

        public ResultBundle(List<ObjectDetectorResult> results, long inferenceTime, int inputImageHeight, int inputImageWidth) {
            this.results = results;
            this.inferenceTime = inferenceTime;
            this.inputImageHeight = inputImageHeight;
            this.inputImageWidth = inputImageWidth;
        }
    }
}
