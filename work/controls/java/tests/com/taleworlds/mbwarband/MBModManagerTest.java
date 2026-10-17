package com.taleworlds.mbwarband;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.Charset;
import java.nio.file.Files;
import java.util.List;

public final class MBModManagerTest {
    private static final Charset UTF8 = Charset.forName("UTF-8");

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void write(File file, String value) throws Exception {
        File parent = file.getParentFile();
        check(parent.isDirectory() || parent.mkdirs(), "create fixture parent");
        FileOutputStream output = new FileOutputStream(file);
        try {
            output.write(value.getBytes(UTF8));
        } finally {
            output.close();
        }
    }

    private static File validModule(File root, String name) throws Exception {
        File module = new File(new File(root, MBModManager.MODULES_DIRECTORY), name);
        String[] required = {"module.ini", "menus.txt", "scripts.txt", "troops.txt", "item_kinds1.txt"};
        for (String file : required) write(new File(module, file), "fixture\n");
        return module;
    }

    public static void main(String[] args) throws Exception {
        check("Android/data/com.taleworlds.mbwarband/files/modules".equals(
                        MBModManager.externalModulesRelativePath(
                                "com.taleworlds.mbwarband")),
                "external modules relative path");
        check("primary:Android/data/com.taleworlds.mbwarband/files/modules".equals(
                        MBModManager.externalModulesDocumentId(
                                "com.taleworlds.mbwarband")),
                "external modules document id");
        boolean rejectedEmptyPackage = false;
        try {
            MBModManager.externalModulesRelativePath("");
        } catch (IllegalArgumentException expected) {
            rejectedEmptyPackage = true;
        }
        check(rejectedEmptyPackage, "reject empty package name");

        File root = Files.createTempDirectory("mb-mod-manager-").toFile();
        try {
            File good = validModule(root, "Prophesy of Pendor 3.611");
            File broken = new File(new File(root, MBModManager.MODULES_DIRECTORY), "Broken");
            check(broken.mkdirs(), "create broken module");
            write(new File(broken, "module.ini"), "name = Broken\n");

            List<MBModManager.Module> modules = MBModManager.scan(root);
            check(modules.size() == 2, "scan both module folders");
            check(!modules.get(0).valid && modules.get(1).valid, "sort and validate modules");
            check(MBModManager.select(root, good.getName()), "select valid module");
            MBModManager.Selection selected = MBModManager.resolve(root);
            check(selected.id.equals(good.getName()), "resolve selected module");
            check(selected.moduleDirectory.getCanonicalFile().equals(good.getCanonicalFile()),
                    "selected canonical folder");
            check(selected.saveDirectory.isDirectory(), "per-module save directory");
            check(selected.saveDirectory.getName().equals(good.getName()), "per-module save identity");
            check(selected.validModuleCount == 1, "valid module count");

            write(new File(new File(root, MBModManager.MODULES_DIRECTORY),
                    MBModManager.SELECTED_FILE), "../escape\n");
            MBModManager.Selection fallback = MBModManager.resolve(root);
            check(fallback.isBaseGame(), "unsafe selection falls back to base");
            check(fallback.fallbackError != null, "fallback explains failure");
            check(new File(new File(root, MBModManager.MODULES_DIRECTORY),
                    MBModManager.ERROR_FILE).isFile(), "fallback writes error file");
            check(fallback.saveDirectory.getCanonicalFile().equals(root.getCanonicalFile()),
                    "base game retains legacy external save root");
            check(!MBModManager.select(root, "Broken"), "reject broken selection");
            check(MBModManager.select(root, MBModManager.BASE_MODULE), "select base game");
        } finally {
            deleteTree(root);
        }
        System.out.println("MBModManagerTest: PASS");
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }
}
