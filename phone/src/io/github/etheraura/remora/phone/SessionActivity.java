package io.github.etheraura.remora.phone;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;
import android.window.OnBackInvokedDispatcher;

import java.io.Closeable;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;

import javax.net.ssl.SSLSocket;

/**
 * One app, opened on the phone: a new display of this view's shape and density on the source
 * (protocol v3 new-display with start_app), at its size or a lighter one (the session size
 * setting), its video and audio decoded here, and the phone's touch, keys, Back and clipboard
 * sent back. When the view changes size — rotation, unfolding, the keyboard — the remote display
 * is asked to reflow (RESIZE), never stretched.
 *
 * Its controls (SessionControls) are a handle on the screen's edge: the keyboard, the phone's
 * clipboard pasted into the app, the session size and the app's scale, both changed in place, and
 * the way out. Back belongs to the remote app, so it cannot be the way out.
 *
 * The screen going off, or the app going to the back, takes the surface away but not the session:
 * the video connection goes, the session and its sound stay, and the picture comes back on a new
 * video connection to the same session when the surface does (the agent opens every new one with
 * a keyframe). Away for five minutes, the session ends: it holds a screen and an encoder on the
 * computer. If the connection was lost meanwhile, the app is opened again in a new session.
 * SessionService is what keeps the connections and the sound alive meanwhile.
 */
public final class SessionActivity extends Activity implements SurfaceHolder.Callback {
    static final String HOST = "host", SOURCE = "source", APP = "app", APP_NAME = "appName",
            KIND = "kind";

    private final ExecutorService writer = Executors.newSingleThreadExecutor();
    private SessionView view;
    private SessionControls controls;
    private TextView inputItem, sizeItem, scaleValue, windowValue, fitItem, deskItem, bypassItem;
    private volatile SSLSocket control, video, audio;
    private volatile boolean ending;
    private boolean started;
    private int width, height;  // the view's, in pixels
    private String lastClip;

    private final Handler ui = new Handler(Looper.getMainLooper());
    static final long AWAY_LIMIT_MS = 5 * 60_000;
    private final Runnable awayTooLong = () -> end(null);
    private volatile Link.Host host;
    private volatile String source;
    private volatile int sessionId = -1;  // -1 until the agent has answered the request
    // {width, height, dpi} the remote display was last asked for: new-display, then RESIZE.
    private volatile int[] asked;
    private final Runnable resizeLater = this::syncSize;
    // A video connection's end is the session's only while it is the current one; one closed
    // because the surface went is not.
    private final AtomicInteger videoGen = new AtomicInteger();
    private volatile Thread videoThread;
    private volatile boolean away;  // the surface is gone; the session waits for it
    private volatile boolean lost;  // and its connection went meanwhile: a new one on return
    private boolean service;  // SessionService runs for this session

    // The session size setting (MainActivity): the remote display's long side, or 0 for the
    // view's own size. A smaller display at proportionally lower density lays the app out the
    // same, and costs the computer, the link and this phone's decoder a fraction of the pixels;
    // the surface scales it up to fill the view. Positions are fractions, so input is unchanged.
    static final String PREF_MAX_LONG_SIDE = "maxLongSide";
    // Asked once: SessionService runs without it, but its notification, the way back to a
    // session that is still open, is then not shown.
    private static final String PREF_ASKED_NOTIFICATIONS = "askedNotifications";

    // Full, Balanced, Light: the remote display's long side, 0 for the view's own.
    static final int[] SIZES = {0, 1920, 1280};
    private static final String[] SIZE_NAMES = {"Full", "Balanced", "Light"};

    // The app's scale, per source, in percent of the phone's own density: smaller fits more of a
    // desktop app on the phone, larger reads more easily. Browser zoom's steps, as far as a
    // phone-sized remote display stays usable.
    private static final int[] SCALES = {50, 67, 75, 80, 90, 100, 110, 125, 150};

