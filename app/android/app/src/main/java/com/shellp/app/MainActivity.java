package com.shellp.app;

import android.app.NativeActivity;
import android.os.Build;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.inputmethod.InputMethodManager;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

public class MainActivity extends NativeActivity {
    private static final String TAG = "shellp_activity";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Register modern Android 13+ (API 33+) Predictive Back Invoked Callback
        // Prevents Samsung keyboard close button and back gestures from calling finish()
        if (Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                new OnBackInvokedCallback() {
                    @Override
                    public void onBackInvoked() {
                        handleBackAction();
                    }
                }
            );
        }
    }

    private void handleBackAction() {
        if (isKeyboardVisible()) {
            hideSoftKeyboard();
        } else {
            // Background the activity cleanly; NEVER finish() or kill the Linux subsystem
            moveTaskToBack(true);
        }
    }

    public boolean isKeyboardVisible() {
        View decor = getWindow().getDecorView();
        if (decor != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsets insets = decor.getRootWindowInsets();
            if (insets != null) {
                return insets.isVisible(WindowInsets.Type.ime());
            }
        }
        return false;
    }

    public void hideSoftKeyboard() {
        View decor = getWindow().getDecorView();
        if (decor != null) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                WindowInsetsController controller = decor.getWindowInsetsController();
                if (controller != null) {
                    controller.hide(WindowInsets.Type.ime());
                    return;
                }
            }
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.hideSoftInputFromWindow(decor.getWindowToken(), 0);
            }
        }
    }

    public void showSoftKeyboard() {
        View decor = getWindow().getDecorView();
        if (decor != null) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                WindowInsetsController controller = decor.getWindowInsetsController();
                if (controller != null) {
                    controller.show(WindowInsets.Type.ime());
                    return;
                }
            }
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.showSoftInput(decor, 0);
            }
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_UP) {
                handleBackAction();
            }
            // Always consume KEYCODE_BACK so Android never triggers default Activity.finish()
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public void onBackPressed() {
        handleBackAction();
        // NEVER call super.onBackPressed() which finishes the activity
    }
}
