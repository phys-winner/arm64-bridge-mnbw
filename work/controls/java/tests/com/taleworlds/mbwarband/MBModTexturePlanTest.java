package com.taleworlds.mbwarband;

import java.io.File;
import java.io.IOException;
import java.nio.charset.Charset;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.CRC32;

/** Transaction tests use a checksummed, atomically replaced fake texture codec. */
public final class MBModTexturePlanTest {
    private static final Charset UTF8 = Charset.forName("UTF-8");
    private static final MBModTexturePlan.Progress SILENT = new MBModTexturePlan.Progress() {
        @Override public void update(int phase, int completed, int total, String file) {}
    };
    private static int assertions;

    private static final class Stop implements MBModTexturePlan.Cancellation {
        boolean stop;
        @Override public boolean isCancelled() { return stop; }
    }

    private static class Codec implements MBModTexturePlan.Codec {
        int converted;
        int inspected;
        int failAt;
        Runnable afterConvert;
        final List<String> changed = new ArrayList<String>();

        @Override public int inspect(File file, int target) throws IOException {
            ++inspected;
            String text = read(file);
            if (text.startsWith("DDS source:")) return 1;
            if (text.equals("DDS supported")) return 0;
            if (!text.startsWith("MBTXC001|")) return -1;
            String[] fields = text.split("\\|", -1);
            if (fields.length != 4 || !fields[1].equals(String.valueOf(target))) return -1;
            return checksum(fields[2]).equals(fields[3]) ? 2 : -1;
        }

        @Override public void convert(File file, int target) throws IOException {
            if (failAt != 0 && converted + 1 == failAt) throw new IOException("injected failure");
            check(inspect(file, target) == 1, "only valid source is passed to codec");
            String value = read(file).substring("DDS source:".length());
            String output = "MBTXC001|" + target + "|" + value + "|" + checksum(value);
            File temporary = new File(file.getPath() + ".mbtex.tmp.test");
            write(temporary, output);
            Files.move(temporary.toPath(), file.toPath(),
                    StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            ++converted;
            changed.add(file.getName());
            if (afterConvert != null) afterConvert.run();
        }
    }

    private static String checksum(String value) {
        CRC32 checksum = new CRC32();
        checksum.update(value.getBytes(UTF8));
        return Long.toHexString(checksum.getValue());
    }

    private static void check(boolean condition, String message) {
        ++assertions;
        if (!condition) throw new AssertionError(message);
    }

    private static void write(File file, String text) throws IOException {
        File parent = file.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("fixture directory");
        Files.write(file.toPath(), text.getBytes(UTF8));
    }

    private static String read(File file) throws IOException {
        return new String(Files.readAllBytes(file.toPath()), UTF8);
    }

    private static MBModTexturePlan scan(File root, Codec codec, Stop stop) throws IOException {
        return MBModTexturePlan.scan(root, MBModTexturePlan.ASTC, codec, stop, SILENT);
    }

    private static File flag(File root) {
        return new File(root, MBModTexturePlan.markerName(MBModTexturePlan.ASTC));
    }

    private static void noFlag(File root, String reason) {
        check(!flag(root).exists(), reason);
        check(!new File(root, MBModTexturePlan.markerName(MBModTexturePlan.ETC1)).exists(), reason);
    }

