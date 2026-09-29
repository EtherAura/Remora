/*
 * Remora system updater — the in-Android half of a host-applied update flow.
 *
 * The device cannot update itself (a container has no recovery/update_engine); the HOST applies
 * updates by recreating the container onto a newer Remora-built image. This screen just reads
 * the host-written state and, on Apply, leaves a signal file the host watcher acts on.
 * Runs as the system uid (sharedUserId) so it can read/write the shared /rezmods mount.
 *
 * Styled after the LineageOS Updater: a large header, a "current system" info card, and an
 * update card with the action button and an applying progress state. All views are programmatic
 * (no layout XML) with explicit colours — the DeviceDefault theme's defaults assume a light
 * background and are near-invisible in the device's dark mode.
 */
package org.remora.updater;

import android.app.Activity;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.time.LocalDateTime;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;

public class UpdaterActivity extends Activity {
    private static final String STATE = "/rezmods/updater/state.json";
    private static final String APPLY = "/rezmods/updater/apply";

    private static final int FG = 0xFFECECEC;        // primary text
    private static final int FG_DIM = 0xFFB0B0B0;    // secondary text
    private static final int FG_FAINT = 0xFF8A8A8A;  // labels / last-checked
    private static final int CARD_BG = 0xFF262B28;   // neutral card
    private static final int GREEN = 0xFF66BB6A;
    private static final int AMBER = 0xFFFFB74D;

    private float dp_;

    private TextView lastChecked_;
    private TextView currentBuilt_;
    private TextView currentTag_;

    private LinearLayout updateCard_;
    private TextView updateLabel_;
    private TextView updateBuilt_;
    private TextView updateSummary_;
    private Button apply_;
    private ProgressBar progress_;
    private TextView applying_;

    private LinearLayout upToDateRow_;
    private TextView upToDateDetail_;

    // Re-read state while visible: after Apply, the host may just reconnect (no container restart,
    // e.g. a no-op re-apply) so there is no onResume to refresh us — poll so "Applying" always
    // resolves back to Up-to-date / Update-available on its own.
    private final Handler poll_ = new Handler(Looper.getMainLooper());
    private final Runnable tick_ = new Runnable() {
        @Override public void run() {
            refresh();
            poll_.postDelayed(this, 3000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // NoActionBar theme (the .Settings action bar overlaid and clipped the title/status).
        if (getActionBar() != null) {
            getActionBar().hide();
        }
        dp_ = getResources().getDisplayMetrics().density;

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(24);
        // extra top inset clears the status bar (no action bar now to push content down)
        root.setPadding(pad, dp(40), pad, pad);

        TextView title = new TextView(this);
        title.setText("System updates");
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26);
        title.setTextColor(FG);
        root.addView(title);

        lastChecked_ = new TextView(this);
        lastChecked_.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        lastChecked_.setTextColor(FG_FAINT);
        lastChecked_.setPadding(0, dp(4), 0, 0);
        root.addView(lastChecked_);

        // -- current system card ------------------------------------------------
        LinearLayout current = card(root);
        current.addView(label("CURRENT SYSTEM", FG_FAINT));

        TextView android_ = new TextView(this);
        android_.setText("Android " + Build.VERSION.RELEASE);
        android_.setTypeface(Typeface.DEFAULT_BOLD);
        android_.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        android_.setTextColor(FG);
        android_.setPadding(0, dp(6), 0, 0);
        current.addView(android_);

        TextView display = new TextView(this);
        display.setText(Build.DISPLAY);
        display.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        display.setTextColor(FG_DIM);
        display.setPadding(0, dp(4), 0, 0);
        current.addView(display);

        currentBuilt_ = new TextView(this);
        currentBuilt_.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        currentBuilt_.setTextColor(FG_DIM);
        currentBuilt_.setPadding(0, dp(2), 0, 0);
        current.addView(currentBuilt_);

        currentTag_ = new TextView(this);
        currentTag_.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        currentTag_.setTextColor(FG_DIM);
        currentTag_.setPadding(0, dp(2), 0, 0);
        current.addView(currentTag_);

        // -- update card (hidden when nothing pending) --------------------------
        updateCard_ = card(root);
        updateLabel_ = label("UPDATE AVAILABLE", AMBER);
        updateCard_.addView(updateLabel_);

        TextView updTitle = new TextView(this);
        updTitle.setText("System image");
        updTitle.setTypeface(Typeface.DEFAULT_BOLD);
        updTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        updTitle.setTextColor(FG);
        updTitle.setPadding(0, dp(6), 0, 0);
        updateCard_.addView(updTitle);

        updateBuilt_ = new TextView(this);
        updateBuilt_.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        updateBuilt_.setTextColor(FG_DIM);
        updateBuilt_.setPadding(0, dp(4), 0, 0);
        updateCard_.addView(updateBuilt_);

        updateSummary_ = new TextView(this);
        updateSummary_.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        updateSummary_.setTextColor(FG);
        updateSummary_.setPadding(0, dp(8), 0, 0);
        updateCard_.addView(updateSummary_);

        apply_ = new Button(this);
        apply_.setText("Download and apply");
        LinearLayout.LayoutParams alp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        alp.topMargin = dp(12);
        alp.gravity = Gravity.END;
        apply_.setLayoutParams(alp);
        apply_.setOnClickListener(v -> apply());
        updateCard_.addView(apply_);

        progress_ = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress_.setIndeterminate(true);
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        plp.topMargin = dp(12);
        progress_.setLayoutParams(plp);
        updateCard_.addView(progress_);

        applying_ = new TextView(this);
        applying_.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        applying_.setTextColor(FG_DIM);
        applying_.setText("The Remora host is recreating the device onto the new image. "
                + "The screen will go away and come back on its own.");
        applying_.setPadding(0, dp(6), 0, 0);
        updateCard_.addView(applying_);

        // -- up-to-date row -----------------------------------------------------
        upToDateRow_ = new LinearLayout(this);
        upToDateRow_.setOrientation(LinearLayout.HORIZONTAL);
        upToDateRow_.setGravity(Gravity.CENTER_VERTICAL);
        upToDateRow_.setPadding(dp(4), dp(24), 0, 0);

        TextView check = new TextView(this);
        check.setText("✓");
        check.setTypeface(Typeface.DEFAULT_BOLD);
        check.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        check.setTextColor(GREEN);
        upToDateRow_.addView(check);

        LinearLayout utdText = new LinearLayout(this);
        utdText.setOrientation(LinearLayout.VERTICAL);
        utdText.setPadding(dp(12), 0, 0, 0);

        TextView utd = new TextView(this);
        utd.setText("Your system is up to date");
        utd.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        utd.setTextColor(FG);
        utdText.addView(utd);

        upToDateDetail_ = new TextView(this);
        upToDateDetail_.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        upToDateDetail_.setTextColor(FG_DIM);
        utdText.addView(upToDateDetail_);

        upToDateRow_.addView(utdText);
        root.addView(upToDateRow_);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
    }

    private int dp(int v) {
        return (int) (v * dp_);
    }

    private LinearLayout card(LinearLayout parent) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(CARD_BG);
        bg.setCornerRadius(dp(16));
        c.setBackground(bg);
        int pad = dp(20);
        c.setPadding(pad, pad, pad, pad);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(20);
        c.setLayoutParams(lp);
        parent.addView(c);
        return c;
    }

