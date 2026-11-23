package io.github.etheraura.remora.phone;

import java.awt.image.BufferedImage;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Random;
import java.util.Set;
import javax.imageio.ImageIO;

/**
 * The phone app's QR decoder against qrencode, which draws the code the computer shows
 * (phone/test/run.sh, bd remora-6a1k.4): every version at every level, Reed-Solomon at its
 * limit, the pairing code as a camera sees the workspace, and frames that hold no code.
 */
public final class QrDecoderTest {
    private static int failures = 0;

    public static void main(String[] args) throws Exception {
        everyVersionAndLevel();
        reedSolomon();
        cameraFrames();
        noCode();
        if (failures > 0) {
            System.out.println("FAIL: " + failures + " check(s)");
            System.exit(1);
        }
        System.out.println("PASS");
    }

    private static void check(boolean ok, String what) {
        if (ok) return;
        failures++;
        System.out.println("  FAIL " + what);
    }

    /** qrencode's code for {@code text}, one pixel a module and no margin. */
    private static boolean[][] qrencode(byte[] text, String... opts) throws IOException, InterruptedException {
        List<String> cmd = new ArrayList<>(Arrays.asList("qrencode", "-t", "PNG", "-s", "1", "-m", "0", "-o", "-"));
        cmd.addAll(Arrays.asList(opts));
        Process p = new ProcessBuilder(cmd).redirectError(ProcessBuilder.Redirect.INHERIT).start();
        try (OutputStream o = p.getOutputStream()) {
            o.write(text);
        }
        byte[] png = p.getInputStream().readAllBytes();
        if (p.waitFor() != 0) throw new IOException("qrencode " + String.join(" ", opts) + " failed");
        BufferedImage img = ImageIO.read(new ByteArrayInputStream(png));
        boolean[][] g = new boolean[img.getHeight()][img.getWidth()];
        for (int y = 0; y < g.length; y++)
            for (int x = 0; x < g.length; x++) g[y][x] = (img.getRGB(x, y) & 0xff) < 128;
        return g;
    }

    /**
     * Text of at most {@code maxBytes} UTF-8 bytes, in runs qrencode codes as different
     * segments: digits (numeric), capitals and space (alphanumeric), the rest (bytes).
     */
    private static String payload(Random rnd, int maxBytes) {
        StringBuilder sb = new StringBuilder();
        String[] runs = {"0123456789", "ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:", "abcxyz?&=[]_~#@!", "é—ü"};
        int bytes = 0;
        while (bytes < maxBytes) {
            String from = runs[rnd.nextInt(runs.length)];
            int len = 1 + rnd.nextInt(14);
            for (int i = 0; i < len; i++) {
                String ch = String.valueOf(from.charAt(rnd.nextInt(from.length())));
                int b = ch.getBytes(StandardCharsets.UTF_8).length;
                if (bytes + b > maxBytes) return sb.toString();
                sb.append(ch);
                bytes += b;
            }
        }
        return sb.toString();
    }

    private static void everyVersionAndLevel() throws Exception {
        System.out.println("every version, every level");
        Random rnd = new Random(1);
        Set<Integer> masks = new HashSet<>();
        for (int v = 1; v <= 40; v++) {
            for (int level = 0; level < 4; level++) {
                int total = QrDecoder.rawModules(v) / 8;
                int data = total - QrDecoder.EC_PER_BLOCK[level][v] * QrDecoder.BLOCKS[level][v];
                // A segment header is at most 20 bits, so this fills the version without overflowing it.
                String text = payload(rnd, 1 + rnd.nextInt(data - 3));
                if (text.isEmpty()) text = "7";
                List<String> opts = new ArrayList<>(Arrays.asList("-l", "LMQH".substring(level, level + 1), "-v", "" + v));
                if (rnd.nextInt(4) == 0) opts.add("-8");
                boolean[][] g = qrencode(text.getBytes(StandardCharsets.UTF_8), opts.toArray(new String[0]));
                String where = "v" + v + "-" + "LMQH".charAt(level);
                check(g.length == 4 * v + 17, where + ": qrencode made a " + g.length + "-module code");
                String got = QrDecoder.decodeGrid(g);
                check(text.equals(got), where + ": read " + (got == null ? "nothing" : "'" + got + "'")
                        + " for '" + text + "'");
                masks.add(QrDecoder.readFormat(g) & 7);
            }
        }
        // qrencode picks the mask, so more small codes until every one of the eight has come up.
        for (int extra = 0; masks.size() < 8 && extra < 400; extra++) {
            String text = payload(rnd, 1 + rnd.nextInt(14));
            if (text.isEmpty()) text = "7";
            boolean[][] g = qrencode(text.getBytes(StandardCharsets.UTF_8), "-l", "M");
            check(text.equals(QrDecoder.decodeGrid(g)), "extra code '" + text + "'");
            masks.add(QrDecoder.readFormat(g) & 7);
        }
        check(masks.size() == 8, "only masks " + masks + " came up");
        // The specification's codeword count, against the tables'.
        for (int v = 1; v <= 40; v++)
            check(QrDecoder.alignmentCentres(v).length == (v == 1 ? 0 : v / 7 + 2), "alignment count v" + v);
    }

