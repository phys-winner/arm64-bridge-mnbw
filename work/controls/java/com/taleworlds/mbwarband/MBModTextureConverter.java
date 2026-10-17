package com.taleworlds.mbwarband;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Application;
import android.content.DialogInterface;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;

/** Launch gate for user-installed module textures. All file and GPU work runs off the UI. */
public final class MBModTextureConverter {
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final ExecutorService WORKER = Executors.newSingleThreadExecutor(
            new ThreadFactory() {
                @Override public Thread newThread(Runnable action) {
                    Thread thread = new Thread(action, "MBModTextures");
                    thread.setDaemon(true);
                    return thread;
                }
            });
    private static Session active;
    private static int supportedTarget;

    static native int nativeInspect(String path, int target);
    static native String nativeConvert(String path, int target);

    private static final MBModTexturePlan.Codec CODEC = new MBModTexturePlan.Codec() {
        @Override public int inspect(File file, int target) {
            return nativeInspect(file.getPath(), target);
        }

        @Override public void convert(File file, int target) throws IOException {
            String error = nativeConvert(file.getPath(), target);
            if (error != null) throw new IOException(file.getName() + ": " + error);
        }
    };

    private MBModTextureConverter() {}

    /** Native/base-game selection passes null and never creates conversion files. */
    public static void prepare(final Activity activity, final File moduleDirectory,
                               final String language, final Runnable onReady,
                               final Runnable onCancel) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            MAIN.post(new Runnable() {
                @Override public void run() {
                    prepare(activity, moduleDirectory, language, onReady, onCancel);
                }
            });
            return;
        }
        if (activity.isFinishing() || activity.isDestroyed()) return;
        if (moduleDirectory == null) {
            if (onReady != null) onReady.run();
            return;
        }
        if (active != null) active.abandon();
        Session session = new Session(activity, moduleDirectory, language, onReady, onCancel);
        active = session;
        session.startScan();
    }

    private static final class Control implements MBModTexturePlan.Cancellation {
        private boolean cancelled;
        private boolean foreground;

        Control(boolean foreground) { this.foreground = foreground; }

        synchronized void foreground(boolean value) {
            foreground = value;
            notifyAll();
        }

        synchronized void cancel() {
            cancelled = true;
            notifyAll();
        }

        synchronized boolean cancelled() { return cancelled; }

        @Override public synchronized boolean isCancelled() {
            while (!foreground && !cancelled) {
                try { wait(); }
                catch (InterruptedException interrupted) {
                    cancelled = true;
                    Thread.currentThread().interrupt();
                }
            }
            return cancelled;
        }
    }

    private static final class Session implements Application.ActivityLifecycleCallbacks,
            MBModTexturePlan.Progress {
        private static final int WORKING = 0;
        private static final int ASK = 1;
        private static final int ERROR = 2;
        private static final int READY = 3;
        private static final int CANCELLED = 4;
        final WeakReference<Activity> owner;
        final Application application;
        final File directory;
        final String language;
        final Control control;
        final boolean addedKeepScreenOn;
        Runnable onReady;
        Runnable onCancel;
        boolean foreground;
        boolean destroyed;
        boolean workerBusy;
        boolean registered = true;
        int state = WORKING;
        int dialogState = -1;
        MBModTexturePlan plan;
        String error;
        AlertDialog dialog;
        TextView statusView;
        TextView timeView;
        TextView fileView;
        ProgressBar progressView;
        volatile int phase = MBModTexturePlan.SCAN;
        volatile int completed;
        volatile int total;
        volatile String currentFile = "";
        long lastProgress;
        volatile long scanStartedAt;
        volatile long scanElapsedMs;
        volatile long conversionStartedAt;
        volatile long conversionElapsedMs;
        volatile long verifyStartedAt;

        Session(Activity activity, File directory, String language,
                Runnable onReady, Runnable onCancel) {
            owner = new WeakReference<Activity>(activity);
            application = activity.getApplication();
            this.directory = directory;
            this.language = MBTouchStrings.normalize(language);
            this.onReady = onReady;
            this.onCancel = onCancel;
            // A gate called from onCreate waits for onResume. A menu action is already visible.
            foreground = activity.getWindow().getDecorView().isShown();
            control = new Control(foreground);
            addedKeepScreenOn = (activity.getWindow().getAttributes().flags
                    & WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) == 0;
            activity.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            application.registerActivityLifecycleCallbacks(this);
        }

        String tr(MBModTextureStrings.Text text) {
            return MBModTextureStrings.get(language, text);
        }

        void startScan() {
            state = WORKING;
            phase = MBModTexturePlan.SCAN;
            completed = total = 0;
            currentFile = "";
            scanStartedAt = SystemClock.elapsedRealtime();
            scanElapsedMs = 0L;
            conversionElapsedMs = 0L;
            workerBusy = true;
            render();
            WORKER.execute(new Runnable() {
                @Override public void run() {
                    try {
                        if (control.isCancelled()) throw new MBModTexturePlan.Cancelled();
                        System.loadLibrary("MBExpMobile");
                        int target = chooseTarget();
                        final MBModTexturePlan scanned = MBModTexturePlan.scan(
                                directory, target, CODEC, control, Session.this);
                        scanElapsedMs = Math.max(1L,
                                SystemClock.elapsedRealtime() - scanStartedAt);
                        if (scanned.pending == 0) {
                            scanned.publish(control);
                        } else {
                            MBModTexturePlan.clearCompletion(scanned.root);
                        }
                        MAIN.post(new Runnable() {
                            @Override public void run() {
                                workerBusy = false;
                                plan = scanned;
                                state = control.cancelled() ? CANCELLED
                                        : scanned.pending == 0 ? READY : ASK;
                                render();
                            }
                        });
                    } catch (IOException failure) { failed(failure); }
                    catch (RuntimeException failure) { failed(failure); }
                    catch (LinkageError failure) { failed(failure); }
                }
            });
        }

        void startConversion() {
            dismissDialog();
            state = WORKING;
            phase = MBModTexturePlan.CONVERT;
            completed = plan.verified;
            total = plan.pending + plan.verified;
            currentFile = "";
            conversionStartedAt = SystemClock.elapsedRealtime();
            verifyStartedAt = 0L;
            workerBusy = true;
            render();
            WORKER.execute(new Runnable() {
                @Override public void run() {
                    try {
                        plan.convert(CODEC, control, Session.this);
                        MAIN.post(new Runnable() {
                            @Override public void run() {
                                workerBusy = false;
                                state = control.cancelled() ? CANCELLED : READY;
                                render();
                            }
                        });
                    } catch (IOException failure) { failed(failure); }
                    catch (RuntimeException failure) { failed(failure); }
                    catch (LinkageError failure) { failed(failure); }
                }
            });
        }

        void failed(final Throwable failure) {
            MAIN.post(new Runnable() {
                @Override public void run() {
                    workerBusy = false;
                    if (failure instanceof MBModTexturePlan.Cancelled || control.cancelled()) {
                        state = CANCELLED;
                    } else {
                        state = ERROR;
                        error = explain(failure);
                    }
                    render();
                }
            });
        }

        @Override public void update(int phase, int completed, int total, String file) {
            if (phase == MBModTexturePlan.VERIFY && this.phase != phase) {
                long now = SystemClock.elapsedRealtime();
                conversionElapsedMs = Math.max(1L, now - conversionStartedAt);
                verifyStartedAt = now;
            }
            this.phase = phase;
            this.completed = completed;
            this.total = total;
            currentFile = file;
            long now = SystemClock.elapsedRealtime();
            if (now - lastProgress < 100L && completed != total) return;
            lastProgress = now;
            MAIN.post(new Runnable() {
                @Override public void run() { if (state == WORKING) render(); }
            });
        }

        void render() {
            if (destroyed) return;
            Activity activity = owner.get();
            if (activity == null || activity.isFinishing() || activity.isDestroyed()) {
                abandon();
                return;
            }
            if (!foreground) return;
            if (state == READY || state == CANCELLED) {
                finish(state == READY && !control.cancelled());
                return;
            }
            if (state == WORKING) {
                if (phase != MBModTexturePlan.SCAN) renderProgress(activity);
            } else if (dialogState != state) {
                dismissDialog();
                if (state == ASK) showConfirmation(activity);
                else showError(activity);
            }
        }

        void renderProgress(Activity activity) {
            if (dialogState != WORKING) {
                dismissDialog();
                LinearLayout layout = new LinearLayout(activity);
                layout.setOrientation(LinearLayout.VERTICAL);
                int padding = (int) (24 * activity.getResources().getDisplayMetrics().density);
                layout.setPadding(padding, padding / 2, padding, padding / 2);
                statusView = new TextView(activity);
                statusView.setTextSize(16);
                layout.addView(statusView);
                progressView = new ProgressBar(activity, null,
                        android.R.attr.progressBarStyleHorizontal);
                progressView.setMax(1000);
                layout.addView(progressView, new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.MATCH_PARENT,
                        LinearLayout.LayoutParams.WRAP_CONTENT));
                timeView = new TextView(activity);
                timeView.setTextSize(13);
                layout.addView(timeView);
                fileView = new TextView(activity);
                fileView.setTextSize(13);
                fileView.setMaxLines(3);
                layout.addView(fileView);
                dialog = new AlertDialog.Builder(activity)
                        .setTitle(tr(MBModTextureStrings.Text.PREPARING_TITLE))
                        .setView(layout)
                        .setNegativeButton(MBTouchStrings.get(language,
                                MBTouchStrings.Text.CANCEL), null).create();
                dialog.setCanceledOnTouchOutside(false);
                dialog.setOnCancelListener(new DialogInterface.OnCancelListener() {
                    @Override public void onCancel(DialogInterface ignored) { requestCancel(); }
                });
                dialog.show();
                dialog.getButton(DialogInterface.BUTTON_NEGATIVE).setOnClickListener(
                        new android.view.View.OnClickListener() {
                            @Override public void onClick(android.view.View view) { requestCancel(); }
                        });
                dialogState = WORKING;
            }
            boolean cancelling = control.cancelled();
            int count = Math.max(0, Math.min(completed, total));
            int percent = total == 0 ? 0 : (int) ((100L * count) / total);
            String label = phase == MBModTexturePlan.CONVERT
                    ? tr(MBModTextureStrings.Text.CONVERTING)
                    : phase == MBModTexturePlan.VERIFY
                    ? tr(MBModTextureStrings.Text.VERIFYING)
                    : tr(MBModTextureStrings.Text.SCANNING);
            if (cancelling) {
                statusView.setText(tr(MBModTextureStrings.Text.STOPPING));
            } else {
                statusView.setText(label + (total > 0
                        ? ": " + count + " / " + total + " (" + percent + "%)" : "…"));
            }
            progressView.setIndeterminate(total == 0);
            if (total != 0) progressView.setProgress((int) ((1000L * count) / total));
            timeView.setText(timeEstimate());
            fileView.setText(currentFile);
            dialog.getButton(DialogInterface.BUTTON_NEGATIVE).setEnabled(!cancelling);
        }

        String timeEstimate() {
            if (phase == MBModTexturePlan.SCAN || plan == null) {
                return tr(MBModTextureStrings.Text.TIME_AFTER_SCAN);
            }
            long now = SystemClock.elapsedRealtime();
            long expectedTotal;
            long remaining;
            if (phase == MBModTexturePlan.CONVERT) {
                int converted = Math.max(0, completed - plan.verified);
                long elapsed = Math.max(0L, now - conversionStartedAt);
                if (converted < 2 || elapsed < 1500L) {
                    return tr(MBModTextureStrings.Text.TIME_MEASURING);
                }
                double perFile = (double) elapsed / converted;
                long conversionTotal = Math.round(perFile * plan.pending);
                long verification = Math.max(1000L, scanElapsedMs);
                expectedTotal = scanElapsedMs + conversionTotal + verification;
                remaining = Math.max(0L, conversionTotal - elapsed) + verification;
            } else {
                long elapsed = Math.max(0L, now - verifyStartedAt);
                long verification = Math.max(1000L, scanElapsedMs);
                if (completed >= 2 && elapsed >= 1500L && total > 0) {
                    verification = Math.max(elapsed,
                            Math.round((double) elapsed * total / completed));
                }
                expectedTotal = scanElapsedMs + conversionElapsedMs + verification;
                remaining = Math.max(0L, verification - elapsed);
            }
            return MBModTextureStrings.estimate(language, expectedTotal, remaining);
        }

        void showConfirmation(Activity activity) {
            String format = plan.target == MBModTexturePlan.ASTC ? "ASTC"
                    : tr(MBModTextureStrings.Text.ETC1_ALPHA);
            String size = String.format(Locale.ROOT, "%.1f", plan.pendingBytes / 1048576.0);
            String message = directory.getName() + "\n\n"
                    + tr(MBModTextureStrings.Text.TO_CONVERT)
                    + plan.pending + " (" + size + " MiB).\n"
                    + tr(MBModTextureStrings.Text.DEVICE_FORMAT) + format + ".\n"
                    + (plan.verified > 0 ? tr(MBModTextureStrings.Text.ALREADY_READY)
                       + plan.verified + ".\n" : "")
                    + "\n" + tr(MBModTextureStrings.Text.REPLACE_WARNING)
                    + "\n\n" + tr(MBModTextureStrings.Text.PROCESS_NOTE);
            dialog = new AlertDialog.Builder(activity)
                    .setTitle(tr(MBModTextureStrings.Text.CONFIRM_TITLE))
                    .setMessage(message)
                    .setPositiveButton(tr(MBModTextureStrings.Text.CONVERT),
                            new DialogInterface.OnClickListener() {
                                @Override public void onClick(DialogInterface ignored, int which) {
                                    startConversion();
                                }
                            })
                    .setNegativeButton(MBTouchStrings.get(language,
                            MBTouchStrings.Text.CANCEL),
                            new DialogInterface.OnClickListener() {
                                @Override public void onClick(DialogInterface ignored, int which) {
                                    requestCancel();
                                }
                            }).create();
            dialog.setCanceledOnTouchOutside(false);
            dialog.setOnCancelListener(new DialogInterface.OnCancelListener() {
                @Override public void onCancel(DialogInterface ignored) { requestCancel(); }
            });
            dialog.show();
            dialogState = ASK;
        }

        void showError(Activity activity) {
            dialog = new AlertDialog.Builder(activity)
                    .setTitle(tr(MBModTextureStrings.Text.FAIL_TITLE))
                    .setMessage(error + "\n\n" + tr(MBModTextureStrings.Text.FAIL_NOTE))
                    .setPositiveButton(tr(MBModTextureStrings.Text.CLOSE),
                            new DialogInterface.OnClickListener() {
                                @Override public void onClick(DialogInterface ignored, int which) {
                                    requestCancel();
                                }
                            }).create();
            dialog.setCanceledOnTouchOutside(false);
            dialog.setOnCancelListener(new DialogInterface.OnCancelListener() {
                @Override public void onCancel(DialogInterface ignored) { requestCancel(); }
            });
            dialog.show();
            dialogState = ERROR;
        }

        String explain(Throwable failure) {
            if (failure instanceof MBModTexturePlan.Problem) {
                MBModTexturePlan.Problem problem = (MBModTexturePlan.Problem) failure;
                String reason = problem.reason;
                String message;
                if (reason.equals("inventory_changed")) {
                    message = tr(MBModTextureStrings.Text.INVENTORY_CHANGED);
                } else if (reason.equals("invalid_texture") || reason.equals("verification_failed")) {
                    message = tr(MBModTextureStrings.Text.INVALID_TEXTURE);
                } else if (reason.equals("unsafe_path")) {
                    message = tr(MBModTextureStrings.Text.UNSAFE_PATH);
                } else if (reason.equals("too_many_files") || reason.equals("texture_too_large")) {
                    message = tr(MBModTextureStrings.Text.LIMIT_EXCEEDED);
                } else {
                    message = tr(MBModTextureStrings.Text.IO_ERROR);
                }
                return message + (problem.file.length() == 0 ? "" : "\n\n" + problem.file);
            }
            if (failure instanceof LinkageError) {
                return tr(MBModTextureStrings.Text.CONVERTER_MISSING);
            }
            if ("gpu_probe_failed".equals(failure.getMessage())) {
                return tr(MBModTextureStrings.Text.GPU_PROBE_FAILED);
            }
            if ("gpu_format_unavailable".equals(failure.getMessage())) {
                return tr(MBModTextureStrings.Text.GPU_UNAVAILABLE);
            }
            return tr(MBModTextureStrings.Text.GENERIC_ERROR);
        }

        void requestCancel() {
            control.cancel();
            if (!workerBusy) state = CANCELLED;
            render();
        }

        void finish(boolean ready) {
            Runnable callback = ready ? onReady : onCancel;
            detach();
            if (callback != null) callback.run();
        }

        void abandon() {
            control.cancel();
            detach();
        }

        void detach() {
            destroyed = true;
            dismissDialog();
            if (registered) {
                application.unregisterActivityLifecycleCallbacks(this);
                registered = false;
            }
            Activity activity = owner.get();
            if (addedKeepScreenOn && activity != null) {
                activity.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            }
            onReady = null;
            onCancel = null;
            owner.clear();
            if (active == this) active = null;
        }

        void dismissDialog() {
            if (dialog != null) {
                dialog.setOnCancelListener(null);
                dialog.dismiss();
            }
            dialog = null;
            dialogState = -1;
        }

        @Override public void onActivityResumed(Activity activity) {
            if (activity == owner.get()) {
                foreground = true;
                control.foreground(true);
                render();
            }
        }

        @Override public void onActivityPaused(Activity activity) {
            if (activity == owner.get()) {
                foreground = false;
                control.foreground(false);
            }
        }

        @Override public void onActivityDestroyed(Activity activity) {
            if (activity == owner.get()) abandon();
        }

        @Override public void onActivityCreated(Activity activity, Bundle state) {}
        @Override public void onActivityStarted(Activity activity) {}
        @Override public void onActivityStopped(Activity activity) {}
        @Override public void onActivitySaveInstanceState(Activity activity, Bundle state) {}
    }

    /** Probe the real driver's ES2 context, matching the legacy texture upload contract. */
    private static int chooseTarget() throws IOException {
        if (supportedTarget != 0) return supportedTarget;
        EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (display == EGL14.EGL_NO_DISPLAY) throw new IOException("gpu_probe_failed");
        EGLContext context = EGL14.EGL_NO_CONTEXT;
        EGLSurface surface = EGL14.EGL_NO_SURFACE;
        boolean current = false;
        try {
            int[] version = new int[2];
            if (!EGL14.eglInitialize(display, version, 0, version, 1)) {
                throw new IOException("gpu_probe_failed");
            }
            int[] attributes = {EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                    EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                    EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8,
                    EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_NONE};
            EGLConfig[] configs = new EGLConfig[1];
            int[] count = new int[1];
            if (!EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0)
                    || count[0] == 0) throw new IOException("gpu_probe_failed");
            context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                    new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            surface = EGL14.eglCreatePbufferSurface(display, configs[0],
                    new int[] {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
            if (context == EGL14.EGL_NO_CONTEXT || surface == EGL14.EGL_NO_SURFACE
                    || !EGL14.eglMakeCurrent(display, surface, surface, context)) {
                throw new IOException("gpu_probe_failed");
            }
            current = true;
            String extensions = GLES20.glGetString(GLES20.GL_EXTENSIONS);
            if (extensions == null) throw new IOException("gpu_probe_failed");
            String padded = " " + extensions + " ";
            boolean astc = padded.contains(" GL_KHR_texture_compression_astc_ldr ")
                    || padded.contains(" GL_KHR_texture_compression_astc_hdr ")
                    || padded.contains(" GL_OES_texture_compression_astc ");
            boolean etc1 = padded.contains(" GL_OES_compressed_ETC1_RGB8_texture ");
            int[] formatsCount = new int[1];
            GLES20.glGetIntegerv(GLES20.GL_NUM_COMPRESSED_TEXTURE_FORMATS, formatsCount, 0);
            if (formatsCount[0] > 0 && formatsCount[0] < 1024) {
                int[] formats = new int[formatsCount[0]];
                GLES20.glGetIntegerv(GLES20.GL_COMPRESSED_TEXTURE_FORMATS, formats, 0);
                for (int format : formats) {
                    if (format == 0x93B0) astc = true; // RGBA ASTC 4x4 LDR.
                    if (format == 0x8D64) etc1 = true;
                }
            }
            if (GLES20.glGetError() != GLES20.GL_NO_ERROR) throw new IOException("gpu_probe_failed");
            supportedTarget = astc ? MBModTexturePlan.ASTC : etc1 ? MBModTexturePlan.ETC1 : 0;
            if (supportedTarget == 0) throw new IOException("gpu_format_unavailable");
            return supportedTarget;
        } finally {
            if (current) EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE,
                    EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (surface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface);
            if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context);
            EGL14.eglReleaseThread();
            // The game may already own this process-wide EGLDisplay. Do not terminate it.
            // Only one successful probe is initialized per process; its context is destroyed.
        }
    }
}
