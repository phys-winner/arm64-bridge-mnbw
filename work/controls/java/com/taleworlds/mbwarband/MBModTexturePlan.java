package com.taleworlds.mbwarband;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.Charset;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;

/** Storage-only conversion transaction. The codec atomically replaces texture files. */
final class MBModTexturePlan {
    static final int ASTC = 1;
    static final int ETC1 = 2;
    static final int SCAN = 0;
    static final int CONVERT = 1;
    static final int VERIFY = 2;
    static final int MAX_ENTRIES = 100000;
    private static final int MAX_DEPTH = 64;
    private static final long MAX_FILE_BYTES = 8L * 1024L * 1024L * 1024L;
    private static final Charset UTF8 = Charset.forName("UTF-8");

    interface Codec {
        int inspect(File file, int target) throws IOException;
        void convert(File file, int target) throws IOException;
    }

    interface Cancellation {
        // A UI implementation may wait here while its activity is in the background.
        boolean isCancelled();
    }

    interface Progress {
        void update(int phase, int completed, int total, String file);
    }

    static final class Cancelled extends IOException {
        Cancelled() { super("cancelled"); }
    }

    static final class Problem extends IOException {
        final String reason;
        final String file;

        Problem(String reason, String file) {
            super(reason + (file.length() == 0 ? "" : ": " + file));
            this.reason = reason;
            this.file = file;
        }
    }

    static final class Entry {
        final File file;
        final String relative;
        final long bytes;
        final String sourceHash;
        final int state;

        Entry(File file, String relative, long bytes, String sourceHash, int state) {
            this.file = file;
            this.relative = relative;
            this.bytes = bytes;
            this.sourceHash = sourceHash;
            this.state = state;
        }
    }

    final File root;
    final int target;
    final List<Entry> entries;
    final int pending;
    final int verified;
    final long pendingBytes;
    final String inventory;
    final boolean completionCurrent;

    private MBModTexturePlan(File root, int target, List<Entry> entries,
                             int pending, int verified, long pendingBytes,
                             String inventory,
                             boolean completionCurrent) {
        this.root = root;
        this.target = target;
        this.entries = Collections.unmodifiableList(entries);
        this.pending = pending;
        this.verified = verified;
        this.pendingBytes = pendingBytes;
        this.inventory = inventory;
        this.completionCurrent = completionCurrent;
    }

    static MBModTexturePlan scan(File directory, int target, Codec codec,
                                 Cancellation cancellation, Progress progress)
            throws IOException {
        try {
            return scan(directory, target, codec, cancellation, progress, SCAN);
        } catch (IOException failure) {
            try { clearCompletion(directory); }
            catch (IOException cleanup) { failure.addSuppressed(cleanup); }
            throw failure;
        }
    }

    private static MBModTexturePlan scan(File directory, int target, Codec codec,
                                         Cancellation cancellation, Progress progress,
                                         int phase) throws IOException {
        if (target != ASTC && target != ETC1) throw new IllegalArgumentException("target");
        check(cancellation);
        File root = directory.getCanonicalFile();
        if (!root.isDirectory()) throw new Problem("missing_directory", "");
        ArrayList<File> candidates = new ArrayList<File>();
        enumerate(root, root, 0, new int[1], candidates, cancellation);
        Collections.sort(candidates, new Comparator<File>() {
            @Override public int compare(File a, File b) {
                return a.getPath().compareTo(b.getPath());
            }
        });
        ArrayList<Entry> entries = new ArrayList<Entry>();
        int pending = 0;
        int verified = 0;
        long pendingBytes = 0L;
        for (int i = 0; i < candidates.size(); ++i) {
            check(cancellation);
            File file = candidates.get(i);
            String relative = relative(root, file);
            progress.update(phase, i, candidates.size(), relative);
            requireSafe(root, file);
            long beforeLength = file.length();
            long beforeTime = file.lastModified();
            String sourceHash = hash(file, cancellation);
            int state = codec.inspect(file, target);
            check(cancellation);
            if (state < 0 || state > 2) throw new Problem("invalid_texture", relative);
            if (beforeLength != file.length() || beforeTime != file.lastModified()) {
                throw new Problem("inventory_changed", relative);
            }
            Entry entry = new Entry(file, relative, beforeLength, sourceHash, state);
            entries.add(entry);
            if (state == 1) {
                ++pending;
                pendingBytes += beforeLength;
            } else if (state == 2) {
                ++verified;
            }
            progress.update(phase, i + 1, candidates.size(), relative);
        }
        String inventory = inventory(target, entries);
        boolean current = pending == 0 && isCompletionCurrent(root, target, inventory);
        return new MBModTexturePlan(root, target, entries, pending, verified,
                pendingBytes, inventory, current);
    }

