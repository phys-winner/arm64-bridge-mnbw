package com.taleworlds.mbwarband;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

public final class MBStorageMigratorTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void write(File file, String text) throws Exception {
        File parent = file.getParentFile();
        if (!parent.exists() && !parent.mkdirs()) throw new AssertionError("mkdir failed");
        FileOutputStream output = new FileOutputStream(file);
        try {
            output.write(text.getBytes(StandardCharsets.UTF_8));
        } finally {
            output.close();
        }
    }

    private static String read(File file) throws Exception {
        FileInputStream input = new FileInputStream(file);
        try {
            byte[] bytes = new byte[(int) file.length()];
            int offset = 0;
            while (offset < bytes.length) {
                int count = input.read(bytes, offset, bytes.length - offset);
                if (count < 0) break;
                offset += count;
            }
            return new String(bytes, 0, offset, StandardCharsets.UTF_8);
        } finally {
            input.close();
        }
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }

    public static void main(String[] args) throws Exception {
        File sandbox = Files.createTempDirectory("warband-storage-migration-").toFile();
        try {
            File source = new File(sandbox, "private");
            File destination = new File(sandbox, "external");
            write(new File(source, "rgl_config.txt"), "legacy-config");
            write(new File(source, "Savegames/Native/sg00.sav"), "legacy-save");
            write(new File(destination, "rgl_config.txt"), "newer-config");

            MBStorageMigrator.Result first = MBStorageMigrator.copyMissing(source, destination);
            check(first.succeeded(), "first migration failed: " + first.failure);
            check(first.copiedFiles == 1, "expected exactly one copied file");
            check(first.skippedFiles == 1, "existing destination must be skipped");
            check("newer-config".equals(read(new File(destination, "rgl_config.txt"))),
                    "migration overwrote a newer external file");
            check("legacy-save".equals(read(new File(destination,
                    "Savegames/Native/sg00.sav"))), "nested save was not copied");

            MBStorageMigrator.Result second = MBStorageMigrator.copyMissing(source, destination);
            check(second.succeeded(), "idempotent migration failed");
            check(second.copiedFiles == 0 && second.skippedFiles == 2,
                    "idempotent migration changed destination files");

            MBStorageMigrator.Result same = MBStorageMigrator.copyMissing(source, source);
            check(!same.succeeded(), "identical roots must fail closed");
        } finally {
            deleteTree(sandbox);
        }
        System.out.println("MBStorageMigratorTest: PASS");
    }
}
