package com.remora.agent;

import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;

import java.util.List;
import java.util.TreeMap;

/**
 * kind=list-apps (bd remora-28ix.3.4): the launchable-app listing, served over the control
 * connection as one u32-prefixed UTF-8 blob instead of a server process's stdout.
 *
 * The line format — " * Name  pkg" for system apps, " - " for user apps, package last — is fixed
 * by its consumer: remora-app-menu.sh's awk, whose contract is the marker, the dot in the last
 * field, and whitespace collapsing. Nothing here may change without that script.
 */
public final class AppList {
    public static byte[] listing() {
        final PackageManager pm = Main.context().getPackageManager();
        // One query for every launcher activity, rather than a per-package launch-intent probe —
        // a binder call per installed package is seconds of enumeration for the same answer.
        Intent launcher = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        List<ResolveInfo> activities =
                pm.queryIntentActivities(launcher, PackageManager.MATCH_ALL);
        // Deduped by package (multi-activity apps list once) and sorted by label for a stable,
        // human-ordered listing; TreeMap keys on "label\0pkg" so equal labels cannot collide.
        TreeMap<String, String> lines = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
        for (ResolveInfo ri : activities) {
            if (ri.activityInfo == null) continue;
            ApplicationInfo app = ri.activityInfo.applicationInfo;
            CharSequence label = pm.getApplicationLabel(app);
            String name = label != null ? label.toString().trim() : app.packageName;
            if (name.isEmpty()) name = app.packageName;
            final boolean system = (app.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
            lines.put(name + "\0" + app.packageName,
                      (system ? " * " : " - ") + name + "  " + app.packageName + "\n");
        }
        StringBuilder sb = new StringBuilder();
        for (String line : lines.values()) sb.append(line);
        return sb.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8);
    }

    private AppList() {}
}
