package com.remora.agent;

import android.content.AttributionSource;
import android.content.Context;
import android.content.ContextWrapper;
import android.os.Process;

/**
 * The system context re-attributed to the shell package. The agent runs as uid 2000 (init:
 * `user shell`), but ActivityThread.systemMain()'s context claims package "android"; services
 * that verify the calling package against the uid (AppOpsManager.checkPackage in
 * ClipboardService) would reject that pairing. "com.android.shell" is the package uid 2000
 * actually owns, and it holds READ_CLIPBOARD_IN_BACKGROUND — the exact exemption that lets a
 * background shell caller read the clipboard at all ("Shell can access the clipboard for
 * testing purposes", ClipboardService).
 *
 * There is no reflection and no service field-poking here: anything
 * needing this attribution is constructed directly against this context (the framework classes
 * are plain API when compiled in-tree, docs/MIRROR_AGENT.md).
 */
public final class ShellContext extends ContextWrapper {
    public static final String PACKAGE_NAME = "com.android.shell";

    public ShellContext(Context systemContext) {
        super(systemContext);
    }

    @Override
    public String getPackageName() {
        return PACKAGE_NAME;
    }

    @Override
    public String getOpPackageName() {
        return PACKAGE_NAME;
    }

    @Override
    public AttributionSource getAttributionSource() {
        return new AttributionSource.Builder(Process.SHELL_UID)
                .setPackageName(PACKAGE_NAME)
                .build();
    }

    @Override
    public Context getApplicationContext() {
        return this;
    }
}
