package com.taleworlds.mbwarband;

import java.util.ArrayList;
import java.util.List;

public final class MBTextInputViewLogicTest {
    private static final class Edit {
        final int deleteCount;
        final String insertion;
        final boolean submit;

        Edit(int deleteCount, String insertion, boolean submit) {
            this.deleteCount = deleteCount;
            this.insertion = insertion;
            this.submit = submit;
        }
    }

    private static final class RecordingSink implements MBTextInputViewLogic.Sink {
        final List<Edit> edits = new ArrayList<Edit>();
        boolean accept = true;

        @Override
        public boolean apply(int deleteBeforeCodePoints, String insertedText, boolean submit) {
            if (!accept) return false;
            edits.add(new Edit(deleteBeforeCodePoints, insertedText, submit));
            return true;
        }
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void checkEdit(Edit edit, int deletion, String insertion,
                                  boolean submit, String message) {
        check(edit.deleteCount == deletion, message + " deletion");
        check(insertion.equals(edit.insertion), message + " insertion");
        check(edit.submit == submit, message + " submit");
    }

    private static void testSupportedBmpContract() {
        check(MBTextInputViewLogic.isSupportedText("ЮлияЁж42"), "Cyrillic probe rejected");
        check(MBTextInputViewLogic.isSupportedText("War band-42"), "ASCII rejected");
        check(MBTextInputViewLogic.isSupportedText("Łukasz"), "printable BMP rejected");
        for (String invalid : new String[] {
                null, "line\nfeed", "tab\t", "\u0000", "\u007f", "\u0085",
                "\ud83d\ude03", "\ud800", "\udfff", "\ufffe", "\uffff"
        }) {
            check(!MBTextInputViewLogic.isSupportedText(invalid), "invalid scalar set accepted");
        }
    }

    private static void testCommitAndCompositionAreNotDuplicated() {
        RecordingSink sink = new RecordingSink();
        MBTextInputViewLogic logic = new MBTextInputViewLogic(sink);
        check(logic.setComposingText("Ю"), "first composition");
        check(logic.setComposingText("Юл"), "revised composition");
        check(logic.setComposingText("Юлия"), "final composition");
        check(sink.edits.isEmpty(), "transient composition leaked to guest");
        check("Юлия".equals(logic.editorText()), "editor composition state");

        check(logic.commitText("Юлия"), "composition commit");
        check(sink.edits.size() == 1, "composition committed more than once");
        checkEdit(sink.edits.get(0), 0, "Юлия", false, "composition commit");
        check("Юлия".equals(logic.committedText()), "committed state");
        check(logic.composingText().length() == 0, "composition not cleared");
        check(logic.finishComposingText(), "empty finish");
        check(sink.edits.size() == 1, "empty finish emitted an edit");

        check(logic.setComposingText("Ёж"), "second composition");
        check(logic.finishComposingText(), "finish composition");
        check(sink.edits.size() == 2, "finish did not emit exactly once");
        checkEdit(sink.edits.get(1), 0, "Ёж", false, "finish composition");
    }

    private static void testDeleteConsumesCompositionThenGuestText() {
        RecordingSink sink = new RecordingSink();
        MBTextInputViewLogic logic = new MBTextInputViewLogic(sink);
        check(logic.commitText("AB"), "ASCII setup");
        check(logic.setComposingText("Ива"), "composition setup");
        check(logic.deleteBeforeCursor(1), "delete inside composition");
        check("Ив".equals(logic.composingText()), "composition deletion result");
        check(sink.edits.size() == 1, "composition-only delete reached guest");

        check(logic.deleteBeforeCursor(3), "mixed composition/guest delete");
        check(logic.composingText().length() == 0, "composition should be empty");
        check("A".equals(logic.committedText()), "committed suffix deletion");
        check(sink.edits.size() == 2, "mixed delete edit count");
        checkEdit(sink.edits.get(1), 1, "", false, "mixed delete");

        check(logic.deleteBeforeCursor(3), "delete into pre-session guest text");
        check(logic.committedText().length() == 0, "local committed text should be empty");
        checkEdit(sink.edits.get(2), 3, "", false, "pre-session delete");
    }

