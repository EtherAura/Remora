package io.github.etheraura.remora.phone;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Reads a QR code out of a camera frame: the pairing code the computer shows. The framework has
 * no decoder and the app carries no libraries, so this is the app's own, written to ISO/IEC 18004
 * and finding the code the way scanners do: the three finder squares, the alignment square
 * near the fourth corner, a perspective map from modules to pixels, then format, codewords,
 * Reed-Solomon and the bit stream. It uses no Android classes, so phone/test runs it on the
 * computer against qrencode's codes, for every version and error-correction level.
 */
final class QrDecoder {
    private QrDecoder() {}

    /**
     * The text of a QR code in this frame, or null when it holds none that reads. {@code lum} is
     * 8-bit luminance, {@code stride} bytes a row: a camera frame's Y plane as it comes.
     */
    static String decode(byte[] lum, int width, int height, int stride) {
        try {
            return decodeOrThrow(lum, width, height, stride);
        } catch (RuntimeException e) {
            // A camera frame is mostly not a QR code. A frame this code cannot make sense of
            // must cost that frame, never the camera thread. The tests call decodeOrThrow.
            return null;
        }
    }

    static String decodeOrThrow(byte[] lum, int width, int height, int stride) {
        Bits img = binarize(lum, width, height, stride);
        if (img == null) return null;
        for (Mark[] three : pickFinders(findFinders(img))) {
            String text;
            try {
                text = read(img, three[0], three[1], three[2]);
            } catch (Unreadable e) {
                text = null;
            }
            if (text != null) return text;
        }
        return null;
    }

    /** What a malformed code throws inside the decoder: this frame does not read. */
    private static final class Unreadable extends RuntimeException {
        Unreadable() {
            super(null, null, false, false);
        }
    }

    // --- binarizing ------------------------------------------------------------------------------

    /** A black-and-white image, one byte a pixel (1 = dark). */
    static final class Bits {
        final int w, h;
        final byte[] b;

        Bits(int w, int h, byte[] b) {
            this.w = w;
            this.h = h;
            this.b = b;
        }

        boolean get(int x, int y) {
            return b[y * w + x] != 0;
        }
    }

    private static final int BLOCK = 8;

