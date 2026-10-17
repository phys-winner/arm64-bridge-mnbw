package com.taleworlds.mbwarband;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/** Validates and persists user modules inside app-specific external storage. */
final class MBModManager {
    static final String BASE_MODULE = "Native";
    static final String MODULES_DIRECTORY = "modules";
    static final String MOD_SAVES_DIRECTORY = "mod_saves";
    static final String SELECTED_FILE = "selected_module.txt";
    static final String ERROR_FILE = "mod_error.txt";
    private static final int MAX_MODULES = 256;
    private static final int MAX_SELECTION_BYTES = 512;
    private static final long MAX_MODULE_INI_BYTES = 4L * 1024L * 1024L;
    private static final Charset UTF8 = Charset.forName("UTF-8");
    private static final String[] REQUIRED_FILES = {
            "module.ini", "menus.txt", "scripts.txt", "troops.txt", "item_kinds1.txt"
    };

    static final class Module {
        final String id;
        final File directory;
        final boolean valid;
        final String error;

        Module(String id, File directory, boolean valid, String error) {
            this.id = id;
            this.directory = directory;
            this.valid = valid;
            this.error = error;
        }
    }

    static final class Selection {
        final String id;
        final File moduleDirectory;
        final File saveDirectory;
        final int validModuleCount;
        final String fallbackError;

        Selection(String id, File moduleDirectory, File saveDirectory,
                  int validModuleCount, String fallbackError) {
            this.id = id;
            this.moduleDirectory = moduleDirectory;
            this.saveDirectory = saveDirectory;
            this.validModuleCount = validModuleCount;
            this.fallbackError = fallbackError;
        }

        boolean isBaseGame() {
            return BASE_MODULE.equals(id);
        }
    }

    private MBModManager() {}

    static String externalModulesRelativePath(String packageName) {
        if (packageName == null || packageName.length() == 0) {
            throw new IllegalArgumentException("package name is empty");
        }
        return "Android/data/" + packageName + "/files/" + MODULES_DIRECTORY;
    }

    static String externalModulesDocumentId(String packageName) {
        return "primary:" + externalModulesRelativePath(packageName);
    }

    static List<Module> scan(File externalFilesDirectory) {
        ArrayList<Module> result = new ArrayList<Module>();
        if (externalFilesDirectory == null) return result;
        File modules = new File(externalFilesDirectory, MODULES_DIRECTORY);
        if (!ensureDirectory(modules)) return result;
        File[] entries = modules.listFiles();
        if (entries == null) return result;
        for (File entry : entries) {
            if (result.size() >= MAX_MODULES) break;
            if (!entry.isDirectory() || entry.getName().startsWith(".")) continue;
            String id = entry.getName();
            String error = validateModuleDirectory(modules, id, entry);
            result.add(new Module(id, entry, error == null, error));
        }
        Collections.sort(result, new Comparator<Module>() {
            @Override public int compare(Module left, Module right) {
                int folded = left.id.compareToIgnoreCase(right.id);
                return folded != 0 ? folded : left.id.compareTo(right.id);
            }
        });
        return result;
    }

    static Selection resolve(File externalFilesDirectory) {
        if (externalFilesDirectory == null || !ensureDirectory(externalFilesDirectory)) {
            throw new IllegalStateException("App-specific external storage is unavailable");
        }
        File modulesRoot = new File(externalFilesDirectory, MODULES_DIRECTORY);
        if (!ensureDirectory(modulesRoot)) {
            throw new IllegalStateException("Cannot create modules directory");
        }
        List<Module> modules = scan(externalFilesDirectory);
        int validCount = 0;
        for (Module module : modules) if (module.valid) ++validCount;

        File selectionFile = new File(modulesRoot, SELECTED_FILE);
        String selected = BASE_MODULE;
        String fallback = null;
        if (selectionFile.isFile()) {
            try {
                selected = readSmallUtf8(selectionFile).trim();
            } catch (IOException exception) {
                selected = BASE_MODULE;
                fallback = "Cannot read selected module: " + exception.getMessage();
            }
        }
        if (selected.length() == 0) selected = BASE_MODULE;

        File selectedDirectory = null;
        if (!BASE_MODULE.equals(selected)) {
            Module match = null;
            for (Module module : modules) {
                if (module.id.equals(selected)) {
                    match = module;
                    break;
                }
            }
            if (match == null || !match.valid) {
                String reason = match == null ? "folder was not found" : match.error;
                fallback = "Module '" + selected + "' is broken: " + reason
                        + ". The base game was restored.";
                selected = BASE_MODULE;
                writeSelection(modulesRoot, BASE_MODULE);
                writeError(modulesRoot, fallback);
            } else {
                selectedDirectory = match.directory;
            }
        }

        File saves = externalFilesDirectory;
        if (!BASE_MODULE.equals(selected)) {
            File saveRoot = new File(externalFilesDirectory, MOD_SAVES_DIRECTORY);
            if (!ensureDirectory(saveRoot)) {
                throw new IllegalStateException("Cannot create per-module save root");
            }
            try {
                saves = new File(saveRoot, selected).getCanonicalFile();
                requireDirectChild(saveRoot.getCanonicalFile(), saves);
            } catch (IOException exception) {
                throw new IllegalStateException("Cannot resolve per-module save directory", exception);
            }
            if (!ensureDirectory(saves)) {
                throw new IllegalStateException("Cannot create per-module save directory");
            }
        }
        return new Selection(selected, selectedDirectory, saves, validCount, fallback);
    }

