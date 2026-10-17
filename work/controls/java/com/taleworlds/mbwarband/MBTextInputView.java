package com.taleworlds.mbwarband;

import android.annotation.TargetApi;
import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.os.Build;
import android.text.Editable;
import android.text.InputType;
import android.text.Selection;
import android.view.Gravity;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.CompletionInfo;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;
import android.view.inputmethod.InputMethodManager;
import android.view.inputmethod.TextAttribute;
import android.widget.EditText;
import android.widget.FrameLayout;

/** A transparent one-pixel editor that gives Android IMEs a real InputConnection. */
public final class MBTextInputView extends EditText {
    private static final String VIEW_TAG =
            "com.taleworlds.mbwarband.MBTextInputView.text-editor";
    private static final int UI_ATTACH = 1;
    private static final int UI_SHOW = 2;
    private static final int UI_HIDE = 3;
    private static final int INPUT_TYPE = InputType.TYPE_CLASS_TEXT
            | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
    private static final int IME_OPTIONS = EditorInfo.IME_ACTION_DONE
            | EditorInfo.IME_FLAG_NO_EXTRACT_UI
            | EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING;

    private MBTextInputViewLogic session;
    private long sessionToken;

    private static native long nativeBeginTextSession();
    private static native boolean nativeApplyTextEdit(
            long token, int deleteBeforeCodePoints, String insertedText, boolean submit);
    private static native void nativeEndTextSession(long token);

    private static final class UiAction implements Runnable {
        private final Activity activity;
        private final int action;

        UiAction(Activity activity, int action) {
            this.activity = activity;
            this.action = action;
        }

        @Override
        public void run() {
            if (action != UI_HIDE && (activity.isFinishing() || activity.isDestroyed())) return;
            if (action == UI_ATTACH) {
                ensureAttached(activity);
            } else if (action == UI_SHOW) {
                MBTextInputView view = ensureAttached(activity);
                if (view != null) view.showOnUiThread();
            } else if (action == UI_HIDE) {
                MBTextInputView view = findAttached(activity);
                if (view != null) view.hideOnUiThread();
            }
        }
    }

    private static final class NativeSink implements MBTextInputViewLogic.Sink {
        private final MBTextInputView owner;
        private final long token;

        NativeSink(MBTextInputView owner, long token) {
            this.owner = owner;
            this.token = token;
        }

        @Override
        public boolean apply(int deleteBeforeCodePoints, String insertedText, boolean submit) {
            return owner.applyNative(token, deleteBeforeCodePoints, insertedText, submit);
        }
    }

    private static class BridgeInputConnection extends InputConnectionWrapper {
        final MBTextInputView owner;
        final MBTextInputViewLogic logic;

        BridgeInputConnection(InputConnection target, MBTextInputView owner,
                              MBTextInputViewLogic logic) {
            super(target, false);
            this.owner = owner;
            this.logic = logic;
        }

        private boolean current() {
            return owner.isCurrentSession(logic);
        }

        private boolean finishOperation(boolean accepted) {
            if (!current()) return false;
            if (!accepted) {
                owner.restartAfterReject(logic);
                return false;
            }
            owner.syncEditor(logic);
            int composingStart = logic.composingStart();
            if (composingStart >= 0) {
                super.setComposingRegion(composingStart, logic.editorText().length());
            } else {
                super.finishComposingText();
            }
            int appendCursor = logic.editorText().length();
            super.setSelection(appendCursor, appendCursor);
            return true;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            if (!current()) return false;
            return finishOperation(logic.commitText(text == null ? null : text.toString()));
        }

        @Override
        public boolean commitCompletion(CompletionInfo completion) {
            if (!current() || completion == null) return false;
            return commitText(completion.getText(), 1);
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            if (!current()) return false;
            return finishOperation(logic.setComposingText(
                    text == null ? null : text.toString()));
        }

