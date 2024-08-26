package com.geehe.fpvue.osd;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Point;
import android.util.AttributeSet;
import android.view.Display;
import android.view.MotionEvent;
import android.view.WindowManager;
import android.widget.LinearLayout;

public class MovableLayout extends LinearLayout {
    private float dX, dY;
    private SharedPreferences preferences;
    private boolean isMovable = false;
    private float defaultX, defaultY;
    private String prefName;

    public MovableLayout(Context context) {
        super(context);
        init(context);
    }

    public MovableLayout(Context context, AttributeSet attrs) {
        super(context, attrs);
        init(context);
    }

    public MovableLayout(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        init(context);
    }

    private void init(Context context) {
        preferences = context.getSharedPreferences("movable_layout_prefs", Context.MODE_PRIVATE);
        WindowManager windowManager = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
        Display display = windowManager.getDefaultDisplay();
        Point displaySize = new Point();
        display.getRealSize(displaySize);

        defaultX = (float) displaySize.x / 2 - ((float) displaySize.y / 8);
        defaultY = (float) displaySize.y / 2 - ((float) displaySize.y / 4);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (!isMovable) {
            return false;
        }

        switch (event.getAction()) {
            case MotionEvent.ACTION_DOWN:
                dX = this.getX() - event.getRawX();
                dY = this.getY() - event.getRawY();
                break;
            case MotionEvent.ACTION_MOVE:
                this.animate()
                        .x(event.getRawX() + dX)
                        .y(event.getRawY() + dY)
                        .setDuration(0)
                        .start();
                break;
            case MotionEvent.ACTION_UP:
                savePosition();
                break;
            default:
                return false;
        }
        return true;
    }

    private void savePosition() {
        SharedPreferences.Editor editor = preferences.edit();
        editor.putFloat(prefName + "_x", getX());
        editor.putFloat(prefName + "_y", getY());
        editor.apply();
    }

    public void restorePosition(String prefName_) {
        prefName = prefName_;
        float x = preferences.getFloat(prefName + "_x", defaultX);
        float y = preferences.getFloat(prefName + "_y", defaultY);
        setX(x);
        setY(y);
    }

    public void setMovable(boolean movable) {
        isMovable = movable;
    }
}
