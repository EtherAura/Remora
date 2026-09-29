package com.remora.agent;

import android.util.Log;

/**
 * Agent logging. Everything goes to logcat (tag "remora-agent") because init discards a
 * service's stdout, and stdout under app_process is block-buffered anyway — a long-lived agent
 * printing there says nothing until it exits (bd remora-28ix.3.5). The System.err mirror is for
 * development runs from an interactive adb shell, where it is unbuffered and immediate.
 */
public final class Ln {
    private static final String TAG = "remora-agent";

    public static void i(String msg) {
        Log.i(TAG, msg);
        System.err.println("remora-agent: " + msg);
    }

    public static void w(String msg) {
        Log.w(TAG, msg);
        System.err.println("remora-agent: W: " + msg);
    }

    public static void e(String msg, Throwable t) {
        Log.e(TAG, msg, t);
        System.err.println("remora-agent: E: " + msg + (t != null ? ": " + t : ""));
    }

    private Ln() {}
}