        @Override
        public boolean setComposingRegion(int start, int end) {
            if (!current()) return false;
            return finishOperation(logic.setComposingRegion(start, end));
        }

        @Override
        public boolean finishComposingText() {
            if (!current()) return false;
            return finishOperation(logic.finishComposingText());
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            if (!current() || beforeLength < 0 || afterLength < 0) return false;
            // All accepted scalars are BMP, so UTF-16 units equal guest code points.
            // The guest editor is append-only: deleting after its fixed end cursor is a no-op.
            return finishOperation(logic.deleteBeforeCursor(beforeLength));
        }

        @Override
        public boolean deleteSurroundingTextInCodePoints(
                int beforeLength, int afterLength) {
            if (!current() || beforeLength < 0 || afterLength < 0) return false;
            return finishOperation(logic.deleteBeforeCursor(beforeLength));
        }

        @Override
        public boolean setSelection(int start, int end) {
            if (!current()) return false;
            int appendCursor = logic.editorText().length();
            return super.setSelection(appendCursor, appendCursor);
        }

        @Override
        public boolean performEditorAction(int actionCode) {
            if (!current()) return false;
            if (actionCode != EditorInfo.IME_ACTION_DONE) {
                return super.performEditorAction(actionCode);
            }
            return finishOperation(logic.submit());
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            if (!current()) return false;
            if (owner.handleInputKeyEvent(logic, event)) return true;
            return super.sendKeyEvent(event);
        }
    }

    /** Android 13 added attributed overloads which must take the same guarded path. */
    @TargetApi(33)
    private static final class Api33BridgeInputConnection extends BridgeInputConnection {
        Api33BridgeInputConnection(InputConnection target, MBTextInputView owner,
                                   MBTextInputViewLogic logic) {
            super(target, owner, logic);
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition,
                                  TextAttribute textAttribute) {
            return commitText(text, newCursorPosition);
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition,
                                        TextAttribute textAttribute) {
            return setComposingText(text, newCursorPosition);
        }

        @Override
        public boolean setComposingRegion(int start, int end, TextAttribute textAttribute) {
            return setComposingRegion(start, end);
        }
    }

