package com.taleworlds.mbwarband;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;

/** Normalizes rgl_config line endings without changing renderer preferences. */
final class MBRglConfigNormalizer {
    private static final long MAX_CONFIG_BYTES = 1024L * 1024L;

    static final class Result {
        int normalizedFiles;
        int unchangedFiles;
        int failedFiles;
    }

    private MBRglConfigNormalizer() {}

    static Result normalizeAll(File externalFilesRoot) {
        Result result = new Result();
        if (externalFilesRoot == null) {
            ++result.failedFiles;
            return result;
        }
        try {
            File root = externalFilesRoot.getCanonicalFile();
            normalizeCandidate(root, new File(root, "rgl_config.txt"), result);

            File modSaves = new File(root, MBModManager.MOD_SAVES_DIRECTORY);
            File[] modules = modSaves.listFiles();
            if (modules == null) return result;
            for (File module : modules) {
                if (module == null || !module.isDirectory()) continue;
                normalizeCandidate(root, new File(module, "rgl_config.txt"), result);
            }
        } catch (IOException | SecurityException exception) {
            ++result.failedFiles;
        }
        return result;
    }

    private static void normalizeCandidate(File root, File candidate, Result result)
            throws IOException {
        File file = candidate.getCanonicalFile();
        requireDescendant(root, file);
        recoverInterruptedReplace(file);
        if (!file.exists()) return;
        if (!file.isFile() || file.length() < 0L || file.length() > MAX_CONFIG_BYTES) {
            ++result.failedFiles;
            return;
        }

        byte[] source = readFully(file);
        byte[] normalized = normalizeConfig(source);
        if (sameBytes(source, normalized)) {
            ++result.unchangedFiles;
            return;
        }
        replaceAtomically(file, normalized);
        ++result.normalizedFiles;
    }

    private static void recoverInterruptedReplace(File destination) throws IOException {
        File parent = destination.getParentFile();
        File temporary = new File(parent, ".rgl_config.normalize.tmp");
        File backup = new File(parent, ".rgl_config.normalize.bak");
        if (!destination.exists() && backup.isFile()) {
            if (!backup.renameTo(destination)) {
                throw new IOException("cannot recover interrupted rgl_config normalization");
            }
        }
        if (destination.exists()) deleteIfPresent(backup);
        deleteIfPresent(temporary);
    }

    static byte[] collapseLineEndingRuns(byte[] source) {
        if (source == null || source.length == 0) return new byte[0];
        ByteArrayOutputStream output = new ByteArrayOutputStream(source.length);
        boolean inLineEnding = false;
        for (byte value : source) {
            int unsigned = value & 0xff;
            if (unsigned == '\r' || unsigned == '\n') {
                if (!inLineEnding) output.write('\n');
                inLineEnding = true;
            } else {
                output.write(unsigned);
                inLineEnding = false;
            }
        }
        return output.toByteArray();
    }

    static byte[] normalizeConfig(byte[] source) {
        return collapseLineEndingRuns(source);
    }

    private static byte[] readFully(File file) throws IOException {
        int length = (int) file.length();
        byte[] bytes = new byte[length];
        FileInputStream input = new FileInputStream(file);
        try {
            int offset = 0;
            while (offset < bytes.length) {
                int count = input.read(bytes, offset, bytes.length - offset);
                if (count < 0) break;
                if (count == 0) continue;
                offset += count;
            }
            if (offset != bytes.length) throw new IOException("short rgl_config read");
            return bytes;
        } finally {
            input.close();
        }
    }

    private static void replaceAtomically(File destination, byte[] bytes) throws IOException {
        File parent = destination.getParentFile();
        File temporary = new File(parent, ".rgl_config.normalize.tmp");
        File backup = new File(parent, ".rgl_config.normalize.bak");
        deleteIfPresent(temporary);
        deleteIfPresent(backup);

        FileOutputStream output = new FileOutputStream(temporary, false);
        try {
            output.write(bytes);
            output.flush();
            output.getFD().sync();
        } finally {
            output.close();
        }

        boolean originalMoved = false;
        try {
            if (!destination.renameTo(backup)) {
                throw new IOException("cannot stage original rgl_config");
            }
            originalMoved = true;
            if (!temporary.renameTo(destination)) {
                throw new IOException("cannot publish normalized rgl_config");
            }
            originalMoved = false;
            deleteIfPresent(backup);
        } finally {
            if (originalMoved && !destination.exists()) backup.renameTo(destination);
            deleteIfPresent(temporary);
        }
    }

    private static void deleteIfPresent(File file) throws IOException {
        if (file.exists() && !file.delete()) {
            throw new IOException("cannot remove stale rgl_config temporary file");
        }
    }

    private static boolean sameBytes(byte[] first, byte[] second) {
        if (first.length != second.length) return false;
        for (int index = 0; index < first.length; ++index) {
            if (first[index] != second[index]) return false;
        }
        return true;
    }

    private static void requireDescendant(File root, File child) throws IOException {
        String rootPath = root.getPath();
        String childPath = child.getPath();
        String prefix = rootPath.endsWith(File.separator)
                ? rootPath : rootPath + File.separator;
        if (!childPath.startsWith(prefix)) {
            throw new IOException("rgl_config path escapes external files root");
        }
    }
}