    private static void testConversionAndResume(File root) throws Exception {
        write(new File(root, "Textures/a.dds"), "DDS source:alpha");
        write(new File(root, "Textures/b.texture"), "DDS source:beta");
        write(new File(root, "Textures/supported.dds"), "DDS supported");
        write(new File(root, "module.ini"), "module contents");
        write(new File(root, "Textures/a.dds.mbtex.tmp.interrupted"), "DDS unfinished");
        Codec codec = new Codec();
        Stop stop = new Stop();
        MBModTexturePlan pending = scan(root, codec, stop);
        check(pending.pending == 2 && pending.verified == 0, "DDS magic detects extensionless input");
        check(pending.entries.size() == 3 && codec.inspected == 3, "only candidate textures inspected");
        noFlag(root, "scan of unconverted input cannot mark completion");
        codec.afterConvert = new Runnable() {
            @Override public void run() { stop.stop = true; }
        };
        boolean cancelled = false;
        try { pending.convert(codec, stop, SILENT); }
        catch (MBModTexturePlan.Cancelled expected) { cancelled = true; }
        check(cancelled && codec.converted == 1, "cancellation stops after current atomic file");
        noFlag(root, "interruption does not publish a completion marker");
        check(read(new File(root, "Textures/a.dds")).startsWith("MBTXC001"),
                "completed replacement survives cancellation");
        check(read(new File(root, "Textures/b.texture")).equals("DDS source:beta"),
                "not-yet-processed source survives cancellation");
        check(read(new File(root, "module.ini")).equals("module contents"), "nontexture unchanged");
        stop.stop = false;
        codec.afterConvert = null;
        MBModTexturePlan resumed = scan(root, codec, stop);
        check(resumed.pending == 1 && resumed.verified == 1, "resume recognizes completed texture");
        resumed.convert(codec, stop, SILENT);
        check(codec.converted == 2, "resume converts only unfinished files");
        check(flag(root).isFile() && flag(root).length() == 0L, "completion flag exists and is empty");
        MBModTexturePlan ready = scan(root, codec, stop);
        check(ready.pending == 0 && ready.verified == 2 && ready.completionCurrent,
                "all checksummed outputs and inventory verified on next launch");
        ready.convert(codec, stop, SILENT);
        check(codec.converted == 2, "repeated conversion never reencodes completed textures");
        check(read(new File(root, "Textures/supported.dds")).equals("DDS supported"),
                "supported native format is not changed");
        check(new File(root, "Textures/b.texture").length() > 0,
                "converted signature retains extensionless texture in inventory");
    }

    private static void testForgedFlagAndCorruptOutput(File root) throws Exception {
        write(new File(root, "a.dds"), "DDS source:payload");
        write(flag(root), "");
        Codec codec = new Codec();
        Stop stop = new Stop();
        MBModTexturePlan plan = scan(root, codec, stop);
        check(plan.pending == 1 && !plan.completionCurrent, "empty flag alone cannot bypass inspection");
        plan.convert(codec, stop, SILENT);
        write(new File(root, "a.dds"), "MBTXC001|1|corrupt|00000000");
        boolean invalid = false;
        try { scan(root, codec, stop); }
        catch (MBModTexturePlan.Problem expected) { invalid = expected.reason.equals("invalid_texture"); }
        check(invalid, "corrupt output is rejected even with completion flag present");
        noFlag(root, "failed verification removes a stale completion flag");
        check(codec.converted == 1, "corrupt converted payload never undergoes lossy conversion");
    }

    private static void testChangedInventory(File root) throws Exception {
        write(new File(root, "a.dds"), "DDS source:payload");
        Codec codec = new Codec();
        Stop stop = new Stop();
        scan(root, codec, stop).convert(codec, stop, SILENT);
        write(new File(root, "b.dds"), "DDS source:added");
        MBModTexturePlan changed = scan(root, codec, stop);
        check(changed.pending == 1 && !changed.completionCurrent,
                "new texture invalidates previously completed inventory");
        changed.convert(codec, stop, SILENT);
        check(codec.converted == 2, "new file alone is processed");
        final File third = new File(root, "c.dds");
        write(third, "DDS source:third");
        MBModTexturePlan consented = scan(root, codec, stop);
        codec.afterConvert = new Runnable() {
            @Override public void run() {
                try { write(new File(root, "d.dds"), "DDS source:concurrent addition"); }
                catch (IOException exception) { throw new RuntimeException(exception); }
            }
        };
        boolean rejected = false;
        try { consented.convert(codec, stop, SILENT); }
        catch (MBModTexturePlan.Problem expected) {
            rejected = expected.reason.equals("inventory_changed");
        }
        check(rejected, "texture added during conversion blocks completion and launch");
        noFlag(root, "concurrent addition cannot publish a marker");
        codec.afterConvert = null;
        MBModTexturePlan next = scan(root, codec, stop);
        check(next.pending == 1 && next.verified == 3, "next attempt resumes after added file");
        write(third, "DDS source:modified after consent");
        rejected = false;
        try { next.convert(codec, stop, SILENT); }
        catch (MBModTexturePlan.Problem expected) {
            rejected = expected.reason.equals("inventory_changed");
        }
        check(rejected, "content replacement after scan is rejected before conversion");
    }

