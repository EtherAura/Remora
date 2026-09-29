package com.remora.agent;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.hardware.display.DisplayManagerGlobal;
import android.hardware.input.InputManager;
import android.hardware.input.InputManagerGlobal;
import android.os.Handler;
import android.os.Looper;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.view.DisplayInfo;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

import com.android.internal.statusbar.IStatusBarService;

import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The control half of a kind=mirror session (bd remora-28ix.3.1): decodes v1 control messages
 * from the connection, injects input through the real framework — InputManagerGlobal, no
 * reflection — and keeps the clipboard synchronized both ways. Runs on its connection's thread
 * until the peer disconnects; the clipboard listener is the only cross-thread visitor and only
 * touches the synchronized device-message writer.
 *
 * The injection semantics are the fork Controller's, ported where this phase implements them:
 * touch (multi-pointer state, the mouse ACTION_BUTTON_PRESS/RELEASE sequence Chrome needs),
 * scroll, keycode, text (dead-key decomposition), back-or-screen-on, panels, clipboard
 * get/set/listen, and the flex reflow (RESIZE_DISPLAY, bd remora-28ix.3.3). The rest of types
 * 0–21 decode — the stream has no resync point, so everything must parse — but only log until a
 * session kind that wants them exists.
 */
public final class Controller {
    private static final int MAX_CLIPBOARD_BYTES = (1 << 18) - 5;  // writer cap: type + u32 len

    // Device→client message types (v1, mirrored by the client's DeviceMessageParser).
    private static final int DEVICE_MSG_CLIPBOARD = 0;
    private static final int DEVICE_MSG_ACK_CLIPBOARD = 1;

    // How long the first event waits for a new display to exist before giving up on it.
    private static final int DISPLAY_WAIT_MS = 5000;

    private final int sessionId;
    private final Session session;
    private final ControlReader reader;
    private final DataOutputStream out;  // device messages; all writes synchronized on it
    private int displayId = -1;          // resolved on first use — see inputDisplay()
    private boolean warnedNoDisplay;

