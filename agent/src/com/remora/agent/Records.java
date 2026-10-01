package com.remora.agent;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;

/**
 * The record framing of protocol v3 (docs/MIRROR_PROTOCOL.md §1): once a connection's setup is
 * done, everything on it, in both directions, is {@code u32 size, u8 type, body}, where size
 * counts the type byte and the body. The size is what makes the protocol extensible — a receiver
 * skips a type it does not know and ignores bytes past the fields it does, so a newer peer's
 * additions never desynchronise an older one.
 *
 * Bodies are parsed with {@code ByteBuffer.wrap(body)}, big-endian by default like the wire. A
 * body shorter than the fields its type needs is a framing error; one that is longer is a later
 * version's growth and is fine.
 */
public final class Records {
    /** Size limits per connection kind: past these a record is a framing error, not a message. */
    public static final int CONTROL_LIMIT = 1 << 20;
    public static final int STREAM_LIMIT = 32 << 20;

    /** One received record. */
    public static final class Record {
        public final int type;
        public final byte[] body;

        Record(int type, byte[] body) {
            this.type = type;
            this.body = body;
        }
    }

    /** Blocking record reader over one direction of a connection. */
    public static final class Reader {
        private final DataInputStream in;
        private final int limit;

        public Reader(DataInputStream in, int limit) {
            this.in = in;
            this.limit = limit;
        }

        /** Blocks for the next record. Throws IOException on EOF or a framing error. */
        public Record next() throws IOException {
            final long size = in.readInt() & 0xffffffffL;
            if (size == 0 || size > limit) {
                throw new IOException("record size " + size + " outside 1.." + limit
                                      + " — framing error, dropping the connection");
            }
            final int type = in.readUnsignedByte();
            final byte[] body = new byte[(int) size - 1];
            in.readFully(body);
            return new Record(type, body);
        }
    }

    /**
     * Record writer over one direction of a connection. Every write is one synchronized call that
     * writes the whole record and flushes, so several threads may share a writer — a control
     * connection's device messages come from both the control thread (clipboard acks) and the
     * clipboard listener's handler thread.
     */
    public static final class Writer {
        private final OutputStream out;
        private final int limit;
        private byte[] head = new byte[32];  // size + type + fixed fields, grown on demand

        public Writer(OutputStream out, int limit) {
            this.out = out;
            this.limit = limit;
        }

        public void write(int type, byte[] body) throws IOException {
            write(type, null, 0, body, 0, body.length);
        }

        /**
         * One record whose body is a fixed-field prefix followed by a payload. The two are never
         * joined into one array first: the header goes out in one write and the payload in
         * another, so a 30 Mb/s video stream pays for no extra copy of every frame.
         */
        public synchronized void write(int type, byte[] fields, int fieldsLen,
                                       byte[] data, int off, int len) throws IOException {
            final long size = 1L + fieldsLen + len;
            if (size > limit) {
                throw new IOException("record type " + type + " of " + size + " bytes is over the "
                                      + limit + "-byte limit");
            }
            final int headLen = 5 + fieldsLen;
            if (head.length < headLen) head = new byte[headLen];
            head[0] = (byte) (size >>> 24);
            head[1] = (byte) (size >>> 16);
            head[2] = (byte) (size >>> 8);
            head[3] = (byte) size;
            head[4] = (byte) type;
            if (fieldsLen > 0) System.arraycopy(fields, 0, head, 5, fieldsLen);
            out.write(head, 0, headLen);
            if (len > 0) out.write(data, off, len);
            out.flush();
        }
    }

    private Records() {}
}
