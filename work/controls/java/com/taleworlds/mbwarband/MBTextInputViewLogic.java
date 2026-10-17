package com.taleworlds.mbwarband;

/**
 * Android-free state machine for the append-only guest text editor.
 *
 * <p>The Android IME may revise a composing suffix many times. That suffix is kept local
 * until commit/finish/submit, so the guest receives the final text exactly once. All text
 * accepted here consists of one UTF-16 code unit per guest BMP code point.</p>
 */
public final class MBTextInputViewLogic {
    public static final int MAX_EDIT_CODE_POINTS = 256;

    public interface Sink {
        /** Applies one indivisible guest edit. No state may change when false is returned. */
        boolean apply(int deleteBeforeCodePoints, String insertedText, boolean submit);
    }

    private final Sink sink;
    private String committedText = "";
    private String composingText = "";
    private boolean active = true;
    private boolean submitted;

    public MBTextInputViewLogic(Sink sink) {
        if (sink == null) throw new NullPointerException("sink");
        this.sink = sink;
    }

    /** Exact scalar subset supported by the guest's one/two/three-byte UTF-8 encoder. */
    public static boolean isSupportedText(String text) {
        if (text == null) return false;
        for (int index = 0; index < text.length(); ++index) {
            int value = text.charAt(index);
            boolean printableAscii = value >= 0x20 && value <= 0x7e;
            boolean printableBmpLow = value >= 0x00a0 && value <= 0xd7ff;
            boolean printableBmpHigh = value >= 0xe000 && value <= 0xfffd;
            if (!printableAscii && !printableBmpLow && !printableBmpHigh) return false;
        }
        return true;
    }

    private boolean canEdit() {
        return active && !submitted;
    }

    private static boolean fits(String prefix, String suffix) {
        return prefix.length() <= MAX_EDIT_CODE_POINTS
                && suffix.length() <= MAX_EDIT_CODE_POINTS - prefix.length();
    }

    private static int commonPrefixLength(String first, String second) {
        int limit = Math.min(first.length(), second.length());
        int index = 0;
        while (index < limit && first.charAt(index) == second.charAt(index)) ++index;
        return index;
    }

    private static String dropLast(String text, int count) {
        return text.substring(0, text.length() - Math.min(count, text.length()));
    }

    public boolean setComposingText(String text) {
        if (!canEdit() || !isSupportedText(text) || !fits(committedText, text)) return false;
        composingText = text;
        return true;
    }

    /** Replaces the transient composing suffix, or appends when there is no composition. */
    public boolean commitText(String text) {
        if (!canEdit() || !isSupportedText(text) || !fits(committedText, text)) return false;
        if (text.length() != 0 && !sink.apply(0, text, false)) return false;
        committedText += text;
        composingText = "";
        return true;
    }

    /** Commits the current composition exactly once. */
    public boolean finishComposingText() {
        if (!canEdit()) return false;
        if (composingText.length() == 0) return true;
        if (!sink.apply(0, composingText, false)) return false;
        committedText += composingText;
        composingText = "";
        return true;
    }

    /**
     * Moves a suffix into composition. This covers IMEs that reopen already committed text
     * for correction. A non-suffix composing region is rejected because the guest cursor is
     * intentionally fixed at the end.
     */
    public boolean setComposingRegion(int start, int end) {
        if (!canEdit()) return false;
        String editorText = editorText();
        if (start < 0 || start > end || end != editorText.length()) return false;

        String nextCommitted = editorText.substring(0, start);
        String nextComposing = editorText.substring(start);
        int common = commonPrefixLength(committedText, nextCommitted);
        int deleteCount = committedText.length() - common;
        String insertion = nextCommitted.substring(common);
        if ((deleteCount != 0 || insertion.length() != 0)
                && !sink.apply(deleteCount, insertion, false)) {
            return false;
        }
        committedText = nextCommitted;
        composingText = nextComposing;
        return true;
    }

    /**
     * Deletes from the append cursor. Pending composition is consumed locally first; any
     * remainder becomes guest backspace events, including deletion into pre-session text.
     */
    public boolean deleteBeforeCursor(int codePointCount) {
        if (!canEdit() || codePointCount < 0 || codePointCount > MAX_EDIT_CODE_POINTS) {
            return false;
        }
        if (codePointCount == 0) return true;

        int composingDelete = Math.min(codePointCount, composingText.length());
        String nextComposing = dropLast(composingText, composingDelete);
        int guestDelete = codePointCount - composingDelete;
        String nextCommitted = dropLast(committedText, guestDelete);
        if (guestDelete != 0 && !sink.apply(guestDelete, "", false)) return false;

        composingText = nextComposing;
        committedText = nextCommitted;
        return true;
    }

    /** Commits pending composition plus a printable IME KeyEvent without duplication. */
    public boolean commitKeyText(String text) {
        if (!canEdit() || !isSupportedText(text)) return false;
        String insertion = composingText + text;
        if (!fits(committedText, insertion)) return false;
        if (insertion.length() != 0 && !sink.apply(0, insertion, false)) return false;
        committedText += insertion;
        composingText = "";
        return true;
    }

    /** Commits pending composition and queues guest Enter in the same atomic edit. */
    public boolean submit() {
        if (!canEdit()) return false;
        if (!sink.apply(0, composingText, true)) return false;
        committedText += composingText;
        composingText = "";
        submitted = true;
        return true;
    }

    /** Permanently invalidates this session; stale InputConnections retain this object. */
    public void close() {
        active = false;
        committedText = "";
        composingText = "";
    }

    boolean isActive() {
        return active;
    }

    boolean isSubmitted() {
        return submitted;
    }

    String committedText() {
        return committedText;
    }

    String composingText() {
        return composingText;
    }

    String editorText() {
        return committedText + composingText;
    }

    int composingStart() {
        return composingText.length() == 0 ? -1 : committedText.length();
    }
}
