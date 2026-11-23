package io.github.etheraura.remora.phone;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * The paired computers and what each offers: this computer's own apps, and every running Remora
 * profile's Android. With nothing paired yet, how to pair.
 */
public final class MainActivity extends Activity {
    private final ExecutorService net = Executors.newSingleThreadExecutor();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private LinearLayout list;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        ScrollView scroll = new ScrollView(this);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        list.setPadding(pad, pad, pad, pad);
        scroll.addView(list);
        scroll.setFitsSystemWindows(true);
        setContentView(scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        render();
    }

    @Override
    protected void onDestroy() {
        net.shutdownNow();
        super.onDestroy();
    }

    private void render() {
        list.removeAllViews();
        List<Link.Host> hosts = Link.hosts(this);
        if (hosts.isEmpty()) {
            list.addView(text("Pair with a computer", 22));
            list.addView(text("On the computer, run `remora link pair` or open the workspace's Phone "
                    + "page, then scan the code it shows.", 15));
            list.addView(scanButton("Scan the code"));
            return;
        }
        for (Link.Host host : hosts) {
            list.addView(text(host.name, 22));
            TextView status = text("Looking…", 14);
            list.addView(status);
            LinearLayout sources = new LinearLayout(this);
            sources.setOrientation(LinearLayout.VERTICAL);
            list.addView(sources);
            net.execute(() -> {
                try {
                    List<Link.Source> found = Link.sources(this, host);
                    ui.post(() -> showSources(host, found, status, sources));
                } catch (Exception e) {
                    ui.post(() -> status.setText("Not reachable: " + e.getMessage()));
                }
            });
        }
        Button another = scanButton("Pair another computer");
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(24);
        list.addView(another, lp);
        list.addView(sessionSizeButton());
    }

    /** Opens the scanner, which also takes a pasted link. */
    private Button scanButton(String label) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setOnClickListener(v -> startActivity(new Intent(this, ScanActivity.class)));
        return b;
    }

    private void showSources(Link.Host host, List<Link.Source> found, TextView status,
                             LinearLayout into) {
        status.setText(found.isEmpty() ? "Nothing to open: start remora-host-agent, or a profile."
                                       : "");
        status.setVisibility(found.isEmpty() ? View.VISIBLE : View.GONE);
        for (Link.Source s : found) {
            Button b = new Button(this);
            b.setText(("computer".equals(s.kind) ? "This computer's apps — " : "Android — ") + s.name);
            b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            b.setAllCaps(false);
            b.setOnClickListener(v -> {
                Intent i = new Intent(this, AppsActivity.class);
                i.putExtra(AppsActivity.HOST, host.fingerprint);
                i.putExtra(AppsActivity.SOURCE, s.id);
                i.putExtra(AppsActivity.SOURCE_NAME, s.name);
                startActivity(i);
            });
            into.addView(b);
        }
    }

    // Full, Balanced, Light: the remote display's long side (SessionActivity.remoteSize), also
    // changed from inside a session.
    private static final int[] SIZES = SessionActivity.SIZES;
    private static final String[] SIZE_NAMES = {"Full (this screen's size)", "Balanced (1920 px)",
                                                "Light (1280 px)"};

    private Button sessionSizeButton() {
        android.content.SharedPreferences p = getSharedPreferences("session", MODE_PRIVATE);
        Button b = new Button(this);
        b.setAllCaps(false);
        Runnable label = () -> {
            int cur = p.getInt(SessionActivity.PREF_MAX_LONG_SIDE, 0);
            int i = 0;
            while (i < SIZES.length && SIZES[i] != cur) i++;
            b.setText("Session size: " + SIZE_NAMES[i < SIZES.length ? i : 0]);
        };
        label.run();
        b.setOnClickListener(v -> {
            int cur = p.getInt(SessionActivity.PREF_MAX_LONG_SIDE, 0);
            int i = 0;
            while (i < SIZES.length && SIZES[i] != cur) i++;
            p.edit().putInt(SessionActivity.PREF_MAX_LONG_SIDE, SIZES[(i + 1) % SIZES.length]).apply();
            label.run();
        });
        return b;
    }

    private TextView text(String s, int sp) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setPadding(0, dp(6), 0, dp(6));
        return t;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    /** The paired host with this fingerprint, for the activities that take one by extra. */
    static Link.Host host(android.content.Context c, String fingerprint) {
        for (Link.Host h : Link.hosts(c))
            if (h.fingerprint.equals(fingerprint)) return h;
        return null;
    }
}
