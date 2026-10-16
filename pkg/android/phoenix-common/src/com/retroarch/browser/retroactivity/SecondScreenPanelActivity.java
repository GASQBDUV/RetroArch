package com.retroarch.browser.retroactivity;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;

import java.io.File;
import java.lang.ref.WeakReference;

/**
 * Hosts SecondScreenRouterView on the DEFAULT display when the game runs on
 * the secondary one - the layout for GammaOS on the Anbernic RG DS / RG DS
 * Plus, whose default display is the bottom (touch) panel. A Presentation
 * can't be used there: the framework refuses presentation windows on the
 * default display.
 *
 * Focus is the hard part. RetroArch reads the controller through its
 * NativeActivity input queue, so key events can't be forwarded from here,
 * and it pauses when its window loses focus. A controller always follows the
 * single focused display, so the game must stay the focused window:
 *
 * - This window has to be focusable while it comes up: an activity that is
 *   the only window of its task with no focusable window makes the input
 *   dispatcher ANR ("Application does not have a focused window"), as
 *   OpenMW-DS measured on device.
 * - Once it has had focus, a 1x1 untouchable "anchor" sub-window takes over
 *   being the focusable window on this display and the panel itself becomes
 *   non-focusable, so taps on the map can never take focus. (Anchor first,
 *   or the ANR comes back.)
 * - Then the game's task is moved back to the front, which makes the game's
 *   display the focused one again.
 */
public class SecondScreenPanelActivity extends Activity {
    private static final String TAG = "SecondScreenPanel";
    static final String NO_SWAP_FILE = "no_swap_screens";

    static volatile WeakReference<SecondScreenPanelActivity> instance;
    static volatile WeakReference<RetroActivityCommon> game;

    private View focusAnchor;
    private boolean focusDropped;

    /** Swapped layout unless an empty no_swap_screens file exists in the app's external files dir. */
    static boolean swapWanted(Context context) {
        File dir = context.getExternalFilesDir(null);
        return dir == null || !new File(dir, NO_SWAP_FILE).exists();
    }

    static Display secondaryDisplay(Context context) {
        DisplayManager dm = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        if (dm == null) return null;
        for (Display d : dm.getDisplays()) {
            if (d != null && d.isValid() && d.getDisplayId() != Display.DEFAULT_DISPLAY) return d;
        }
        return null;
    }

    static boolean isRunning() {
        WeakReference<SecondScreenPanelActivity> ref = instance;
        SecondScreenPanelActivity a = ref == null ? null : ref.get();
        return a != null && !a.isFinishing();
    }

    static void finishIfRunning() {
        WeakReference<SecondScreenPanelActivity> ref = instance;
        SecondScreenPanelActivity a = ref == null ? null : ref.get();
        if (a != null) a.finish();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        RetroActivityCommon activity = game == null ? null : game.get();
        if (activity == null) {
            finish();
            return;
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setBackgroundColor(Color.BLACK);
        applyImmersive();
        try {
            SecondScreenRouterView router = new SecondScreenRouterView(this, activity);
            setContentView(router, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        } catch (Throwable e) {
            // Shares the game's process; drop the panel rather than the game.
            Log.e(TAG, "Panel failed to come up", e);
            finish();
            return;
        }
        instance = new WeakReference<>(this);
        Log.i(TAG, "Panel activity up on display " + getWindowManager().getDefaultDisplay().getDisplayId());
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersive();
            dropFocusabilityAndReturnFocusToGame();
        }
    }

    private void dropFocusabilityAndReturnFocusToGame() {
        if (focusDropped) return;
        focusDropped = true;
        getWindow().getDecorView().post(new Runnable() {
            @Override
            public void run() {
                if (addFocusAnchor()) {
                    getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
                    applyImmersive();
                } else {
                    // Fail safe: staying focusable can cost the game focus on
                    // a tap, but never an ANR.
                    focusDropped = false;
                    Log.e(TAG, "No focus anchor, staying focusable");
                }
                bringGameToFront();
            }
        });
    }

    private boolean addFocusAnchor() {
        if (focusAnchor != null) return true;
        if (getWindow().getDecorView().getWindowToken() == null) return false;
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                1, 1,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
                PixelFormat.TRANSPARENT);
        lp.token = getWindow().getDecorView().getWindowToken();
        lp.gravity = Gravity.TOP | Gravity.START;
        View view = new View(this);
        try {
            getWindowManager().addView(view, lp);
            focusAnchor = view;
            return true;
        } catch (RuntimeException e) {
            Log.e(TAG, "Focus anchor addView failed", e);
            return false;
        }
    }

    private void bringGameToFront() {
        RetroActivityCommon activity = game == null ? null : game.get();
        if (activity == null) return;
        try {
            ActivityManager am = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
            if (am != null) am.moveTaskToFront(activity.getTaskId(), 0);
        } catch (RuntimeException e) {
            Log.w(TAG, "moveTaskToFront failed", e);
        }
    }

    private void applyImmersive() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override
    public void onBackPressed() {
        // Swallow BACK so a stray gamepad B can't close the panel mid-game;
        // the game closes it when it stops.
    }

    @Override
    protected void onDestroy() {
        if (focusAnchor != null) {
            try {
                getWindowManager().removeView(focusAnchor);
            } catch (RuntimeException ignored) {
            }
            focusAnchor = null;
        }
        WeakReference<SecondScreenPanelActivity> ref = instance;
        if (ref != null && ref.get() == this) instance = null;
        super.onDestroy();
    }
}
