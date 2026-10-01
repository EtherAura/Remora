package com.remora.agent;

import android.app.ActivityThread;
import android.content.Context;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Looper;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The Remora agent — the device-side half of the mirror (bd remora-28ix.3, docs/MIRROR_AGENT.md).
 *
 * A persistent init service (or, during development, an app_process launched by hand) listening
 * on localabstract:remora_agent. Compiled in-tree against the platform framework.jar — hidden
 * APIs are plain calls here, never reflection. Protocol v3, Remora's own: a versioned RMRA hello
 * on every connection, then either a session request (control) or an attach (video, audio), and
 * from there on nothing but records (docs/MIRROR_PROTOCOL.md). kind=mirror is complete — control
 * (bd remora-28ix.3.1) and video (bd remora-28ix.3.2); the other kinds land in
 * bd remora-28ix.3.3.
 */
public final class Main {
    static final int PROTO_VERSION = 3;
    static final int CAPABILITIES = 0;  // capability bits — none yet
    static final String SOCKET_NAME = "remora_agent";
    private static final byte[] MAGIC = {'R', 'M', 'R', 'A'};

    static final int KIND_MIRROR = 1;
    static final int KIND_COMPOSE = 2;
    static final int KIND_NEW_DISPLAY = 3;
    static final int KIND_LIST_APPS = 4;
    // The connection ROLE, the byte immediately after the hello. One listener serves both kinds
    // of connection, so it must be able to tell them apart before reading anything role-specific
    // — and "kind=mirror" and "attach video" would otherwise both be a leading 0x01.
    static final int ROLE_CONTROL = 0;
    static final int ROLE_VIDEO = 1;
    // Audio is a ROLE, not a positional third socket: with roles, a session that has no video
    // never renumbers everything after it (bd remora-28ix.3.6).
    static final int ROLE_AUDIO = 2;

    private static final AtomicInteger nextSessionId = new AtomicInteger(1);
    private static final Map<Integer, Session> sessions = new ConcurrentHashMap<>();
    private static Context shellContext;

    /** The shell-attributed system context; see ShellContext for why it is not the plain one. */
    public static Context context() {
        return shellContext;
    }

    public static void main(String[] args) {
        Ln.i("proto v" + PROTO_VERSION + " starting on " + SOCKET_NAME);
        // The framework bootstrap, in system-server order: a main looper, then the system
        // ActivityThread whose context every manager construction hangs off. The looper is not
        // decoration — clipboard change callbacks dispatch through a Handler on it, so main()
        // parks in Looper.loop() and the accept loop gets its own thread.
        Looper.prepareMainLooper();
        shellContext = new ShellContext(ActivityThread.systemMain().getSystemContext());
        Thread acceptThread = new Thread(Main::acceptLoop, "accept");
        acceptThread.setDaemon(false);
        acceptThread.start();
        Looper.loop();
        Ln.w("main looper quit — exiting for init to restart us");
        System.exit(1);
    }

    private static void acceptLoop() {
        try {
            LocalServerSocket server = new LocalServerSocket(SOCKET_NAME);
            for (;;) {
                LocalSocket conn = server.accept();
                Thread t = new Thread(() -> serve(conn), "conn");
                t.setDaemon(true);
                t.start();
            }
        } catch (IOException e) {
            // init supervises: exit nonzero and let it restart us with a clean listener.
            Ln.e("fatal", e);
            System.exit(1);
        }
    }

