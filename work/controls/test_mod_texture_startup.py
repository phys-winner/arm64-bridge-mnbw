"""Exercise the production dialog routing without an Android runtime."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent / "java/com/taleworlds/mbwarband"


def check(source_root=ROOT):
    converter = (source_root / "MBModTextureConverter.java").read_text(encoding="utf-8")
    render = re.search(r"        void render\(\) \{.*?\n        \}", converter, re.S).group()
    constants = "\n".join(re.findall(r"        private static final int \w+ = \d+;", converter))
    plan = (ROOT / "MBModTexturePlan.java").read_text(encoding="utf-8")
    phases = "\n".join(re.findall(r"    static final int (?:SCAN|CONVERT|VERIFY) = \d+;", plan))
    fixture = """
import java.lang.ref.WeakReference;
public class StartupCheck {
    static class MBModTexturePlan { PHASES }
    static class Activity {
        boolean finishing, destroyed;
        boolean isFinishing() { return finishing; }
        boolean isDestroyed() { return destroyed; }
    }
    static class Control {
        boolean cancelled;
        boolean cancelled() { return cancelled; }
    }
    CONSTANTS
    Activity activity = new Activity();
    WeakReference<Activity> owner = new WeakReference<>(activity);
    Control control = new Control();
    boolean destroyed, foreground = true;
    int state = WORKING, phase = MBModTexturePlan.SCAN, dialogState = -1;
    String result = "silent";
    void abandon() { result = "abandoned"; }
    void finish(boolean ready) { result = ready ? "ready" : "cancelled"; }
    void dismissDialog() {}
    void renderProgress(Activity ignored) { result = "progress"; }
    void showConfirmation(Activity ignored) { result = "confirmation"; }
    void showError(Activity ignored) { result = "error"; }
    RENDER
    static void expect(StartupCheck session, String expected) {
        session.render();
        if (!session.result.equals(expected)) throw new AssertionError(
                "state=" + session.state + " phase=" + session.phase + ": " + session.result);
    }
    public static void main(String[] args) {
        for (int phase : new int[]{MBModTexturePlan.SCAN, MBModTexturePlan.CONVERT, MBModTexturePlan.VERIFY}) {
            StartupCheck session = new StartupCheck(); session.phase = phase;
            expect(session, phase == MBModTexturePlan.SCAN ? "silent" : "progress");
        }
        for (int state : new int[]{ASK, ERROR, READY, CANCELLED}) {
            StartupCheck session = new StartupCheck(); session.state = state;
            expect(session, state == ASK ? "confirmation" : state == ERROR ? "error"
                    : state == READY ? "ready" : "cancelled");
        }
        StartupCheck session = new StartupCheck(); session.state = READY;
        session.control.cancelled = true; expect(session, "cancelled");
        session = new StartupCheck(); session.foreground = false; session.state = READY;
        expect(session, "silent"); session.foreground = true; expect(session, "ready");
        session = new StartupCheck(); session.destroyed = true; expect(session, "silent");
        session = new StartupCheck(); session.owner.clear(); expect(session, "abandoned");
        session = new StartupCheck(); session.activity.finishing = true; expect(session, "abandoned");
        session = new StartupCheck(); session.activity.destroyed = true; expect(session, "abandoned");
        System.out.println("PASS: 14 production dialog/lifecycle checks");
    }
}
""".replace("PHASES", phases).replace("CONSTANTS", constants).replace("RENDER", render)
    with tempfile.TemporaryDirectory(prefix="warband-startup-") as directory:
        source = Path(directory) / "StartupCheck.java"
        source.write_text(fixture, encoding="utf-8")
        subprocess.run(["java", str(source)], check=True, timeout=30)
    launcher = (source_root / "MBLaunchActivity.java").read_text(encoding="utf-8")
    assert "TextView" not in launcher and "Mount & Blade: Warband" not in launcher
    assert "MBModTextureConverter.prepare(" in launcher
    print("PASS: launcher splash removed; texture launch gate retained")


if __name__ == "__main__":
    check()
