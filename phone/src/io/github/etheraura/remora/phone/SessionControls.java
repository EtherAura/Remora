package io.github.etheraura.remora.phone;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Collections;

/**
 * A session's own controls, over the remote app: a small handle on the edge of the screen, which
 * can be dragged anywhere along either side and stays where it was left, and the panel it opens
 * beside it. The handle is all that shows while the app is in use. While the panel is open it takes
 * the screen's touches, so a tap beside it closes it rather than reaching the app.
 */
final class SessionControls extends ViewGroup {
    private static final String PREF_RIGHT = "controlsRight", PREF_Y = "controlsY";
    private static final int TEXT = 0xFFFFFFFF, RIPPLE = 0x40FFFFFF;

    private final SharedPreferences prefs;
    private final ImageView handle;
    private final LinearLayout panel;
    private final int slop;
    private boolean right;
    private float fraction;  // the handle's centre, as a fraction of the height
    private float downX, downY;
    private boolean dragging;

    SessionControls(Context c, SharedPreferences prefs) {
        super(c);
        this.prefs = prefs;
        right = prefs.getBoolean(PREF_RIGHT, true);
        fraction = prefs.getFloat(PREF_Y, 0.5f);
        slop = ViewConfiguration.get(c).getScaledTouchSlop();

        handle = new ImageView(c);
        handle.setImageResource(R.drawable.ic_controls);
        handle.setContentDescription("Session controls");
        handle.setScaleType(ImageView.ScaleType.CENTER);
        GradientDrawable dot = new GradientDrawable();
        dot.setShape(GradientDrawable.OVAL);
        dot.setColor(0x99000000);
        dot.setStroke(dp(1), 0x66FFFFFF);  // seen on a dark app as well as a light one
        handle.setBackground(new RippleDrawable(ColorStateList.valueOf(RIPPLE), dot, null));
        handle.setAlpha(0.6f);
        handle.setOnClickListener(v -> {
            if (isOpen()) close();
            else open();
        });
        handle.setOnTouchListener(this::onHandleTouch);
        addView(handle, new LayoutParams(dp(44), dp(44)));

        panel = new LinearLayout(c);
        panel.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable card = new GradientDrawable();
        card.setColor(0xEB202124);
        card.setCornerRadius(dp(14));
        panel.setBackground(card);
        panel.setPadding(0, dp(6), 0, dp(6));
        panel.setElevation(dp(8));
        panel.setMinimumWidth(dp(220));
        panel.setClickable(true);  // a tap between its rows is the panel's, not one beside it
        panel.setVisibility(GONE);
        addView(panel, new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT));
    }

    boolean isOpen() {
        return panel.getVisibility() == VISIBLE;
    }

    void open() {
        panel.setVisibility(VISIBLE);
        handle.setAlpha(1f);
    }

    void close() {
        panel.setVisibility(GONE);
        handle.setAlpha(0.6f);
    }

    /** A row of the panel: a label that does `action` when tapped. */
    TextView addItem(CharSequence label, Runnable action) {
        TextView t = label(label);
        t.setPadding(dp(18), 0, dp(18), 0);
        t.setOnClickListener(v -> action.run());
        panel.addView(t, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(48)));
        return t;
    }

    /** A row with − and + either side of a value, which stays open while it is stepped. */
    TextView addStepper(String less, String more, Runnable down, Runnable up) {
        LinearLayout row = new LinearLayout(getContext());
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(6), 0, dp(6), 0);
        TextView minus = label("−"), value = label(""), plus = label("+");
        for (TextView t : new TextView[] {minus, plus}) {
            t.setGravity(Gravity.CENTER);
            t.setTextSize(22);
        }
        minus.setContentDescription(less);
        plus.setContentDescription(more);
        minus.setOnClickListener(v -> down.run());
        plus.setOnClickListener(v -> up.run());
        value.setGravity(Gravity.CENTER);
        value.setBackground(null);
        row.addView(minus, new LinearLayout.LayoutParams(dp(48), dp(48)));
        row.addView(value, new LinearLayout.LayoutParams(0, dp(48), 1));
        row.addView(plus, new LinearLayout.LayoutParams(dp(48), dp(48)));
        panel.addView(row, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        return value;
    }

    void addDivider() {
        // As wide as the panel and never wider. A plain View takes all the width it is offered, so
        // the panel, sized to its widest row, came out as wide as the screen and over the handle.
        View line = new View(getContext()) {
            @Override
            protected void onMeasure(int widthSpec, int heightSpec) {
                setMeasuredDimension(resolveSize(0, widthSpec), resolveSize(0, heightSpec));
            }
        };
        line.setBackground(new ColorDrawable(0x33FFFFFF));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, Math.max(1, dp(1) / 2));
        lp.setMargins(dp(12), dp(4), dp(12), dp(4));
        panel.addView(line, lp);
    }

    private TextView label(CharSequence s) {
        TextView t = new TextView(getContext());
        t.setText(s);
        t.setTextColor(TEXT);
        t.setTextSize(15);
        t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        t.setGravity(Gravity.CENTER_VERTICAL);
        t.setSingleLine(true);
        t.setBackground(new RippleDrawable(ColorStateList.valueOf(RIPPLE), null,
                                           new ColorDrawable(0xFFFFFFFF)));
        return t;
    }

    // --- the handle: tapped, it opens the panel; dragged, it moves, and docks to a side -------

    private boolean onHandleTouch(View v, MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = e.getRawX();
                downY = e.getRawY();
                dragging = false;
                v.setPressed(true);
                return true;
            case MotionEvent.ACTION_MOVE:
                if (!dragging && Math.hypot(e.getRawX() - downX, e.getRawY() - downY) > slop) {
                    dragging = true;
                    v.setPressed(false);
                    close();
                }
                if (dragging) {
                    int[] at = new int[2];
                    getLocationOnScreen(at);
                    v.setX(e.getRawX() - at[0] - v.getWidth() / 2f);
                    v.setY(e.getRawY() - at[1] - v.getHeight() / 2f);
                }
                return true;
            case MotionEvent.ACTION_UP:
                v.setPressed(false);
                if (dragging) dock(v);
                else v.performClick();
                dragging = false;
                return true;
            case MotionEvent.ACTION_CANCEL:
                v.setPressed(false);
                if (dragging) dock(v);
                dragging = false;
                return true;
            default:
                return false;
        }
    }

    /** To the nearer side, at the height it was left; remembered for the next session. */
    private void dock(View v) {
        float cx = v.getX() + v.getWidth() / 2f, cy = v.getY() + v.getHeight() / 2f;
        right = cx > getWidth() / 2f;
        fraction = getHeight() > 0 ? Math.max(0f, Math.min(1f, cy / getHeight())) : 0.5f;
        prefs.edit().putBoolean(PREF_RIGHT, right).putFloat(PREF_Y, fraction).apply();
        v.setTranslationX(0);
        v.setTranslationY(0);
        requestLayout();
    }

    // --- layout: the handle where it was docked, the panel beside it ---------------------------

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        int w = MeasureSpec.getSize(widthSpec), h = MeasureSpec.getSize(heightSpec);
        setMeasuredDimension(w, h);
        int margin = dp(8);
        for (View child : new View[] {handle, panel}) {
            LayoutParams lp = child.getLayoutParams();
            child.measure(spec(lp.width, w - 2 * margin), spec(lp.height, h - 2 * margin));
        }
    }

    private static int spec(int size, int room) {
        return size >= 0 ? MeasureSpec.makeMeasureSpec(size, MeasureSpec.EXACTLY)
                         : MeasureSpec.makeMeasureSpec(Math.max(0, room), MeasureSpec.AT_MOST);
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        int w = r - l, h = b - t, edge = dp(4), margin = dp(8);
        int hw = handle.getMeasuredWidth(), hh = handle.getMeasuredHeight();
        int hx = right ? w - edge - hw : edge;
        int hy = clamp(Math.round(fraction * h) - hh / 2, margin, h - margin - hh);
        handle.layout(hx, hy, hx + hw, hy + hh);
        // Dragging the handle along the edge must not be read as the system's Back swipe.
        setSystemGestureExclusionRects(
                Collections.singletonList(new Rect(hx, hy, hx + hw, hy + hh)));
        if (panel.getVisibility() == GONE) return;
        int pw = panel.getMeasuredWidth(), ph = panel.getMeasuredHeight();
        int px = clamp(right ? hx - margin - pw : hx + hw + margin, margin, w - margin - pw);
        int py = clamp(hy + hh / 2 - ph / 2, margin, h - margin - ph);
        panel.layout(px, py, px + pw, py + ph);
    }

    private static int clamp(int v, int lo, int hi) {
        return hi < lo ? lo : Math.max(lo, Math.min(hi, v));
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        // Closed, the screen's touches go on to the app beneath; open, one beside the panel
        // closes it and goes no further.
        if (!isOpen()) return false;
        if (e.getActionMasked() == MotionEvent.ACTION_UP) close();
        return true;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
