package com.taleworlds.mbwarband;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/**
 * Runs in a tiny process separate from the native game. The module root is a
 * boot-time VFS decision, so switching modules must create a fresh game
 * process; an in-process Activity recreation would retain the old guest VM.
 */
public final class MBRestartActivity extends Activity {
    private static final long RESTART_DELAY_MS = 650L;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override public void run() {
                Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
                if (launch != null) {
                    launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                            | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                    startActivity(launch);
                }
                if (android.os.Build.VERSION.SDK_INT >= 21) {
                    finishAndRemoveTask();
                } else {
                    finish();
                }
            }
        }, RESTART_DELAY_MS);
    }
}