    MBModTexturePlan convert(Codec codec, Cancellation cancellation, Progress progress)
            throws IOException {
        clearCompletion(root);
        try {
            int completed = verified;
            int total = pending + verified;
            ArrayList<Entry> expected = new ArrayList<Entry>();
            for (Entry entry : entries) {
                check(cancellation);
                if (entry.state == 0) {
                    expected.add(entry);
                    continue;
                }
                requireSafe(root, entry.file);
                if (!entry.sourceHash.equals(hash(entry.file, cancellation))) {
                    throw new Problem("inventory_changed", entry.relative);
                }
                int state = codec.inspect(entry.file, target);
                if (state < 1 || state > 2) {
                    throw new Problem("invalid_texture", entry.relative);
                }
                if (state == 1) {
                    progress.update(CONVERT, completed, total, entry.relative);
                    check(cancellation);
                    codec.convert(entry.file, target);
                    check(cancellation);
                    if (codec.inspect(entry.file, target) != 2) {
                        throw new Problem("verification_failed", entry.relative);
                    }
                }
                // Successful atomic replacement changes size and checksum intentionally.
                // Unrelated changes and additions must still fail the final inventory gate.
                requireSafe(root, entry.file);
                expected.add(new Entry(entry.file, entry.relative, entry.file.length(),
                        hash(entry.file, cancellation), 2));
                if (entry.state == 1) ++completed;
                progress.update(CONVERT, completed, total, entry.relative);
            }
            check(cancellation);
            MBModTexturePlan finalPlan = scan(root, target, codec, cancellation,
                    progress, VERIFY);
            if (!inventory(target, expected).equals(finalPlan.inventory)) {
                throw new Problem("inventory_changed", "");
            }
            if (finalPlan.pending != 0) throw new Problem("verification_failed", "");
            finalPlan.publish(cancellation);
            return finalPlan;
        } catch (IOException failure) {
            clearCompletion(root);
            throw failure;
        } catch (RuntimeException failure) {
            clearCompletion(root);
            throw failure;
        }
    }

    /** Called only for a fully inspected inventory; the empty flag is published last. */
    void publish(Cancellation cancellation) throws IOException {
        if (pending != 0) throw new Problem("verification_failed", "");
        if (completionCurrent) {
            check(cancellation);
            return;
        }
        clearCompletion(root);
        File marker = new File(root, markerName(target));
        File manifest = new File(root, markerName(target) + ".inventory");
        File temporary = null;
        boolean committed = false;
        try {
            check(cancellation);
            temporary = File.createTempFile(".mb_textures_tmp_", ".inventory", root);
            writeSynced(temporary, manifestText(target, inventory).getBytes(UTF8));
            check(cancellation);
            requireSafe(root, manifest);
            if (!temporary.renameTo(manifest)) throw new Problem("write_failed", manifest.getName());
            temporary = File.createTempFile(".mb_textures_tmp_", ".flag", root);
            writeSynced(temporary, new byte[0]);
            check(cancellation);
            requireSafe(root, marker);
            if (!temporary.renameTo(marker)) throw new Problem("write_failed", marker.getName());
            check(cancellation);
            committed = true;
        } finally {
            if (temporary != null && temporary.exists()) temporary.delete();
            if (!committed) clearCompletion(root);
        }
    }

    static void clearCompletion(File directory) throws IOException {
        File root = directory.getCanonicalFile();
        for (int target : new int[] {ASTC, ETC1}) {
            for (String suffix : new String[] {"", ".inventory"}) {
                File file = new File(root, markerName(target) + suffix);
                requireSafe(root, file);
                if (file.exists() && (!file.isFile() || !file.delete())) {
                    throw new Problem("write_failed", file.getName());
                }
            }
        }
    }

    static String markerName(int target) {
        if (target == ASTC) return ".mb_textures_astc_v1";
        if (target == ETC1) return ".mb_textures_etc1_v1";
        throw new IllegalArgumentException("target");
    }

