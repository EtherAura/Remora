package io.github.etheraura.remora.phone;

import android.content.Context;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

/**
 * The remote app's surface, and the phone's input into it: every finger as a pointer of its own
 * (protocol v3 POINTER, positions as fractions of the view), and the keyboard through an input
 * connection — committed text as TEXT, keys as KEY.
 */
final class SessionView extends SurfaceView {
    interface Input {
        void send(byte[] message);
    }

    private Input input;

    SessionView(Context c) {
        super(c);
        setFocusable(true);
        setFocusableInTouchMode(true);
    }

    void setInput(Input input) {
        this.input = input;
    }

    private void send(byte[] m) {
        if (input != null) input.send(m);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
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
        float x = getWidth() > 0 ? e.getX(i) / getWidth() : 0;
        float y = getHeight() > 0 ? e.getY(i) / getHeight() : 0;
        return Proto.pointer(action, e.getPointerId(i), x, y, Math.min(1f, e.getPressure(i)), 0);
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
