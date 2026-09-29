package com.remora.agent;

/**
 * One decoded v1 control message (docs/MIRROR_PROTOCOL.md). Type ids and field layouts are the
 * fork's, byte-identical, because the client's MirrorProto encoders carry over to protocol v2
 * unchanged (docs/MIRROR_AGENT.md). A plain field bag, not a class hierarchy: the reader fills
 * what the type uses and the controller switches on {@link #type}.
 */
public final class ControlMessage {
    public static final int TYPE_INJECT_KEYCODE = 0;
    public static final int TYPE_INJECT_TEXT = 1;
    public static final int TYPE_INJECT_TOUCH_EVENT = 2;
    public static final int TYPE_INJECT_SCROLL_EVENT = 3;
    public static final int TYPE_BACK_OR_SCREEN_ON = 4;
    public static final int TYPE_EXPAND_NOTIFICATION_PANEL = 5;
    public static final int TYPE_EXPAND_SETTINGS_PANEL = 6;
    public static final int TYPE_COLLAPSE_PANELS = 7;
    public static final int TYPE_GET_CLIPBOARD = 8;
    public static final int TYPE_SET_CLIPBOARD = 9;
    public static final int TYPE_SET_DISPLAY_POWER = 10;
    public static final int TYPE_ROTATE_DEVICE = 11;
    public static final int TYPE_UHID_CREATE = 12;
    public static final int TYPE_UHID_INPUT = 13;
    public static final int TYPE_UHID_DESTROY = 14;
    public static final int TYPE_OPEN_HARD_KEYBOARD_SETTINGS = 15;
    public static final int TYPE_START_APP = 16;
    public static final int TYPE_RESET_VIDEO = 17;
    public static final int TYPE_CAMERA_SET_TORCH = 18;
    public static final int TYPE_CAMERA_ZOOM_IN = 19;
    public static final int TYPE_CAMERA_ZOOM_OUT = 20;
    public static final int TYPE_RESIZE_DISPLAY = 21;

    public static final long SEQUENCE_INVALID = 0;

    public static final int COPY_KEY_NONE = 0;
    public static final int COPY_KEY_COPY = 1;
    public static final int COPY_KEY_CUT = 2;

    public int type;
    public String text;         // INJECT_TEXT, SET_CLIPBOARD, UHID name, START_APP
    public int metaState;       // KeyEvent.META_*
    public int action;          // KeyEvent.ACTION_* or MotionEvent.ACTION_*
    public int keycode;         // KeyEvent.KEYCODE_*
    public int actionButton;    // MotionEvent.BUTTON_*
    public int buttons;         // MotionEvent.BUTTON_*
    public long pointerId;
    public float pressure;
    public int x, y;            // in the client's video coordinate space…
    public int screenWidth, screenHeight;  // …whose size is this
    public float hScroll, vScroll;
    public int copyKey;
    public boolean paste;
    public int repeat;
    public long sequence;
    public int id;              // uhid
    public byte[] data;         // uhid
    public boolean on;
    public int width, height;   // RESIZE_DISPLAY
}