    private static void serve(LocalSocket conn) {
        try (LocalSocket c = conn) {
            DataInputStream in = new DataInputStream(c.getInputStream());
            DataOutputStream out = new DataOutputStream(c.getOutputStream());

            // Hello: magic, u16 version, u16 flags — both directions. The agent always answers
            // with its own version, so a client of any version learns what it is talking to and
            // can say so; then a client that is not v3 is dropped. There is no negotiating down:
            // this agent speaks v3 only, and the user-facing refusal is the client's to make.
            byte[] magic = new byte[4];
            in.readFully(magic);
            if (!java.util.Arrays.equals(magic, MAGIC)) {
                Ln.w("bad magic, dropping connection");
                return;
            }
            int clientVersion = in.readUnsignedShort();
            in.readUnsignedShort();  // client flags — reserved, ignored
            out.write(MAGIC);
            out.writeShort(PROTO_VERSION);
            out.writeShort(CAPABILITIES);
            out.flush();
            if (clientVersion != PROTO_VERSION) {
                Ln.w("client speaks protocol v" + clientVersion + ", this agent v" + PROTO_VERSION
                     + " — dropping connection");
                return;
            }

            final int role = in.readUnsignedByte();
            if (role == ROLE_VIDEO) {
                serveVideo(in, out);
            } else if (role == ROLE_AUDIO) {
                serveAudio(in, out);
            } else if (role == ROLE_CONTROL) {
                serveControl(in, out);
            } else {
                Ln.w("unknown connection role " + role + ", dropping connection");
            }
        } catch (EOFException e) {
            // peer went away mid-handshake — normal teardown, nothing to log
        } catch (IOException e) {
            Ln.w("connection error: " + e);
        } catch (RuntimeException e) {
            // One connection's bug ends that connection. Uncaught, it would end the process — and
            // with it every other session the agent is serving (bd remora-hsdz).
            Ln.e("connection failed", e);
        }
    }

    // A control connection: u8 kind, u16-prefixed "key=value\n" option block, then the connection
    // IS the control socket for the session it opened.
    private static void serveControl(DataInputStream in, DataOutputStream out) throws IOException {
        final int kind = in.readUnsignedByte();
        int optLen = in.readUnsignedShort();
        byte[] optRaw = new byte[optLen];
        in.readFully(optRaw);
        Map<String, String> opts = parseOptions(optRaw);
        Ln.i("session request kind=" + kind + " opts=" + opts);

        // Reply shape is the contract: u8 status, u32 sessionId, then (status != 0) a
        // u16-prefixed UTF-8 reason.
        final Capture capture;
        switch (kind) {
            case KIND_MIRROR:
            case KIND_COMPOSE:
                // Both mirror an existing display; they differ only in who consumes the frames —
                // a MediaCodec on the video connection, or an ImageReader so the HOST encoder can.
                capture = new MirrorCapture(intOpt(opts, "display_id", 0));
                break;
            case KIND_NEW_DISPLAY: {
                final Session[] self = new Session[1];
                capture = new NewDisplayCapture(
                        opts.get("new_display"), opts.get("start_app"),
                        "true".equals(opts.get("close_on_app_exit")),
                        () -> {
                            // The watched app was left: end the session, which closes the
                            // client's window exactly as a disconnect would.
                            Session s = self[0];
                            if (s != null) {
                                sessions.remove(s.id);
                                s.close();
                            }
                        });
                Session session = new Session(nextSessionId.getAndIncrement(), opts, capture);
                self[0] = session;
                runControlSession(session, kind, in, out);
                return;
            }
            case KIND_LIST_APPS: {
                // Request/response on this connection (docs/MIRROR_AGENT.md): the ok reply, then
                // one u32-prefixed UTF-8 blob of " * Name  pkg" lines (AppList). No session
                // outlives the answer — the connection is done when the blob is written.
                out.writeByte(0);
                out.writeInt(nextSessionId.getAndIncrement());
                byte[] listing = AppList.listing();
                out.writeInt(listing.length);
                out.write(listing);
                out.flush();
                return;
            }
            default: {
                byte[] reason = ("unknown session kind " + kind).getBytes(StandardCharsets.UTF_8);
                out.writeByte(1);
                out.writeInt(0);
                out.writeShort(reason.length);
                out.write(reason);
                out.flush();
                return;
            }
        }
        runControlSession(new Session(nextSessionId.getAndIncrement(), opts, capture), kind,
                          in, out);
    }

