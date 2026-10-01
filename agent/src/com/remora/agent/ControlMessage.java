package com.remora.agent;

/**
 * One decoded control message, client → agent (protocol v3, docs/MIRROR_PROTOCOL.md §3). A plain
 * field bag, not a class hierarchy: the reader fills what the type uses and the controller
 * switches on {@link #type}. Every type here is one the agent acts on.
 */
public final class ControlMessage {
    public static final int TYPE_KEY = 0x01;
    public static final int TYPE_TEXT = 0x02;
    public static final int TYPE_POINTER = 0x03;
    public static final int TYPE_SCROLL = 0x04;
    public static final int TYPE_BACK = 0x05;
    public static final int TYPE_PANEL = 0x06;
    public static final int TYPE_CLIPBOARD = 0x07;
    public static final int TYPE_RESIZE = 0x08;

    public static final int TOOL_FINGER = 0;
    public static final int TOOL_MOUSE = 1;

    public static final int PANEL_COLLAPSE = 0;
    public static final int PANEL_NOTIFICATIONS = 1;
    public static final int PANEL_SETTINGS = 2;

    public static final long SEQUENCE_NONE = 0;  // a CLIPBOARD that wants no acknowledgement

    public int type;
    public String text;         // TEXT, CLIPBOARD
    public int metaState;       // KeyEvent.META_*
    public int action;          // KeyEvent.ACTION_* or MotionEvent.ACTION_*
    public int keycode;         // KeyEvent.KEYCODE_*
    public int repeat;
    public int tool;            // TOOL_*
    public long pointerId;      // u32, unsigned
    public float x, y;          // fractions of the frame the client is showing
    public float pressure;      // [0, 1]
    public int actionButton;    // MotionEvent.BUTTON_*
    public int buttons;         // MotionEvent.BUTTON_*
    public float hScroll, vScroll;  // AXIS_HSCROLL/AXIS_VSCROLL units, one notch = 1.0
    public int panel;           // PANEL_*
    public long sequence;
    public boolean paste;
    public int width, height;   // RESIZE
}
