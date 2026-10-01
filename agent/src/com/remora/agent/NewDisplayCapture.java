package com.remora.agent;

import android.app.ActivityOptions;
import android.app.ActivityTaskManager;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.hardware.display.DisplayManager;
import android.hardware.display.DisplayManagerGlobal;
import android.hardware.display.VirtualDisplay;
import android.hardware.display.VirtualDisplayConfig;
import android.os.Bundle;
import android.view.DisplayInfo;
import android.view.Surface;

import java.util.List;
import java.util.Locale;

/**
 * A session on its own virtual display — Remora's desktop/app windows — rather than a mirror of
 * an existing one (bd remora-28ix.3.3). Owns the display's whole lifecycle: create it, optionally
 * launch an app onto it, reflow it when the client's window is resized, and watch for the user
 * leaving the app when the session exists only to host it.
 *
 * The reflow is not opt-in; it is simply how a new display behaves. The client window IS the
 * display, so a resized window that stretches pixels instead of reflowing content is never what
 * was wanted. Density is rescaled with the size for the same
 * reason — a fixed initial dpi makes a grown window's UI look tiny.
 */
public final class NewDisplayCapture implements Capture {
    private static final int ACTIVITY_TYPE_HOME = 2;
    // A flex resize recreates the display and can momentarily hide the app during task
    // migration, so the watchdog needs the launcher to read topmost more than once (a real
    // back-out stays; a resize transient does not) and must ignore the window right after a
    // resize request.
    private static final int RESIZE_SETTLE_MS = 1500;
    private static final int HOME_POLLS_TO_CONFIRM = 2;

    private final DisplayManager dm;
    private final boolean closeOnAppExit;
    private final String startApp;
    private final Runnable onAppExit;

    private VirtualDisplay display;
    private Surface lastSurface;
    // Written by the control thread (requestResize) and read by the video thread (prepare, the
    // rebuild after a reflow) — volatile is the whole synchronization, single writer per field.
    private volatile int width, height, dpi;
    private final int refWidth, refHeight, refDpi;  // anchors for density rescaling
    private volatile long lastResizeMs;
    private volatile boolean appSeen;
    private Thread watchdog;
    private boolean appStarted;

    /**
     * @param size "WxH/dpi", "WxH", or empty/"auto" to follow the main display
     */
    public NewDisplayCapture(String size, String startApp, boolean closeOnAppExit,
                             Runnable onAppExit) {
        this.dm = new DisplayManager(Main.context());
        this.startApp = startApp;
        this.closeOnAppExit = closeOnAppExit;
        this.onAppExit = onAppExit;

        int w = 0, h = 0, d = 0;
        if (size != null && !size.isEmpty() && !"auto".equals(size)) {
            String dims = size;
            int slash = size.indexOf('/');
            if (slash >= 0) {
                dims = size.substring(0, slash);
                try {
                    d = Integer.parseInt(size.substring(slash + 1));
                } catch (NumberFormatException e) {
                    d = 0;
                }
            }
            int x = dims.toLowerCase(Locale.ROOT).indexOf('x');
            if (x > 0) {
                try {
                    w = Integer.parseInt(dims.substring(0, x));
                    h = Integer.parseInt(dims.substring(x + 1));
                } catch (NumberFormatException e) {
                    w = h = 0;
                }
            }
        }
        if (w <= 0 || h <= 0) {
            // Follow the main display, in its natural orientation.
            DisplayInfo main = DisplayManagerGlobal.getInstance().getDisplayInfo(0);
            if (main != null) {
                w = main.logicalWidth;
                h = main.logicalHeight;
                if (d == 0) d = main.logicalDensityDpi;
            } else {
                w = 1920;
                h = 1080;
                if (d == 0) d = 240;
            }
        }
        if (d <= 0) d = 160;
        refWidth = w;
        refHeight = h;
        refDpi = d;
        width = w;
        height = h;
        dpi = d;
    }