    public MBTextInputView(Context context) {
        super(context);
        setSingleLine(true);
        setInputType(INPUT_TYPE);
        setImeOptions(IME_OPTIONS);
        setTextIsSelectable(false);
        setFocusable(true);
        setFocusableInTouchMode(true);
        setClickable(false);
        setLongClickable(false);
        setContextClickable(false);
        setCursorVisible(false);
        setSaveEnabled(false);
        setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        // Autofill was added in API 26; keep the invocation out of the API-25
        // execution path so the old framework never resolves a missing method.
        if (Build.VERSION.SDK_INT >= 26) {
            setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO);
        }
        setBackground(null);
        setTextColor(Color.TRANSPARENT);
        setHintTextColor(Color.TRANSPARENT);
        setHighlightColor(Color.TRANSPARENT);
        setPadding(0, 0, 0, 0);
        setTextSize(1.0f);
        setAlpha(0.0f);
        setVisibility(View.INVISIBLE);
    }

    private static void dispatch(Activity activity, int action) {
        if (activity != null) activity.runOnUiThread(new UiAction(activity, action));
    }

    /** Adds one editor to the Activity decor. Safe to call repeatedly or off the UI thread. */
    public static void attach(Activity activity) {
        dispatch(activity, UI_ATTACH);
    }

    /** Begins/focuses one session and asks the IME to show. Repeated calls reuse the token. */
    public static void show(Activity activity) {
        dispatch(activity, UI_SHOW);
    }

    /** Ends only the current session and hides the IME. Repeated calls are harmless. */
    public static void hide(Activity activity) {
        dispatch(activity, UI_HIDE);
    }

    private static MBTextInputView findAttached(Activity activity) {
        Window window = activity.getWindow();
        if (window == null) return null;
        View candidate = window.getDecorView().findViewWithTag(VIEW_TAG);
        return candidate instanceof MBTextInputView ? (MBTextInputView) candidate : null;
    }

    private static MBTextInputView ensureAttached(Activity activity) {
        MBTextInputView existing = findAttached(activity);
        if (existing != null) return existing;
        if (activity.isFinishing() || activity.isDestroyed()) return null;

        MBTextInputView created = new MBTextInputView(activity);
        created.setTag(VIEW_TAG);
        FrameLayout.LayoutParams layout = new FrameLayout.LayoutParams(
                1, 1, Gravity.TOP | Gravity.LEFT);
        activity.addContentView(created, layout);
        return created;
    }

    private InputMethodManager inputMethodManager() {
        Object service = getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        return service instanceof InputMethodManager ? (InputMethodManager) service : null;
    }

    private long beginNativeSession() {
        try {
            return nativeBeginTextSession();
        } catch (UnsatisfiedLinkError unavailable) {
            return 0L;
        }
    }

    private boolean applyNative(long token, int deleteBeforeCodePoints,
                                String insertedText, boolean submit) {
        if (token == 0L || token != sessionToken || session == null) return false;
        try {
            return nativeApplyTextEdit(token, deleteBeforeCodePoints, insertedText, submit);
        } catch (UnsatisfiedLinkError unavailable) {
            return false;
        }
    }

    private void endNativeSession(long token) {
        if (token == 0L) return;
        try {
            nativeEndTextSession(token);
        } catch (UnsatisfiedLinkError unavailable) {
            // The Java half may be built before the matching bridge half is merged.
        }
    }

    private boolean isCurrentSession(MBTextInputViewLogic candidate) {
        return candidate != null && candidate == session && candidate.isActive()
                && sessionToken != 0L;
    }

    private void showOnUiThread() {
        if (session == null) {
            long token = beginNativeSession();
            if (token == 0L) return;
            sessionToken = token;
            session = new MBTextInputViewLogic(new NativeSink(this, token));
            syncEditor(session);
        }

        setVisibility(View.VISIBLE);
        if (!requestFocus()) {
            endSession();
            setVisibility(View.INVISIBLE);
            return;
        }
        InputMethodManager manager = inputMethodManager();
        if (manager == null) {
            endSession();
            clearFocus();
            setVisibility(View.INVISIBLE);
            return;
        }
        manager.restartInput(this);
        manager.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
    }

    private void hideOnUiThread() {
        endSession();
        InputMethodManager manager = inputMethodManager();
        if (manager != null && getWindowToken() != null) {
            manager.hideSoftInputFromWindow(getWindowToken(), 0);
        }
        clearFocus();
        setVisibility(View.INVISIBLE);
    }

    private void endSession() {
        MBTextInputViewLogic oldSession = session;
        long oldToken = sessionToken;
        session = null;
        sessionToken = 0L;
        if (oldSession != null) oldSession.close();
        clearEditor();
        endNativeSession(oldToken);
    }

    private void clearEditor() {
        Editable editable = getText();
        if (editable == null) return;
        BaseInputConnection.removeComposingSpans(editable);
        editable.clear();
        Selection.setSelection(editable, 0);
    }

    private void syncEditor(MBTextInputViewLogic logic) {
        if (!isCurrentSession(logic)) return;
        Editable editable = getText();
        if (editable == null) return;
        BaseInputConnection.removeComposingSpans(editable);
        editable.replace(0, editable.length(), logic.editorText());
        Selection.setSelection(editable, editable.length());
    }

    private void restartAfterReject(MBTextInputViewLogic logic) {
        if (!isCurrentSession(logic)) return;
        syncEditor(logic);
        InputMethodManager manager = inputMethodManager();
        if (manager != null) manager.restartInput(this);
    }

    private boolean finishKeyOperation(MBTextInputViewLogic logic, boolean accepted) {
        if (accepted) syncEditor(logic);
        else restartAfterReject(logic);
        // A rejected text key is still consumed so it cannot re-enter through AInputQueue.
        return true;
    }

    @SuppressWarnings("deprecation")
    private boolean handleInputKeyEvent(MBTextInputViewLogic logic, KeyEvent event) {
        if (!isCurrentSession(logic) || event == null) return false;
        int action = event.getAction();
        int keyCode = event.getKeyCode();

        if (action == KeyEvent.ACTION_MULTIPLE) {
            String characters = event.getCharacters();
            if (characters == null || characters.length() == 0) return false;
            return finishKeyOperation(logic, logic.commitKeyText(characters));
        }

        boolean deleteKey = keyCode == KeyEvent.KEYCODE_DEL
                || keyCode == KeyEvent.KEYCODE_FORWARD_DEL;
        boolean doneKey = keyCode == KeyEvent.KEYCODE_ENTER
                || keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER;
        int unicode = event.getUnicodeChar(event.getMetaState());
        boolean textKey = unicode != 0;
        if (action == KeyEvent.ACTION_UP) return deleteKey || doneKey || textKey;
        if (action != KeyEvent.ACTION_DOWN) return false;

        if (keyCode == KeyEvent.KEYCODE_DEL) {
            return finishKeyOperation(logic, logic.deleteBeforeCursor(1));
        }
        if (keyCode == KeyEvent.KEYCODE_FORWARD_DEL) return true;
        if (doneKey) return finishKeyOperation(logic, logic.submit());
        if (!textKey) return false;
        if ((unicode & KeyCharacterMap.COMBINING_ACCENT) != 0
                || !Character.isValidCodePoint(unicode)) {
            return true;
        }
        return finishKeyOperation(logic,
                logic.commitKeyText(new String(Character.toChars(unicode))));
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        MBTextInputViewLogic current = session;
        if (!isCurrentSession(current)) return null;
        syncEditor(current);
        InputConnection target = super.onCreateInputConnection(outAttrs);
        if (target == null) return null;

        int composingStart = current.composingStart();
        if (composingStart >= 0) {
            target.setComposingRegion(composingStart, current.editorText().length());
        }
        target.setSelection(current.editorText().length(), current.editorText().length());

        outAttrs.inputType = INPUT_TYPE;
        outAttrs.imeOptions = IME_OPTIONS;
        outAttrs.initialSelStart = current.editorText().length();
        outAttrs.initialSelEnd = outAttrs.initialSelStart;
        if (Build.VERSION.SDK_INT >= 33) {
            return new Api33BridgeInputConnection(target, this, current);
        }
        return new BridgeInputConnection(target, this, current);
    }

    @Override
    public boolean onTouchEvent(android.view.MotionEvent event) {
        return false;
    }

    @Override
    public boolean onKeyPreIme(int keyCode, KeyEvent event) {
        MBTextInputViewLogic current = session;
        // Consume before NativePostIme writes and then forwards an unhandled text key.
        if (keyCode != KeyEvent.KEYCODE_BACK && !KeyEvent.isGamepadButton(keyCode)
                && isCurrentSession(current) && handleInputKeyEvent(current, event)) return true;
        return super.onKeyPreIme(keyCode, event);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        MBTextInputViewLogic current = session;
        if (isCurrentSession(current) && handleInputKeyEvent(current, event)) return true;
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        MBTextInputViewLogic current = session;
        if (isCurrentSession(current) && handleInputKeyEvent(current, event)) return true;
        return super.onKeyUp(keyCode, event);
    }

    @Override
    public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) {
        MBTextInputViewLogic current = session;
        if (isCurrentSession(current) && handleInputKeyEvent(current, event)) return true;
        return super.onKeyMultiple(keyCode, repeatCount, event);
    }

    @Override
    protected void onDetachedFromWindow() {
        endSession();
        super.onDetachedFromWindow();
    }
}
