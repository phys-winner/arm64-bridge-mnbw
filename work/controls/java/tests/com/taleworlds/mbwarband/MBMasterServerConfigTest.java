package com.taleworlds.mbwarband;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.Charset;

public final class MBMasterServerConfigTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void testHostnameValidation() {
        check("master.example.org".equals(
                        MBMasterServerConfig.normalizeHost(" MASTER.Example.Org ")),
                "hostname should be trimmed and lowercased");
        check("192.0.2.10".equals(MBMasterServerConfig.normalizeHost("192.0.2.10")),
                "IPv4 literal should be accepted");
        check("".equals(MBMasterServerConfig.normalizeHost("")), "empty host fallback");
        check("".equals(MBMasterServerConfig.normalizeHost("https://master.example.org")),
                "scheme must not be accepted as part of the hostname");
        check("".equals(MBMasterServerConfig.normalizeHost("bad..example")),
                "empty DNS labels must be rejected");
        check("".equals(MBMasterServerConfig.normalizeHost("-bad.example")),
                "DNS labels cannot begin with a hyphen");
        check("".equals(MBMasterServerConfig.normalizeHost("this-hostname-is-too-long.example")),
                "hostname must fit the stock guest URL storage");
    }

    private static void testConfigRead() throws Exception {
        File directory = new File(System.getProperty("java.io.tmpdir"),
                "mb-master-config-test-" + System.nanoTime());
        check(directory.mkdirs(), "temporary directory should be created");
        File config = new File(directory, "rgl_config.txt");
        try {
            check("".equals(MBMasterServerConfig.readHost(config)),
                    "missing config should retain the stock master");
            FileOutputStream output = new FileOutputStream(config);
            try {
                output.write(("# comment\nother_setting = 3\n"
                        + "master_server_host = first.example\n"
                        + "master_server_host = second.example\n")
                        .getBytes(Charset.forName("UTF-8")));
            } finally {
                output.close();
            }
            check("second.example".equals(MBMasterServerConfig.readHost(config)),
                    "last valid host setting should win");
            output = new FileOutputStream(config);
            try {
                output.write("master_server_host = invalid/path\n"
                        .getBytes(Charset.forName("UTF-8")));
            } finally {
                output.close();
            }
            check("".equals(MBMasterServerConfig.readHost(config)),
                    "invalid configured host should fall back safely");
        } finally {
            config.delete();
            directory.delete();
        }
    }

    private static void testSavedServerChoices() throws Exception {
        File directory = new File(System.getProperty("java.io.tmpdir"),
                "mb-master-selection-test-" + System.nanoTime());
        check(directory.mkdirs(), "selection test directory should be created");
        File config = new File(directory, "rgl_config.txt");
        try {
            MBMasterServerConfig.Selection selection = MBMasterServerConfig.readSelection(config);
            check(MBMasterServerConfig.MODE_LAN.equals(selection.mode),
                    "a fresh install must default to LAN only");
            check("".equals(selection.host), "LAN-only mode must not select a public host");

            MBMasterServerConfig.writeMode(config, MBMasterServerConfig.MODE_OFFICIAL);
            selection = MBMasterServerConfig.readSelection(config);
            check(MBMasterServerConfig.MODE_OFFICIAL.equals(selection.mode),
                    "official server choice must persist");
            check("".equals(selection.host), "official mode leaves the stock URL intact");

            MBMasterServerConfig.writeMode(config, MBMasterServerConfig.MODE_OREO922);
            selection = MBMasterServerConfig.readSelection(config);
            check(MBMasterServerConfig.MODE_OREO922.equals(selection.mode),
                    "community server choice must persist");
            check(MBMasterServerConfig.OREO922_HOST.equals(selection.host),
                    "community choice must resolve to its preset host");

            MBMasterServerConfig.writeHost(config, "legacy.example.net");
            MBMasterServerConfig.writeMode(config, MBMasterServerConfig.MODE_LAN);
            selection = MBMasterServerConfig.readSelection(config);
            check(MBMasterServerConfig.MODE_LAN.equals(selection.mode),
                    "selected mode must remain independent from saved custom host");
            check("legacy.example.net".equals(selection.savedHost),
                    "saved custom host must remain available after switching modes");
            MBMasterServerConfig.writeMode(config, MBMasterServerConfig.MODE_CUSTOM);
            selection = MBMasterServerConfig.readSelection(config);
            check(MBMasterServerConfig.MODE_CUSTOM.equals(selection.mode),
                    "saved custom choice must be selectable");
            check("legacy.example.net".equals(selection.host),
                    "custom selection must use its saved hostname");

            MBMasterServerConfig.saveCustomHost(config, "first.example.org");
            MBMasterServerConfig.saveCustomHost(config, "SECOND.example.org");
            selection = MBMasterServerConfig.readSelection(config);
            check(selection.savedHosts.length == 3,
                    "legacy and two added hosts must all remain saved");
            check("second.example.org".equals(selection.host),
                    "the most recently added host must be selected");
            MBMasterServerConfig.selectCustomHost(config, "first.example.org");
            selection = MBMasterServerConfig.readSelection(config);
            check("first.example.org".equals(selection.host),
                    "a previous saved hostname must remain selectable");
            MBMasterServerConfig.removeCustomHost(config, "first.example.org");
            selection = MBMasterServerConfig.readSelection(config);
            check(selection.savedHosts.length == 2,
                    "removing one hostname must retain the other saved hosts");
            check(!"first.example.org".equals(selection.host),
                    "removed selected hostname must no longer be active");
            MBMasterServerConfig.writeMode(config, MBMasterServerConfig.MODE_LAN);
            selection = MBMasterServerConfig.readSelection(config);
            check(selection.savedHosts.length == 2 && "".equals(selection.host),
                    "LAN-only selection must retain saved hosts without using a public host");
        } finally {
            config.delete();
            directory.delete();
        }
    }

    private static void testLegacyMigration() throws Exception {
        File directory = new File(System.getProperty("java.io.tmpdir"),
                "mb-master-migration-test-" + System.nanoTime());
        check(directory.mkdirs(), "migration test directory should be created");
        File config = new File(directory, "rgl_config.txt");
        try {
            FileOutputStream output = new FileOutputStream(config);
            try {
                output.write("master_server_host = oreo922.cn\n"
                        .getBytes(Charset.forName("UTF-8")));
            } finally {
                output.close();
            }
            MBMasterServerConfig.Selection selection = MBMasterServerConfig.readSelection(config);
            check(MBMasterServerConfig.MODE_OREO922.equals(selection.mode),
                    "legacy community hostname must migrate to its preset");

            output = new FileOutputStream(config);
            try {
                output.write("master_server_host = old.example\n"
                        .getBytes(Charset.forName("UTF-8")));
            } finally {
                output.close();
            }
            selection = MBMasterServerConfig.readSelection(config);
            check(MBMasterServerConfig.MODE_CUSTOM.equals(selection.mode),
                    "legacy custom hostname must remain selectable");
            check("old.example".equals(selection.host),
                    "legacy custom hostname must survive migration");
        } finally {
            config.delete();
            directory.delete();
        }
    }

    public static void main(String[] args) throws Exception {
        testHostnameValidation();
        testConfigRead();
        testSavedServerChoices();
        testLegacyMigration();
        System.out.println("MBMasterServerConfigTest: PASS");
    }
}
