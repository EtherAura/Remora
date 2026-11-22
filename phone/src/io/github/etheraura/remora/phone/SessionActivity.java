package io.github.etheraura.remora.phone;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.Toast;
import android.window.OnBackInvokedDispatcher;

import java.io.Closeable;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import javax.net.ssl.SSLSocket;

/**
 * One app, opened on the phone: a new display of this view's shape and density on the source
 * (protocol v3 new-display with start_app), at its size or a lighter one (the session size
 * setting), its video and audio decoded here, and the phone's touch, keys, Back and clipboard
 * sent back. When the view changes size — rotation, unfolding, the keyboard — the remote display
 * is asked to reflow (RESIZE), never stretched.
 */
public final class SessionActivity extends Activity implements SurfaceHolder.Callback {
    static final String HOST = "host", SOURCE = "source", APP = "app", APP_NAME = "appName";

    private final ExecutorService writer = Executors.newSingleThreadExecutor();
    private SessionView view;
    private volatile SSLSocket control, video, audio;
    private volatile boolean ending;
    private boolean started;
    private int width, height;  // the view's, in pixels
    private String lastClip;

    // The session size setting (MainActivity): the remote display's long side, or 0 for the
    // view's own size. A smaller display at proportionally lower density lays the app out the
    // same, and costs the computer, the link and this phone's decoder a fraction of the pixels;
    // the surface scales it up to fill the view. Positions are fractions, so input is unchanged.
    static final String PREF_MAX_LONG_SIDE = "maxLongSide";

    private int maxLongSide() {
        return getSharedPreferences("session", MODE_PRIVATE).getInt(PREF_MAX_LONG_SIDE, 0);
    }

    /** {width, height, dpi} of the remote display for a view of w×h pixels. */
    private int[] remoteSize(int w, int h) {
        int dpi = getResources().getDisplayMetrics().densityDpi;
        int cap = maxLongSide();
        int longSide = Math.max(w, h);
        if (cap <= 0 || longSide <= cap) return new int[] {w, h, dpi};
        double f = (double) cap / longSide;
        // Even sizes: video encoders work in 2×2 blocks.
        return new int[] {(int) Math.round(w * f) & ~1, (int) Math.round(h * f) & ~1,
                          Math.max(1, (int) Math.round(dpi * f))};
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        FrameLayout root = new FrameLayout(this);
        view = new SessionView(this);
        view.setInput(this::send);
        view.getHolder().addCallback(this);
        root.addView(view);
        // The keyboard, on demand: a remote text field cannot summon the phone's.
        Button keyboard = new Button(this);
        keyboard.setText("⌨");
        keyboard.setAlpha(0.55f);
        keyboard.setOnClickListener(v -> toggleKeyboard());
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM | Gravity.END);
        root.addView(keyboard, lp);
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
        // A session is a screen being watched: the phone must not sleep under it, which would
        // take the surface — and with it the session — away.
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        immersive();
        if (Build.VERSION.SDK_INT >= 33)
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::sendBack);
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        sendBack();  // before Android 13; later releases call the callback above instead
    }

    private void sendBack() {
        send(Proto.back(0));
        send(Proto.back(1));
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
        } else if (w != width || h != height) {
            width = w;
            height = h;
            int[] r = remoteSize(w, h);
            send(Proto.resize(r[0], r[1]));
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // The decoder renders into this surface; without it the session has nowhere to go.
        end(null);
    }

    // --- the session --------------------------------------------------------------------------

    private void start(Surface surface, int w, int h) {
        Link.Host host = MainActivity.host(this, getIntent().getStringExtra(HOST));
        String source = getIntent().getStringExtra(SOURCE);
        String app = getIntent().getStringExtra(APP);
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
                opts.put("new_display", r[0] + "x" + r[1] + "/" + r[2]);
                opts.put("start_app", app);
                opts.put("close_on_app_exit", "true");
                opts.put("video_codec", Streams.hasHevcDecoder() ? "h265" : "h264");
                out.write(Proto.sessionRequest(Proto.KIND_NEW_DISPLAY, opts));
                out.flush();
                int id = Proto.readSessionReply(in);

                SSLSocket v = attach(host, source, Proto.ROLE_VIDEO, id);
                video = v;
                new Thread(() -> Streams.video(inputOf(v), surface, this::end), "video").start();
                SSLSocket a = attach(host, source, Proto.ROLE_AUDIO, id);
                audio = a;
                new Thread(() -> Streams.audio(inputOf(a), why -> {}), "audio").start();
                readDeviceMessages(new DataInputStream(in));
            } catch (Exception e) {
                end(e.getMessage());
            }
        }, "session").start();
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
            if (c == null) return;
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
