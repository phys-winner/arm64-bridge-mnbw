package com.taleworlds.mbwarband;

import java.util.HashSet;
import java.util.Set;

public final class MBTouchStringsTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void testLanguageContract() {
        String[] expected = {
                "cns", "cnt", "cz", "de", "en", "es",
                "fr", "hu", "ja", "pl", "ru", "tr"
        };
        String[] codes = MBTouchStrings.codes();
        String[] names = MBTouchStrings.displayNames();
        check(codes.length == expected.length, "language count");
        check(names.length == expected.length, "display-name count");
        for (int index = 0; index < expected.length; ++index) {
            check(expected[index].equals(codes[index]), "language order at " + index);
            check(MBTouchStrings.isSupported(expected[index]), "language rejected: " + expected[index]);
            check(MBTouchStrings.indexOf(expected[index]) == index, "index mismatch: " + expected[index]);
            check(expected[index].equals(MBTouchStrings.codeAt(index)), "code mismatch: " + expected[index]);
            check(names[index].contains("(" + expected[index] + ")"),
                    "display name lacks exact code: " + expected[index]);
        }
        for (String invalid : new String[] {null, "", "EN", " en", "cs", "zh"}) {
            check(!MBTouchStrings.isSupported(invalid), "invalid language accepted: " + invalid);
            check("en".equals(MBTouchStrings.normalize(invalid)), "fallback mismatch: " + invalid);
            check("Game language".equals(MBTouchStrings.get(
                    invalid, MBTouchStrings.Text.LANGUAGE_DIALOG_TITLE)),
                    "invalid language did not select English: " + invalid);
        }
        check("en".equals(MBTouchStrings.codeAt(-1)), "negative index fallback");
        check("en".equals(MBTouchStrings.codeAt(expected.length)), "large index fallback");

        // The system per-app locale arrives as a BCP-47 tag and has to land on
        // one of the engine's own codes.
        String[][] bcp47 = {
                {"en", "en"}, {"en-US", "en"}, {"ru-RU", "ru"}, {"ru", "ru"},
                {"de-AT", "de"}, {"ja-JP", "ja"}, {"pl", "pl"}, {"tr-TR", "tr"},
                {"cs-CZ", "cz"}, {"cs", "cz"},
                {"zh-CN", "cns"}, {"zh-Hans", "cns"}, {"zh", "cns"},
                {"zh-TW", "cnt"}, {"zh-Hant-HK", "cnt"}, {"zh-HK", "cnt"},
                {"fr-FR", "fr"}, {"hu-HU", "hu"}, {"es-419", "es"},
                {"cns", "cns"}, {"cnt", "cnt"}, {"cz", "cz"},
                {"ko-KR", "en"}, {"it-IT", "en"}, {"pt-BR", "en"},
        };
        for (String[] pair : bcp47) {
            String mapped = MBTouchStrings.normalizeBcp47(pair[0]);
            check(pair[1].equals(mapped),
                    "bcp47 mismatch: " + pair[0] + " -> " + mapped + " (want " + pair[1] + ")");
        }
        check("en".equals(MBTouchStrings.normalizeBcp47(null)), "bcp47 null fallback");