    @Override
    public int[] prepare(int alignment, int maxSize) {
        // OUR size fields are authoritative, not a DisplayInfo read-back: this capture is the
        // display's only resizer, and DisplayManagerGlobal's cache can still hold the old size
        // right after requestResize() — the rebuild would latch that stale size with nothing
        // left to trigger a second rebuild once the resize lands.
        int[] size = Sizes.constrain(width, height, alignment, maxSize, Integer.MAX_VALUE,
                                     Integer.MAX_VALUE, 0);
        width = size[0];
        height = size[1];
        return size;
    }

    @Override
    public void start(Surface surface) {
        lastSurface = surface;
        if (display == null) {
            // OWN_CONTENT_ONLY so this is a real second display rather than another view of the
            // first; SUPPORTS_TOUCH and OWN_FOCUS so injected input lands on it; TRUSTED and
            // ALWAYS_UNLOCKED so apps actually launch there; SHOULD_SHOW_SYSTEM_DECORATIONS so it
            // gets a launcher and IME instead of a bare surface.
            int flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_SUPPORTS_TOUCH
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_ROTATES_WITH_CONTENT
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_DESTROY_CONTENT_ON_REMOVAL
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_SHOULD_SHOW_SYSTEM_DECORATIONS
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_TRUSTED
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_DISPLAY_GROUP
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_ALWAYS_UNLOCKED
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_TOUCH_FEEDBACK_DISABLED
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_FOCUS;
            VirtualDisplayConfig config =
                    new VirtualDisplayConfig.Builder("remora-display", width, height, dpi)
                            .setFlags(flags)
                            .setSurface(surface)
                            .build();
            display = dm.createVirtualDisplay(config);
            if (display == null) throw new IllegalStateException("could not create a new display");
            Ln.i("new display " + width + "x" + height + "/" + dpi + " (id="
                 + display.getDisplay().getDisplayId() + ")");
            if (startApp != null && !startApp.isEmpty() && !appStarted) {
                appStarted = true;
                new Thread(this::launchApp, "start-app").start();
            }
        } else {
            // Reattaching after a resize: same display, new consumer surface.
            display.setSurface(surface);
        }
    }

    @Override
    public void stop() {
        // Deliberately NOT releasing the display: a resize rebuilds the consumer around the same
        // display, and releasing here would take the running app down with it
        // (DESTROY_CONTENT_ON_REMOVAL). release() ends it for good.
        if (display != null) display.setSurface(null);
    }

    public void release() {
        if (watchdog != null) {
            watchdog.interrupt();
            watchdog = null;
        }
        if (display != null) {
            display.release();
            display = null;
        }
    }

    @Override
    public int inputDisplayId() {
        return display != null ? display.getDisplay().getDisplayId() : -1;
    }

    @Override
    public void requestRecompose(boolean hard) {
        VirtualDisplay vd = display;
        if (vd == null) return;
        try {
            if (hard) {
                Surface s = lastSurface;
                vd.setSurface(null);
                vd.setSurface(s);
            } else {
                vd.resize(width, height, dpi);
            }
        } catch (Exception e) {
            Ln.w("could not force a recomposition: " + e);
        }
    }

    /**
     * The client's window was resized: reflow the display to match instead of stretching its
     * pixels. Density is rescaled with the size so UI elements keep their physical size.
     */
    public void requestResize(int w, int h) {
        VirtualDisplay vd = display;
        if (vd == null || w <= 0 || h <= 0) return;
        lastResizeMs = System.currentTimeMillis();
        int[] size = Sizes.constrain(w, h, 2, 0, Integer.MAX_VALUE, Integer.MAX_VALUE, 0);
        width = size[0];
        height = size[1];
        dpi = scaleDpi(size[0], size[1]);
        try {
            vd.resize(width, height, dpi);
        } catch (Exception e) {
            Ln.w("could not resize the display: " + e);
        }
    }

    private int scaleDpi(int w, int h) {
        final int den = Math.max(refWidth, refHeight);
        final int num = Math.max(w, h);
        return den > 0 ? Math.max(1, refDpi * num / den) : refDpi;
    }

