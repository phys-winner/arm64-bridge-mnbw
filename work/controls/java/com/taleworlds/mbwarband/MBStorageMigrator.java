package com.taleworlds.mbwarband;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;

/** Copies legacy private files into app-specific external storage without overwriting. */
final class MBStorageMigrator {
    private static final int MAX_DEPTH = 32;
    private static final int MAX_ENTRIES = 20000;
    private static final byte[] COPY_BUFFER = new byte[64 * 1024];

    static final class Result {
        int copiedFiles;
        int skippedFiles;
        int visitedEntries;
        long copiedBytes;
        String failure;

        boolean succeeded() {
            return failure == null;
        }
    }

    private MBStorageMigrator() {}

    static synchronized Result copyMissing(File sourceRoot, File destinationRoot) {
        Result result = new Result();
        try {
            if (sourceRoot == null || destinationRoot == null) {
                throw new IOException("storage root is null");
            }
            File source = sourceRoot.getCanonicalFile();
            File destination = destinationRoot.getCanonicalFile();
            if (source.equals(destination)) {
                throw new IOException("source and destination storage roots are identical");
            }
            if (!source.exists()) return result;
            if (!source.isDirectory()) throw new IOException("legacy storage root is not a directory");
            ensureDirectory(destination);
            copyDirectory(source, source, destination, destination, 0, result);
        } catch (IOException | SecurityException exception) {
            result.failure = exception.getClass().getSimpleName() + ": " + exception.getMessage();
        }
        return result;
    }

    private static void copyDirectory(File sourceRoot, File sourceDirectory,
                                      File destinationRoot, File destinationDirectory,
                                      int depth, Result result) throws IOException {
        if (depth > MAX_DEPTH) throw new IOException("legacy storage nesting exceeds limit");
        File[] children = sourceDirectory.listFiles();
        if (children == null) throw new IOException("cannot enumerate " + sourceDirectory);
        for (File child : children) {
            if (++result.visitedEntries > MAX_ENTRIES) {
                throw new IOException("legacy storage entry count exceeds limit");
            }
            File canonicalChild = child.getCanonicalFile();
            requireDescendant(sourceRoot, canonicalChild, "legacy source");
            File destinationChild = new File(destinationDirectory, child.getName()).getCanonicalFile();
            requireDescendant(destinationRoot, destinationChild, "migration destination");
            if (canonicalChild.isDirectory()) {
                ensureDirectory(destinationChild);
                copyDirectory(sourceRoot, canonicalChild, destinationRoot,
                        destinationChild, depth + 1, result);
            } else if (canonicalChild.isFile()) {
                copyFileIfMissing(canonicalChild, destinationChild, result);
            }
        }
    }

    private static void copyFileIfMissing(File source, File destination,
                                          Result result) throws IOException {
        if (destination.exists()) {
            ++result.skippedFiles;
            return;
        }
        ensureDirectory(destination.getParentFile());
        File temporary = new File(destination.getParentFile(),
                "." + destination.getName() + ".migration.tmp");
        if (temporary.exists() && !temporary.delete()) {
            throw new IOException("cannot remove stale migration temporary file");
        }
        long bytes = 0L;
        try {
            FileInputStream input = new FileInputStream(source);
            try {
                FileOutputStream output = new FileOutputStream(temporary, false);
                try {
                    while (true) {
                        int count = input.read(COPY_BUFFER);
                        if (count < 0) break;
                        if (count == 0) continue;
                        output.write(COPY_BUFFER, 0, count);
                        bytes += count;
                    }
                    output.flush();
                    output.getFD().sync();
                } finally {
                    output.close();
                }
            } finally {
                input.close();
            }
            if (destination.exists()) {
                ++result.skippedFiles;
                return;
            }
            if (!temporary.renameTo(destination)) {
                throw new IOException("cannot publish migrated file " + destination.getName());
            }
            ++result.copiedFiles;
            result.copiedBytes += bytes;
        } finally {
            if (temporary.exists() && !temporary.equals(destination)) temporary.delete();
        }
    }

    private static void ensureDirectory(File directory) throws IOException {
        if (directory == null) throw new IOException("directory parent is null");
        if (!directory.exists() && !directory.mkdirs() && !directory.exists()) {
            throw new IOException("cannot create directory " + directory);
        }
        if (!directory.isDirectory()) throw new IOException("path is not a directory " + directory);
    }

    private static void requireDescendant(File root, File child, String description)
            throws IOException {
        String rootPath = root.getPath();
        String childPath = child.getPath();
        String prefix = rootPath.endsWith(File.separator)
                ? rootPath : rootPath + File.separator;
        if (!childPath.startsWith(prefix)) {
            throw new IOException(description + " escapes its root");
        }
    }
}
