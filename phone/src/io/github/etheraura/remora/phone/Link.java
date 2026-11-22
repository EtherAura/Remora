package io.github.etheraura.remora.phone;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.math.BigInteger;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.Principal;
import java.security.PrivateKey;
import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.List;

import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.X509ExtendedKeyManager;
import javax.net.ssl.X509TrustManager;
import javax.security.auth.x500.X500Principal;

/**
 * The link, the phone's side (docs/LINK_PROTOCOL.md): TLS 1.3 to a paired computer with both
 * certificates pinned by fingerprint, one request per connection — pair, sources, connect.
 *
 * The phone's key lives in the Android Keystore and never leaves it; the computer pins the
 * self-signed certificate the Keystore made for it. The computer is trusted for exactly the
 * fingerprint its pairing code named, never by a certificate authority.
 */
final class Link {
    private static final String KEY_ALIAS = "remora-link";
    private static final int PAIR = 1, SOURCES = 2, CONNECT = 3;
    private static final int CONNECT_TIMEOUT_MS = 4000;

    private Link() {}

    // --- the paired computers -----------------------------------------------------------------

    static final class Host {
        final String name, fingerprint;
        final List<String> addresses;  // host:port, most recently working first
        Host(String name, String fingerprint, List<String> addresses) {
            this.name = name;
            this.fingerprint = fingerprint;
            this.addresses = addresses;
        }
    }

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("link", Context.MODE_PRIVATE);
    }

    static List<Host> hosts(Context c) {
        List<Host> out = new ArrayList<>();
        try {
            JSONArray arr = new JSONArray(prefs(c).getString("hosts", "[]"));
            for (int i = 0; i < arr.length(); i++) {
                JSONObject o = arr.getJSONObject(i);
                List<String> addrs = new ArrayList<>();
                JSONArray a = o.getJSONArray("addresses");
                for (int j = 0; j < a.length(); j++) addrs.add(a.getString(j));
                out.add(new Host(o.getString("name"), o.getString("fingerprint"), addrs));
            }
        } catch (JSONException e) {
            // A store that does not parse is one with nothing usable in it.
        }
        return out;
    }

    static void saveHosts(Context c, List<Host> hosts) {
        JSONArray arr = new JSONArray();
        try {
            for (Host h : hosts)
                arr.put(new JSONObject().put("name", h.name).put("fingerprint", h.fingerprint)
                        .put("addresses", new JSONArray(h.addresses)));
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
        prefs(c).edit().putString("hosts", arr.toString()).apply();
    }

    // --- pairing --------------------------------------------------------------------------------

    /** What a pairing code carries (remora-link://pair?h=…&fp=…&t=…&n=…). */
    static final class PairingCode {
        final List<String> addresses;
        final String fingerprint, token, hostName;
        PairingCode(List<String> addresses, String fingerprint, String token, String hostName) {
            this.addresses = addresses;
            this.fingerprint = fingerprint;
            this.token = token;
            this.hostName = hostName;
        }

        static PairingCode parse(String text) {
            Uri uri = Uri.parse(text.trim());
            if (!"remora-link".equals(uri.getScheme()) || !"pair".equals(uri.getHost())) return null;
            List<String> addresses = uri.getQueryParameters("h");
            String fp = uri.getQueryParameter("fp"), token = uri.getQueryParameter("t");
            if (addresses.isEmpty() || fp == null || fp.length() != 64 || token == null) return null;
            String name = uri.getQueryParameter("n");
            return new PairingCode(new ArrayList<>(addresses), fp.toLowerCase(), token,
                    name != null ? name : addresses.get(0));
        }
    }

    /** Pairs with the computer the code names and remembers it; returns its name. */
    static Host pair(Context c, PairingCode code, String deviceName) throws IOException {
        Host host = new Host(code.hostName, code.fingerprint, code.addresses);
        try (SSLSocket s = open(c, host)) {
            write(s, request(PAIR, code.token, deviceName));
            reply(s.getInputStream());
        }
        List<Host> hosts = hosts(c);
        hosts.removeIf(h -> h.fingerprint.equals(host.fingerprint));
        hosts.add(0, host);
        saveHosts(c, hosts);
        return host;
    }

    // --- sources and connecting ---------------------------------------------------------------

    static final class Source {
        final String id, name, kind;
        Source(String id, String name, String kind) {
            this.id = id;
            this.name = name;
            this.kind = kind;
        }
    }

    static List<Source> sources(Context c, Host host) throws IOException {
        List<Source> out = new ArrayList<>();
        try (SSLSocket s = open(c, host)) {
            write(s, request(SOURCES));
            JSONArray arr = new JSONArray(new String(reply(s.getInputStream()), StandardCharsets.UTF_8));
            for (int i = 0; i < arr.length(); i++) {
                JSONObject o = arr.getJSONObject(i);
                out.add(new Source(o.getString("id"), o.getString("name"), o.getString("kind")));
            }
        } catch (JSONException e) {
            throw new IOException("the computer sent a source list this app cannot read");
        }
        return out;
    }

    /** A connection that IS the source's after this returns: protocol v3 runs on it as is. */
    static SSLSocket connect(Context c, Host host, String sourceId) throws IOException {
        SSLSocket s = open(c, host);
        try {
            write(s, request(CONNECT, sourceId));
            reply(s.getInputStream());
            s.setTcpNoDelay(true);
            return s;
        } catch (IOException e) {
            s.close();
            throw e;
        }
    }

    // --- the wire -----------------------------------------------------------------------------

    private static byte[] request(int kind, String... fields) {
        int size = 7;
        byte[][] utf8 = new byte[fields.length][];
        for (int i = 0; i < fields.length; i++) {
            utf8[i] = fields[i].getBytes(StandardCharsets.UTF_8);
            size += 2 + utf8[i].length;
        }
        ByteBuffer b = ByteBuffer.allocate(size);
        b.put(new byte[] {'R', 'M', 'L', 'K'}).putShort((short) 1).put((byte) kind);
        for (byte[] f : utf8) b.putShort((short) f.length).put(f);
        return b.array();
    }

    /** u8 status u32 length payload: the payload, or the refusal's reason as an exception. */
    private static byte[] reply(InputStream in) throws IOException {
        DataInputStream d = new DataInputStream(in);
        int status = d.readUnsignedByte();
        int len = d.readInt();
        if (len < 0 || len > (8 << 20)) throw new IOException("implausible link reply");
        byte[] payload = new byte[len];
        d.readFully(payload);
        if (status != 0) throw new IOException(new String(payload, StandardCharsets.UTF_8));
        return payload;
    }

    private static void write(Socket s, byte[] bytes) throws IOException {
        OutputStream out = s.getOutputStream();
        out.write(bytes);
        out.flush();
    }

    /** TLS to the first of the host's addresses that answers, pinned to its fingerprint. */
    private static SSLSocket open(Context c, Host host) throws IOException {
        IOException last = null;
        for (String address : host.addresses) {
            int colon = address.lastIndexOf(':');
            String h = address.substring(0, colon).replace("[", "").replace("]", "");
            int port = Integer.parseInt(address.substring(colon + 1));
            Socket raw = new Socket();
            try {
                raw.connect(new InetSocketAddress(h, port), CONNECT_TIMEOUT_MS);
                SSLSocket s = (SSLSocket) context(host.fingerprint).getSocketFactory()
                        .createSocket(raw, h, port, true);
                s.setEnabledProtocols(new String[] {"TLSv1.3"});
                s.startHandshake();
                return s;
            } catch (IOException e) {
                raw.close();
                last = e;
            }
        }
        throw last != null ? last : new IOException("the computer has no address to try");
    }

    private static SSLContext context(String pinnedFingerprint) throws IOException {
        try {
            SSLContext ctx = SSLContext.getInstance("TLSv1.3");
            ctx.init(new javax.net.ssl.KeyManager[] {new IdentityKeyManager()},
                     new javax.net.ssl.TrustManager[] {new PinnedTrust(pinnedFingerprint)}, null);
            return ctx;
        } catch (Exception e) {
            throw new IOException("could not set up TLS: " + e.getMessage(), e);
        }
    }

    static String fingerprint(byte[] der) {
        try {
            byte[] h = MessageDigest.getInstance("SHA-256").digest(der);
            StringBuilder sb = new StringBuilder();
            for (byte b : h) sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    /** The computer is trusted for its pinned fingerprint and nothing else. */
    private static final class PinnedTrust implements X509TrustManager {
        private final String pinned;
        PinnedTrust(String pinned) { this.pinned = pinned; }

        @Override
        public void checkServerTrusted(X509Certificate[] chain, String authType)
                throws CertificateException {
            if (chain == null || chain.length == 0 || !pinned.equals(fingerprint(chain[0].getEncoded())))
                throw new CertificateException("this is not the computer this phone paired with");
        }

        @Override
        public void checkClientTrusted(X509Certificate[] chain, String authType)
                throws CertificateException {
            throw new CertificateException("the phone is never a TLS server");
        }

        @Override
        public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
    }

    // --- the phone's identity -----------------------------------------------------------------

    /** The phone's certificate fingerprint — what the computer pins for it. */
    static String phoneFingerprint() throws IOException {
        try {
            return fingerprint(certificate().getEncoded());
        } catch (Exception e) {
            throw new IOException("the phone's identity is unreadable: " + e.getMessage(), e);
        }
    }

    private static KeyStore keyStore() throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        if (!ks.containsAlias(KEY_ALIAS)) {
            // An EC key made inside the Keystore, with the self-signed certificate it issues for
            // it. TLS signs with it through the Keystore; DIGEST_NONE lets the TLS stack hand it
            // a digest it computed itself.
            Calendar end = Calendar.getInstance();
            end.add(Calendar.YEAR, 25);
            KeyPairGenerator g = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC,
                                                              "AndroidKeyStore");
            g.initialize(new KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_SIGN)
                    .setDigests(KeyProperties.DIGEST_NONE, KeyProperties.DIGEST_SHA256,
                                KeyProperties.DIGEST_SHA384, KeyProperties.DIGEST_SHA512)
                    .setCertificateSubject(new X500Principal("CN=Remora phone"))
                    .setCertificateSerialNumber(BigInteger.ONE)
                    .setCertificateNotAfter(end.getTime())
                    .build());
            g.generateKeyPair();
        }
        return ks;
    }

    private static X509Certificate certificate() throws Exception {
        return (X509Certificate) keyStore().getCertificate(KEY_ALIAS);
    }

    /** Presents the Keystore key and its certificate as the TLS client identity. */
    private static final class IdentityKeyManager extends X509ExtendedKeyManager {
        @Override
        public String chooseClientAlias(String[] keyType, Principal[] issuers, Socket socket) {
            return KEY_ALIAS;
        }

        @Override
        public X509Certificate[] getCertificateChain(String alias) {
            try {
                return new X509Certificate[] {certificate()};
            } catch (Exception e) {
                return null;
            }
        }

        @Override
        public PrivateKey getPrivateKey(String alias) {
            try {
                return (PrivateKey) keyStore().getKey(KEY_ALIAS, null);
            } catch (Exception e) {
                return null;
            }
        }

        @Override
        public String[] getClientAliases(String keyType, Principal[] issuers) {
            return new String[] {KEY_ALIAS};
        }

        @Override
        public String[] getServerAliases(String keyType, Principal[] issuers) { return null; }

        @Override
        public String chooseServerAlias(String keyType, Principal[] issuers, Socket socket) {
            return null;
        }
    }

    // --- one protocol v3 exchange through the link --------------------------------------------

    /** list-apps or app-icon on a fresh connection: the ok reply's blob. */
    static byte[] blob(Context c, Host host, String sourceId, int kind,
                       java.util.Map<String, String> options) throws IOException {
        try (SSLSocket s = connect(c, host, sourceId)) {
            InputStream in = s.getInputStream();
            OutputStream out = s.getOutputStream();
            Proto.hello(in, out);
            out.write(Proto.sessionRequest(kind, options));
            out.flush();
            return Proto.readBlobReply(in);
        }
    }
}
