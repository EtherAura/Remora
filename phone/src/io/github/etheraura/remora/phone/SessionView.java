package io.github.etheraura.remora.phone;

import android.content.Context;
import android.text.InputType;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.ViewConfiguration;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

/**
 * The remote app's surface, and the phone's input into it: every finger as a pointer of its own
 * (protocol v3 POINTER, positions as fractions of the view), or in pointer mode the screen as a
 * mouse; and the keyboard through an input connection — committed text as TEXT, keys as KEY.
 */
final class SessionView extends SurfaceView {
    interface Input {
        void send(byte[] message);
    }

    private Input input;
    private boolean pointerMode;
    private int sharePercent = 100;
    private final int slop;
    private final float pxPerNotch;

    SessionView(Context c) {
        super(c);
        setFocusable(true);
        setFocusableInTouchMode(true);
        slop = ViewConfiguration.get(c).getScaledTouchSlop();
        pxPerNotch = 40 * getResources().getDisplayMetrics().density;
    }

    void setPointerMode(boolean on) {
        endGesture();
        pointerMode = on;
    }

    void setInput(Input input) {
        this.input = input;
    }

    /**
     * The share of each side of the space it is given that the view takes, centred by its parent.
     * The remote display follows the view's size, so a smaller view is a smaller screen for the
     * app on the computer, at the same density: its window, filling that screen, comes out smaller.
     */
    void setShare(int percent) {
        if (percent == sharePercent) return;
        sharePercent = percent;
        requestLayout();
    }

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        super.onMeasure(widthSpec, heightSpec);
        if (sharePercent >= 100) return;
        // Even sizes: video encoders work in 2×2 blocks.
        setMeasuredDimension(getMeasuredWidth() * sharePercent / 100 & ~1,
                             getMeasuredHeight() * sharePercent / 100 & ~1);
    }

    private void send(byte[] m) {
        if (input != null) input.send(m);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (pointerMode) return onPointerTouch(e);
        final int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = e.getActionIndex();
                send(pointerAt(e, action, i));
                if (action == MotionEvent.ACTION_DOWN) requestFocus();
                return true;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++)
                    send(pointerAt(e, MotionEvent.ACTION_MOVE, i));
                return true;
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < e.getPointerCount(); i++)
                    send(pointerAt(e, MotionEvent.ACTION_CANCEL, i));
                return true;
            default:
                return super.onTouchEvent(e);
        }
    }

    private byte[] pointerAt(MotionEvent e, int action, int i) {
        return Proto.pointer(action, Proto.TOOL_FINGER, e.getPointerId(i), fx(e.getX(i)),
                             fy(e.getY(i)), Math.min(1f, e.getPressure(i)), 0, 0);
    }

    private float fx(float x) {
        return getWidth() > 0 ? x / getWidth() : 0;
    }

    private float fy(float y) {
        return getHeight() > 0 ? y / getHeight() : 0;
    }

    // --- pointer mode: the screen as a mouse, for desktop apps ---------------------------------
    //
    // The pointer goes where the finger is. A tap clicks there, a long press clicks the secondary
    // button, one finger dragged holds the primary button (a selection, a slider, a scrollbar),
    // and two fingers scroll, the content following them as it does on the phone.

    private static final int PRIMARY = MotionEvent.BUTTON_PRIMARY,
            SECONDARY = MotionEvent.BUTTON_SECONDARY;
    private boolean gesture, dragging, scrolling, longPressed;
    private float downX, downY, lastX, lastY, scrollX, scrollY;
    private final Runnable longPress = this::secondaryClick;

    private boolean onPointerTouch(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                requestFocus();
                gesture = true;
                dragging = scrolling = longPressed = false;
                downX = lastX = e.getX();
                downY = lastY = e.getY();
                // The pointer arrives before it clicks, as a mouse does: hover effects, tooltips.
                mouse(MotionEvent.ACTION_HOVER_MOVE, downX, downY, 0, 0);
                postDelayed(longPress, ViewConfiguration.getLongPressTimeout());
                return true;
            case MotionEvent.ACTION_POINTER_DOWN:
                removeCallbacks(longPress);
                if (dragging) {
                    mouse(MotionEvent.ACTION_UP, lastX, lastY, PRIMARY, 0);
                    dragging = false;
                }
                if (gesture && !longPressed && e.getPointerCount() == 2) {
                    scrolling = true;
                    scrollX = centroid(e, true);
                    scrollY = centroid(e, false);
                }
                return true;
            case MotionEvent.ACTION_MOVE:
                if (!gesture) return true;
                if (scrolling) {
                    if (e.getPointerCount() < 2) return true;
                    float cx = centroid(e, true), cy = centroid(e, false);
                    // Fingers up scroll down (Android's AXIS_VSCROLL is negative downwards), and
                    // fingers to the left scroll right: the content goes the way they do.
                    float h = (scrollX - cx) / pxPerNotch, v = (cy - scrollY) / pxPerNotch;
                    if (h != 0 || v != 0) send(Proto.scroll(fx(cx), fy(cy), h, v));
                    scrollX = cx;
                    scrollY = cy;
                    return true;
                }
                if (longPressed || e.getPointerCount() > 1) return true;
                lastX = e.getX();
                lastY = e.getY();
                if (!dragging && Math.hypot(lastX - downX, lastY - downY) > slop) {
                    removeCallbacks(longPress);
                    dragging = true;
                    mouse(MotionEvent.ACTION_DOWN, downX, downY, PRIMARY, PRIMARY);
                }
                if (dragging) mouse(MotionEvent.ACTION_MOVE, lastX, lastY, 0, PRIMARY);
                return true;
            case MotionEvent.ACTION_UP:
                removeCallbacks(longPress);
                if (dragging) mouse(MotionEvent.ACTION_UP, e.getX(), e.getY(), PRIMARY, 0);
                else if (gesture && !longPressed && !scrolling) click(PRIMARY, downX, downY);
                gesture = dragging = scrolling = longPressed = false;
                return true;
            case MotionEvent.ACTION_CANCEL:
                endGesture();
                return true;
            default:
                return true;  // a finger lifted while another stays: the scroll goes on
        }
    }

    private void secondaryClick() {
        if (!gesture || dragging || scrolling) return;
        longPressed = true;
        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
        click(SECONDARY, downX, downY);
    }

    private void click(int button, float x, float y) {
        mouse(MotionEvent.ACTION_DOWN, x, y, button, button);
        mouse(MotionEvent.ACTION_UP, x, y, button, 0);
    }

    /** Lets go of whatever the gesture holds: the app must not keep a button down. */
    private void endGesture() {
        removeCallbacks(longPress);
        if (dragging) mouse(MotionEvent.ACTION_UP, lastX, lastY, PRIMARY, 0);
        gesture = dragging = scrolling = longPressed = false;
    }

    private void mouse(int action, float x, float y, int actionButton, int buttons) {
        send(Proto.pointer(action, Proto.TOOL_MOUSE, 0, fx(x), fy(y), buttons != 0 ? 1f : 0f,
                           actionButton, buttons));
    }

    private static float centroid(MotionEvent e, boolean x) {
        float sum = 0;
        for (int i = 0; i < e.getPointerCount(); i++) sum += x ? e.getX(i) : e.getY(i);
        return sum / e.getPointerCount();
    }

    // --- keys: a hardware keyboard, and the soft keyboard's non-text keys ---------------------

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent e) {
        if (keyCode == KeyEvent.KEYCODE_BACK) return super.onKeyDown(keyCode, e);
        send(Proto.key(0, keyCode, e.getMetaState()));
        return true;
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent e) {
        if (keyCode == KeyEvent.KEYCODE_BACK) return super.onKeyUp(keyCode, e);
        send(Proto.key(1, keyCode, e.getMetaState()));
        return true;
    }

    // --- the soft keyboard --------------------------------------------------------------------

    @Override
    public boolean onCheckIsTextEditor() {
        return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo out) {
        // No suggestions or composing: what is typed goes straight to the remote app, which has
        // its own text field and its own idea of what is in it.
        out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
        out.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_ACTION_NONE;
        return new BaseInputConnection(this, false) {
            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                if (text.length() > 0) send(Proto.text(text.toString()));
                return true;
            }

            @Override
            public boolean deleteSurroundingText(int before, int after) {
                for (int i = 0; i < before; i++) {
                    send(Proto.key(0, KeyEvent.KEYCODE_DEL, 0));
                    send(Proto.key(1, KeyEvent.KEYCODE_DEL, 0));
                }
                return true;
            }

            @Override
            public boolean sendKeyEvent(KeyEvent e) {
                if (e.getAction() == KeyEvent.ACTION_DOWN || e.getAction() == KeyEvent.ACTION_UP)
                    send(Proto.key(e.getAction() == KeyEvent.ACTION_DOWN ? 0 : 1, e.getKeyCode(),
                                   e.getMetaState()));
                return true;
            }
        };
    }
}
