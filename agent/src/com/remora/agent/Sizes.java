package com.remora.agent;

/**
 * Output-size arithmetic, shared by every capture: cut a display size to the consumer's limits
 * and to max_size, preserve aspect, and align both dimensions down to what the consumer needs.
 *
 * An encoder configured past its capabilities fails at start() with nothing useful in the
 * message, so this is what turns "3760x1992 into a 1920-wide encoder" into a working stream
 * rather than a mystery. The compose path has no codec and therefore no real limits — it passes
 * MAX_VALUE bounds and an alignment of 8, which only keeps the size even for the host encoder's
 * RGBA→NV12 conversion.
 */
public final class Sizes {
    /**
     * @param maxSize   0 for "no request"
     * @param maxW/maxH the consumer's own upper bounds
     * @param minSize   the consumer's lower bound, honoured even against maxSize
     */
    public static int[] constrain(int width, int height, int alignment, int maxSize, int maxW,
                                  int maxH, int minSize) {
        if (alignment <= 0) alignment = 2;
        if (maxSize > 0) {
            maxW = Math.min(maxW, maxSize);
            maxH = Math.min(maxH, maxSize);
        }
        maxW = maxW / alignment * alignment;
        maxH = maxH / alignment * alignment;

        int w = width, h = height;
        if (w > maxW || h > maxH) {
            // Fit inside the box on whichever axis binds first, keeping the aspect ratio. The
            // long multiply matters: 3760 * 1992 overflows nothing, but a 16384-wide bound times
            // a 4K height does.
            if ((long) width * maxH > (long) height * maxW) {
                w = maxW;
                h = (int) ((long) height * maxW / width);
            } else {
                w = (int) ((long) width * maxH / height);
                h = maxH;
            }
        }
        w = w / alignment * alignment;
        h = h / alignment * alignment;

        final int minAligned = (minSize + alignment - 1) / alignment * alignment;
        return new int[]{Math.max(w, minAligned), Math.max(h, minAligned)};
    }

    private Sizes() {}
}