    private static void runControlSession(Session session, int kind, DataInputStream in,
                                          DataOutputStream out) throws IOException {
        sessions.put(session.id, session);
        out.writeByte(0);
        out.writeInt(session.id);
        out.flush();

        // A compose session has no video connection — nothing would ever attach and start the
        // capture — so it runs off this connection instead, for as long as it lives.
        // (video_compose lets a NEW-DISPLAY session opt into the same: its frames go to the host
        // encoder too, it just also owns the display.)
        Thread composeThread = null;
        if (kind == KIND_COMPOSE || "true".equals(session.opts.get("video_compose"))) {
            Composer composer = new Composer(session, intOpt(session.opts, "max_size", 0));
            if (session.attachComposer(composer)) {
                composeThread = new Thread(composer::run, "compose");
                composeThread.start();
            }
        }

        try {
            new Controller(shellContext, session, in, out).run();
        } finally {
            // The session dies with its control connection — the client is gone, so its video
            // has nowhere to go either.
            sessions.remove(session.id);
            session.close();
            if (composeThread != null) {
                try {
                    composeThread.join(2000);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
        }
    }

    private static int intOpt(Map<String, String> opts, String key, int fallback) {
        try {
            String v = opts.get(key);
            return v != null && !v.isEmpty() ? Integer.parseInt(v) : fallback;
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    // An audio connection: u32 sessionId, then a v3 media stream — START, CONFIG and FRAMEs,
    // no FORMAT (audio has no geometry). Same attach shape as video, different role.
    private static void serveAudio(DataInputStream in, DataOutputStream out) throws IOException {
        final int sessionId = in.readInt();
        Session session = sessions.get(sessionId);
        VideoStreamer streamer = new VideoStreamer(out);
        if (session == null) {
            Ln.w("audio attach for unknown session " + sessionId);
            streamer.writeEnd(VideoStreamer.END_FAILED, "unknown session " + sessionId);
            return;
        }
        AudioEncoder audio = new AudioEncoder(sessionId, session.opts, streamer);
        if (!session.attachAudio(audio)) {
            // the control connection closed while we were setting up
            streamer.writeEnd(VideoStreamer.END_FAILED, "session closed during setup");
            return;
        }
        try {
            audio.run();
        } finally {
            session.detachAudio(audio);
        }
    }

    // A video connection: u32 sessionId, then a v3 media stream in the other direction.
    private static void serveVideo(DataInputStream in, DataOutputStream out) throws IOException {
        final int sessionId = in.readInt();
        Session session = sessions.get(sessionId);
        VideoStreamer streamer = new VideoStreamer(out);
        if (session == null) {
            // No session reply exists on this connection to carry an error, so say it the way
            // every stream can: END in place of START.
            Ln.w("video attach for unknown session " + sessionId);
            streamer.writeEnd(VideoStreamer.END_FAILED, "unknown session " + sessionId);
            return;
        }
        ScreenEncoder encoder;
        try {
            encoder = new ScreenEncoder(session, streamer);
        } catch (ScreenEncoder.ConfigurationException e) {
            Ln.w("session " + sessionId + " video: " + e.getMessage());
            streamer.writeEnd(VideoStreamer.END_FAILED, e.getMessage());
            return;
        }
        if (!session.attachEncoder(encoder)) {
            // the control connection closed while we were setting up
            streamer.writeEnd(VideoStreamer.END_FAILED, "session closed during setup");
            return;
        }
        try {
            encoder.run();
        } finally {
            session.detachEncoder(encoder);
        }
    }

    private static Map<String, String> parseOptions(byte[] raw) {
        Map<String, String> opts = new HashMap<>();
        for (String line : new String(raw, StandardCharsets.UTF_8).split("\n")) {
            int eq = line.indexOf('=');
            if (eq > 0) opts.put(line.substring(0, eq), line.substring(eq + 1));
        }
        return opts;
    }

    private Main() {}
}
