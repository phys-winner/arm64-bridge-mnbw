package com.taleworlds.mbwarband;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.os.Bundle;
import android.view.WindowManager;

import java.util.Locale;

/** Checks the selected module before the native engine opens any of its textures. */
public final class MBLaunchActivity extends Activity {
    private boolean launching;

    @Override protected void onCreate(Bundle savedState) {
        super.onCreate(savedState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(5894);
        final String language = MBTouchStrings.normalizeBcp47(
                Locale.getDefault().toLanguageTag());
        getWindow().getDecorView().post(new Runnable() {
            @Override public void run() {
                if (isFinishing() || isDestroyed()) return;
                try {
                    MBModManager.Selection selection = MBModManager.resolve(getExternalFilesDir(null));
                    MBModTextureConverter.prepare(MBLaunchActivity.this,
                            selection.moduleDirectory, language, new Runnable() {
                                @Override public void run() { launchGame(); }
                            }, new Runnable() {
                                @Override public void run() { showCancelled(language); }
                            });
                } catch (RuntimeException error) {
                    new AlertDialog.Builder(MBLaunchActivity.this)
                            .setTitle(tr(language, MBModTextureStrings.Text.OPEN_FAILED))
                            .setMessage(tr(language, MBModTextureStrings.Text.OPEN_FAILED_NOTE))
                            .setPositiveButton(tr(language, MBModTextureStrings.Text.CLOSE),
                                    new DialogInterface.OnClickListener() {
                                        @Override public void onClick(DialogInterface dialog, int which) {
                                            finish();
                                        }
                                    }).setOnCancelListener(new DialogInterface.OnCancelListener() {
                                        @Override public void onCancel(DialogInterface dialog) { finish(); }
                                    }).show();
                }
            }
        });
    }

    private static String tr(String language, MBModTextureStrings.Text text) {
        return MBModTextureStrings.get(language, text);
    }

    private void showCancelled(String language) {
        if (isFinishing() || isDestroyed()) return;
        new AlertDialog.Builder(this)
                .setTitle(tr(language, MBModTextureStrings.Text.LAUNCH_CANCELLED))
                .setMessage(tr(language, MBModTextureStrings.Text.RESUME_NOTE))
                .setPositiveButton(tr(language, MBModTextureStrings.Text.BASE_GAME),
                        new DialogInterface.OnClickListener() {
                            @Override public void onClick(DialogInterface dialog, int which) {
                                if (MBModManager.select(getExternalFilesDir(null), MBModManager.BASE_MODULE)) {
                                    launchGame();
                                } else {
                                    finish();
                                }
                            }
                        })
                .setNegativeButton(tr(language, MBModTextureStrings.Text.CLOSE),
                        new DialogInterface.OnClickListener() {
                            @Override public void onClick(DialogInterface dialog, int which) { finish(); }
                        }).setOnCancelListener(new DialogInterface.OnCancelListener() {
                            @Override public void onCancel(DialogInterface dialog) { finish(); }
                        }).show();
    }

    private void launchGame() {
        if (launching || isFinishing() || isDestroyed()) return;
        launching = true;
        Intent game = new Intent().setClassName(this,
                "com.taleworlds.mbwarband.MBNativeActivity");
        game.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(game);
        finish();
    }
}