    private final KeyCharacterMap charMap = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);
    private final PointersState pointersState = new PointersState();
    private final MotionEvent.PointerProperties[] pointerProperties =
            new MotionEvent.PointerProperties[PointersState.MAX_POINTERS];
    private final MotionEvent.PointerCoords[] pointerCoords =
            new MotionEvent.PointerCoords[PointersState.MAX_POINTERS];
    private long lastTouchDown;

    private final ClipboardManager clipboard;  // null when the service is unavailable
    private final AtomicBoolean settingClipboard = new AtomicBoolean();
    // Last text synchronized in either direction. The change listener drops texts equal to it:
    // that suppresses both the echo of our own set (the settingClipboard flag alone races the
    // async dispatch) and the duplicate dispatches ClipboardService emits per change on this
    // image (observed: every listener notified twice per setPrimaryClip). Volatile is enough —
    // control thread and handler thread, single writer per direction, worst case one dupe.
    private volatile String lastClipboardSync;
    private final ClipboardManager.OnPrimaryClipChangedListener clipListener =
            this::onDeviceClipboardChanged;

    private final boolean[] warnedUnimplemented = new boolean[22];

    public Controller(Context context, Session session, InputStream in, OutputStream rawOut) {
        this.sessionId = session.id;
        this.session = session;
        this.reader = new ControlReader(in);
        this.out = new DataOutputStream(rawOut);
        for (int i = 0; i < PointersState.MAX_POINTERS; ++i) {
            MotionEvent.PointerProperties props = new MotionEvent.PointerProperties();
            props.toolType = MotionEvent.TOOL_TYPE_FINGER;
            MotionEvent.PointerCoords coords = new MotionEvent.PointerCoords();
            coords.orientation = 0;
            coords.size = 0;
            pointerProperties[i] = props;
            pointerCoords[i] = coords;
        }
        // Constructed directly over the shell-attributed context instead of getSystemService():
        // the registry would hand back a manager bound to the system context, whose package
        // ("android") uid 2000 does not own. The handler serves the listener callbacks; Main
        // keeps that looper alive for the life of the process.
        ClipboardManager cm = null;
        try {
            cm = new ClipboardManager(context, new Handler(Looper.getMainLooper()));
        } catch (Exception e) {
            Ln.e("no clipboard service — clipboard sync disabled for session " + sessionId, e);
        }
        clipboard = cm;
    }

    /**
     * The display this session's events target, or -1 if it never arrived. It comes from the
     * session's CAPTURE, not from a display_id option: a mirror knows its source display up
     * front, but a new display only exists once the video connection has started the capture —
     * which happens after this control connection is already serving. Reading an option here is
     * what made a desktop window's input land on display 0, where the mouse shortcuts (Back,
     * Home, Recents, notifications) acted on the mirror behind it instead.
     *
     * Resolved once and kept: a flex resize reflows the same display rather than recreating it,
     * so the id is stable for the life of the session.
     */
    private int inputDisplay() {
        if (displayId < 0) displayId = session.awaitInputDisplay(DISPLAY_WAIT_MS);
        return displayId;
    }

    /** Blocks until the connection dies. Never throws: teardown belongs to the caller's finally. */
    public void run() {
        if (clipboard != null) clipboard.addPrimaryClipChangedListener(clipListener);
        try {
            for (;;) {
                handle(reader.next());
            }
        } catch (IOException e) {
            // EOF or protocol violation — either way the session is over; the message says which.
            Ln.i("session " + sessionId + " control connection closed: " + e);
        } finally {
            if (clipboard != null) clipboard.removePrimaryClipChangedListener(clipListener);
        }
    }

    private void handle(ControlMessage m) {
        switch (m.type) {
            case ControlMessage.TYPE_INJECT_KEYCODE:
                injectKeyEvent(m.action, m.keycode, m.repeat, m.metaState,
                               InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);
                break;
            case ControlMessage.TYPE_INJECT_TEXT:
                injectText(m.text);
                break;
            case ControlMessage.TYPE_INJECT_TOUCH_EVENT:
                injectTouch(m);
                break;
            case ControlMessage.TYPE_INJECT_SCROLL_EVENT:
                injectScroll(m);
                break;
            case ControlMessage.TYPE_BACK_OR_SCREEN_ON:
                pressBackOrTurnScreenOn(m.action);
                break;
            case ControlMessage.TYPE_EXPAND_NOTIFICATION_PANEL:
                statusBar("expandNotificationsPanel");
                break;
            case ControlMessage.TYPE_EXPAND_SETTINGS_PANEL:
                statusBar("expandSettingsPanel");
                break;
            case ControlMessage.TYPE_COLLAPSE_PANELS:
                statusBar("collapsePanels");
                break;
            case ControlMessage.TYPE_GET_CLIPBOARD:
                getClipboard(m.copyKey);
                break;
            case ControlMessage.TYPE_SET_CLIPBOARD:
                setClipboard(m.text, m.paste, m.sequence);
                break;
            case ControlMessage.TYPE_RESIZE_DISPLAY:
                // The client's window was resized; reflow a new display to match (a mirror
                // ignores this — its size follows its source).
                session.requestResize(m.width, m.height);
                break;
            default:
                // Decoded so the stream stays in sync; acting on it belongs to a later phase.
                if (!warnedUnimplemented[m.type]) {
                    warnedUnimplemented[m.type] = true;
                    Ln.w("control message type " + m.type + " decoded but not implemented yet");
                }
                break;
        }
    }

    // Input injection ----------------------------------------------------------------------------

    private boolean injectEvent(InputEvent event, int mode) {
        final int display = inputDisplay();
        if (display < 0) {
            // Dropping beats injecting into display 0: a session whose display never came up has
            // nowhere for its input to land, and the default display is somebody else's screen.
            if (!warnedNoDisplay) {
                warnedNoDisplay = true;
                Ln.w("session " + sessionId + " has no display — dropping input");
            }
            return false;
        }
        if (display != 0) event.setDisplayId(display);
        return InputManagerGlobal.getInstance().injectInputEvent(event, mode);
    }

    private boolean injectKeyEvent(int action, int keycode, int repeat, int metaState, int mode) {
        long now = SystemClock.uptimeMillis();
        KeyEvent event = new KeyEvent(now, now, action, keycode, repeat, metaState,
                                      KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0,
                                      InputDevice.SOURCE_KEYBOARD);
        return injectEvent(event, mode);
    }

    private boolean pressReleaseKeycode(int keycode, int mode) {
        return injectKeyEvent(KeyEvent.ACTION_DOWN, keycode, 0, 0, mode)
                && injectKeyEvent(KeyEvent.ACTION_UP, keycode, 0, 0, mode);
    }

    private void injectText(String text) {
        for (char c : text.toCharArray()) {
            String decomposed = KeyComposition.decompose(c);
            char[] chars = decomposed != null ? decomposed.toCharArray() : new char[]{c};
            KeyEvent[] events = charMap.getEvents(chars);
            if (events == null) {
                Ln.w("could not inject char u+" + String.format("%04x", (int) c));
                continue;
            }
            for (KeyEvent event : events) {
                if (!injectEvent(event, InputManager.INJECT_INPUT_EVENT_MODE_ASYNC)) return;
            }
        }
    }

    /**
     * Client coordinates arrive in the video space the client is looking at; the display is the
     * space events land in. Pure scaling between the two — where v1 dropped events whose declared
     * size mismatched the current video size, scaling stays correct against max_size caps and
     * needs no capture-side handshake. The DisplayInfo lookup is cheap (client-side cached in
     * DisplayManagerGlobal), so it tracks a mid-session resize without extra wiring.
     */
    private float[] mapToDisplay(ControlMessage m) {
        if (m.screenWidth <= 0 || m.screenHeight <= 0) return null;
        final int display = inputDisplay();
        if (display < 0) return null;
        DisplayInfo info = DisplayManagerGlobal.getInstance().getDisplayInfo(display);
        if (info == null) {
            Ln.w("no display info for display " + display + " — dropping positional event");
            return null;
        }
        return new float[]{m.x * (float) info.logicalWidth / m.screenWidth,
                           m.y * (float) info.logicalHeight / m.screenHeight};
    }

    private void injectTouch(ControlMessage m) {
        long now = SystemClock.uptimeMillis();
        float[] point = mapToDisplay(m);
        if (point == null) return;

        int action = m.action;
        int buttons = m.buttons;
        int pointerIndex = pointersState.getPointerIndex(m.pointerId);
        if (pointerIndex == -1) {
            Ln.w("too many pointers for touch event");
            return;
        }
        PointersState.Pointer pointer = pointersState.get(pointerIndex);
        pointer.x = point[0];
        pointer.y = point[1];
        pointer.pressure = m.pressure;

        int source;
        boolean activeSecondaryButtons =
                ((m.actionButton | buttons) & ~MotionEvent.BUTTON_PRIMARY) != 0;
        if (m.pointerId == -1
                && (action == MotionEvent.ACTION_HOVER_MOVE || activeSecondaryButtons)) {
            // A real mouse event, or one a finger cannot express.
            pointerProperties[pointerIndex].toolType = MotionEvent.TOOL_TYPE_MOUSE;
            source = InputDevice.SOURCE_MOUSE;
            pointer.up = buttons == 0;
        } else {
            pointerProperties[pointerIndex].toolType = MotionEvent.TOOL_TYPE_FINGER;
            source = InputDevice.SOURCE_TOUCHSCREEN;
            buttons = 0;  // buttons must not be set for touch events
            pointer.up = action == MotionEvent.ACTION_UP;
        }

        int pointerCount = pointersState.update(pointerProperties, pointerCoords);
        if (pointerCount == 1) {
            if (action == MotionEvent.ACTION_DOWN) lastTouchDown = now;
        } else {
            // Secondary pointers use ACTION_POINTER_* with the index encoded in the action.
            if (action == MotionEvent.ACTION_UP) {
                action = MotionEvent.ACTION_POINTER_UP
                        | (pointerIndex << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
            } else if (action == MotionEvent.ACTION_DOWN) {
                action = MotionEvent.ACTION_POINTER_DOWN
                        | (pointerIndex << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
            }
        }

        /* Mouse buttons need the full sequence or Chrome misbehaves (fork Controller, upstream
         * issue 3635): the first press is ACTION_DOWN, every press ACTION_BUTTON_PRESS, every
         * release ACTION_BUTTON_RELEASE, the last release ACTION_UP. */
        if (source == InputDevice.SOURCE_MOUSE) {
            if (action == MotionEvent.ACTION_DOWN) {
                if (m.actionButton == buttons
                        && !injectMouse(MotionEvent.ACTION_DOWN, now, pointerCount, buttons, 0)) {
                    return;
                }
                injectMouse(MotionEvent.ACTION_BUTTON_PRESS, now, pointerCount, buttons,
                            m.actionButton);
                return;
            }
            if (action == MotionEvent.ACTION_UP) {
                if (!injectMouse(MotionEvent.ACTION_BUTTON_RELEASE, now, pointerCount, buttons,
                                 m.actionButton)) {
                    return;
                }
                if (buttons == 0) {
                    injectMouse(MotionEvent.ACTION_UP, now, pointerCount, buttons, 0);
                }
                return;
            }
        }

        MotionEvent event = MotionEvent.obtain(lastTouchDown, now, action, pointerCount,
                                               pointerProperties, pointerCoords, 0, buttons, 1f,
                                               1f, 0, 0, source, 0);
        injectEvent(event, InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);
    }

    private boolean injectMouse(int action, long now, int pointerCount, int buttons,
                                int actionButton) {
        MotionEvent event = MotionEvent.obtain(lastTouchDown, now, action, pointerCount,
                                               pointerProperties, pointerCoords, 0, buttons, 1f,
                                               1f, 0, 0, InputDevice.SOURCE_MOUSE, 0);
        if (actionButton != 0) event.setActionButton(actionButton);
        return injectEvent(event, InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);
    }

    private void injectScroll(ControlMessage m) {
        long now = SystemClock.uptimeMillis();
        float[] point = mapToDisplay(m);
        if (point == null) return;
        pointerProperties[0].id = 0;
        MotionEvent.PointerCoords coords = pointerCoords[0];
        coords.x = point[0];
        coords.y = point[1];
        coords.setAxisValue(MotionEvent.AXIS_HSCROLL, m.hScroll);
        coords.setAxisValue(MotionEvent.AXIS_VSCROLL, m.vScroll);
        MotionEvent event = MotionEvent.obtain(lastTouchDown, now, MotionEvent.ACTION_SCROLL, 1,
                                               pointerProperties, pointerCoords, 0, m.buttons, 1f,
                                               1f, 0, 0, InputDevice.SOURCE_MOUSE, 0);
        injectEvent(event, InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);
    }

    private void pressBackOrTurnScreenOn(int action) {
        final int display = inputDisplay();
        if (display < 0) return;  // no display to go back on, and none to wake either
        DisplayInfo info = DisplayManagerGlobal.getInstance().getDisplayInfo(display);
        boolean screenOn = info != null && info.state == android.view.Display.STATE_ON;
        if (screenOn) {
            injectKeyEvent(action, KeyEvent.KEYCODE_BACK, 0, 0,
                           InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);
        } else if (action == KeyEvent.ACTION_DOWN) {
            // Screen off: POWER wakes it, pressed once on the DOWN half only.
            pressReleaseKeycode(KeyEvent.KEYCODE_POWER,
                                InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);
        }
    }

    private void statusBar(String what) {
        try {
            IStatusBarService svc = IStatusBarService.Stub.asInterface(
                    ServiceManager.getService(Context.STATUS_BAR_SERVICE));
            switch (what) {
                case "expandNotificationsPanel": svc.expandNotificationsPanel(); break;
                case "expandSettingsPanel": svc.expandSettingsPanel(null); break;
                case "collapsePanels": svc.collapsePanels(); break;
            }
        } catch (Exception e) {
            Ln.e("status bar " + what + " failed", e);
        }
    }

    // Clipboard ----------------------------------------------------------------------------------

    private String readClipboardText() {
        if (clipboard == null) return null;
        try {
            ClipData clip = clipboard.getPrimaryClip();
            if (clip == null || clip.getItemCount() == 0) return null;
            CharSequence text = clip.getItemAt(0).getText();
            return text != null ? text.toString() : null;
        } catch (Exception e) {
            Ln.e("clipboard read failed", e);
            return null;
        }
    }

    private void getClipboard(int copyKey) {
        // COPY/CUT are injected synchronously so the read that follows sees their result.
        if (copyKey != ControlMessage.COPY_KEY_NONE) {
            int key = copyKey == ControlMessage.COPY_KEY_COPY ? KeyEvent.KEYCODE_COPY
                                                              : KeyEvent.KEYCODE_CUT;
            pressReleaseKeycode(key, InputManager.INJECT_INPUT_EVENT_MODE_WAIT_FOR_FINISH);
        }
        // An explicit GET always gets an explicit answer, even though the change listener will
        // usually have sent the same text already — the duplicate is idempotent client-side, and
        // a request that can go unanswered is untestable.
        String text = readClipboardText();
        if (text != null) sendClipboard(text);
    }

    private void setClipboard(String text, boolean paste, long sequence) {
        lastClipboardSync = text;
        if (clipboard != null && !text.equals(readClipboardText())) {
            // The equality guard mirrors the fork: pasting sets the clipboard, and setting the
            // same text twice would notify listeners twice, flooding keyboard clipboard history.
            settingClipboard.set(true);
            try {
                clipboard.setPrimaryClip(ClipData.newPlainText(null, text));
            } catch (Exception e) {
                Ln.e("clipboard set failed", e);
            } finally {
                settingClipboard.set(false);
            }
        }
        if (paste) {
            pressReleaseKeycode(KeyEvent.KEYCODE_PASTE,
                                InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);
        }
        if (sequence != ControlMessage.SEQUENCE_INVALID) {
            try {
                synchronized (out) {
                    out.writeByte(DEVICE_MSG_ACK_CLIPBOARD);
                    out.writeLong(sequence);
                    out.flush();
                }
            } catch (IOException e) {
                Ln.e("clipboard ack write failed", e);
            }
        }
    }

    private void onDeviceClipboardChanged() {
        // Fires on the main-looper handler. Our own set is not news to the client.
        if (settingClipboard.get()) return;
        String text = readClipboardText();
        if (text == null || text.equals(lastClipboardSync)) return;
        sendClipboard(text);
    }

    private void sendClipboard(String text) {
        lastClipboardSync = text;
        byte[] raw = text.getBytes(StandardCharsets.UTF_8);
        int len = raw.length;
        if (len > MAX_CLIPBOARD_BYTES) {
            len = MAX_CLIPBOARD_BYTES;
            while (len > 0 && (raw[len] & 0xC0) == 0x80) --len;  // never split a codepoint
        }
        try {
            synchronized (out) {
                out.writeByte(DEVICE_MSG_CLIPBOARD);
                out.writeInt(len);
                out.write(raw, 0, len);
                out.flush();
            }
        } catch (IOException e) {
            Ln.e("clipboard device message write failed", e);
        }
    }
}
