package com.taleworlds.mbwarband;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.Locale;

/** Saved master-server choice used at startup and for live server switching. */
final class MBMasterServerConfig {
    static final String KEY = "master_server_host";
    static final String SAVED_HOSTS_KEY = "master_server_saved_hosts";
    static final String MODE_KEY = "master_server_mode";
    static final String MODE_LAN = "lan";
    static final String MODE_OFFICIAL = "official";
    static final String MODE_OREO922 = "oreo922";
    static final String MODE_CUSTOM = "custom";
    static final String OREO922_HOST = "oreo922.cn";
    static final int MAX_HOST_LENGTH = 26;
    static final int MAX_SAVED_HOSTS = 32;
    private static final long MAX_CONFIG_BYTES = 1024L * 1024L;
    private static final Charset UTF_8 = Charset.forName("UTF-8");

    static final class Selection {
        final String mode;
        final String host;
        final String savedHost;
        final String[] savedHosts;

        Selection(String mode, String host, String savedHost) {
            this(mode, host, savedHost, new String[0]);
        }

        Selection(String mode, String host, String savedHost, String[] savedHosts) {
            this.mode = mode;
            this.host = host;
            this.savedHost = savedHost;
            this.savedHosts = savedHosts;
        }
    }

    private MBMasterServerConfig() {}

    static String normalizeHost(String value) {
        if (value == null) return "";
        String host = value.trim().toLowerCase(Locale.US);
        if (host.length() == 0 || host.length() > MAX_HOST_LENGTH) return "";
        if (host.charAt(0) == '.' || host.charAt(host.length() - 1) == '.') return "";
        boolean previousDot = false;
        int labelLength = 0;
        for (int index = 0; index < host.length(); ++index) {
            char valueAt = host.charAt(index);
            if (valueAt == '.') {
                if (previousDot || labelLength == 0 || host.charAt(index - 1) == '-') return "";
                previousDot = true;
                labelLength = 0;
            } else {
                boolean alphaNumeric = (valueAt >= 'a' && valueAt <= 'z')
                        || (valueAt >= '0' && valueAt <= '9');
                if (!alphaNumeric && valueAt != '-') return "";
                if (labelLength == 0 && valueAt == '-') return "";
                previousDot = false;
                ++labelLength;
            }
        }
        if (labelLength == 0 || host.charAt(host.length() - 1) == '-') return "";
        return host;
    }

    static Selection readSelection(File configFile) throws IOException {
        String savedHost = normalizeHost(readSetting(configFile, KEY));
        String[] savedHosts = readSavedHosts(configFile);
        if (savedHost.length() == 0 && savedHosts.length != 0) {
            savedHost = savedHosts[0];
        }
        String requestedMode = readSetting(configFile, MODE_KEY).trim().toLowerCase(Locale.US);
        String mode;
        if (isMode(requestedMode)) {
            mode = requestedMode;
        } else if (requestedMode.length() == 0) {
            // Upgrade older installs without a saved mode. Keep a prior custom host,
            // while a fresh install now starts in LAN-only mode.
            if (savedHost.length() == 0) mode = MODE_LAN;
            else if (OREO922_HOST.equals(savedHost)) mode = MODE_OREO922;
            else mode = MODE_CUSTOM;
        } else {
            mode = MODE_LAN;
        }
        if (MODE_CUSTOM.equals(mode) && OREO922_HOST.equals(savedHost)) {
            mode = MODE_OREO922;
        }
        if (MODE_CUSTOM.equals(mode) && savedHost.length() == 0) mode = MODE_LAN;
        String host = MODE_OREO922.equals(mode) ? OREO922_HOST
                : MODE_CUSTOM.equals(mode) ? savedHost : "";
        return new Selection(mode, host, savedHost, savedHosts);
    }

