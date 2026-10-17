#!/usr/bin/env bash
set -eu

ROOT="$(cd -- "$(dirname -- "$0")" && pwd)"
: "${JAVA_HOME:?Set JAVA_HOME to an existing JDK}"
: "${ANDROID_SDK_ROOT:?Set ANDROID_SDK_ROOT to an existing Android SDK}"
: "${BAKSMALI_JAR:?Set BAKSMALI_JAR to an executable baksmali JAR}"
JAVA_EXE="$JAVA_HOME/bin/java.exe"
BUILD_TOOLS="$ANDROID_SDK_ROOT/build-tools/36.1.0"
ANDROID_JAR="$ANDROID_SDK_ROOT/platforms/android-36/android.jar"
test -f "$ANDROID_JAR" || { echo "Missing Android 36 platform" >&2; exit 1; }

JDK_BIN="$(dirname "$JAVA_EXE")"
JAVAC="$JDK_BIN/javac.exe"
JAR="$JDK_BIN/jar.exe"
D8_JAR="$BUILD_TOOLS/lib/d8.jar"
test -f "$JAVAC" && test -f "$JAR" && test -f "$D8_JAR" || { echo "Missing Java/D8 tools" >&2; exit 1; }

OUT="$ROOT/build"
rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/test-classes" "$OUT/dex" "$OUT/smali"
ROOT_WIN="$(cygpath -w "$ROOT")"
OUT_WIN="$(cygpath -w "$OUT")"
ANDROID_JAR_WIN="$(cygpath -w "$ANDROID_JAR")"
JAVA_SOURCES=()
while IFS= read -r -d '' source; do
  JAVA_SOURCES+=("$(cygpath -w "$source")")
done < <(find "$ROOT/com" -type f -name '*.java' -print0 | sort -z)
test "${#JAVA_SOURCES[@]}" -gt 0 || { echo "Missing Java control sources" >&2; exit 1; }
"$JAVAC" -encoding UTF-8 -source 8 -target 8 -classpath "$ANDROID_JAR_WIN" -d "$OUT_WIN\\classes" "${JAVA_SOURCES[@]}"
"$JAR" cf "$OUT_WIN\\controls.jar" -C "$OUT_WIN\\classes" .
"$JAVA_EXE" -cp "$D8_JAR" com.android.tools.r8.D8 --min-api 24 --lib "$ANDROID_JAR_WIN" --output "$OUT_WIN\\dex" "$OUT_WIN\\controls.jar"
TEST_SOURCES=()
while IFS= read -r -d '' source; do
  TEST_SOURCES+=("$(cygpath -w "$source")")
done < <(find "$ROOT/tests" -type f -name '*.java' -print0 | sort -z)
test "${#TEST_SOURCES[@]}" -gt 0 || { echo "Missing Java control tests" >&2; exit 1; }
"$JAVAC" -encoding UTF-8 -source 8 -target 8 -classpath "$ANDROID_JAR_WIN;$OUT_WIN/classes" -d "$OUT_WIN/test-classes" "${TEST_SOURCES[@]}"
for test_class in \
  com.taleworlds.mbwarband.MBTouchOverlayLogicTest \
  com.taleworlds.mbwarband.MBInputStallPolicyTest \
  com.taleworlds.mbwarband.MBTouchStringsTest \
  com.taleworlds.mbwarband.MBBattleTouchPolicyTest \
  com.taleworlds.mbwarband.MBButtonLayoutPolicyTest \
  com.taleworlds.mbwarband.MBLanguageButtonPolicyTest \
  com.taleworlds.mbwarband.MBTextInputViewLogicTest \
  com.taleworlds.mbwarband.MBStorageMigratorTest \
  com.taleworlds.mbwarband.MBRglConfigNormalizerTest \
  com.taleworlds.mbwarband.MBMasterServerConfigTest \
  com.taleworlds.mbwarband.MBModTexturePlanTest \
  com.taleworlds.mbwarband.MBModTextureStringsTest \
  com.taleworlds.mbwarband.MBModManagerTest; do
  "$JAVA_EXE" -classpath "$ANDROID_JAR_WIN;$OUT_WIN/classes;$OUT_WIN/test-classes" "$test_class"
done
"$JAVA_EXE" -jar "$BAKSMALI_JAR" disassemble "$OUT/dex/classes.dex" -o "$OUT/smali"
test -f "$OUT/smali/com/taleworlds/mbwarband/MBTouchOverlay.smali" || { echo "Missing generated overlay smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBInputStallPolicy.smali" || { echo "Missing generated input-stall policy smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBTouchStrings.smali" || { echo "Missing generated touch strings smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBButtonLayoutPolicy.smali" || { echo "Missing generated button layout policy smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBBattleTouchPolicy.smali" || { echo "Missing generated battle touch policy smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBLanguageButtonPolicy.smali" || { echo "Missing generated language policy smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBTextInputViewLogic.smali" || { echo "Missing generated text input logic smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBTextInputView.smali" || { echo "Missing generated text input view smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBStorageMigrator.smali" || { echo "Missing generated storage migrator smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBRglConfigNormalizer.smali" || { echo "Missing generated rgl_config normalizer smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBMasterServerConfig.smali" || { echo "Missing generated master server config smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBModManager.smali" || { echo "Missing generated mod manager smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBModTextureStrings.smali" || { echo "Missing generated module strings smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBModTextureStrings\$Text.smali" || { echo "Missing generated module text keys smali" >&2; exit 1; }
test -f "$OUT/smali/com/taleworlds/mbwarband/MBRestartActivity.smali" || { echo "Missing generated restart activity smali" >&2; exit 1; }
printf 'OK %s\n' "$OUT/smali/com/taleworlds/mbwarband/MBTouchOverlay.smali"
printf 'OK %s\n' "$OUT/smali/com/taleworlds/mbwarband/MBTextInputView.smali"