    private static void testComposingRegionRewritesCommittedSuffix() {
        RecordingSink sink = new RecordingSink();
        MBTextInputViewLogic logic = new MBTextInputViewLogic(sink);
        check(logic.commitText("alpha"), "region setup");
        check(logic.setComposingRegion(2, 5), "suffix composing region");
        checkEdit(sink.edits.get(1), 3, "", false, "region rollback");
        check("al".equals(logic.committedText()), "region committed prefix");
        check("pha".equals(logic.composingText()), "region composition");
        check(!logic.setComposingRegion(0, 1), "non-suffix region accepted");

        check(logic.setComposingText("фа"), "region replacement");
        check(logic.commitText("фа"), "region commit");
        checkEdit(sink.edits.get(2), 0, "фа", false, "region replacement commit");
    }

    private static void testDoneIsAtomicAndIdempotent() {
        RecordingSink sink = new RecordingSink();
        MBTextInputViewLogic logic = new MBTextInputViewLogic(sink);
        check(logic.setComposingText("ЮлияЁж42"), "Done composition setup");
        check(logic.submit(), "Done rejected");
        check(sink.edits.size() == 1, "Done must be one atomic edit");
        checkEdit(sink.edits.get(0), 0, "ЮлияЁж42", true, "Done edit");
        check(logic.isSubmitted(), "submitted state missing");
        check(!logic.submit(), "duplicate Done accepted");
        check(!logic.commitText("X"), "text accepted after Done");
        check(!logic.deleteBeforeCursor(1), "delete accepted after Done");
        check(sink.edits.size() == 1, "post-Done edit leaked");
    }

    private static void testNativeRejectionIsTransactional() {
        RecordingSink sink = new RecordingSink();
        MBTextInputViewLogic logic = new MBTextInputViewLogic(sink);
        check(logic.setComposingText("Иван"), "rejection setup");
        sink.accept = false;
        check(!logic.finishComposingText(), "rejected finish reported success");
        check("Иван".equals(logic.composingText()), "rejected finish changed composition");
        check(logic.committedText().length() == 0, "rejected finish changed committed text");
        check(sink.edits.isEmpty(), "rejecting sink recorded an edit");

        sink.accept = true;
        check(logic.finishComposingText(), "finish retry");
        sink.accept = false;
        check(!logic.deleteBeforeCursor(2), "rejected delete reported success");
        check("Иван".equals(logic.committedText()), "rejected delete changed state");
    }

    private static void testKeyEventAndSessionInvalidation() {
        RecordingSink sink = new RecordingSink();
        MBTextInputViewLogic logic = new MBTextInputViewLogic(sink);
        check(logic.setComposingText("Ив"), "key composition setup");
        check(logic.commitKeyText("a"), "key commit");
        checkEdit(sink.edits.get(0), 0, "Ивa", false, "key edit");
        logic.close();
        check(!logic.isActive(), "closed session remains active");
        check(!logic.commitText("stale"), "stale commit accepted");
        check(!logic.setComposingText("stale"), "stale composition accepted");
        check(!logic.submit(), "stale Done accepted");
        check(sink.edits.size() == 1, "stale connection reached sink");
    }

    private static void testBounds() {
        RecordingSink sink = new RecordingSink();
        MBTextInputViewLogic logic = new MBTextInputViewLogic(sink);
        StringBuilder limit = new StringBuilder();
        for (int index = 0; index < MBTextInputViewLogic.MAX_EDIT_CODE_POINTS; ++index) {
            limit.append('Я');
        }
        check(logic.setComposingText(limit.toString()), "maximum composition rejected");
        check(!logic.setComposingText(limit.toString() + "Я"), "oversized composition accepted");
        check(!logic.deleteBeforeCursor(MBTextInputViewLogic.MAX_EDIT_CODE_POINTS + 1),
                "oversized delete accepted");
        check(sink.edits.isEmpty(), "bounds rejection reached sink");
    }

    public static void main(String[] args) {
        testSupportedBmpContract();
        testCommitAndCompositionAreNotDuplicated();
        testDeleteConsumesCompositionThenGuestText();
        testComposingRegionRewritesCommittedSuffix();
        testDoneIsAtomicAndIdempotent();
        testNativeRejectionIsTransactional();
        testKeyEventAndSessionInvalidation();
        testBounds();
        System.out.println("MBTextInputViewLogicTest: PASS");
    }
}