    private static void testCodecFailureAndTarget(File root) throws Exception {
        write(new File(root, "a.dds"), "DDS source:first");
        write(new File(root, "b.dds"), "DDS source:second");
        Codec codec = new Codec();
        Stop stop = new Stop();
        MBModTexturePlan plan = scan(root, codec, stop);
        codec.failAt = 2;
        boolean failed = false;
        try { plan.convert(codec, stop, SILENT); }
        catch (IOException expected) { failed = true; }
        check(failed && codec.converted == 1, "injected codec failure stops transaction");
        noFlag(root, "codec failure never publishes completion");
        check(read(new File(root, "b.dds")).equals("DDS source:second"),
                "failure leaves original current file intact");
        codec.failAt = 0;
        scan(root, codec, stop).convert(codec, stop, SILENT);
        boolean mismatch = false;
        try { MBModTexturePlan.scan(root, MBModTexturePlan.ETC1, codec, stop, SILENT); }
        catch (MBModTexturePlan.Problem expected) { mismatch = true; }
        check(mismatch, "converted texture for another target cannot be silently reused");
        noFlag(root, "target mismatch invalidates old marker");
    }

    private static void testBoundsAndLinks(File root) throws Exception {
        File current = new File(root, "depth");
        for (int i = 0; i < 66; ++i) current = new File(current, "d");
        check(current.mkdirs(), "nested fixture");
        boolean bounded = false;
        try { scan(root, new Codec(), new Stop()); }
        catch (MBModTexturePlan.Problem expected) { bounded = expected.reason.equals("too_many_files"); }
        check(bounded, "deep directory tree is bounded");
        File linkRoot = new File(root.getParentFile(), "links");
        check(linkRoot.mkdir(), "link fixture root");
        Path link = new File(linkRoot, "outside").toPath();
        boolean linked = false;
        try {
            Files.createSymbolicLink(link, root.toPath());
            linked = true;
        } catch (IOException unsupported) {
            System.out.println("MBModTexturePlanTest: symlink creation unavailable on host: "
                    + unsupported.getClass().getSimpleName());
        } catch (UnsupportedOperationException unsupported) {
            System.out.println("MBModTexturePlanTest: symlink creation unsupported on host");
        }
        if (linked) {
            boolean rejected = false;
            try { scan(linkRoot, new Codec(), new Stop()); }
            catch (MBModTexturePlan.Problem expected) {
                rejected = expected.reason.equals("unsafe_path");
            }
            check(rejected, "symlink escape is rejected before traversal");
            Files.delete(link);
            Files.createSymbolicLink(link, linkRoot.toPath());
            rejected = false;
            try { scan(linkRoot, new Codec(), new Stop()); }
            catch (MBModTexturePlan.Problem expected) { rejected = true; }
            check(rejected, "symlink cycle is rejected");
            Files.delete(link);
        }
    }

    public static void main(String[] arguments) throws Exception {
        File root = Files.createTempDirectory("mb-texture-plan-").toFile().getCanonicalFile();
        try {
            testConversionAndResume(new File(root, "resume"));
            testForgedFlagAndCorruptOutput(new File(root, "corrupt"));
            testChangedInventory(new File(root, "inventory"));
            testCodecFailureAndTarget(new File(root, "failure"));
            testBoundsAndLinks(new File(root, "bounds"));
        } finally {
            deleteFixture(root.toPath());
        }
        System.out.println("MBModTexturePlanTest: PASS (" + assertions + " assertions)");
    }

    private static void deleteFixture(Path path) throws IOException {
        if (Files.isDirectory(path, java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
            java.nio.file.DirectoryStream<Path> stream = Files.newDirectoryStream(path);
            try { for (Path child : stream) deleteFixture(child); }
            finally { stream.close(); }
        }
        Files.deleteIfExists(path);
    }
}