    static String[] readSavedHosts(File configFile) throws IOException {
        ArrayList<String> hosts = new ArrayList<String>();
        String raw = readSetting(configFile, SAVED_HOSTS_KEY);
        for (String item : raw.split(",")) {
            String host = normalizeHost(item);
            if (host.length() != 0 && !OREO922_HOST.equals(host)
                    && !hosts.contains(host) && hosts.size() < MAX_SAVED_HOSTS) {
                hosts.add(host);
            }
        }
        // Older builds persisted one host in KEY. Include it in the new list.
        String legacy = normalizeHost(readSetting(configFile, KEY));
        if (legacy.length() != 0 && !OREO922_HOST.equals(legacy) && !hosts.contains(legacy)) {
            if (hosts.size() == MAX_SAVED_HOSTS) hosts.remove(hosts.size() - 1);
            hosts.add(legacy);
        }
        return hosts.toArray(new String[hosts.size()]);
    }

    static void saveCustomHost(File configFile, String requestedHost) throws IOException {
        String host = normalizeHost(requestedHost);
        if (host.length() == 0 || OREO922_HOST.equals(host)) {
            throw new IllegalArgumentException("Invalid custom master server hostname");
        }
        String[] saved = readSavedHosts(configFile);
        ArrayList<String> hosts = new ArrayList<String>();
        for (String item : saved) hosts.add(item);
        if (!hosts.contains(host)) {
            if (hosts.size() >= MAX_SAVED_HOSTS) {
                throw new IllegalArgumentException("Too many saved master servers");
            }
            hosts.add(host);
        }
        writeSetting(configFile, SAVED_HOSTS_KEY, joinHosts(hosts));
        writeSetting(configFile, KEY, host);
        writeSetting(configFile, MODE_KEY, MODE_CUSTOM);
    }

    static void selectCustomHost(File configFile, String requestedHost) throws IOException {
        String host = normalizeHost(requestedHost);
        if (host.length() == 0 || !containsHost(readSavedHosts(configFile), host)) {
            throw new IllegalArgumentException("Master server hostname is not saved");
        }
        writeSetting(configFile, KEY, host);
        writeSetting(configFile, MODE_KEY, MODE_CUSTOM);
    }

    static void removeCustomHost(File configFile, String requestedHost) throws IOException {
        String host = normalizeHost(requestedHost);
        if (host.length() == 0 || !containsHost(readSavedHosts(configFile), host)) {
            throw new IllegalArgumentException("Master server hostname is not saved");
        }
        ArrayList<String> hosts = new ArrayList<String>();
        for (String item : readSavedHosts(configFile)) {
            if (!host.equals(item)) hosts.add(item);
        }
        String selected = normalizeHost(readSetting(configFile, KEY));
        writeSetting(configFile, SAVED_HOSTS_KEY, joinHosts(hosts));
        if (host.equals(selected)) {
            writeSetting(configFile, KEY, hosts.isEmpty() ? "" : hosts.get(0));
            if (hosts.isEmpty() && MODE_CUSTOM.equals(readSetting(configFile, MODE_KEY))) {
                writeSetting(configFile, MODE_KEY, MODE_LAN);
            }
        }
    }

    private static boolean containsHost(String[] hosts, String host) {
        for (String item : hosts) if (item.equals(host)) return true;
        return false;
    }

    private static String joinHosts(ArrayList<String> hosts) {
        StringBuilder joined = new StringBuilder();
        for (String host : hosts) {
            if (joined.length() != 0) joined.append(',');
            joined.append(host);
        }
        return joined.toString();
    }

    static boolean isMode(String mode) {
        return MODE_LAN.equals(mode) || MODE_OFFICIAL.equals(mode)
                || MODE_OREO922.equals(mode) || MODE_CUSTOM.equals(mode);
    }

    static void writeMode(File configFile, String requestedMode) throws IOException {
        String mode = requestedMode == null ? "" : requestedMode.trim().toLowerCase(Locale.US);
        if (!isMode(mode)) throw new IllegalArgumentException("Invalid master server mode");
        if (MODE_CUSTOM.equals(mode) && normalizeHost(readSetting(configFile, KEY)).length() == 0) {
            throw new IllegalArgumentException("No saved custom master server hostname");
        }
        writeSetting(configFile, MODE_KEY, mode);
    }

    static String readHost(File configFile) throws IOException {
        return normalizeHost(readSetting(configFile, KEY));
    }

