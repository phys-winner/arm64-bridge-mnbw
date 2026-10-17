"""Check compiled production PreIme ownership; Android stage order is documented in evidence."""

from pathlib import Path
import os
import re
import shutil
import subprocess
import sys

classes = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "java/build/classes"
javap = str(Path(os.environ["JAVA_HOME"]) / "bin/javap") if os.environ.get("JAVA_HOME") else shutil.which("javap")
assert javap, "Set JAVA_HOME or add javap to PATH"
dump = subprocess.check_output([
    str(javap), "-p", "-c", "-classpath", str(classes),
    "com.taleworlds.mbwarband.MBTextInputView"], text=True, timeout=20)


def body(name):
    header = re.search(r"(?m)^  [^\n]+ " + re.escape(name) + r"\([^\n]*\);\n", dump)
    assert header, "Production method missing: " + name
    code = re.split(r"(?m)^  \S", dump[header.end():], maxsplit=1)[0]
    assert "Code:" in code, "No production bytecode: " + name
    return code


pre = body("onKeyPreIme")
assert "public boolean onKeyPreIme(int, android.view.KeyEvent);" in dump
guard = pre.index("KeyEvent.isGamepadButton")
current = pre.index("isCurrentSession")
handle = pre.index("handleInputKeyEvent")
fallback = pre.index("EditText.onKeyPreIme")
assert "iconst_4" in pre[:guard], "Back must retain superclass handling"
assert guard < current < handle < fallback, "Ownership must precede superclass fallback"
assert re.search(r"iconst_1\s*\n\s*\d+: ireturn", pre[handle:fallback]), \
    "Accepted/rejected text keys must stop at ViewPreIme, before NativePostIme"
assert pre.count("ireturn") == 2 and "onKeyDown" not in pre

key = body("handleInputKeyEvent")
for entry in ("isCurrentSession", "getAction", "getKeyCode", "getCharacters",
              "getUnicodeChar", "commitKeyText", "deleteBeforeCursor", "submit"):
    assert entry in key, "Existing key handler lost: " + entry
assert "iconst_2" in key and "iconst_1" in key, "MULTIPLE/UP paths must remain"
assert key.index("getCharacters") < key.index("getUnicodeChar")
finish = body("finishKeyOperation")
assert "syncEditor" in finish and "restartAfterReject" in finish
assert re.search(r"iconst_1\s*\n\s*\d+: ireturn", finish), "Rejected text keys must be consumed"
session = body("isCurrentSession")
assert "isActive" in session and "sessionToken" in session and "lcmp" in session
print("PASS: compiled ViewPreIme consumes owned text before NativePostIme; Back/gamepad fallback; existing key/session/reject paths wired")
