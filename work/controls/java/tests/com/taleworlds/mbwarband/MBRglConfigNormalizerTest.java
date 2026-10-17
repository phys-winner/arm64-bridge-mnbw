package com.taleworlds.mbwarband;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

public final class MBRglConfigNormalizerTest {
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
        File root = Files.createTempDirectory("warband-rgl-normalize-").toFile();
        try {
            File base = new File(root, "rgl_config.txt");
            File mod = new File(root, "mod_saves/Test Module/rgl_config.txt");
            File interrupted = new File(root,
                    "mod_saves/Interrupted Module/rgl_config.txt");
            File interruptedBackup = new File(interrupted.getParentFile(),
                    ".rgl_config.normalize.bak");
            File interruptedTemporary = new File(interrupted.getParentFile(),
                    ".rgl_config.normalize.tmp");
            File unrelated = new File(root, "mod_saves/Test Module/notes.txt");
            write(base, "alpha = 1\n\npostfx_hdr = 1\r\npostfx_dof=1\r\n\r\n");
            write(mod, "gamma = 3\n\nPOSTFX_HDR = 1\npostfx_dof_extra = 1\n");
            write(interruptedBackup,
                    "epsilon = 5\n\n  PoStFx_DoF  = 1\n\n");
            write(interruptedTemporary, "incomplete");
            write(unrelated, "keep\n\nblank\n");

            MBRglConfigNormalizer.Result first = MBRglConfigNormalizer.normalizeAll(root);
            check(first.normalizedFiles == 3, "legacy configs were not normalized");
            check(first.unchangedFiles == 0, "line endings were not normalized");
            check(first.failedFiles == 0, "normalization unexpectedly failed");
            check("alpha = 1\npostfx_hdr = 1\npostfx_dof=1\n".equals(read(base)),
                    "base config changed enabled renderer preferences");
            check("gamma = 3\nPOSTFX_HDR = 1\npostfx_dof_extra = 1\n".equals(read(mod)),
                    "mod config changed renderer preferences");
            check("epsilon = 5\n  PoStFx_DoF  = 1\n".equals(read(interrupted)),
                    "interrupted replacement was not recovered");
            check(!interruptedBackup.exists() && !interruptedTemporary.exists(),
                    "recovery left transaction files behind");
            check("keep\n\nblank\n".equals(read(unrelated)),
                    "unrelated file was modified");

            MBRglConfigNormalizer.Result second = MBRglConfigNormalizer.normalizeAll(root);
            check(second.normalizedFiles == 0 && second.unchangedFiles == 3
                            && second.failedFiles == 0,
                    "normalization is not idempotent");
        } finally {
            deleteTree(root);
        }
        System.out.println("MBRglConfigNormalizerTest: PASS");
    }
}