    private TextView label(String text, int color) {
        TextView l = new TextView(this);
        l.setText(text);
        l.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        l.setTextColor(color);
        l.setLetterSpacing(0.08f);
        l.setTypeface(Typeface.DEFAULT_BOLD);
        return l;
    }

    // Host writes docker-image UTC timestamps trimmed to seconds ("2026-07-18T17:25:33");
    // show them in the device's zone, Lineage-style. Fall back to the raw string.
    private String fmt(String iso) {
        try {
            return LocalDateTime.parse(iso)
                    .atOffset(ZoneOffset.UTC)
                    .atZoneSameInstant(ZoneId.systemDefault())
                    .format(DateTimeFormatter.ofPattern("MMMM d, yyyy h:mm a"));
        } catch (Exception e) {
            return iso;
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        poll_.removeCallbacks(tick_);
        poll_.post(tick_);  // refresh now, then every 3s while visible
    }

    @Override
    protected void onPause() {
        super.onPause();
        poll_.removeCallbacks(tick_);
    }

    private void refresh() {
        boolean pending = false;
        String built = "";
        String current = "";
        String tag = "";
        String summary = "";
        try {
            String raw = new String(Files.readAllBytes(Paths.get(STATE)));
            JSONObject o = new JSONObject(raw);
            pending = o.optBoolean("pending", false);
            built = o.optString("built", "");
            current = o.optString("current", "");
            tag = o.optString("tag", "");
            summary = o.optString("summary", "");
        } catch (Exception ignored) {
            // no state file yet: the host has never written one — treat as up to date
        }

        lastChecked_.setText("Last checked " + java.time.LocalDateTime.now()
                .format(DateTimeFormatter.ofPattern("MMMM d, yyyy h:mm a")));
        currentBuilt_.setVisibility(current.isEmpty() ? View.GONE : View.VISIBLE);
        currentBuilt_.setText("Image built " + fmt(current));
        currentTag_.setVisibility(tag.isEmpty() ? View.GONE : View.VISIBLE);
        currentTag_.setText(tag);

        final boolean applying = new File(APPLY).exists();
        if (applying || pending) {
            updateCard_.setVisibility(View.VISIBLE);
            upToDateRow_.setVisibility(View.GONE);
            updateLabel_.setText(applying ? "APPLYING UPDATE" : "UPDATE AVAILABLE");
            updateBuilt_.setVisibility(built.isEmpty() ? View.GONE : View.VISIBLE);
            updateBuilt_.setText("Built " + fmt(built));
            updateSummary_.setText(summary.isEmpty()
                    ? "A newer system image was built on the Remora host."
                    : summary);
            apply_.setVisibility(applying ? View.GONE : View.VISIBLE);
            progress_.setVisibility(applying ? View.VISIBLE : View.GONE);
            applying_.setVisibility(applying ? View.VISIBLE : View.GONE);
        } else {
            updateCard_.setVisibility(View.GONE);
            upToDateRow_.setVisibility(View.VISIBLE);
            upToDateDetail_.setText(built.isEmpty() ? "" : "Latest image built " + fmt(built));
        }
    }

    private void apply() {
        try {
            File dir = new File("/rezmods/updater");
            dir.mkdirs();
            try (FileOutputStream f = new FileOutputStream(APPLY)) {
                f.write("apply\n".getBytes());
            }
        } catch (Exception e) {
            updateSummary_.setText("Could not signal the host: " + e.getMessage());
            return;
        }
        refresh();
    }
}
