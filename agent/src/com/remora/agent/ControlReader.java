package com.remora.agent;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

/**
 * Blocking decoder for the client→agent direction of a control connection: protocol v3 control
 * messages (docs/MIRROR_PROTOCOL.md §3), each one record, pinned on the client side by
 * tests/test_mirrorproto.cpp. A type this agent does not know is skipped, not fatal — the record
 * size is the resync point, so a newer client's additions cost an older agent nothing.
 */
public final class ControlReader {
    // The fixed fields each type needs; a shorter body is a framing error.
    private static final int KEY_SIZE = 1 + 4 + 4 + 4;
    private static final int POINTER_SIZE = 1 + 1 + 4 + 4 + 4 + 4 + 4 + 4;
    private static final int SCROLL_SIZE = 4 + 4 + 4 + 4 + 4;
    private static final int CLIPBOARD_SIZE = 8 + 1;

    private final Records.Reader reader;
    private final boolean[] warnedUnknown = new boolean[256];

    public ControlReader(InputStream raw) {
        reader = new Records.Reader(new DataInputStream(new BufferedInputStream(raw)),
                                    Records.CONTROL_LIMIT);
    }

    /** Blocks for the next message this agent knows. Throws IOException on EOF or a framing error. */
    public ControlMessage next() throws IOException {
        for (;;) {
            Records.Record r = reader.next();
            ControlMessage m = decode(r.type, r.body);
            if (m != null) return m;
            if (!warnedUnknown[r.type]) {
                warnedUnknown[r.type] = true;
                Ln.w("skipping control message type 0x" + Integer.toHexString(r.type)
                     + " — unknown to this agent");
            }
        }
    }

    /** The message in this record, or null for a type this agent does not know. */
    private static ControlMessage decode(int type, byte[] body) throws IOException {
        ControlMessage m = new ControlMessage();
        m.type = type;
        ByteBuffer b = ByteBuffer.wrap(body);
        switch (type) {
            case ControlMessage.TYPE_KEY:
                need(type, body, KEY_SIZE);
                m.action = b.get() & 0xff;
                m.keycode = b.getInt();
                m.repeat = b.getInt();
                m.metaState = b.getInt();
                break;
            case ControlMessage.TYPE_TEXT:
                m.text = new String(body, StandardCharsets.UTF_8);
                break;
            case ControlMessage.TYPE_POINTER:
                need(type, body, POINTER_SIZE);
                m.action = b.get() & 0xff;
                m.tool = b.get() & 0xff;
                m.pointerId = b.getInt() & 0xffffffffL;
                m.x = b.getFloat();
                m.y = b.getFloat();
                m.pressure = b.getFloat();
                m.actionButton = b.getInt();
                m.buttons = b.getInt();
                break;
            case ControlMessage.TYPE_SCROLL:
                need(type, body, SCROLL_SIZE);
                m.x = b.getFloat();
                m.y = b.getFloat();
                m.hScroll = b.getFloat();
                m.vScroll = b.getFloat();
                m.buttons = b.getInt();
                break;
            case ControlMessage.TYPE_BACK:
                need(type, body, 1);
                m.action = b.get() & 0xff;
                break;
            case ControlMessage.TYPE_PANEL:
                need(type, body, 1);
                m.panel = b.get() & 0xff;
                break;
            case ControlMessage.TYPE_CLIPBOARD:
                need(type, body, CLIPBOARD_SIZE);
                m.sequence = b.getLong();
                m.paste = b.get() != 0;
                m.text = new String(body, CLIPBOARD_SIZE, body.length - CLIPBOARD_SIZE,
                                    StandardCharsets.UTF_8);
                break;
            case ControlMessage.TYPE_RESIZE:
                need(type, body, 2 + 2);
                m.width = b.getShort() & 0xffff;
                m.height = b.getShort() & 0xffff;
                break;
            default:
                return null;
        }
        return m;
    }

    private static void need(int type, byte[] body, int size) throws IOException {
        if (body.length < size) {
            throw new IOException("control message type 0x" + Integer.toHexString(type) + " has a "
                                  + body.length + "-byte body, needs " + size
                                  + " — framing error, dropping the connection");
        }
    }
}