    // A computer's app: its window's share of each side of the phone's screen, per app; whether
    // the phone's screen extends that computer's desk — its pointer and windows can cross onto
    // it — or stands apart from it; and whether the app bypasses the desktop, both per computer.
    // Bypassing it, the app fills a screen of its own on the computer, maximized, where no
    // wallpaper shows and tiling scripts leave it alone, and a smaller window is a smaller screen,
    // shown at the same size on black. Not bypassing it, the app is a window of the desk's like
    // any other at that size, with the desktop behind it and tiling scripts arranging it.
    private static final int[] WINDOWS = {50, 60, 75, 90, 100};
    // Which of its sides a smaller window keeps whole: Stretch, neither (Window shrinks both);
    // Width, the screen's full width, Window setting the height; Height, the other way about.
    private static final String[] FITS = {"Stretch", "Width", "Height"};
    private static final int FIT_STRETCH = 0, FIT_WIDTH = 1, FIT_HEIGHT = 2;

    private boolean computer() {
        return "computer".equals(getIntent().getStringExtra(KIND));
    }

    private String windowKey() {
        Intent i = getIntent();
        return "window:" + i.getStringExtra(HOST) + ":" + i.getStringExtra(SOURCE) + ":"
                + i.getStringExtra(APP);
    }

    private int windowPercent() {
        return prefs().getInt(windowKey(), 100);
    }

    private String fitKey() {
        return windowKey().replaceFirst("^window:", "fit:");
    }

    private int fit() {
        int f = prefs().getInt(fitKey(), FIT_STRETCH);
        return f >= 0 && f < FITS.length ? f : FIT_STRETCH;
    }

    /** {width, height}: the window's sides as percentages of the screen's, by Window and Fit. */
    private int[] windowShare() {
        int p = windowPercent(), f = fit();
        return new int[] {f == FIT_WIDTH ? 100 : p, f == FIT_HEIGHT ? 100 : p};
    }

    private String deskKey() {
        return "extendDesk:" + getIntent().getStringExtra(HOST);
    }

    private boolean extendDesk() {
        return prefs().getBoolean(deskKey(), true);
    }

    private String bypassKey() {
        return "bypassDesktop:" + getIntent().getStringExtra(HOST);
    }

    private boolean bypassDesktop() {
        return prefs().getBoolean(bypassKey(), true);
    }

    /** Window and Fit, as Bypass desktop says to carry them out; `tell`: the computer too. */
    private void applyWindow(boolean tell) {
        boolean bypass = bypassDesktop();
        int[] share = windowShare();
        if (bypass) view.setShare(share[0], share[1]);
        else view.setShare(100, 100);
        if (tell) send(bypass ? Proto.window(100, 100, false)
                              : Proto.window(share[0], share[1], true));
    }

    private SharedPreferences prefs() {
        return getSharedPreferences("session", MODE_PRIVATE);
    }

    private int maxLongSide() {
        return prefs().getInt(PREF_MAX_LONG_SIDE, 0);
    }

    private String scaleKey() {
        Intent i = getIntent();
        return "scale:" + i.getStringExtra(HOST) + ":" + i.getStringExtra(SOURCE);
    }

    private int scalePercent() {
        return prefs().getInt(scaleKey(), 100);
    }

    // Pointer or touch, per source. A desktop app is made for a mouse — hover, the secondary
    // button, a wheel — so this computer's apps start with the pointer and Android's with touch.
    private String pointerKey() {
        Intent i = getIntent();
        return "pointer:" + i.getStringExtra(HOST) + ":" + i.getStringExtra(SOURCE);
    }

    private boolean pointerMode() {
        return prefs().getBoolean(pointerKey(), "computer".equals(getIntent().getStringExtra(KIND)));
    }

    /** {width, height, dpi} of the remote display for a view of w×h pixels. */
    private int[] remoteSize(int w, int h) {
        int dpi = getResources().getDisplayMetrics().densityDpi;
        int cap = maxLongSide();
        int longSide = Math.max(w, h);
        double f = cap > 0 && longSide > cap ? (double) cap / longSide : 1;
        int scaled = Math.max(1, (int) Math.round(dpi * f * scalePercent() / 100.0));
        if (f == 1) return new int[] {w, h, scaled};
        // Even sizes: video encoders work in 2×2 blocks.
        return new int[] {(int) Math.round(w * f) & ~1, (int) Math.round(h * f) & ~1, scaled};
    }

