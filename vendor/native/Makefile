# Build the vendored native helpers from source (resolution 0a).
# Built the way the helpers have always shipped: `cc -static -O2`.
# Built artifacts are gitignored; only the .c sources are committed.
CC ?= cc
CFLAGS ?= -static -O2
BINS := fwd uinput-kbd

.PHONY: all clean host-encoder
all: $(BINS)

fwd: fwd.c
	$(CC) $(CFLAGS) fwd.c -o fwd

# The phantom keyboard Android needs to believe a physical one is attached. Static for the same
# reason fwd is: it runs inside the container, which has no host libc to link against.
uinput-kbd: uinput-kbd.c
	$(CC) $(CFLAGS) uinput-kbd.c -o uinput-kbd

# The host-side mirror encoder is NOT part of `all`: it links against ffmpeg
# (libavcodec/libavfilter/libavutil), which the guest helpers do not need and
# which many hosts will not have. Deliberately opt-in so a missing ffmpeg cannot
# break the ordinary deploy — `remora up` reports it as a clear prerequisite
# when host_encode is on. Not static: it needs the host's own ffmpeg and Vulkan.
host-encoder: remora-frame-encoder
remora-frame-encoder: remora-frame-encoder.c
	$(CC) -O2 -Wall -Wextra -o $@ $< \
	    $(shell pkg-config --cflags --libs libavcodec libavfilter libavutil)

# Host-side video decoder for in-guest playback (bd remora-e5x.3): opt-in for
# the same reason as the encoder, plus libva — it calls vaExportSurfaceHandle
# itself because ffmpeg's hwmap path cannot be relied on (see
# vendor/host-prereqs/nvdec-vaapi/README.md).
host-decoder: remora-frame-decoder
remora-frame-decoder: remora-frame-decoder.c
	$(CC) -O2 -Wall -Wextra -o $@ $< \
	    $(shell pkg-config --cflags --libs libavcodec libavutil libva)

clean:
	rm -f $(BINS) remora-frame-encoder remora-frame-decoder