    static boolean select(File externalFilesDirectory, String id) {
        if (externalFilesDirectory == null || id == null) return false;
        File modulesRoot = new File(externalFilesDirectory, MODULES_DIRECTORY);
        if (!ensureDirectory(modulesRoot)) return false;
        if (!BASE_MODULE.equals(id)) {
            Module match = null;
            for (Module module : scan(externalFilesDirectory)) {
                if (module.id.equals(id)) {
                    match = module;
                    break;
                }
            }
            if (match == null || !match.valid) return false;
        }
        try {
            writeSelection(modulesRoot, id);
            File error = new File(modulesRoot, ERROR_FILE);
            if (error.exists()) error.delete();
            return true;
        } catch (IllegalStateException exception) {
            return false;
        }
    }

    private static String validateModuleDirectory(File modulesRoot, String id, File directory) {
        if (!safeIdentifier(id)) return "unsafe folder name";
        try {
            File canonicalRoot = modulesRoot.getCanonicalFile();
            File canonicalDirectory = directory.getCanonicalFile();
            requireDirectChild(canonicalRoot, canonicalDirectory);
            if (!canonicalDirectory.equals(directory.getAbsoluteFile().getCanonicalFile())) {
                return "folder alias is not allowed";
            }
            for (String required : REQUIRED_FILES) {
                File file = new File(canonicalDirectory, required).getCanonicalFile();
                if (!file.getParentFile().equals(canonicalDirectory)
                        || !file.isFile() || file.length() <= 0L) {
                    return "missing or empty " + required;
                }
                if ("module.ini".equals(required) && file.length() > MAX_MODULE_INI_BYTES) {
                    return "module.ini is too large";
                }
            }
        } catch (IOException | SecurityException exception) {
            return "cannot validate folder";
        }
        return null;
    }

    private static boolean safeIdentifier(String value) {
        if (value.length() == 0 || value.length() > 96
                || ".".equals(value) || "..".equals(value)) return false;
        for (int i = 0; i < value.length(); ++i) {
            char c = value.charAt(i);
            if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') return false;
        }
        return true;
    }

    private static void requireDirectChild(File root, File child) throws IOException {
        File parent = child.getParentFile();
        if (parent == null || !parent.equals(root)) {
            throw new IOException("path escapes modules root");
        }
    }

    private static String readSmallUtf8(File file) throws IOException {
        long size = file.length();
        if (size < 0L || size > MAX_SELECTION_BYTES) {
            throw new IOException("selection file is too large");
        }
        byte[] data = new byte[(int)size];
        FileInputStream input = new FileInputStream(file);
        try {
            int offset = 0;
            while (offset < data.length) {
                int count = input.read(data, offset, data.length - offset);
                if (count < 0) throw new IOException("short selection read");
                offset += count;
            }
        } finally {
            input.close();
        }
        return new String(data, UTF8);
    }

    private static void writeSelection(File modulesRoot, String id) {
        if (!BASE_MODULE.equals(id) && !safeIdentifier(id)) {
            throw new IllegalStateException("unsafe module identifier");
        }
        writeAtomic(new File(modulesRoot, SELECTED_FILE), id + "\n");
    }

    private static void writeError(File modulesRoot, String message) {
        writeAtomic(new File(modulesRoot, ERROR_FILE), message + "\n");
    }

    private static void writeAtomic(File destination, String text) {
        File temporary = new File(destination.getParentFile(), "." + destination.getName() + ".tmp");
        byte[] data = text.getBytes(UTF8);
        try {
            FileOutputStream output = new FileOutputStream(temporary, false);
            try {
                output.write(data);
                output.flush();
                output.getFD().sync();
            } finally {
                output.close();
            }
            if (destination.exists() && !destination.delete()) {
                throw new IOException("cannot replace " + destination.getName());
            }
            if (!temporary.renameTo(destination)) {
                throw new IOException("cannot publish " + destination.getName());
            }
        } catch (IOException | SecurityException exception) {
            throw new IllegalStateException("cannot write " + destination.getName(), exception);
        } finally {
            if (temporary.exists()) temporary.delete();
        }
    }

    private static boolean ensureDirectory(File directory) {
        return directory != null
                && (directory.isDirectory()
                    || (directory.mkdirs() && directory.isDirectory()));
    }
}
