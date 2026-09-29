// Frame export for the Remora composer HAL (bd remora-emoe phase 2).
//
// WHAT IT REPLACES. Remora's host encoder obtains composed frames today by INTERCEPTING them in a
// patched Venus render server, which means inferring which of several render-server contexts is
// the screen — remora-frame-encoder.c's pick_display() is ~60 lines of liveness/extent heuristic
// whose own comments record three bugs it exists to paper over. A composer does not infer: the
// buffer SurfaceFlinger hands setClientTarget IS the screen, one per frame, by definition.
//
// OPT-IN AND BEST-EFFORT, both deliberate. Export is off unless the host names a socket in
// ro.boot.remora_frame_socket, so an image with this code behaves exactly as before wherever
// compose mode is not in use — which is every profile today, the daily driver included. And once
// on, nothing here may take the display down: a composer that blocks or aborts is a black screen,
// so every failure path degrades to "stop exporting" rather than propagating.
#pragma once

#include <hardware/gralloc.h>

namespace remora {

// HIDDEN ON PURPOSE. This HAL exports exactly one symbol, HMI, and that is not incidental tidiness:
// bd remora-emoe measured the closed prebuilt's export list as part of establishing what it does,
// and matching it is how we show this module is the same kind of thing. Phase 2 adds a second
// translation unit, and without this its two entry points would join HMI in the dynamic table.
#define REMORA_HAL_LOCAL __attribute__((visibility("hidden")))

// Called once at HAL open. Reads the socket path property and loads the gralloc module used to
// query buffer layout; both absent is the normal, silent, disabled case.
REMORA_HAL_LOCAL void frameExportInit();

// Called from present(), AFTER the client target's acquire fence has been waited on — that wait is
// what makes the buffer safe to read, so exporting before it would publish a half-drawn frame.
// Does nothing if export is disabled or the encoder is not connected.
REMORA_HAL_LOCAL void frameExportPublish(buffer_handle_t target, int width, int height);

}  // namespace remora
