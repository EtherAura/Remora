package io.github.etheraura.remora.phone;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * Protocol v3, the phone's side (docs/MIRROR_AGENT.md, docs/MIRROR_PROTOCOL.md): the same bytes the
 * desktop mirror sends, reaching an agent through the link. Big-endian throughout.
 */
final class Proto {
    static final int VERSION = 3;
    static final int ROLE_CONTROL = 0, ROLE_VIDEO = 1, ROLE_AUDIO = 2;
    static final int KIND_NEW_DISPLAY = 3, KIND_LIST_APPS = 4, KIND_APP_ICON = 5;

    // Stream records (§2).
    static final int START = 1, FORMAT = 2, CONFIG = 3, FRAME = 4, END = 5;
    static final int CODEC_H264 = 0x01, CODEC_H265 = 0x02, CODEC_AV1 = 0x03, CODEC_OPUS = 0x81;
    // Control messages, phone → agent (§3).
    static final int KEY = 1, TEXT = 2, POINTER = 3, SCROLL = 4, BACK = 5, CLIPBOARD = 7, RESIZE = 8;
    // Device messages, agent → phone (§4).
    static final int DEVICE_CLIPBOARD = 1;

    static final int CONTROL_LIMIT = 1 << 20, STREAM_LIMIT = 32 << 20;

    private Proto() {}

    /** "RMRA" u16 version u16 flags; then reads and checks the agent's. */
    static void hello(InputStream in, OutputStream out) throws IOException {
        out.write(new byte[] {'R', 'M', 'R', 'A', 0, VERSION, 0, 0});
        out.flush();
        byte[] reply = new byte[8];
        new DataInputStream(in).readFully(reply);
        if (reply[0] != 'R' || reply[1] != 'M' || reply[2] != 'R' || reply[3] != 'A')
            throw new IOException("something answered that is not a Remora agent");
        int version = ((reply[4] & 0xff) << 8) | (reply[5] & 0xff);
        if (version != VERSION)
            throw new IOException("the agent speaks protocol v" + version + " and this app v" + VERSION
                    + " — update " + (version > VERSION ? "the app" : "Remora on the computer"));
    }

    /** role 0, kind, u16-prefixed "key=value\n" lines. */
    static byte[] sessionRequest(int kind, Map<String, String> options) {
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, String> e : options.entrySet())
            sb.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        byte[] opts = sb.toString().getBytes(StandardCharsets.UTF_8);
        ByteBuffer b = ByteBuffer.allocate(4 + opts.length);
        b.put((byte) ROLE_CONTROL).put((byte) kind).putShort((short) opts.length).put(opts);
        return b.array();
    }

    static byte[] attach(int role, int sessionId) {
        return ByteBuffer.allocate(5).put((byte) role).putInt(sessionId).array();
    }

    /** u8 status u32 id [u16 reason] — returns the session id, or throws with the agent's reason. */
    static int readSessionReply(InputStream in) throws IOException {
        DataInputStream d = new DataInputStream(in);
        int status = d.readUnsignedByte();
        int id = d.readInt();
        if (status == 0) return id;
        byte[] reason = new byte[d.readUnsignedShort()];
        d.readFully(reason);
        throw new IOException(new String(reason, StandardCharsets.UTF_8));
    }

    /** The ok reply, then one u32-prefixed blob: list-apps' listing, app-icon's PNG. */
    static byte[] readBlobReply(InputStream in) throws IOException {
        readSessionReply(in);
        DataInputStream d = new DataInputStream(in);
        int len = d.readInt();
        if (len < 0 || len > (8 << 20)) throw new IOException("implausible reply length " + len);
        byte[] blob = new byte[len];
        d.readFully(blob);
        return blob;
    }

    /** One launchable app from a list-apps line: " * Name  id" or " - Name  id". */
    static final class App {
        final String name, id;
        App(String name, String id) { this.name = name; this.id = id; }
    }

    static List<App> parseAppList(byte[] listing) {
        List<App> apps = new ArrayList<>();
        for (String line : new String(listing, StandardCharsets.UTF_8).split("\n")) {
            String t = line.trim();
            if (t.length() < 3 || (t.charAt(0) != '*' && t.charAt(0) != '-')) continue;
            t = t.substring(1).trim();
            int split = t.lastIndexOf("  ");
            if (split < 0) continue;
            apps.add(new App(t.substring(0, split).trim(), t.substring(split).trim()));
        }
        return apps;
    }

    // --- records ----------------------------------------------------------------------------

    static byte[] record(int type, byte[] body) {
        return ByteBuffer.allocate(5 + body.length).putInt(1 + body.length).put((byte) type).put(body)
                .array();
    }

    static final class Record {
        final int type;
        final byte[] body;
        Record(int type, byte[] body) { this.type = type; this.body = body; }
    }

    /** One record, blocking; null at a clean end of stream. */
    static Record readRecord(DataInputStream in, int limit) throws IOException {
        int size;
        try {
            size = in.readInt();
        } catch (EOFException e) {
            return null;
        }
        if (size <= 0 || size > limit) throw new IOException("bad record size " + size);
        int type = in.readUnsignedByte();
        byte[] body = new byte[size - 1];
        in.readFully(body);
        return new Record(type, body);
    }

    // --- control messages -------------------------------------------------------------------

    static final int TOOL_FINGER = 0, TOOL_MOUSE = 1;

    /**
     * x and y are fractions of the frame shown. A mouse's actionButton is the button a DOWN or UP
     * presses or lets go, and buttons those held after it; with none held, a mouse only moves.
     */
    static byte[] pointer(int action, int tool, int pointerId, float x, float y, float pressure,
                          int actionButton, int buttons) {
        ByteBuffer b = ByteBuffer.allocate(26);
        b.put((byte) action).put((byte) tool).putInt(pointerId).putFloat(x).putFloat(y)
                .putFloat(pressure).putInt(actionButton).putInt(buttons);
        return record(POINTER, b.array());
    }

    /** Wheel notches at a point (fractions of the frame): Android's AXIS_HSCROLL/AXIS_VSCROLL. */
    static byte[] scroll(float x, float y, float hScroll, float vScroll) {
        return record(SCROLL, ByteBuffer.allocate(20).putFloat(x).putFloat(y).putFloat(hScroll)
                .putFloat(vScroll).putInt(0).array());
    }

    static byte[] key(int action, int keycode, int metaState) {
        return record(KEY, ByteBuffer.allocate(13).put((byte) action).putInt(keycode).putInt(0)
                .putInt(metaState).array());
    }

    static byte[] text(String text) {
        return record(TEXT, text.getBytes(StandardCharsets.UTF_8));
    }

    static byte[] back(int action) {
        return record(BACK, new byte[] {(byte) action});
    }

    /** The remote display's new size and density; an agent before the density ignores it. */
    static byte[] resize(int width, int height, int dpi) {
        return record(RESIZE, ByteBuffer.allocate(6).putShort((short) width).putShort((short) height)
                .putShort((short) dpi).array());
    }

    static byte[] clipboard(String text, boolean paste) {
        byte[] utf8 = text.getBytes(StandardCharsets.UTF_8);
        ByteBuffer b = ByteBuffer.allocate(9 + utf8.length);
        b.putLong(0).put((byte) (paste ? 1 : 0)).put(utf8);
        return record(CLIPBOARD, b.array());
    }

    static String utf8(byte[] b, int from) {
        return new String(b, from, b.length - from, StandardCharsets.UTF_8);
    }

    static byte[] concat(byte[]... parts) {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        for (byte[] p : parts) o.write(p, 0, p.length);
        return o.toByteArray();
    }
}