    /**
     * Each pixel against the local level around it, not one level for the frame: a screen seen
     * by a camera is brighter in the middle than at its edges. The level is each 8x8 block's
     * mean, averaged over the 5x5 blocks around it; a flat block takes its neighbours' level.
     */
    static Bits binarize(byte[] lum, int w, int h, int stride) {
        int bw = (w + BLOCK - 1) / BLOCK, bh = (h + BLOCK - 1) / BLOCK;
        if (bw < 5 || bh < 5) return null;
        int[] level = new int[bw * bh];
        for (int by = 0; by < bh; by++) {
            int y0 = Math.min(by * BLOCK, h - BLOCK);
            for (int bx = 0; bx < bw; bx++) {
                int x0 = Math.min(bx * BLOCK, w - BLOCK);
                int sum = 0, min = 255, max = 0;
                for (int y = 0; y < BLOCK; y++) {
                    int o = (y0 + y) * stride + x0;
                    for (int x = 0; x < BLOCK; x++) {
                        int v = lum[o + x] & 0xff;
                        sum += v;
                        if (v < min) min = v;
                        if (v > max) max = v;
                    }
                }
                int avg;
                if (max - min > 24) {
                    avg = sum / (BLOCK * BLOCK);
                } else {
                    // All light or all dark. Light, unless the blocks above and to the left say
                    // this is the inside of something dark.
                    avg = min / 2;
                    if (by > 0 && bx > 0) {
                        int around = (level[(by - 1) * bw + bx] + 2 * level[by * bw + bx - 1]
                                + level[(by - 1) * bw + bx - 1]) / 4;
                        if (min < around) avg = around;
                    }
                }
                level[by * bw + bx] = avg;
            }
        }
        byte[] out = new byte[w * h];
        for (int by = 0; by < bh; by++) {
            int y0 = Math.min(by * BLOCK, h - BLOCK);
            int cy = Math.max(2, Math.min(by, bh - 3));
            for (int bx = 0; bx < bw; bx++) {
                int x0 = Math.min(bx * BLOCK, w - BLOCK);
                int cx = Math.max(2, Math.min(bx, bw - 3));
                int sum = 0;
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++) sum += level[(cy + dy) * bw + cx + dx];
                int t = sum / 25;
                for (int y = 0; y < BLOCK; y++) {
                    int o = (y0 + y) * stride + x0, q = (y0 + y) * w + x0;
                    for (int x = 0; x < BLOCK; x++) out[q + x] = (byte) ((lum[o + x] & 0xff) <= t ? 1 : 0);
                }
            }
        }
        return new Bits(w, h, out);
    }

    // --- finding the code ------------------------------------------------------------------------

    /** A finder or alignment square's centre, its module size, and how many scans found it. */
    static final class Mark {
        float x, y, size;
        int count = 1;

        Mark(float x, float y, float size) {
            this.x = x;
            this.y = y;
            this.size = size;
        }

        boolean near(float px, float py, float s) {
            if (Math.abs(py - y) > s || Math.abs(px - x) > s) return false;
            float d = Math.abs(s - size);
            return d <= 1f || d <= size;
        }

        void add(float px, float py, float s) {
            x = (count * x + px) / (count + 1);
            y = (count * y + py) / (count + 1);
            size = (count * size + s) / (count + 1);
            count++;
        }
    }

    /** Every row's dark-light-dark-light-dark runs in the ratio 1:1:3:1:1, checked across. */
    static List<Mark> findFinders(Bits img) {
        List<Mark> found = new ArrayList<>();
        int[] sc = new int[5];
        for (int y = 0; y < img.h; y++) {
            Arrays.fill(sc, 0);
            int state = 0, row = y * img.w;
            for (int x = 0; x < img.w; x++) {
                if (img.b[row + x] != 0) {
                    if ((state & 1) == 1) state++;
                    sc[state]++;
                } else if ((state & 1) == 0) {
                    if (state == 4) {
                        if (finderRatio(sc) && finderAt(img, found, sc, y, x)) {
                            state = 0;
                            Arrays.fill(sc, 0);
                        } else {
                            sc[0] = sc[2];
                            sc[1] = sc[3];
                            sc[2] = sc[4];
                            sc[3] = 1;
                            sc[4] = 0;
                            state = 3;
                        }
                    } else {
                        sc[++state]++;
                    }
                } else {
                    sc[state]++;
                }
            }
            if (state == 4 && finderRatio(sc)) finderAt(img, found, sc, y, img.w);
        }
        return found;
    }

    private static boolean finderRatio(int[] sc) {
        int total = 0;
        for (int c : sc) {
            if (c == 0) return false;
            total += c;
        }
        if (total < 7) return false;
        float m = total / 7f, v = m / 2f;
        return Math.abs(m - sc[0]) < v && Math.abs(m - sc[1]) < v && Math.abs(3 * m - sc[2]) < 3 * v
                && Math.abs(m - sc[3]) < v && Math.abs(m - sc[4]) < v;
    }

    private static boolean finderAt(Bits img, List<Mark> found, int[] sc, int y, int end) {
        int total = sc[0] + sc[1] + sc[2] + sc[3] + sc[4];
        float cx = end - sc[4] - sc[3] - sc[2] / 2f;
        float cy = crossCheck(img, (int) cx, y, true, sc[2], total, 0.4f);
        if (Float.isNaN(cy)) return false;
        cx = crossCheck(img, (int) cx, (int) cy, false, sc[2], total, 0.2f);
        if (Float.isNaN(cx)) return false;
        float size = total / 7f;
        for (Mark m : found) {
            if (m.near(cx, cy, size)) {
                m.add(cx, cy, size);
                return true;
            }
        }
        found.add(new Mark(cx, cy, size));
        return true;
    }

    /**
     * The 1:1:3:1:1 runs again, along the other axis through (x, y): where their centre falls on
     * that axis, or NaN when they are not there or their length differs from the first by more
     * than {@code maxDiff}.
     */
    private static float crossCheck(Bits img, int x, int y, boolean vertical, int maxCount,
                                    int firstTotal, float maxDiff) {
        int[] sc = new int[5];
        int limit = vertical ? img.h : img.w, start = vertical ? y : x;
        int i = start;
        while (i >= 0 && dark(img, x, y, vertical, i)) { sc[2]++; i--; }
        if (i < 0) return Float.NaN;
        while (i >= 0 && !dark(img, x, y, vertical, i) && sc[1] <= maxCount) { sc[1]++; i--; }
        if (i < 0 || sc[1] > maxCount) return Float.NaN;
        while (i >= 0 && dark(img, x, y, vertical, i) && sc[0] <= maxCount) { sc[0]++; i--; }
        if (sc[0] > maxCount) return Float.NaN;
        i = start + 1;
        while (i < limit && dark(img, x, y, vertical, i)) { sc[2]++; i++; }
        if (i == limit) return Float.NaN;
        while (i < limit && !dark(img, x, y, vertical, i) && sc[3] < maxCount) { sc[3]++; i++; }
        if (i == limit || sc[3] >= maxCount) return Float.NaN;
        while (i < limit && dark(img, x, y, vertical, i) && sc[4] < maxCount) { sc[4]++; i++; }
        if (sc[4] >= maxCount) return Float.NaN;
        int total = sc[0] + sc[1] + sc[2] + sc[3] + sc[4];
        if (Math.abs(total - firstTotal) >= maxDiff * firstTotal) return Float.NaN;
        return finderRatio(sc) ? i - sc[4] - sc[3] - sc[2] / 2f : Float.NaN;
    }

    private static boolean dark(Bits img, int x, int y, boolean vertical, int i) {
        return vertical ? img.get(x, i) : img.get(i, y);
    }

    /**
     * The triples of finders that best make three corners of a square, best first, each as
     * {bottom left, top left, top right}: two equal sides at a right angle, module sizes alike.
     * A finder must have been crossed on three rows; a speck of text crossed on two is not one.
     */
    static List<Mark[]> pickFinders(List<Mark> all) {
        List<Mark[]> picks = new ArrayList<>();
        List<Mark> c = new ArrayList<>();
        for (Mark m : all)
            if (m.count >= 3) c.add(m);
        if (c.size() < 3) return picks;
        c.sort((a, b) -> b.count - a.count);
        if (c.size() > 12) c = c.subList(0, 12);
        List<double[]> scored = new ArrayList<>();  // {distortion, i, j, k}
        for (int i = 0; i < c.size(); i++) {
            for (int j = i + 1; j < c.size(); j++) {
                for (int k = j + 1; k < c.size(); k++) {
                    Mark a = c.get(i), b = c.get(j), d = c.get(k);
                    float lo = Math.min(a.size, Math.min(b.size, d.size));
                    float hi = Math.max(a.size, Math.max(b.size, d.size));
                    // Loose: a finder's size is read across rows, and a turned square's width
                    // varies from row to row. The geometry and the decode itself do the rest.
                    if (hi > lo * 2f) continue;
                    double[] s = {dist2(a, b), dist2(b, d), dist2(a, d)};
                    Arrays.sort(s);
                    // The short sides span 14 modules at the least (version 1).
                    float mod = (a.size + b.size + d.size) / 3f;
                    if (s[0] < 100.0 * mod * mod) continue;
                    double distortion = (Math.abs(s[2] - 2 * s[1]) + Math.abs(s[2] - 2 * s[0])) / s[2];
                    if (distortion < 1.0) scored.add(new double[] {distortion, i, j, k});
                }
            }
        }
        scored.sort((x, y) -> Double.compare(x[0], y[0]));
        for (int n = 0; n < scored.size() && n < 3; n++) {
            double[] t = scored.get(n);
            picks.add(corners(c.get((int) t[1]), c.get((int) t[2]), c.get((int) t[3])));
        }
        return picks;
    }

    private static Mark[] corners(Mark p0, Mark p1, Mark p2) {
        // The top left is the corner opposite the longest side.
        Mark tl, bl, tr;
        double d01 = dist2(p0, p1), d12 = dist2(p1, p2), d02 = dist2(p0, p2);
        if (d12 >= d01 && d12 >= d02) { tl = p0; bl = p1; tr = p2; }
        else if (d02 >= d01) { tl = p1; bl = p0; tr = p2; }
        else { tl = p2; bl = p0; tr = p1; }
        // Turning from the top right to the bottom left about the top left is clockwise on a
        // screen (y grows downwards); the other way round, the two were swapped.
        if ((tr.x - tl.x) * (bl.y - tl.y) - (tr.y - tl.y) * (bl.x - tl.x) < 0) {
            Mark t = bl;
            bl = tr;
            tr = t;
        }
        return new Mark[] {bl, tl, tr};
    }

    private static double dist2(Mark a, Mark b) {
        double dx = a.x - b.x, dy = a.y - b.y;
        return dx * dx + dy * dy;
    }

    private static float dist(float ax, float ay, float bx, float by) {
        return (float) Math.hypot(ax - bx, ay - by);
    }

    /**
     * Samples the grid the three finders frame and decodes it. How many modules a side has is
     * an estimate, which an angled code can miss by a version, so the sizes nearest it are tried
     * in turn: a wrong one does not read, since format and Reed-Solomon check every reading.
     */
    private static String read(Bits img, Mark bl, Mark tl, Mark tr) {
        // The module size along the code's own axes, which a finder measured across a rotated
        // code overstates, and along each side, which perspective makes differ.
        float acrossModule = moduleSize(img, tl, tr), downModule = moduleSize(img, tl, bl);
        if (!(acrossModule >= 1f) || !(downModule >= 1f)) return null;
        float est = (dist(tl.x, tl.y, tr.x, tr.y) / acrossModule
                + dist(tl.x, tl.y, bl.x, bl.y) / downModule) / 2 + 7;
        List<Integer> dims = new ArrayList<>();
        for (int v = 1; v <= 40; v++) {
            int d = 4 * v + 17;
            if (Math.abs(d - est) <= Math.max(4.5f, 0.12f * est)) dims.add(d);
        }
        dims.sort((a, b) -> Float.compare(Math.abs(a - est), Math.abs(b - est)));
        for (int d : dims) {
            Mark align = d >= 25 ? findAlignment(img, bl, tl, tr, d) : null;
            // Without the alignment square the fourth corner is where a parallelogram puts it:
            // the fallback when the square was not found, or what was found was not it.
            for (int pass = align != null ? 0 : 1; pass < 2; pass++) {
                boolean[][] grid = sample(img, bl, tl, tr, pass == 0 ? align : null, d);
                String text = grid != null ? decodeGrid(grid) : null;
                if (text != null) return text;
            }
        }
        return null;
    }

    private static float moduleSize(Bits img, Mark a, Mark b) {
        float e1 = runBothWays(img, (int) a.x, (int) a.y, (int) b.x, (int) b.y);
        float e2 = runBothWays(img, (int) b.x, (int) b.y, (int) a.x, (int) a.y);
        if (Float.isNaN(e1)) return e2 / 7f;
        if (Float.isNaN(e2)) return e1 / 7f;
        return (e1 + e2) / 14f;
    }

    /** A finder's width through its centre towards (toX, toY), and the other way. */
    private static float runBothWays(Bits img, int fromX, int fromY, int toX, int toY) {
        float result = run(img, fromX, fromY, toX, toY);
        float scale = 1f;
        int otherX = fromX - (toX - fromX);
        if (otherX < 0) {
            scale = fromX / (float) (fromX - otherX);
            otherX = 0;
        } else if (otherX >= img.w) {
            scale = (img.w - 1 - fromX) / (float) (otherX - fromX);
            otherX = img.w - 1;
        }
        int otherY = (int) (fromY - (toY - fromY) * scale);
        scale = 1f;
        if (otherY < 0) {
            scale = fromY / (float) (fromY - otherY);
            otherY = 0;
        } else if (otherY >= img.h) {
            scale = (img.h - 1 - fromY) / (float) (otherY - fromY);
            otherY = img.h - 1;
        }
        otherX = (int) (fromX + (otherX - fromX) * scale);
        result += run(img, fromX, fromY, otherX, otherY);
        return result - 1f;
    }

    /** From a finder's centre along a line: the distance to the end of dark, light, dark. */
    private static float run(Bits img, int fromX, int fromY, int toX, int toY) {
        boolean steep = Math.abs(toY - fromY) > Math.abs(toX - fromX);
        if (steep) {
            int t = fromX; fromX = fromY; fromY = t;
            t = toX; toX = toY; toY = t;
        }
        int dx = Math.abs(toX - fromX), dy = Math.abs(toY - fromY);
        int error = -dx / 2;
        int xstep = fromX < toX ? 1 : -1, ystep = fromY < toY ? 1 : -1;
        int state = 0;
        int xLimit = toX + xstep;
        for (int x = fromX, y = fromY; x != xLimit; x += xstep) {
            int rx = steep ? y : x, ry = steep ? x : y;
            if (rx < 0 || ry < 0 || rx >= img.w || ry >= img.h) break;
            if ((state == 1) == img.get(rx, ry)) {
                if (state == 2) return dist(x, y, fromX, fromY);
                state++;
            }
            error += dy;
            if (error > 0) {
                if (y == toY) break;
                y += ystep;
                error -= dx;
            }
        }
        // Dark, light, dark to the edge of the image: count the edge as the light after it.
        if (state == 2) return dist(toX + xstep, toY, fromX, fromY);
        return Float.NaN;
    }

    // Where each module is read: its centre and a quarter module either way along each axis.
    private static final double[] VOTE_U = {0, -0.25, 0.25, 0, 0}, VOTE_V = {0, 0, 0, -0.25, 0.25};

    /**
     * Reads the grid through the projective map that takes the finders' centres, and the
     * alignment square's (or, without one, the parallelogram's fourth corner), to their pixels.
     * Each module is the majority of five readings, which a noisy or blurred frame needs.
     */
    private static boolean[][] sample(Bits img, Mark bl, Mark tl, Mark tr, Mark align, int dim) {
        double far = dim - 3.5;
        double[] src = align != null
                ? new double[] {3.5, 3.5, far, 3.5, far - 3, far - 3, 3.5, far}
                : new double[] {3.5, 3.5, far, 3.5, far, far, 3.5, far};
        double[] dst = align != null
                ? new double[] {tl.x, tl.y, tr.x, tr.y, align.x, align.y, bl.x, bl.y}
                : new double[] {tl.x, tl.y, tr.x, tr.y, tr.x - tl.x + bl.x, tr.y - tl.y + bl.y, bl.x, bl.y};
        double[] h = homography(src, dst);
        if (h == null) return null;
        boolean[][] grid = new boolean[dim][dim];
        for (int r = 0; r < dim; r++) {
            for (int c = 0; c < dim; c++) {
                int votes = 0;
                for (int s = 0; s < VOTE_U.length; s++) {
                    double u = c + 0.5 + VOTE_U[s], v = r + 0.5 + VOTE_V[s];
                    double w = h[6] * u + h[7] * v + 1;
                    int px = (int) Math.floor((h[0] * u + h[1] * v + h[2]) / w);
                    int py = (int) Math.floor((h[3] * u + h[4] * v + h[5]) / w);
                    // A module a pixel past the edge is the edge's; further out, the code is cut off.
                    if (px < -1 || py < -1 || px > img.w || py > img.h) return null;
                    px = Math.max(0, Math.min(img.w - 1, px));
                    py = Math.max(0, Math.min(img.h - 1, py));
                    if (img.get(px, py)) votes++;
                }
                grid[r][c] = votes * 2 > VOTE_U.length;
            }
        }
        return grid;
    }

    /**
     * The bottom-right alignment square of a {@code dim}-module code, or null. Searched for as
     * itself, 5x5 modules of dark ring, light ring and dark centre, within ten modules of where
     * the finders put it: a lone dark module in the data looks like its middle row and column,
     * not like all of it. Nearest wins among equal matches.
     */
    private static Mark findAlignment(Bits img, Mark bl, Mark tl, Mark tr, int dim) {
        float n = dim - 7, corr = 1f - 3f / n;
        float ex = tl.x + corr * (tr.x - tl.x + bl.x - tl.x), ey = tl.y + corr * (tr.y - tl.y + bl.y - tl.y);
        // One module across and one down, scaled for the corner's distance from the camera:
        // perspective makes the far corner's modules as much smaller as its finders are.
        float k = Math.max(0.7f, Math.min(1.4f, (tr.size + bl.size) / (2 * tl.size)));
        float ax = k * (tr.x - tl.x) / n, ay = k * (tr.y - tl.y) / n;
        float bx = k * (bl.x - tl.x) / n, by = k * (bl.y - tl.y) / n;
        float module = (float) Math.sqrt(Math.abs(ax * by - ay * bx));
        if (!(module >= 1f)) return null;
        float reach = 10 * module, step = Math.max(1f, module / 4);
        int best = 0;
        float bestX = 0, bestY = 0, bestD = Float.MAX_VALUE;
        for (float oy = -reach; oy <= reach; oy += step) {
            for (float ox = -reach; ox <= reach; ox += step) {
                float cx = ex + ox, cy = ey + oy;
                int score = 0;
                for (int j = -2; j <= 2 && score >= 0; j++) {
                    for (int i = -2; i <= 2; i++) {
                        int px = (int) Math.floor(cx + i * ax + j * bx), py = (int) Math.floor(cy + i * ay + j * by);
                        if (px < 0 || py < 0 || px >= img.w || py >= img.h) {
                            score = -1;
                            break;
                        }
                        boolean ring = Math.max(Math.abs(i), Math.abs(j)) == 1;
                        if (img.get(px, py) != ring) score++;
                    }
                }
                float d = ox * ox + oy * oy;
                if (score > best || (score == best && d < bestD)) {
                    best = score;
                    bestX = cx;
                    bestY = cy;
                    bestD = d;
                }
            }
        }
        if (best < 20) return null;
        // The centre of the best matches about the one chosen: a module's worth of them.
        float sx = 0, sy = 0;
        int count = 0;
        for (float oy = -module / 2; oy <= module / 2; oy += step / 2) {
            for (float ox = -module / 2; ox <= module / 2; ox += step / 2) {
                float cx = bestX + ox, cy = bestY + oy;
                int score = 0;
                for (int j = -2; j <= 2; j++)
                    for (int i = -2; i <= 2; i++) {
                        int px = (int) Math.floor(cx + i * ax + j * bx), py = (int) Math.floor(cy + i * ay + j * by);
                        if (px < 0 || py < 0 || px >= img.w || py >= img.h) continue;
                        boolean ring = Math.max(Math.abs(i), Math.abs(j)) == 1;
                        if (img.get(px, py) != ring) score++;
                    }
                if (score >= best) {
                    sx += cx;
                    sy += cy;
                    count++;
                }
            }
        }
        return count == 0 ? new Mark(bestX, bestY, module) : new Mark(sx / count, sy / count, module);
    }

    /**
     * The projective map taking the four source points to the four destination points, as
     * {h0..h7} with h8 = 1; null when the points are degenerate.
     */
    static double[] homography(double[] src, double[] dst) {
        double[][] a = new double[8][9];
        for (int i = 0; i < 4; i++) {
            double u = src[2 * i], v = src[2 * i + 1], x = dst[2 * i], y = dst[2 * i + 1];
            a[2 * i] = new double[] {u, v, 1, 0, 0, 0, -u * x, -v * x, x};
            a[2 * i + 1] = new double[] {0, 0, 0, u, v, 1, -u * y, -v * y, y};
        }
        for (int col = 0; col < 8; col++) {
            int pivot = col;
            for (int r = col + 1; r < 8; r++)
                if (Math.abs(a[r][col]) > Math.abs(a[pivot][col])) pivot = r;
            if (Math.abs(a[pivot][col]) < 1e-12) return null;
            double[] t = a[col]; a[col] = a[pivot]; a[pivot] = t;
            for (int r = 0; r < 8; r++) {
                if (r == col) continue;
                double f = a[r][col] / a[col][col];
                if (f == 0) continue;
                for (int k = col; k < 9; k++) a[r][k] -= f * a[col][k];
            }
        }
        double[] h = new double[8];
        for (int i = 0; i < 8; i++) h[i] = a[i][8] / a[i][i];
        return h;
    }

    // --- decoding the grid -----------------------------------------------------------------------

    /** The text a module grid ({@code grid[row][col]}, true = dark) holds, or null. */
    static String decodeGrid(boolean[][] grid) {
        int dim = grid.length;
        if (dim < 21 || dim > 177 || (dim - 17) % 4 != 0) return null;
        int version = (dim - 17) / 4;
        int format = readFormat(grid);
        if (format < 0) return null;
        if (version >= 7 && readVersion(grid) != version) return null;
        int level = LEVEL_FROM_BITS[format >> 3], mask = format & 7;
        try {
            byte[] raw = codewords(grid, version, mask);
            byte[] data = correct(raw, version, level);
            return data == null ? null : text(data, version);
        } catch (Unreadable e) {
            return null;
        }
    }

    /** Error-correction level, as the tables index it (L, M, Q, H), from its two format bits. */
    private static final int[] LEVEL_FROM_BITS = {1, 0, 3, 2};

    private static final int[] FORMATS = new int[32];
    private static final int[] VERSIONS = new int[41];

    static {
        for (int d = 0; d < 32; d++) FORMATS[d] = ((d << 10) | bchRemainder(d, 0x537)) ^ 0x5412;
        for (int v = 7; v <= 40; v++) VERSIONS[v] = (v << 12) | bchRemainder(v, 0x1F25);
    }

    private static int bchRemainder(int value, int poly) {
        int degree = 31 - Integer.numberOfLeadingZeros(poly);
        int r = value << degree;
        while (r != 0 && 31 - Integer.numberOfLeadingZeros(r) >= degree)
            r ^= poly << (31 - Integer.numberOfLeadingZeros(r) - degree);
        return r;
    }

    private static int bit(boolean[][] g, int x, int y) {
        return g[y][x] ? 1 : 0;
    }

    /** The five format bits (level, mask) from whichever copy reads closer to a valid word. */
    static int readFormat(boolean[][] g) {
        int dim = g.length, a = 0, b = 0;
        for (int x = 0; x <= 5; x++) a = (a << 1) | bit(g, x, 8);
        a = (a << 1) | bit(g, 7, 8);
        a = (a << 1) | bit(g, 8, 8);
        a = (a << 1) | bit(g, 8, 7);
        for (int y = 5; y >= 0; y--) a = (a << 1) | bit(g, 8, y);
        for (int y = dim - 1; y >= dim - 7; y--) b = (b << 1) | bit(g, 8, y);
        for (int x = dim - 8; x < dim; x++) b = (b << 1) | bit(g, x, 8);
        return nearest(FORMATS, 0, a, b);
    }

    private static int readVersion(boolean[][] g) {
        int dim = g.length, a = 0, b = 0;
        for (int y = 5; y >= 0; y--)
            for (int x = dim - 9; x >= dim - 11; x--) a = (a << 1) | bit(g, x, y);
        for (int x = 5; x >= 0; x--)
            for (int y = dim - 9; y >= dim - 11; y--) b = (b << 1) | bit(g, x, y);
        return nearest(VERSIONS, 7, a, b);
    }

    /** The index of the code word within three bit errors of either reading; -1 for none. */
    private static int nearest(int[] words, int from, int a, int b) {
        int best = -1, bestDist = 4;
        for (int i = from; i < words.length; i++) {
            int d = Math.min(Integer.bitCount(a ^ words[i]), Integer.bitCount(b ^ words[i]));
            if (d < bestDist) {
                bestDist = d;
                best = i;
            }
        }
        return best;
    }

    /** The modules that carry no data: finders, separators, format, timing, alignment, version. */
    static boolean[][] functionModules(int version) {
        int dim = 4 * version + 17;
        boolean[][] f = new boolean[dim][dim];
        region(f, 0, 0, 9, 9);
        region(f, dim - 8, 0, 8, 9);
        region(f, 0, dim - 8, 9, 8);
        int[] centres = alignmentCentres(version);
        int n = centres.length;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                // None where a finder is.
                if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
                region(f, centres[j] - 2, centres[i] - 2, 5, 5);
            }
        region(f, 6, 9, 1, dim - 17);
        region(f, 9, 6, dim - 17, 1);
        if (version >= 7) {
            region(f, dim - 11, 0, 3, 6);
            region(f, 0, dim - 11, 6, 3);
        }
        return f;
    }

    private static void region(boolean[][] f, int left, int top, int width, int height) {
        for (int y = top; y < top + height; y++)
            for (int x = left; x < left + width; x++) f[y][x] = true;
    }

    static int[] alignmentCentres(int version) {
        if (version == 1) return new int[0];
        int n = version / 7 + 2, dim = 4 * version + 17;
        int step = (version * 8 + n * 3 + 5) / (n * 4 - 4) * 2;
        int[] c = new int[n];
        c[0] = 6;
        for (int i = n - 1, pos = dim - 7; i >= 1; i--, pos -= step) c[i] = pos;
        return c;
    }

    /** Modules that hold data and error correction, from the specification's own count. */
    static int rawModules(int version) {
        int r = (16 * version + 128) * version + 64;
        if (version >= 2) {
            int n = version / 7 + 2;
            r -= (25 * n - 10) * n - 55;
            if (version >= 7) r -= 36;
        }
        return r;
    }

    private static boolean masked(int mask, int i, int j) {
        switch (mask) {
            case 0: return ((i + j) & 1) == 0;
            case 1: return (i & 1) == 0;
            case 2: return j % 3 == 0;
            case 3: return (i + j) % 3 == 0;
            case 4: return ((i / 2 + j / 3) & 1) == 0;
            case 5: return (i * j) % 6 == 0;
            case 6: return (i * j) % 6 < 3;
            default: return ((i + j + (i * j) % 3) & 1) == 0;
        }
    }

    /** The codewords in placement order: column pairs from the right, up then down, unmasked. */
    private static byte[] codewords(boolean[][] g, int version, int mask) {
        int dim = g.length;
        boolean[][] fn = functionModules(version);
        byte[] out = new byte[rawModules(version) / 8];
        int n = 0, cur = 0, bits = 0;
        boolean up = true;
        for (int x = dim - 1; x > 0; x -= 2) {
            if (x == 6) x--;  // the vertical timing column
            for (int k = 0; k < dim; k++) {
                int y = up ? dim - 1 - k : k;
                for (int c = 0; c < 2; c++) {
                    int xx = x - c;
                    if (fn[y][xx]) continue;
                    cur = (cur << 1) | ((g[y][xx] ^ masked(mask, y, xx)) ? 1 : 0);
                    if (++bits == 8) {
                        if (n == out.length) break;  // the remainder bits
                        out[n++] = (byte) cur;
                        bits = 0;
                        cur = 0;
                    }
                }
            }
            up = !up;
        }
        if (n != out.length) throw new Unreadable();
        return out;
    }

    // Error-correction codewords per block, and blocks, by level (L, M, Q, H) and version.
    static final int[][] EC_PER_BLOCK = {
        {-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
        {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
        {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
        {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    };
    static final int[][] BLOCKS = {
        {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
        {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
        {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
        {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},
    };

    /**
     * Undoes the interleaving and corrects each block. The short blocks come first, and a long
     * block has one data codeword more; the data codewords of every block are interleaved, then
     * their error-correction codewords. Null when a block has more errors than it can correct.
     */
    private static byte[] correct(byte[] raw, int version, int level) {
        int ecLen = EC_PER_BLOCK[level][version], blocks = BLOCKS[level][version];
        int shortLen = raw.length / blocks, longCount = raw.length % blocks;
        int shortCount = blocks - longCount, shortData = shortLen - ecLen;
        int[][] blk = new int[blocks][];
        for (int b = 0; b < blocks; b++) blk[b] = new int[shortLen + (b >= shortCount ? 1 : 0)];
        int k = 0;
        for (int i = 0; i < shortData; i++)
            for (int b = 0; b < blocks; b++) blk[b][i] = raw[k++] & 0xff;
        for (int b = shortCount; b < blocks; b++) blk[b][shortData] = raw[k++] & 0xff;
        for (int i = 0; i < ecLen; i++)
            for (int b = 0; b < blocks; b++) blk[b][blk[b].length - ecLen + i] = raw[k++] & 0xff;
        ByteArrayOutputStream data = new ByteArrayOutputStream();
        for (int[] cw : blk) {
            if (!rsCorrect(cw, ecLen)) return null;
            for (int i = 0; i < cw.length - ecLen; i++) data.write(cw[i]);
        }
        return data.toByteArray();
    }

    // GF(256) over x^8 + x^4 + x^3 + x^2 + 1, generator 2: QR's field.
    private static final int[] EXP = new int[512], LOG = new int[256];

    static {
        int x = 1;
        for (int i = 0; i < 255; i++) {
            EXP[i] = x;
            LOG[x] = i;
            x <<= 1;
            if (x >= 256) x ^= 0x11D;
        }
        for (int i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
    }

    static int mul(int a, int b) {
        return a == 0 || b == 0 ? 0 : EXP[LOG[a] + LOG[b]];
    }

    private static int div(int a, int b) {
        return a == 0 ? 0 : EXP[LOG[a] + 255 - LOG[b]];
    }

    /**
     * Corrects a Reed-Solomon block in place (first codeword the highest power, roots
     * 2^0 .. 2^(ecLen-1)): Berlekamp-Massey for the error locator, Chien search for where, Forney
     * for what. False when the block has more errors than ecLen / 2.
     */
    static boolean rsCorrect(int[] cw, int ecLen) {
        int n = cw.length;
        int[] s = syndromes(cw, ecLen);
        if (s == null) return true;
        int[] c = new int[ecLen + 1], b = new int[ecLen + 1];
        c[0] = 1;
        b[0] = 1;
        int len = 0, m = 1, lastD = 1;
        for (int r = 0; r < ecLen; r++) {
            int d = s[r];
            for (int i = 1; i <= len; i++) d ^= mul(c[i], s[r - i]);
            if (d == 0) {
                m++;
                continue;
            }
            int coef = div(d, lastD);
            if (2 * len <= r) {
                int[] t = c.clone();
                for (int i = m; i <= ecLen; i++) c[i] ^= mul(coef, b[i - m]);
                len = r + 1 - len;
                b = t;
                lastD = d;
                m = 1;
            } else {
                for (int i = m; i <= ecLen; i++) c[i] ^= mul(coef, b[i - m]);
                m++;
            }
        }
        if (2 * len > ecLen) return false;
        // Chien search: the locator's roots are the inverses of 2^p, p counted from the last codeword.
        int[] where = new int[len];
        int found = 0;
        for (int p = 0; p < n; p++) {
            int xinv = EXP[(255 - p) % 255], v = 0;
            for (int i = len; i >= 0; i--) v = mul(v, xinv) ^ c[i];
            if (v == 0) {
                if (found == len) return false;
                where[found++] = p;
            }
        }
        if (found != len) return false;
        // Forney, for roots starting at 2^0: e = X * omega(X^-1) / lambda'(X^-1).
        int[] omega = new int[ecLen];
        for (int i = 0; i < ecLen; i++) {
            int v = 0;
            for (int j = 0; j <= Math.min(i, len); j++) v ^= mul(c[j], s[i - j]);
            omega[i] = v;
        }
        for (int e = 0; e < found; e++) {
            int p = where[e], xinv = EXP[(255 - p) % 255];
            int num = 0;
            for (int i = ecLen - 1; i >= 0; i--) num = mul(num, xinv) ^ omega[i];
            int den = 0;
            for (int i = 1; i <= len; i += 2) den ^= mul(c[i], EXP[(LOG[xinv] * (i - 1)) % 255]);
            if (den == 0) return false;
            cw[n - 1 - p] ^= mul(EXP[p % 255], div(num, den));
        }
        return syndromes(cw, ecLen) == null;
    }

    /** The block's syndromes, or null when they are all zero (no errors). */
    private static int[] syndromes(int[] cw, int ecLen) {
        int[] s = new int[ecLen];
        boolean any = false;
        for (int i = 0; i < ecLen; i++) {
            int a = EXP[i], v = 0;
            for (int x : cw) v = mul(v, a) ^ x;
            s[i] = v;
            any |= v != 0;
        }
        return any ? s : null;
    }

    // --- the bit stream --------------------------------------------------------------------------

    private static final String ALNUM = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

    /** The segments' text: numeric, alphanumeric and byte (read as UTF-8); ECI is skipped. */
    private static String text(byte[] data, int version) {
        int[] pos = {0};
        int sizeClass = version <= 9 ? 0 : version <= 26 ? 1 : 2;
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        while (data.length * 8 - pos[0] >= 4) {
            int mode = take(data, pos, 4);
            if (mode == 0) break;  // terminator
            switch (mode) {
                case 1: {  // numeric: three digits in 10 bits
                    int count = take(data, pos, new int[] {10, 12, 14}[sizeClass]);
                    for (; count >= 3; count -= 3) digits(out, take(data, pos, 10), 3);
                    if (count == 2) digits(out, take(data, pos, 7), 2);
                    else if (count == 1) digits(out, take(data, pos, 4), 1);
                    break;
                }
                case 2: {  // alphanumeric: two characters in 11 bits
                    int count = take(data, pos, new int[] {9, 11, 13}[sizeClass]);
                    for (; count >= 2; count -= 2) {
                        int v = take(data, pos, 11);
                        if (v >= 45 * 45) throw new Unreadable();
                        out.write(ALNUM.charAt(v / 45));
                        out.write(ALNUM.charAt(v % 45));
                    }
                    if (count == 1) {
                        int v = take(data, pos, 6);
                        if (v >= 45) throw new Unreadable();
                        out.write(ALNUM.charAt(v));
                    }
                    break;
                }
                case 4: {  // bytes
                    int count = take(data, pos, new int[] {8, 16, 16}[sizeClass]);
                    for (int i = 0; i < count; i++) out.write(take(data, pos, 8));
                    break;
                }
                case 7: {  // ECI: a 1-, 2- or 3-byte designator; the bytes are read as UTF-8 anyway
                    int first = take(data, pos, 8);
                    if ((first & 0x80) == 0) break;
                    if ((first & 0xC0) == 0x80) take(data, pos, 8);
                    else if ((first & 0xE0) == 0xC0) take(data, pos, 16);
                    else throw new Unreadable();
                    break;
                }
                case 3: take(data, pos, 16); break;  // structured append: index, parity
                case 5: break;  // FNC1, first position
                case 9: take(data, pos, 8); break;  // FNC1, second position
                default: throw new Unreadable();  // kanji, or not a mode
            }
        }
        return new String(out.toByteArray(), StandardCharsets.UTF_8);
    }

    private static void digits(ByteArrayOutputStream out, int value, int count) {
        if (value >= new int[] {0, 10, 100, 1000}[count]) throw new Unreadable();
        String s = Integer.toString(value);
        for (int i = s.length(); i < count; i++) out.write('0');
        for (int i = 0; i < s.length(); i++) out.write(s.charAt(i));
    }

    private static int take(byte[] data, int[] pos, int bits) {
        if (data.length * 8 - pos[0] < bits) throw new Unreadable();
        int v = 0;
        for (int i = 0; i < bits; i++, pos[0]++)
            v = (v << 1) | ((data[pos[0] >> 3] >> (7 - (pos[0] & 7))) & 1);
        return v;
    }
}