    private static int[] rsEncode(int[] data, int ecLen) {
        int[] g = {1};
        int root = 1;
        for (int i = 0; i < ecLen; i++) {
            int[] ng = new int[g.length + 1];
            for (int j = 0; j < g.length; j++) {
                ng[j] ^= g[j];
                ng[j + 1] ^= QrDecoder.mul(g[j], root);
            }
            g = ng;
            root = QrDecoder.mul(root, 2);
        }
        int[] rem = new int[ecLen];
        for (int d : data) {
            int f = d ^ rem[0];
            for (int j = 0; j < ecLen - 1; j++) rem[j] = rem[j + 1] ^ QrDecoder.mul(f, g[j + 1]);
            rem[ecLen - 1] = QrDecoder.mul(f, g[ecLen]);
        }
        int[] cw = Arrays.copyOf(data, data.length + ecLen);
        System.arraycopy(rem, 0, cw, data.length, ecLen);
        return cw;
    }

    private static void reedSolomon() {
        System.out.println("Reed-Solomon up to its limit");
        Random rnd = new Random(2);
        for (int trial = 0; trial < 2000; trial++) {
            int ecLen = 7 + rnd.nextInt(24);
            int[] data = new int[1 + rnd.nextInt(150)];
            for (int i = 0; i < data.length; i++) data[i] = rnd.nextInt(256);
            int[] cw = rsEncode(data, ecLen);
            int[] bad = cw.clone();
            int errors = rnd.nextInt(ecLen / 2 + 1);
            Set<Integer> at = new HashSet<>();
            while (at.size() < errors) at.add(rnd.nextInt(cw.length));
            for (int p : at) bad[p] ^= 1 + rnd.nextInt(255);
            boolean ok = QrDecoder.rsCorrect(bad, ecLen);
            check(ok && Arrays.equals(bad, cw), "trial " + trial + ": " + errors + " errors in "
                    + cw.length + " codewords, " + ecLen + " for correction");
        }
    }

    /** The URI `remora link pair` makes, with this many addresses. */
    private static String pairingUri(Random rnd, int addresses) {
        StringBuilder sb = new StringBuilder("remora-link://pair?");
        String[] hosts = {"192.168.0.10:27200", "%5Bfd00::1a2b:3c4d:5e6f:7a8b%5D:27200", "10.8.0.2:27200",
                          "%5B2a01:4f8:c17:1234::1%5D:27200", "100.64.12.7:27200"};
        for (int i = 0; i < addresses; i++) sb.append("h=").append(hosts[i]).append('&');
        sb.append("fp=").append(hex(rnd, 64)).append("&t=").append(hex(rnd, 32)).append("&n=dev-host");
        return sb.toString();
    }

