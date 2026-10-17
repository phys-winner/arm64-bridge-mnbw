#!/usr/bin/env bash
set -eu

ROOT="$(cd -- "$(dirname -- "$0")" && pwd)"
: "${JAVA_HOME:?Set JAVA_HOME to an existing JDK}"
JAVA_EXE="$JAVA_HOME/bin/java.exe"

JDK_BIN="$(dirname "$JAVA_EXE")"
JAVAC="$JDK_BIN/javac.exe"
test -f "$JAVAC" && test -f "$JAVA_EXE" || { echo "Missing Java tools" >&2; exit 1; }

OUT="$ROOT/build/pure-test-classes"
rm -rf "$OUT"
mkdir -p "$OUT"
OUT_WIN="$(cygpath -w "$OUT")"
SOURCES=(
  "$ROOT/com/taleworlds/mbwarband/MBTouchStrings.java"
  "$ROOT/com/taleworlds/mbwarband/MBInputStallPolicy.java"
  "$ROOT/com/taleworlds/mbwarband/MBButtonLayoutPolicy.java"
  "$ROOT/com/taleworlds/mbwarband/MBBattleTouchPolicy.java"
  "$ROOT/com/taleworlds/mbwarband/MBLanguageButtonPolicy.java"
  "$ROOT/com/taleworlds/mbwarband/MBTextInputViewLogic.java"
  "$ROOT/com/taleworlds/mbwarband/MBStorageMigrator.java"
  "$ROOT/com/taleworlds/mbwarband/MBRglConfigNormalizer.java"
  "$ROOT/com/taleworlds/mbwarband/MBMasterServerConfig.java"
  "$ROOT/com/taleworlds/mbwarband/MBModManager.java"
  "$ROOT/tests/com/taleworlds/mbwarband/MBTouchStringsTest.java"
  "$ROOT/tests/com/taleworlds/mbwarband/MBInputStallPolicyTest.java"
  "$ROOT/tests/com/taleworlds/mbwarband/MBButtonLayoutPolicyTest.java"
  "$ROOT/tests/com/taleworlds/mbwarband/MBBattleTouchPolicyTest.java"
  "$ROOT/tests/com/taleworlds/mbwarband/MBLanguageButtonPolicyTest.java"
  "$ROOT/tests/com/taleworlds/mbwarband/MBTextInputViewLogicTest.java"
  "$ROOT/tests/com/taleworlds/mbwarband/MBStorageMigratorTest.java"
  "$ROOT/tests/com/taleworlds/mbwarband/MBRglConfigNormalizerTest.java"
  "$ROOT/tests/com/taleworlds/mbwarband/MBMasterServerConfigTest.java"
  "$ROOT/tests/com/taleworlds/mbwarband/MBModManagerTest.java"
)
SOURCES_WIN=()
for source in "${SOURCES[@]}"; do
  SOURCES_WIN+=("$(cygpath -w "$source")")
done

"$JAVAC" -encoding UTF-8 -source 8 -target 8 -d "$OUT_WIN" "${SOURCES_WIN[@]}"
"$JAVA_EXE" -cp "$OUT_WIN" com.taleworlds.mbwarband.MBTouchStringsTest
"$JAVA_EXE" -cp "$OUT_WIN" com.taleworlds.mbwarband.MBInputStallPolicyTest
"$JAVA_EXE" -cp "$OUT_WIN" com.taleworlds.mbwarband.MBButtonLayoutPolicyTest
"$JAVA_EXE" -cp "$OUT_WIN" com.taleworlds.mbwarband.MBBattleTouchPolicyTest
"$JAVA_EXE" -cp "$OUT_WIN" com.taleworlds.mbwarband.MBLanguageButtonPolicyTest
"$JAVA_EXE" -cp "$OUT_WIN" com.taleworlds.mbwarband.MBTextInputViewLogicTest
"$JAVA_EXE" -cp "$OUT_WIN" com.taleworlds.mbwarband.MBStorageMigratorTest
"$JAVA_EXE" -cp "$OUT_WIN" com.taleworlds.mbwarband.MBRglConfigNormalizerTest
"$JAVA_EXE" -cp "$OUT_WIN" com.taleworlds.mbwarband.MBMasterServerConfigTest
"$JAVA_EXE" -cp "$OUT_WIN" com.taleworlds.mbwarband.MBModManagerTest
