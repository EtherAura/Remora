package io.github.etheraura.remora.phone;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.LruCache;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.GridView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * One source's apps, as a launcher shows them: icon and name, tap to open. Icons come one request
 * each (app-icon) as they scroll into view; a source that does not serve icons gets lettered tiles.
 */
public final class AppsActivity extends Activity {
    static final String HOST = "host", SOURCE = "source", SOURCE_NAME = "sourceName";

    private final ExecutorService listing = Executors.newSingleThreadExecutor();
    private final ExecutorService icons = Executors.newFixedThreadPool(4);
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final LruCache<String, Bitmap> iconCache = new LruCache<>(256);
    private final Set<String> asked = Collections.synchronizedSet(new HashSet<>());
    private volatile boolean noIcons;
    private Link.Host host;
    private String source;
    private List<Proto.App> apps = new ArrayList<>();
    private BaseAdapter adapter;
    private TextView status;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        host = MainActivity.host(this, getIntent().getStringExtra(HOST));
        source = getIntent().getStringExtra(SOURCE);
        setTitle(getIntent().getStringExtra(SOURCE_NAME));
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setFitsSystemWindows(true);
        status = new TextView(this);
        status.setPadding(dp(16), dp(12), dp(16), dp(12));
        status.setText("Listing…");
        col.addView(status);
        GridView grid = new GridView(this);
        grid.setNumColumns(GridView.AUTO_FIT);
        grid.setColumnWidth(dp(88));
        grid.setVerticalSpacing(dp(12));
        grid.setStretchMode(GridView.STRETCH_COLUMN_WIDTH);
        adapter = new AppAdapter();
        grid.setAdapter(adapter);
        grid.setOnItemClickListener((parent, view, pos, id) -> open(apps.get(pos)));
        col.addView(grid, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(col);
        if (host == null) {
            status.setText("That computer is no longer paired.");
            return;
        }
        listing.execute(() -> {
            try {
                List<Proto.App> found = Proto.parseAppList(
                        Link.blob(this, host, source, Proto.KIND_LIST_APPS, Collections.emptyMap()));
                ui.post(() -> {
                    apps = found;
                    status.setVisibility(found.isEmpty() ? View.VISIBLE : View.GONE);
                    status.setText("No apps.");
                    adapter.notifyDataSetChanged();
                });
            } catch (Exception e) {
                ui.post(() -> status.setText("Could not list the apps: " + e.getMessage()));
            }
        });
    }

    @Override
    protected void onDestroy() {
        listing.shutdownNow();
        icons.shutdownNow();
        super.onDestroy();
    }

    private void open(Proto.App app) {
        Intent i = new Intent(this, SessionActivity.class);
        i.putExtra(SessionActivity.HOST, host.fingerprint);
        i.putExtra(SessionActivity.SOURCE, source);
        i.putExtra(SessionActivity.APP, app.id);
        i.putExtra(SessionActivity.APP_NAME, app.name);
        startActivity(i);
    }

    private void loadIcon(Proto.App app) {
        if (noIcons || !asked.add(app.id)) return;
        icons.execute(() -> {
            try {
                Map<String, String> opts = new LinkedHashMap<>();
                opts.put("app", app.id);
                opts.put("size", String.valueOf(dp(48)));
                byte[] png = Link.blob(this, host, source, Proto.KIND_APP_ICON, opts);
                Bitmap b = BitmapFactory.decodeByteArray(png, 0, png.length);
                if (b != null) {
                    iconCache.put(app.id, b);
                    ui.post(adapter::notifyDataSetChanged);
                }
            } catch (java.io.EOFException e) {
                // An agent that does not know the request (an image's agent from before the phone
                // app) closes the connection: stop asking, and draw letters.
                noIcons = true;
            } catch (Exception e) {
                // One app without an icon to draw keeps its letter; the others still ask.
                if (e.getMessage() != null && e.getMessage().contains("unknown session kind"))
                    noIcons = true;
            }
        });
    }

    private Bitmap letterTile(String name) {
        int size = dp(48);
        Bitmap b = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(b);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setColor(0xFF2B6CB0);
        c.drawRoundRect(0, 0, size, size, size / 5f, size / 5f, p);
        p.setColor(0xFFFFFFFF);
        p.setTextSize(size * 0.5f);
        p.setTextAlign(Paint.Align.CENTER);
        String letter = name.isEmpty() ? "?" : name.substring(0, 1).toUpperCase();
        c.drawText(letter, size / 2f, size / 2f - (p.descent() + p.ascent()) / 2, p);
        return b;
    }

    private final class AppAdapter extends BaseAdapter {
        @Override public int getCount() { return apps.size(); }
        @Override public Object getItem(int i) { return apps.get(i); }
        @Override public long getItemId(int i) { return i; }

        @Override
        public View getView(int pos, View convert, ViewGroup parent) {
            LinearLayout cell = (LinearLayout) convert;
            if (cell == null) {
                cell = new LinearLayout(AppsActivity.this);
                cell.setOrientation(LinearLayout.VERTICAL);
                cell.setGravity(Gravity.CENTER_HORIZONTAL);
                ImageView icon = new ImageView(AppsActivity.this);
                cell.addView(icon, new LinearLayout.LayoutParams(dp(48), dp(48)));
                TextView label = new TextView(AppsActivity.this);
                label.setGravity(Gravity.CENTER_HORIZONTAL);
                label.setMaxLines(2);
                label.setTextSize(12);
                cell.addView(label);
            }
            Proto.App app = apps.get(pos);
            Bitmap icon = iconCache.get(app.id);
            if (icon == null) {
                loadIcon(app);
                icon = letterTile(app.name);
            }
            ((ImageView) cell.getChildAt(0)).setImageBitmap(icon);
            ((TextView) cell.getChildAt(1)).setText(app.name);
            return cell;
        }
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