        codes[0] = "broken";
        names[0] = "broken";
        check("cns".equals(MBTouchStrings.codes()[0]), "codes() exposed mutable storage");
        check(!"broken".equals(MBTouchStrings.displayNames()[0]),
                "displayNames() exposed mutable storage");
    }

    private static void testCompleteTranslations() {
        check(MBTouchStrings.Text.values().length == 75, "touch string inventory changed");
        Set<String> localizedTitles = new HashSet<String>();
        for (String code : MBTouchStrings.codes()) {
            String title = MBTouchStrings.get(code, MBTouchStrings.Text.LANGUAGE_DIALOG_TITLE);
            localizedTitles.add(title);
            for (MBTouchStrings.Text text : MBTouchStrings.Text.values()) {
                String value = MBTouchStrings.get(code, text);
                check(value != null && value.length() > 0, "empty " + code + "/" + text);
                check(value.equals(value.trim()), "untrimmed " + code + "/" + text);
                check(value.indexOf('\ufeff') < 0, "BOM in " + code + "/" + text);
                check(value.indexOf('\ufffd') < 0, "replacement character in " + code + "/" + text);
            }
            for (MBTouchStrings.Text label : new MBTouchStrings.Text[] {
                    MBTouchStrings.Text.ATTACK_LABEL,
                    MBTouchStrings.Text.BLOCK_LABEL,
                    MBTouchStrings.Text.USE_LABEL,
                    MBTouchStrings.Text.KICK_LABEL,
                    MBTouchStrings.Text.JUMP_LABEL,
                    MBTouchStrings.Text.DROP_LABEL,
                    MBTouchStrings.Text.CYCLE_WEAPON_LABEL,
                    MBTouchStrings.Text.CYCLE_SHIELD_LABEL,
                    MBTouchStrings.Text.ZOOM_LABEL,
                    MBTouchStrings.Text.VIEW_LABEL,
                    MBTouchStrings.Text.SHEATH_LABEL,
                    MBTouchStrings.Text.BACK_LABEL,
                    MBTouchStrings.Text.ORDERS_LABEL,
                    MBTouchStrings.Text.WEAPON_MODE_LABEL,
                    MBTouchStrings.Text.SETTINGS_LABEL,
                    MBTouchStrings.Text.PAUSE_LABEL,
                    MBTouchStrings.Text.MORE_ACTIONS_LABEL,
                    MBTouchStrings.Text.CHAT_LABEL
            }) {
                String value = MBTouchStrings.get(code, label);
                check(value.codePointCount(0, value.length()) <= 4,
                        "button label is too long for its circle: " + code + "/" + label);
            }
        }
        check(localizedTitles.size() == 12, "a language title silently fell back to another locale");
        check("Язык игры".equals(MBTouchStrings.get(
                "ru", MBTouchStrings.Text.LANGUAGE_DIALOG_TITLE)), "Russian sentinel");
        check("ゲーム言語".equals(MBTouchStrings.get(
                "ja", MBTouchStrings.Text.LANGUAGE_DIALOG_TITLE)), "Japanese sentinel");
        check("遊戲語言".equals(MBTouchStrings.get(
                "cnt", MBTouchStrings.Text.LANGUAGE_DIALOG_TITLE)), "Traditional Chinese sentinel");
        String russianDismountHint = MBTouchStrings.get(
                "ru", MBTouchStrings.Text.DISMOUNT_HINT);
        check(russianDismountHint.contains("Спешиться"),
                "Russian dismount action sentinel");
        check(russianDismountHint.contains("посмотрите вниз"),
                "Russian look-down instruction sentinel");
    }

    public static void main(String[] args) {
        testLanguageContract();
        testCompleteTranslations();
        check(MBTouchStrings.inventoryTransferFailure(0) == null,
                "successful transfer must not show a failure toast");
        check(MBTouchStrings.inventoryTransferFailure(-1) == null
                        && MBTouchStrings.inventoryTransferFailure(7) == null,
                "unknown result must not produce a misleading toast");
        MBTouchStrings.Text[] expectedFailures = {
                MBTouchStrings.Text.INVENTORY_SELECTION_CHANGED,
                MBTouchStrings.Text.INVENTORY_NO_FREE_SLOT,
                MBTouchStrings.Text.INVENTORY_NOT_EQUIPPABLE,
                MBTouchStrings.Text.INVENTORY_REQUIREMENTS_NOT_MET,
                MBTouchStrings.Text.INVENTORY_UNSUPPORTED_TARGET,
                MBTouchStrings.Text.INVENTORY_TRANSFER_REJECTED
        };
        for (int index = 0; index < expectedFailures.length; ++index) {
            check(MBTouchStrings.inventoryTransferFailure(index + 1) == expectedFailures[index],
                    "inventory result mapping " + (index + 1));
        }
        System.out.println("MBTouchStringsTest: PASS");
    }
}