    /** Retained for migration tools and callers from older control builds. */
    static void writeHost(File configFile, String requestedHost) throws IOException {
        String host = normalizeHost(requestedHost);
        if (requestedHost == null || (requestedHost.trim().length() != 0 && host.length() == 0)) {
            throw new IllegalArgumentException("Invalid master server hostname");
        }
        writeSetting(configFile, KEY, host);
    }

    private static String readSetting(File configFile, String key) throws IOException {
        if (configFile == null || !configFile.isFile()) return "";
        long length = configFile.length();
        if (length < 0L || length > MAX_CONFIG_BYTES) return "";
        FileInputStream input = new FileInputStream(configFile);
        byte[] contents;
        try {
            ByteArrayOutputStream output = new ByteArrayOutputStream((int) length);
            byte[] buffer = new byte[4096];
            int count;
            while ((count = input.read(buffer)) >= 0) {
                if (count == 0) continue;
                output.write(buffer, 0, count);
                if (output.size() > MAX_CONFIG_BYTES) return "";
            }
            contents = output.toByteArray();
        } finally {
            input.close();
        }

        String value = "";
        String[] lines = new String(contents, UTF_8).split("\\r?\\n");
        for (String line : lines) {
            String trimmed = line.trim();
            if (trimmed.length() == 0 || trimmed.startsWith("#") || trimmed.startsWith(";")) {
                continue;
            }
            int equals = trimmed.indexOf('=');
            if (equals >= 0 && key.equals(trimmed.substring(0, equals).trim())) {
                value = trimmed.substring(equals + 1).trim();
            }
        }
        return value;
    }

    private static void writeSetting(File configFile, String key, String value)
            throws IOException {
        File parent = configFile.getParentFile();
        if (parent == null || !parent.isDirectory()) {
            throw new IOException("Configuration directory is unavailable");
        }
        String original = "";
        if (configFile.isFile()) {
            long length = configFile.length();
            if (length < 0L || length > MAX_CONFIG_BYTES) {
                throw new IOException("Configuration file is too large");
            }
            FileInputStream input = new FileInputStream(configFile);
            try {
                ByteArrayOutputStream output = new ByteArrayOutputStream((int) length);
                byte[] buffer = new byte[4096];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    output.write(buffer, 0, count);
                    if (output.size() > MAX_CONFIG_BYTES) {
                        throw new IOException("Configuration file is too large");
                    }
                }
                original = new String(output.toByteArray(), UTF_8);
            } finally {
                input.close();
            }
        }
        StringBuilder updated = new StringBuilder(original.length() + 64);
        String[] lines = original.split("\\r?\\n", -1);
        for (int index = 0; index < lines.length; ++index) {
            String line = lines[index];
            if (index == lines.length - 1 && line.length() == 0) continue;
            int equals = line.indexOf('=');
            if (equals >= 0 && key.equals(line.substring(0, equals).trim())) continue;
            if (line.length() == 0 && updated.length() == 0) continue;
            updated.append(line).append('\n');
        }
        if (value.length() != 0) updated.append(key).append('=').append(value).append('\n');
        if (updated.length() > MAX_CONFIG_BYTES) {
            throw new IOException("Configuration file is too large");
        }
        File temporary = new File(parent, ".rgl_config.master.tmp");
        File backup = new File(parent, ".rgl_config.master.bak");
        if (temporary.exists() && !temporary.delete()) throw new IOException("Cannot clear staged config");
        FileOutputStream output = new FileOutputStream(temporary, false);
        try {
            output.write(updated.toString().getBytes(UTF_8));
            output.flush();
            output.getFD().sync();
        } finally {
            output.close();
        }
        if (backup.exists() && !backup.delete()) throw new IOException("Cannot clear config backup");
        boolean originalMoved = false;
        try {
            if (configFile.exists()) {
                if (!configFile.renameTo(backup)) throw new IOException("Cannot stage original config");
                originalMoved = true;
            }
            if (!temporary.renameTo(configFile)) throw new IOException("Cannot publish config");
            originalMoved = false;
            if (backup.exists() && !backup.delete()) throw new IOException("Cannot clear config backup");
        } finally {
            if (originalMoved && !configFile.exists()) backup.renameTo(configFile);
            if (temporary.exists()) temporary.delete();
        }
    }
}