    private void launchApp() {
        String name = startApp;
        boolean forceStop = name.startsWith("+");
        if (forceStop) name = name.substring(1);
        final PackageManager pm = Main.context().getPackageManager();
        String pkg = name;
        if (name.startsWith("?")) {
            pkg = findByLabel(pm, name.substring(1));
            if (pkg == null) {
                Ln.w("no app found matching \"" + name.substring(1) + "\"");
                return;
            }
        }
        Intent intent = pm.getLaunchIntentForPackage(pkg);
        if (intent == null) intent = pm.getLeanbackLaunchIntentForPackage(pkg);
        if (intent == null) {
            Ln.w("no launch intent for package " + pkg);
            return;
        }
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        ActivityOptions options = ActivityOptions.makeBasic();
        options.setLaunchDisplayId(inputDisplayId());
        Bundle bundle = options.toBundle();
        try {
            if (forceStop) {
                android.app.ActivityManager am =
                        Main.context().getSystemService(android.app.ActivityManager.class);
                if (am != null) am.forceStopPackage(pkg);
            }
            // NOT context.startActivity(): ContextImpl stamps its OWN package on the call —
            // "android", because the base is the system context — and ATMS rejects a package
            // that does not belong to uid 2000, wrapper overrides notwithstanding. The direct
            // call is the same binder transaction with the attribution chosen honestly.
            int res = ActivityTaskManager.getService().startActivityAsUser(
                    null, ShellContext.PACKAGE_NAME, null, intent,
                    intent.resolveTypeIfNeeded(Main.context().getContentResolver()),
                    null, null, 0, 0, null, bundle, 0);
            if (res < 0) {  // ActivityManager.START_* — failures are the negative codes
                Ln.w("could not start " + pkg + " (start code " + res + ")");
                return;
            }
            Ln.i("started " + pkg + " on display " + inputDisplayId());
        } catch (Exception e) {
            Ln.e("could not start " + pkg, e);
            return;
        }
        if (closeOnAppExit) startWatchdog(pkg);
    }

    private static String findByLabel(PackageManager pm, String prefix) {
        final String needle = prefix.toLowerCase(Locale.ROOT);
        for (ApplicationInfo info : pm.getInstalledApplications(PackageManager.GET_META_DATA)) {
            if (!info.enabled) continue;
            CharSequence label = pm.getApplicationLabel(info);
            if (label != null && label.toString().toLowerCase(Locale.ROOT).startsWith(needle)) {
                return info.packageName;
            }
        }
        return null;
    }

    /**
     * close_on_app_exit: the started app IS the window, so the session should end when the user
     * leaves it rather than linger on the launcher.
     *
     * The signal is the display's root-task Z-ORDER, not composition or task-change callbacks.
     * On a virtual display the launcher takes ~2s to actually draw after a back-out, and every
     * draw-gated signal — TaskStackListener, RunningTaskInfo.isVisible(), SurfaceFlinger window
     * visibility — lags by that whole 2s. The window-manager z-order flips within ~100ms, as soon
     * as the transition is collected. So: the app left iff the topmost root task is HOME.
     */
    private void startWatchdog(String pkg) {
        watchdog = new Thread(() -> {
            int homePolls = 0;
            while (!Thread.currentThread().isInterrupted()) {
                try {
                    Thread.sleep(100);
                } catch (InterruptedException e) {
                    return;
                }
                final int did = inputDisplayId();
                if (did < 0 || System.currentTimeMillis() - lastResizeMs < RESIZE_SETTLE_MS) {
                    homePolls = 0;
                    continue;
                }
                try {
                    List<ActivityTaskManager.RootTaskInfo> roots =
                            ActivityTaskManager.getService().getAllRootTaskInfosOnDisplay(did);
                    if (roots == null || roots.isEmpty()) continue;  // display in flux; no verdict
                    // Top-to-bottom, so the first entry is the topmost root task.
                    if (roots.get(0).getActivityType() != ACTIVITY_TYPE_HOME) {
                        appSeen = true;
                        homePolls = 0;
                    } else if (appSeen && ++homePolls >= HOME_POLLS_TO_CONFIRM) {
                        Ln.i(pkg + " left display " + did + " (launcher on top) — closing session");
                        onAppExit.run();
                        return;
                    }
                } catch (Exception e) {
                    Ln.w("app exit watchdog stopped: " + e);
                    return;
                }
            }
        }, "app-exit-watchdog");
        watchdog.setDaemon(true);
        watchdog.start();
    }
}
