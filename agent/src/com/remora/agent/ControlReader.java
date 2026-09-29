package com.remora.agent;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/**
 * Blocking decoder for the client→agent direction of a control connection: v1 control messages,
 * types 0–21, byte-identical to the fork's reader so the client's MirrorProto encoders (pinned
 * by tests/test_mirrorproto.cpp) keep working against the agent. Every type decodes — an
 * unknown id is fatal for the connection because the stream has no resync point — even where
 * the agent then declines to act (Controller decides that).
 */
public final class ControlReader {
    private final DataInputStream in;

    public ControlReader(InputStream raw) {
        in = new DataInputStream(new BufferedInputStream(raw));
    }

    /** Blocks for the next message. Throws IOException on EOF/protocol violation. */
    public ControlMessage next() throws IOException {
        ControlMessage m = new ControlMessage();
        m.type = in.readUnsignedByte();
        switch (m.type) {
            case ControlMessage.TYPE_INJECT_KEYCODE:
                m.action = in.readUnsignedByte();
                m.keycode = in.readInt();
                m.repeat = in.readInt();
                m.metaState = in.readInt();
                break;
            case ControlMessage.TYPE_INJECT_TEXT:
                m.text = readString(4);
                break;
            case ControlMessage.TYPE_INJECT_TOUCH_EVENT:
                m.action = in.readUnsignedByte();
                m.pointerId = in.readLong();
                readPosition(m);
                m.pressure = u16FixedPointToFloat(in.readShort());
                m.actionButton = in.readInt();
                m.buttons = in.readInt();
                break;
            case ControlMessage.TYPE_INJECT_SCROLL_EVENT:
                readPosition(m);
                // The wire i16 covers [-1,1] of value/16 (MirrorProto scrollToI16); scale back.
                m.hScroll = i16FixedPointToFloat(in.readShort()) * 16;
                m.vScroll = i16FixedPointToFloat(in.readShort()) * 16;
                m.buttons = in.readInt();
                break;
            case ControlMessage.TYPE_BACK_OR_SCREEN_ON:
                m.action = in.readUnsignedByte();
                break;
            case ControlMessage.TYPE_GET_CLIPBOARD:
                m.copyKey = in.readUnsignedByte();
                break;
            case ControlMessage.TYPE_SET_CLIPBOARD:
                m.sequence = in.readLong();
                m.paste = in.readByte() != 0;
                m.text = readString(4);
                break;
            case ControlMessage.TYPE_SET_DISPLAY_POWER:
            case ControlMessage.TYPE_CAMERA_SET_TORCH:
                m.on = in.readBoolean();
                break;
            case ControlMessage.TYPE_EXPAND_NOTIFICATION_PANEL:
            case ControlMessage.TYPE_EXPAND_SETTINGS_PANEL:
            case ControlMessage.TYPE_COLLAPSE_PANELS:
            case ControlMessage.TYPE_ROTATE_DEVICE:
            case ControlMessage.TYPE_OPEN_HARD_KEYBOARD_SETTINGS:
            case ControlMessage.TYPE_RESET_VIDEO:
            case ControlMessage.TYPE_CAMERA_ZOOM_IN:
            case ControlMessage.TYPE_CAMERA_ZOOM_OUT:
                break;  // no payload
            case ControlMessage.TYPE_UHID_CREATE:
                m.id = in.readUnsignedShort();
                // vendor and product ids, unused here but part of the layout
                in.readUnsignedShort();
                in.readUnsignedShort();
                m.text = readString(1);
                m.data = readByteArray(2);
                break;
            case ControlMessage.TYPE_UHID_INPUT:
                m.id = in.readUnsignedShort();
                m.data = readByteArray(2);
                break;
            case ControlMessage.TYPE_UHID_DESTROY:
                m.id = in.readUnsignedShort();
                break;
            case ControlMessage.TYPE_START_APP:
                m.text = readString(1);
                break;
            case ControlMessage.TYPE_RESIZE_DISPLAY:
                m.width = in.readUnsignedShort();
                m.height = in.readUnsignedShort();
                break;
            default:
                throw new IOException("unknown control message type " + m.type
                                      + " — no resync point, dropping the connection");
        }
        return m;
    }

    private void readPosition(ControlMessage m) throws IOException {
        m.x = in.readInt();
        m.y = in.readInt();
        m.screenWidth = in.readUnsignedShort();
        m.screenHeight = in.readUnsignedShort();
    }

    private int readLength(int sizeBytes) throws IOException {
        int value = 0;
        for (int i = 0; i < sizeBytes; ++i) {
            value = (value << 8) | in.readUnsignedByte();
        }
        return value;
    }

    private byte[] readByteArray(int sizeBytes) throws IOException {
        byte[] data = new byte[readLength(sizeBytes)];
        in.readFully(data);
        return data;
    }

    private String readString(int sizeBytes) throws IOException {
        return new String(readByteArray(sizeBytes), StandardCharsets.UTF_8);
    }

    // The client's fixed-point conversions (MirrorProto pressureToU16/scrollToI16), inverted.
    private static float u16FixedPointToFloat(short value) {
        int unsigned = value & 0xFFFF;
        return unsigned == 0xFFFF ? 1.0f : unsigned / 65536.0f;
    }

    private static float i16FixedPointToFloat(short value) {
        return value == 0x7FFF ? 1.0f : value / 32768.0f;
    }
}
