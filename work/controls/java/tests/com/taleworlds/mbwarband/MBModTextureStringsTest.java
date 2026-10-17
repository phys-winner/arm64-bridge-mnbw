package com.taleworlds.mbwarband;

/** Completeness and formatting checks for every shipped module dialog language. */
public final class MBModTextureStringsTest {
    private static int assertions;

    private static void check(boolean valid, String message) {
        ++assertions;
        if (!valid) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        String[] languages = MBTouchStrings.codes();
        check(languages.length == 12, "shipped language count");
        for (String language : languages) {
            for (MBModTextureStrings.Text key : MBModTextureStrings.Text.values()) {
                String value = MBModTextureStrings.get(language, key);
                check(value != null && !value.trim().isEmpty(), language + " " + key);
            }
            String estimate = MBModTextureStrings.estimate(language, 125000L, 57000L);
            check(estimate.contains("2:05") && estimate.contains("0:57"),
                    language + " time values");
            check(!estimate.contains("%1$s") && !estimate.contains("%2$s"),
                    language + " unexpanded time format");
            check(!MBModTextureStrings.get(language,
                            MBModTextureStrings.Text.REPLACE_WARNING).equals(
                    MBModTextureStrings.get("en",
                            MBModTextureStrings.Text.REPLACE_WARNING))
                    || "en".equals(language), language + " replacement warning fallback");
        }
        check("1:00:01".equals(MBModTextureStrings.duration(3600001L)), "hour duration");
        check("0:00".equals(MBModTextureStrings.duration(0L)), "zero duration");
        check("en".equals(MBTouchStrings.normalize("unknown")), "unknown fallback");
        System.out.println("MBModTextureStringsTest: " + assertions + " assertions passed");
    }
}
