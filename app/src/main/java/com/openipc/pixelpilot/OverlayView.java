package com.openipc.pixelpilot;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.view.View;

import com.google.mediapipe.tasks.components.containers.Detection;
import com.google.mediapipe.tasks.vision.objectdetector.ObjectDetectorResult;

public class OverlayView extends View {
    private ObjectDetectorResult results;
    private final Paint boxPaint = new Paint();
    private final Paint textBackgroundPaint = new Paint();
    private final Paint textPaint = new Paint();
    private float scaleFactor = 1.0f;
    private final Rect bounds = new Rect();
    private int outputWidth = 0;
    private int outputHeight = 0;

    private static final int BOUNDING_RECT_TEXT_PADDING = 8;

    public OverlayView(Context context, AttributeSet attrs) {
        super(context, attrs);
        initPaints();
    }

    private void initPaints() {
        textBackgroundPaint.setColor(Color.BLACK);
        textBackgroundPaint.setStyle(Paint.Style.FILL);
        textBackgroundPaint.setTextSize(40f);

        textPaint.setColor(Color.WHITE);
        textPaint.setStyle(Paint.Style.FILL);
        textPaint.setTextSize(40f);

        boxPaint.setColor(Color.GREEN);
        boxPaint.setStrokeWidth(6f);
        boxPaint.setStyle(Paint.Style.STROKE);
    }

    public void clear() {
        results = null;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (results == null || results.detections() == null) return;

        for (Detection detection : results.detections()) {
            RectF box = detection.boundingBox();
            
            // Adjust box coordinates to view scaling
            float left = box.left * scaleFactor;
            float top = box.top * scaleFactor;
            float right = box.right * scaleFactor;
            float bottom = box.bottom * scaleFactor;

            // Draw bounding box
            canvas.drawRect(left, top, right, bottom, boxPaint);

            // Category text
            if (!detection.categories().isEmpty()) {
                String categoryName = detection.categories().get(0).categoryName();
                float score = detection.categories().get(0).score();
                String text = String.format("%s %.2f", categoryName, score);

                // Draw background rect for text
                textBackgroundPaint.getTextBounds(text, 0, text.length(), bounds);
                float textWidth = bounds.width();
                float textHeight = bounds.height();
                
                canvas.drawRect(
                        left,
                        top,
                        left + textWidth + BOUNDING_RECT_TEXT_PADDING,
                        top + textHeight + BOUNDING_RECT_TEXT_PADDING,
                        textBackgroundPaint
                );

                // Draw text
                canvas.drawText(text, left, top + textHeight, textPaint);
            }
        }
    }

    public void setResults(ObjectDetectorResult detectionResults, int inputHeight, int inputWidth) {
        this.results = detectionResults;
        this.outputWidth = inputWidth;
        this.outputHeight = inputHeight;

        // Calculate scaling factor to scale bounding boxes to match actual display size of the View
        scaleFactor = Math.min(getWidth() * 1.0f / inputWidth, getHeight() * 1.0f / inputHeight);

        invalidate();
    }
}