    /**
     * Asks for the remote display this view and the settings now call for, if it is not the one
     * last asked for. Every RESIZE names its density: the agent would otherwise keep the one it
     * began with, and the scale could not change.
     */
    private void syncSize() {
        if (!started || away || sessionId < 0) return;
        int[] r = remoteSize(width, height);
        if (Arrays.equals(r, asked)) return;
        asked = r;
        send(Proto.resize(r[0], r[1], r[2]));
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (getIntent().getStringExtra(HOST) == null) {
            finish();  // the notification's way back, after the session it named had ended
            return;
        }
        FrameLayout root = new FrameLayout(this);
        view = new SessionView(this);
        view.setInput(this::send);
        view.setPointerMode(pointerMode());
        view.getHolder().addCallback(this);
        root.setBackgroundColor(Color.BLACK);
        root.addView(view, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER));
        if (computer()) applyWindow(false);
        controls = new SessionControls(this, prefs());
        // The keyboard, on demand: a remote text field cannot summon the phone's.
        controls.addItem("Keyboard", () -> {
            controls.close();
            toggleKeyboard();
        });
        // What the phone copied, typed into the app's focused field.
        controls.addItem("Paste from phone", () -> {
            controls.close();
            pasteFromPhone();
        });
        inputItem = controls.addItem(inputLabel(), this::toggleInput);
        controls.addDivider();
        sizeItem = controls.addItem(sizeLabel(), this::nextSize);
        scaleValue = controls.addStepper("Smaller", "Larger", () -> stepScale(-1),
                                         () -> stepScale(1));
        scaleValue.setText(scaleLabel());
        if (computer()) {
            windowValue = controls.addStepper("Smaller window", "Larger window",
                                              () -> stepWindow(-1), () -> stepWindow(1));
            windowValue.setText(windowLabel());
            fitItem = controls.addItem(fitLabel(), this::nextFit);
            deskItem = controls.addItem(deskLabel(), this::toggleDesk);
            bypassItem = controls.addItem(bypassLabel(), this::toggleBypass);
        }
        controls.addDivider();
        TextView endItem = controls.addItem("End session", () -> end(null));
        endItem.setTextColor(0xFFFF8A80);
        root.addView(controls, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        // The keyboard takes the bottom of the window and the remote app reflows into what is
        // left (RESIZE), never covered. Drawn edge to edge for immersive mode, the window is not
        // shrunk by adjustResize any more, so the keyboard's inset is applied here.
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            int bottom = Build.VERSION.SDK_INT >= 30
                    ? insets.getInsets(WindowInsets.Type.ime()).bottom
                    : insets.getSystemWindowInsetBottom();
            v.setPadding(0, 0, 0, bottom);
            return insets;
        });
        setContentView(root);
        // Focused from the start: a hardware keyboard's keys go to the focused view, and would
        // otherwise do nothing until the first touch.
        view.requestFocus();
        // A session is a screen being watched: the phone must not sleep under it. The session
        // survives the screen going off (see the class comment), but a picture that goes dark
        // while someone is reading it is still wrong.
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        immersive();
        if (Build.VERSION.SDK_INT >= 33)
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::sendBack);
        startSessionService();
        SharedPreferences prefs = getSharedPreferences("session", MODE_PRIVATE);
        if (Build.VERSION.SDK_INT >= 33 && !prefs.getBoolean(PREF_ASKED_NOTIFICATIONS, false)
                && checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)
                        != PackageManager.PERMISSION_GRANTED) {
            prefs.edit().putBoolean(PREF_ASKED_NOTIFICATIONS, true).apply();
            requestPermissions(new String[] {android.Manifest.permission.POST_NOTIFICATIONS}, 0);
        }
    }

    // Started while on screen: Android refuses a foreground service started from the background,
    // and the session would then simply end when it leaves the screen.
    private void startSessionService() {
        try {
            startForegroundService(new Intent(this, SessionService.class)
                    .putExtra(SessionService.APP_NAME, getIntent().getStringExtra(APP_NAME)));
            service = true;
        } catch (IllegalStateException ignored) {
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        // The service posted its notification before the answer, unseen: posted again now.
        if (service && results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED)
            startSessionService();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        sendBack();  // before Android 13; later releases call the callback above instead
    }

    private void sendBack() {
        if (controls.isOpen()) {  // Back closes the panel first, as it would a menu
            controls.close();
            return;
        }
        send(Proto.back(0));
        send(Proto.back(1));
    }

    // --- the controls' settings, changed in place ---------------------------------------------

    private static int indexOf(int[] values, int v, int fallback) {
        for (int i = 0; i < values.length; i++)
            if (values[i] == v) return i;
        return fallback;
    }

    private String sizeLabel() {
        return "Size: " + SIZE_NAMES[indexOf(SIZES, maxLongSide(), 0)];
    }

    private String scaleLabel() {
        return "Scale " + scalePercent() + "%";
    }

    private String inputLabel() {
        return pointerMode() ? "Input: Pointer" : "Input: Touch";
    }

    private String windowLabel() {
        int p = windowPercent();
        return p >= 100 ? "Window: Full screen" : "Window: " + p + "%";
    }

    private void stepWindow(int by) {
        int cur = windowPercent(), i = 0;
        while (i < WINDOWS.length - 1 && WINDOWS[i] < cur) i++;
        int next = WINDOWS[Math.max(0, Math.min(WINDOWS.length - 1, i + by))];
        prefs().edit().putInt(windowKey(), next).apply();
        windowValue.setText(windowLabel());
        applyWindow(true);
    }

    private String fitLabel() {
        return "Fit: " + FITS[fit()];
    }

    /** Stretch → Width → Height → Stretch. */
    private void nextFit() {
        prefs().edit().putInt(fitKey(), (fit() + 1) % FITS.length).apply();
        fitItem.setText(fitLabel());
        applyWindow(true);
    }

    private String bypassLabel() {
        return bypassDesktop() ? "Bypass desktop: On" : "Bypass desktop: Off";
    }

    private void toggleBypass() {
        prefs().edit().putBoolean(bypassKey(), !bypassDesktop()).apply();
        bypassItem.setText(bypassLabel());
        applyWindow(true);
    }

    private String deskLabel() {
        return extendDesk() ? "Desk: Extended" : "Desk: Separate";
    }

    private void toggleDesk() {
        boolean next = !extendDesk();
        prefs().edit().putBoolean(deskKey(), next).apply();
        deskItem.setText(deskLabel());
        send(Proto.desk(next));
    }

    private void toggleInput() {
        boolean next = !pointerMode();
        prefs().edit().putBoolean(pointerKey(), next).apply();
        inputItem.setText(inputLabel());
        view.setPointerMode(next);
    }

    /** Full → Balanced → Light → Full. The main screen's setting, so the next session keeps it. */
    private void nextSize() {
        int next = SIZES[(indexOf(SIZES, maxLongSide(), 0) + 1) % SIZES.length];
        prefs().edit().putInt(PREF_MAX_LONG_SIDE, next).apply();
        sizeItem.setText(sizeLabel());
        resizeSoon();
    }

    private void stepScale(int by) {
        int cur = scalePercent(), i = 0;
        while (i < SCALES.length - 1 && SCALES[i] < cur) i++;
        int next = SCALES[Math.max(0, Math.min(SCALES.length - 1, i + by))];
        prefs().edit().putInt(scaleKey(), next).apply();
        scaleValue.setText(scaleLabel());
        resizeSoon();
    }

    // A few taps in a row become one change: each one lays the remote app out again.
    private void resizeSoon() {
        ui.removeCallbacks(resizeLater);
        ui.postDelayed(resizeLater, 300);
    }

    private void pasteFromPhone() {
        ClipData clip = getSystemService(ClipboardManager.class).getPrimaryClip();
        CharSequence t = clip != null && clip.getItemCount() > 0
                ? clip.getItemAt(0).coerceToText(this) : null;
        if (t == null || t.length() == 0) {
            Toast.makeText(this, "The phone's clipboard is empty.", Toast.LENGTH_SHORT).show();
            return;
        }
        lastClip = t.toString();
        send(Proto.clipboard(lastClip, true));
    }

    private void immersive() {
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            if (getWindow().getInsetsController() != null)
                getWindow().getInsetsController().hide(WindowInsets.Type.systemBars());
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    private void toggleKeyboard() {
        view.requestFocus();
        // Shown or not, as the window's insets say. hideSoftInputFromWindow's answer cannot tell:
        // measured on Android 17, it answers true for a keyboard that was already hidden, so a
        // toggle built on it only ever hid.
        if (Build.VERSION.SDK_INT >= 30 && getWindow().getInsetsController() != null) {
            WindowInsets now = view.getRootWindowInsets();
            if (now != null && now.isVisible(WindowInsets.Type.ime()))
                getWindow().getInsetsController().hide(WindowInsets.Type.ime());
            else
                getWindow().getInsetsController().show(WindowInsets.Type.ime());
            return;
        }
        InputMethodManager imm = getSystemService(InputMethodManager.class);
        if (!imm.hideSoftInputFromWindow(view.getWindowToken(), 0)) imm.showSoftInput(view, 0);
    }

    // --- the surface decides the remote display's size ----------------------------------------

    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
        if (!started) {
            started = true;
            width = w;
            height = h;
            start(holder.getSurface(), w, h);
            return;
        }
        if (away) {
            away = false;
            ui.removeCallbacks(awayTooLong);
            Surface surface = holder.getSurface();
            if (lost) {
                lost = false;
                width = w;
                height = h;
                start(surface, w, h);
                return;
            }
            new Thread(() -> {
                try {
                    openVideo(surface);
                } catch (IOException e) {
                    end("The connection to the computer was lost.");
                }
            }, "video-attach").start();
        }
        width = w;
        height = h;
        syncSize();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (ending) return;
        away = true;
        ui.postDelayed(awayTooLong, AWAY_LIMIT_MS);
        videoGen.incrementAndGet();  // its end is the surface's, not the session's
        SSLSocket v = video;
        video = null;
        Thread decoder = videoThread;
        videoThread = null;
        // The decoder must let go of the surface before this returns: the platform takes it then.
        // Closing its connection ends it; a socket is closed off the main thread.
        Thread closer = new Thread(() -> closeQuietly(v), "video-close");
        closer.start();
        try {
            closer.join(500);
            if (decoder != null) decoder.join(500);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
    }

    // --- the session --------------------------------------------------------------------------

    private void start(Surface surface, int w, int h) {
        host = MainActivity.host(this, getIntent().getStringExtra(HOST));
        source = getIntent().getStringExtra(SOURCE);
        String app = getIntent().getStringExtra(APP);
        sessionId = -1;
        if (host == null) {
            end("That computer is no longer paired.");
            return;
        }
        new Thread(() -> {
            try {
                SSLSocket c = Link.connect(this, host, source);
                control = c;
                InputStream in = c.getInputStream();
                OutputStream out = c.getOutputStream();
                Proto.hello(in, out);
                Map<String, String> opts = new LinkedHashMap<>();
                int[] r = remoteSize(w, h);
                asked = r;
                opts.put("new_display", r[0] + "x" + r[1] + "/" + r[2]);
                opts.put("start_app", app);
                opts.put("close_on_app_exit", "true");
                if (computer()) {
                    boolean bypass = bypassDesktop();
                    int[] share = bypass ? new int[] {100, 100} : windowShare();
                    opts.put("window", share[0] + "x" + share[1]);
                    opts.put("desktop", String.valueOf(!bypass));
                    opts.put("extend_desk", String.valueOf(extendDesk()));
                }
                opts.put("video_codec", Streams.hasHevcDecoder() ? "h265" : "h264");
                out.write(Proto.sessionRequest(Proto.KIND_NEW_DISPLAY, opts));
                out.flush();
                int id = Proto.readSessionReply(in);
                sessionId = id;
                ui.post(this::syncSize);  // the settings may have changed while it connected

                openVideo(surface);
                SSLSocket a = attach(host, source, Proto.ROLE_AUDIO, id);
                audio = a;
                new Thread(() -> Streams.audio(inputOf(a), why -> {}), "audio").start();
                readDeviceMessages(new DataInputStream(in));
            } catch (Exception e) {
                end(e.getMessage());
            }
        }, "session").start();
    }

    /** The session's video into `surface`, on a new connection; off the main thread. */
    private void openVideo(Surface surface) throws IOException {
        int gen = videoGen.incrementAndGet();
        SSLSocket v = attach(host, source, Proto.ROLE_VIDEO, sessionId);
        if (gen != videoGen.get() || away) {  // the surface went while this connected
            closeQuietly(v);
            return;
        }
        video = v;
        Thread t = new Thread(() -> Streams.video(inputOf(v), surface, why -> {
            if (gen == videoGen.get()) end(why);
        }), "video");
        videoThread = t;
        t.start();
    }

    private static void closeQuietly(Closeable c) {
        try {
            if (c != null) c.close();
        } catch (IOException ignored) {
        }
    }

    private SSLSocket attach(Link.Host host, String source, int role, int id) throws IOException {
        SSLSocket s = Link.connect(this, host, source);
        Proto.hello(s.getInputStream(), s.getOutputStream());
        s.getOutputStream().write(Proto.attach(role, id));
        s.getOutputStream().flush();
        return s;
    }

    private static InputStream inputOf(SSLSocket s) {
        try {
            return s.getInputStream();
        } catch (IOException e) {
            return new java.io.ByteArrayInputStream(new byte[0]);
        }
    }

    /** The agent's messages, until the session ends: it closing the control connection is how. */
    private void readDeviceMessages(DataInputStream in) {
        try {
            for (Proto.Record r; (r = Proto.readRecord(in, Proto.CONTROL_LIMIT)) != null; ) {
                if (r.type == Proto.DEVICE_NOTICE) {
                    String text = Proto.utf8(r.body, 0);
                    runOnUiThread(() -> Toast.makeText(this, text, Toast.LENGTH_LONG).show());
                }
                if (r.type == Proto.DEVICE_CLIPBOARD) {
                    String text = Proto.utf8(r.body, 0);
                    runOnUiThread(() -> {
                        lastClip = text;
                        getSystemService(ClipboardManager.class)
                                .setPrimaryClip(ClipData.newPlainText("Remora", text));
                    });
                }
            }
            end("The app was closed.");
        } catch (IOException e) {
            if (away && !ending) {
                // Lost while nobody was looking (the phone asleep, the network changed): opened
                // again when the surface is back, rather than an error to come back to.
                lost = true;
                for (Closeable s : new Closeable[] {control, video, audio}) closeQuietly(s);
                control = video = audio = null;
                return;
            }
            end(ending ? null : "The connection to the computer was lost.");
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus) return;
        immersive();
        // The phone's clipboard, offered to the remote app when the session comes back to the
        // front — the one moment an app may read it.
        ClipData clip = getSystemService(ClipboardManager.class).getPrimaryClip();
        if (clip == null || clip.getItemCount() == 0) return;
        CharSequence t = clip.getItemAt(0).coerceToText(this);
        if (t != null && t.length() > 0 && !t.toString().equals(lastClip)) {
            lastClip = t.toString();
            send(Proto.clipboard(lastClip, false));
        }
    }

    private void send(byte[] message) {
        if (writer.isShutdown()) return;
        writer.execute(() -> {
            SSLSocket c = control;
            // Nothing before the agent's answer: input written then would land in the middle of
            // the session request, and the agent would read it as part of one.
            if (c == null || sessionId < 0) return;
            try {
                c.getOutputStream().write(message);
                c.getOutputStream().flush();
            } catch (IOException ignored) {
                // The reader sees the same broken connection and ends the session.
            }
        });
    }

    private void end(String why) {
        runOnUiThread(() -> {
            if (ending) return;
            ending = true;
            if (why != null) Toast.makeText(this, why, Toast.LENGTH_LONG).show();
            finish();
        });
    }

    @Override
    protected void onDestroy() {
        ending = true;
        ui.removeCallbacks(awayTooLong);
        ui.removeCallbacks(resizeLater);
        if (service) stopService(new Intent(this, SessionService.class));
        writer.shutdownNow();
        // Closing the control connection is what ends the session on the computer.
        new Thread(() -> {
            for (Closeable s : new Closeable[] {control, video, audio}) {
                try {
                    if (s != null) s.close();
                } catch (IOException ignored) {
                }
            }
        }, "close").start();
        super.onDestroy();
    }
}