    private static String hex(Random rnd, int n) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; i++) sb.append("0123456789abcdef".charAt(rnd.nextInt(16)));
        return sb.toString();
    }

    /**
     * The pairing code as the phone's camera sees it on the workspace's Phone page: qrencode's
     * picture with its two-module white margin on a dark window, turned any way, at an angle,
     * out of focus, unevenly lit and noisy, among other things on the screen.
     */
    private static void cameraFrames() throws Exception {
        System.out.println("the pairing code through a camera");
        Random rnd = new Random(3);
        int frames = 0, read = 0, wrong = 0;
        // By module size in pixels: under 3.5, 3.5 to 5, 5 to 7, 7 and over.
        int[] seenBy = new int[4], readBy = new int[4];
        long nanos = 0;
        for (int addresses = 1; addresses <= 5; addresses += 2) {
            for (int trial = 0; trial < 100; trial++) {
                String uri = pairingUri(rnd, addresses);
                // The workspace and `remora link pair` draw level M; L is qrencode's own default.
                boolean[][] g = qrencode(uri.getBytes(StandardCharsets.UTF_8), "-l", trial % 2 == 0 ? "M" : "L");
                boolean big = rnd.nextInt(3) == 0;
                int w = big ? 1920 : 1280, h = big ? 1080 : 720;
                int stride = w + (rnd.nextBoolean() ? 0 : 64);
                double module = Math.min(2.6 + rnd.nextDouble() * 7, 0.62 * h / (g.length + 4));
                byte[] lum = render(rnd, g, module, w, h, stride);
                long t0 = System.nanoTime();
                String got = QrDecoder.decodeOrThrow(lum, w, h, stride);
                nanos += System.nanoTime() - t0;
                int bucket = module < 3.5 ? 0 : module < 5 ? 1 : module < 7 ? 2 : 3;
                frames++;
                seenBy[bucket]++;
                if (got == null) continue;
                if (got.equals(uri)) {
                    read++;
                    readBy[bucket]++;
                } else {
                    wrong++;
                }
            }
        }
        System.out.printf("  read %d of %d frames, %.1f ms a frame; by module size <3.5 px %d/%d, "
                + "3.5-5 %d/%d, 5-7 %d/%d, 7+ %d/%d%n", read, frames, nanos / 1e6 / frames,
                readBy[0], seenBy[0], readBy[1], seenBy[1], readBy[2], seenBy[2], readBy[3], seenBy[3]);
        check(wrong == 0, wrong + " frame(s) read as the wrong text");
        // The Phone page draws 8 px a module; from a phone a forearm away that is 8-10 px in a
        // 1080p frame. Under 3.5 px, blurred, the code spans a fifth of the frame and the next
        // frame, nearer, is the one that reads.
        int clear = seenBy[1] + seenBy[2] + seenBy[3], clearRead = readBy[1] + readBy[2] + readBy[3];
        check(clearRead >= clear * 98 / 100, "read " + clearRead + " of " + clear + " frames at 3.5 px a module and over");
        check(readBy[0] * 2 >= seenBy[0], "read " + readBy[0] + " of " + seenBy[0] + " frames under 3.5 px a module");
    }

    private static byte[] render(Random rnd, boolean[][] g, double module, int w, int h, int stride) {
        int dim = g.length, side = dim + 4;  // two modules of white margin, as the Phone page draws it
        double half = side * module / 2, reach = half * Math.sqrt(2) * 1.15;
        double cx = reach + rnd.nextDouble() * Math.max(1, w - 2 * reach);
        double cy = reach + rnd.nextDouble() * Math.max(1, h - 2 * reach);
        // The screen tilted up to 35 degrees about any axis in its plane, seen through a pinhole
        // 2.5 to 5.5 code widths away, then turned any way in the frame.
        double th = rnd.nextDouble() * 2 * Math.PI, cos = Math.cos(th), sin = Math.sin(th);
        double phi = Math.toRadians(rnd.nextDouble() * 35), axis = rnd.nextDouble() * 2 * Math.PI;
        double kx = Math.cos(axis), ky = Math.sin(axis), cp = Math.cos(phi), sp = Math.sin(phi);
        double size = side * module, dist = size * (2.5 + rnd.nextDouble() * 3);
        double[] src = {0, 0, side, 0, side, side, 0, side}, dst = new double[8];
        for (int i = 0; i < 4; i++) {
            double u = (src[2 * i] - side / 2.0) * module, v = (src[2 * i + 1] - side / 2.0) * module;
            double dot = kx * u + ky * v;  // Rodrigues, about (kx, ky, 0)
            double x = u * cp + kx * dot * (1 - cp), y = v * cp + ky * dot * (1 - cp), z = (kx * v - ky * u) * sp;
            x = dist * x / (dist + z);
            y = dist * y / (dist + z);
            dst[2 * i] = cx + x * cos - y * sin;
            dst[2 * i + 1] = cy + x * sin + y * cos;
        }
        double[] inv = QrDecoder.homography(dst, src);  // frame pixel -> module
        int white = 195 + rnd.nextInt(50), black = 15 + rnd.nextInt(45), window = 40 + rnd.nextInt(15);
        double[] img = new double[w * h];
        Arrays.fill(img, window);
        // Other things on the screen: text-sized light marks.
        for (int k = 0; k < 60; k++) {
            int rw = 3 + rnd.nextInt(40), rh = 3 + rnd.nextInt(14);
            int rx = rnd.nextInt(w - rw), ry = rnd.nextInt(h - rh), c = 120 + rnd.nextInt(120);
            for (int y = ry; y < ry + rh; y++) Arrays.fill(img, y * w + rx, y * w + rx + rw, c);
        }
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                double sum = 0;
                int in = 0;
                for (int s = 0; s < 4; s++) {
                    double px = x + 0.25 + 0.5 * (s & 1), py = y + 0.25 + 0.5 * (s >> 1);
                    double q = inv[6] * px + inv[7] * py + 1;
                    double u = (inv[0] * px + inv[1] * py + inv[2]) / q, v = (inv[3] * px + inv[4] * py + inv[5]) / q;
                    if (u < 0 || v < 0 || u >= side || v >= side) continue;
                    int mu = (int) u - 2, mv = (int) v - 2;
                    boolean dark = mu >= 0 && mv >= 0 && mu < dim && mv < dim && g[mv][mu];
                    sum += dark ? black : white;
                    in++;
                }
                if (in > 0) img[y * w + x] = (sum + (4 - in) * img[y * w + x]) / 4;
            }
        }
        for (int pass = rnd.nextInt(3); pass > 0; pass--) img = boxBlur(img, w, h);
        double gx = rnd.nextDouble() * 2 - 1, gy = rnd.nextDouble() * 2 - 1, noise = 3 + rnd.nextDouble() * 7;
        byte[] lum = new byte[stride * h];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                double light = 0.7 + 0.3 * (0.5 + 0.5 * (gx * (x / (double) w - 0.5) + gy * (y / (double) h - 0.5)));
                double v = img[y * w + x] * light + rnd.nextGaussian() * noise;
                lum[y * stride + x] = (byte) Math.max(0, Math.min(255, (int) Math.round(v)));
            }
        }
        return lum;
    }

    private static double[] boxBlur(double[] img, int w, int h) {
        double[] out = new double[img.length];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                double s = 0;
                int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        s += img[yy * w + xx];
                        n++;
                    }
                out[y * w + x] = s / n;
            }
        return out;
    }

    /**
     * Frames with no code in them, finder-like squares included: nothing to read, and nothing
     * thrown, since this is most of what a camera sees.
     */
    private static void noCode() {
        System.out.println("frames that hold no code");
        Random rnd = new Random(4);
        int found = 0;
        for (int trial = 0; trial < 300; trial++) {
            int w = 320 + rnd.nextInt(960), h = 240 + rnd.nextInt(480);
            byte[] lum = new byte[w * h];
            for (int i = 0; i < lum.length; i++) lum[i] = (byte) (trial % 3 == 0 ? rnd.nextInt(256) : 128);
            int marks = rnd.nextInt(8);
            for (int k = 0; k < 40; k++) {
                int rw = 1 + rnd.nextInt(60), rh = 1 + rnd.nextInt(60);
                int rx = rnd.nextInt(w - rw), ry = rnd.nextInt(h - rh);
                byte c = (byte) rnd.nextInt(256);
                for (int y = ry; y < ry + rh; y++) Arrays.fill(lum, y * w + rx, y * w + rx + rw, c);
            }
            // Three finders where a code's corners would be, with noise for modules between.
            int m = 2 + rnd.nextInt(6), dim = 21 + 4 * rnd.nextInt(10);
            int ox = rnd.nextInt(Math.max(1, w - dim * m)), oy = rnd.nextInt(Math.max(1, h - dim * m));
            for (int my = 0; my < dim && oy + my * m + m <= h; my++)
                for (int mx = 0; mx < dim && ox + mx * m + m <= w; mx++) {
                    boolean dark = finderModule(mx, my, dim) != null ? finderModule(mx, my, dim) : rnd.nextBoolean();
                    for (int y = 0; y < m; y++)
                        Arrays.fill(lum, (oy + my * m + y) * w + ox + mx * m, (oy + my * m + y) * w + ox + mx * m + m,
                                (byte) (dark ? 20 : 230));
                }
            for (int k = 0; k < marks; k++) {
                int fm = 2 + rnd.nextInt(6), fx = rnd.nextInt(Math.max(1, w - 7 * fm)), fy = rnd.nextInt(Math.max(1, h - 7 * fm));
                for (int my = 0; my < 7; my++)
                    for (int mx = 0; mx < 7 && fy + my * fm + fm <= h && fx + mx * fm + fm <= w; mx++)
                        for (int y = 0; y < fm; y++)
                            Arrays.fill(lum, (fy + my * fm + y) * w + fx + mx * fm, (fy + my * fm + y) * w + fx + mx * fm + fm,
                                    (byte) (finderModule(mx, my, 7) ? 20 : 230));
            }
            try {
                if (QrDecoder.decodeOrThrow(lum, w, h, w) != null) found++;
            } catch (RuntimeException e) {
                failures++;
                System.out.println("  FAIL frame " + trial + " threw:");
                e.printStackTrace(System.out);
            }
        }
        check(found == 0, found + " frame(s) without a code read as one");
    }

    /** Whether module (x, y) of a code this size is dark because a finder is there; null if none is. */
    private static Boolean finderModule(int x, int y, int dim) {
        int[][] corners = {{0, 0}, {dim - 7, 0}, {0, dim - 7}};
        for (int[] c : corners) {
            int fx = x - c[0], fy = y - c[1];
            if (fx < 0 || fy < 0 || fx > 6 || fy > 6) continue;
            int ring = Math.min(Math.min(fx, fy), Math.min(6 - fx, 6 - fy));
            return ring != 1;
        }
        return null;
    }
}