    private static void enumerate(File root, File directory, int depth, int[] count,
                                  List<File> candidates, Cancellation cancellation)
            throws IOException {
        check(cancellation);
        if (depth > MAX_DEPTH) throw new Problem("too_many_files", relative(root, directory));
        requireSafe(root, directory);
        File[] children = directory.listFiles();
        if (children == null) throw new Problem("read_failed", relative(root, directory));
        for (File child : children) {
            check(cancellation);
            if (++count[0] > MAX_ENTRIES) throw new Problem("too_many_files", "");
            // Check all children, including converter temporaries, before following a path.
            requireSafe(root, child);
            String name = child.getName();
            if (child.isDirectory()) {
                enumerate(root, child, depth + 1, count, candidates, cancellation);
            } else if (!child.isFile()) {
                throw new Problem("unsafe_path", relative(root, child));
            } else if (!isGenerated(name) && isTextureCandidate(child)) {
                if (child.length() > MAX_FILE_BYTES) {
                    throw new Problem("texture_too_large", relative(root, child));
                }
                candidates.add(child);
            }
        }
    }

    private static boolean isGenerated(String name) {
        return name.contains(".mbtex.tmp.")
                || name.equals(markerName(ASTC)) || name.equals(markerName(ETC1))
                || name.equals(markerName(ASTC) + ".inventory")
                || name.equals(markerName(ETC1) + ".inventory")
                || name.startsWith(".mb_textures_tmp_");
    }

    private static boolean isTextureCandidate(File file) throws IOException {
        if (file.getName().toLowerCase(Locale.ROOT).endsWith(".dds")) return true;
        FileInputStream input = new FileInputStream(file);
        try {
            int first = input.read();
            if (first == 'D') return input.read() == 'D' && input.read() == 'S'
                    && input.read() == ' ';
            if (first != 'M') return false;
            byte[] suffix = {'B', 'T', 'X', 'C', '0', '0', '1'};
            for (byte value : suffix) if (input.read() != value) return false;
            return true;
        } finally {
            input.close();
        }
    }

    private static void requireSafe(File root, File file) throws IOException {
        File absolute = file.getAbsoluteFile();
        File canonical = file.getCanonicalFile();
        String prefix = root.getPath() + File.separator;
        if (!absolute.equals(canonical) || (!canonical.equals(root)
                && !canonical.getPath().startsWith(prefix))) {
            throw new Problem("unsafe_path", file.getName());
        }
    }

    private static boolean isCompletionCurrent(File root, int target, String inventory)
            throws IOException {
        File marker = new File(root, markerName(target));
        File manifest = new File(root, markerName(target) + ".inventory");
        requireSafe(root, marker);
        requireSafe(root, manifest);
        String expected = manifestText(target, inventory);
        byte[] bytes = expected.getBytes(UTF8);
        if (!marker.isFile() || marker.length() != 0L
                || !manifest.isFile() || manifest.length() != bytes.length) return false;
        FileInputStream input = new FileInputStream(manifest);
        try {
            for (byte value : bytes) if (input.read() != (value & 0xff)) return false;
            return input.read() == -1;
        } finally {
            input.close();
        }
    }

    private static String manifestText(int target, String inventory) {
        return "MB_MOD_TEXTURES 1\n" + target + "\n" + inventory + "\n";
    }

    private static String inventory(int target, List<Entry> entries) {
        MessageDigest result = digest();
        update(result, "mb-textures-v1\n" + target + "\n");
        for (Entry entry : entries) {
            update(result, entry.relative.length() + ":" + entry.relative + ":"
                    + entry.bytes + ":" + entry.sourceHash + "\n");
        }
        return hex(result.digest());
    }

    private static String hash(File file, Cancellation cancellation) throws IOException {
        MessageDigest result = digest();
        FileInputStream input = new FileInputStream(file);
        try {
            byte[] buffer = new byte[128 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                check(cancellation);
                result.update(buffer, 0, count);
            }
        } finally {
            input.close();
        }
        return hex(result.digest());
    }

    private static MessageDigest digest() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException impossible) { throw new AssertionError(impossible); }
    }

    private static void update(MessageDigest digest, String value) {
        digest.update(value.getBytes(UTF8));
    }

    private static String hex(byte[] bytes) {
        final char[] alphabet = "0123456789abcdef".toCharArray();
        char[] value = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; ++i) {
            value[2 * i] = alphabet[(bytes[i] >>> 4) & 15];
            value[2 * i + 1] = alphabet[bytes[i] & 15];
        }
        return new String(value);
    }

    private static void writeSynced(File file, byte[] bytes) throws IOException {
        FileOutputStream output = new FileOutputStream(file, false);
        try {
            output.write(bytes);
            output.flush();
            output.getFD().sync();
        } finally {
            output.close();
        }
    }

    private static String relative(File root, File file) {
        if (root.equals(file)) return "";
        return file.getPath().substring(root.getPath().length() + 1).replace(File.separatorChar, '/');
    }

    private static void check(Cancellation cancellation) throws Cancelled {
        if (cancellation.isCancelled()) throw new Cancelled();
    }
}
