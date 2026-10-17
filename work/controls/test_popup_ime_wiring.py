"""Check compiled popup inputs to Android's IME flag calculation, without emulating it."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys

classes = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "java/build/classes"
android_jar = Path(os.environ["ANDROID_SDK_ROOT"]) / "platforms/android-36/android.jar"
javap = str(Path(os.environ["JAVA_HOME"]) / "bin/javap") if os.environ.get("JAVA_HOME") else shutil.which("javap")
assert javap, "Set JAVA_HOME or add javap to PATH"
constants = subprocess.check_output([
    str(javap), "-constants", "-classpath", str(android_jar),
    "android.widget.PopupWindow"], text=True, timeout=20)
needed = int(re.search(r"INPUT_METHOD_NEEDED = (\d+);", constants).group(1))
dump = subprocess.check_output([
    str(javap), "-p", "-c", "-classpath", str(classes),
    "com.taleworlds.mbwarband.MBTouchOverlay$1"], text=True, timeout=20)
assert re.search(
    r"iconst_m1\s*\n\s*\d+: iconst_m1\s*\n\s*\d+: iconst_0\s*\n"
    r'\s*\d+: invokespecial[^\n]+PopupWindow\."<init>":\(Landroid/view/View;IIZ\)V', dump), \
    "Fullscreen popup must retain MATCH_PARENT dimensions and focusable=false"
modes = list(re.finditer(
    r"iconst_(\d)\s*\n\s*\d+: invokevirtual[^\n]+PopupWindow\.setInputMethodMode:\(I\)V", dump))
assert len(modes) == 1 and int(modes[0].group(1)) == needed, \
    "Fullscreen production popup must use Android INPUT_METHOD_NEEDED"
assert modes[0].end() < dump.index("PopupWindow.showAtLocation"), "Mode must apply before show"
assert "PopupWindow.setTouchable:(Z)V" in dump, "Mission touch capture must remain wired"
assert "PopupWindow.setFocusable" not in dump, "Popup must not steal the Activity editor's focus"
print(f"PASS: compiled fullscreen popup remains nonfocusable/touchable; INPUT_METHOD_NEEDED={needed} before show enables Android's below-IME flag combination")
