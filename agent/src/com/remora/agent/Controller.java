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

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The control half of a session (bd remora-28ix.3.1): decodes protocol v3 control messages from
 * the connection (docs/MIRROR_PROTOCOL.md §3), injects input through the real framework —
 * InputManagerGlobal, no reflection — and keeps the clipboard synchronized both ways. Runs on its
 * connection's thread until the peer disconnects; the clipboard listener is the only cross-thread
 * visitor and only touches the synchronized device-message writer.
 *
 * Every v3 type is acted on: key, text (dead-key decomposition), pointer (multi-pointer state,
 * the mouse ACTION_BUTTON_PRESS/RELEASE sequence Chrome needs), scroll, back-or-screen-on, the
 * status-bar panels, clipboard set with paste and acknowledgement, and the flex reflow (RESIZE,
 * bd remora-28ix.3.3). Types the agent does not know never reach here — the reader skips them.
 */
public final class Controller {
    // A device CLIPBOARD record's body is the text alone, so it gets the whole record but the
    // type byte.
    private static final int MAX_CLIPBOARD_BYTES = Records.CONTROL_LIMIT - 1;

    // Device→client message types (docs/MIRROR_PROTOCOL.md §4).
    private static final int DEVICE_MSG_CLIPBOARD = 0x01;
    private static final int DEVICE_MSG_CLIPBOARD_ACK = 0x02;

    // How long the first event waits for a new display to exist before giving up on it.
    private static final int DISPLAY_WAIT_MS = 5000;

    private final int sessionId;
    private final Session session;
    private final ControlReader reader;
    private final Records.Writer out;    // device messages; every write is one synchronized record
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

    public Controller(Context context, Session session, InputStream in, OutputStream rawOut) {
        this.sessionId = session.id;
        this.session = session;
        this.reader = new ControlReader(in);
        this.out = new Records.Writer(rawOut, Records.CONTROL_LIMIT);
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
            // EOF or a framing error — either way the session is over; the message says which.
            Ln.i("session " + sessionId + " control connection closed: " + e);
        } finally {
            if (clipboard != null) clipboard.removePrimaryClipChangedListener(clipListener);
        }
    }

    private void handle(ControlMessage m) {
        switch (m.type) {
            case ControlMessage.TYPE_KEY:
                injectKeyEvent(m.action, m.keycode, m.repeat, m.metaState,
                               InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);
                break;
            case ControlMessage.TYPE_TEXT:
                injectText(m.text);
                break;
            case ControlMessage.TYPE_POINTER:
                injectPointer(m);
                break;
            case ControlMessage.TYPE_SCROLL:
                injectScroll(m);
                break;
            case ControlMessage.TYPE_BACK:
                pressBackOrTurnScreenOn(m.action);
                break;
            case ControlMessage.TYPE_PANEL:
                panel(m.panel);
                break;
            case ControlMessage.TYPE_CLIPBOARD:
                setClipboard(m.text, m.paste, m.sequence);
                break;
            case ControlMessage.TYPE_RESIZE:
                // The client's window was resized; reflow a new display to match (a mirror
                // ignores this — its size follows its source).
                session.requestResize(m.width, m.height);
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
     * Client coordinates arrive as fractions of the frame the client is showing; the display is
     * the space events land in, so the point is the fraction times the display's current size.
     * The client never needs to know that size — a max_size-capped video, a mid-session resize and
     * a session with no video connection all map the same way. The DisplayInfo lookup is cheap
     * (client-side cached in DisplayManagerGlobal), so it tracks a resize without extra wiring.
     * Values past [0, 1] are a drag beyond the edge and pass through.
     */
    private float[] mapToDisplay(ControlMessage m) {
        if (!Float.isFinite(m.x) || !Float.isFinite(m.y)) return null;
        final int display = inputDisplay();
        if (display < 0) return null;
        DisplayInfo info = DisplayManagerGlobal.getInstance().getDisplayInfo(display);
        if (info == null) {
            Ln.w("no display info for display " + display + " — dropping positional event");
            return null;
        }
        return new float[]{m.x * info.logicalWidth, m.y * info.logicalHeight};
    }

    /** The PointersState key: the tool in the high half, so a finger and the mouse never collide. */
    private static long pointerKey(ControlMessage m) {
        return ((long) m.tool << 32) | (m.pointerId & 0xffffffffL);
    }

    private void injectPointer(ControlMessage m) {
        long now = SystemClock.uptimeMillis();
        float[] point = mapToDisplay(m);
        if (point == null) return;

        int action = m.action;
        int buttons = m.buttons;
        int pointerIndex = pointersState.getPointerIndex(pointerKey(m));
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
        if (m.tool == ControlMessage.TOOL_MOUSE
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

        /* Mouse buttons need the full sequence or Chrome misbehaves: the first press is
         * ACTION_DOWN, every press ACTION_BUTTON_PRESS, every release ACTION_BUTTON_RELEASE, the
         * last release ACTION_UP. */
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

    private void panel(int which) {
        try {
            IStatusBarService svc = IStatusBarService.Stub.asInterface(
                    ServiceManager.getService(Context.STATUS_BAR_SERVICE));
            switch (which) {
                case ControlMessage.PANEL_COLLAPSE: svc.collapsePanels(); break;
                case ControlMessage.PANEL_NOTIFICATIONS: svc.expandNotificationsPanel(); break;
                case ControlMessage.PANEL_SETTINGS: svc.expandSettingsPanel(null); break;
                default: Ln.w("unknown panel " + which + " — ignored"); break;
            }
        } catch (Exception e) {
            Ln.e("status bar panel " + which + " failed", e);
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

    private void setClipboard(String text, boolean paste, long sequence) {
        lastClipboardSync = text;
        if (clipboard != null && !text.equals(readClipboardText())) {
            // The equality guard is load-bearing: pasting sets the clipboard, and setting the
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
        if (sequence != ControlMessage.SEQUENCE_NONE) {
            try {
                out.write(DEVICE_MSG_CLIPBOARD_ACK, ByteBuffer.allocate(8).putLong(sequence).array());
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
            out.write(DEVICE_MSG_CLIPBOARD, null, 0, raw, 0, len);
        } catch (IOException e) {
            Ln.e("clipboard device message write failed", e);
        }
    }
}
