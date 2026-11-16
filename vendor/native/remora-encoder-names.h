/*
 * The encoder names remora-frame-encoder accepts for --codec (bd remora-8tpz).
 *
 * Kept apart from remora-frame-encoder.c, with no ffmpeg in it, so the prereqs suite can run the
 * very function the encoder runs: the encoder needs libavcodec and a GPU to build and start, the
 * suite needs neither.
 *
 * --codec used to pass any explicit name straight to avcodec_find_encoder_by_name, so whatever
 * encoder the host's ffmpeg happened to carry could be opened on our frames — one that was never
 * measured or exercised here (ffv1_vulkan, say). Remora itself only ever sends a generic "hevc" or
 * "h264" (buildHostEncoderArgv); the explicit names are the Vulkan Video and NVENC backends of the
 * three codecs the stream format carries, which is everything resolve_codec and the NVENC path
 * already handle. main() checks this before it touches a device, so a refused name costs nothing.
 */
#ifndef REMORA_ENCODER_NAMES_H
#define REMORA_ENCODER_NAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const char *const remora_encoder_names[] = {
   /* generic: resolve_codec picks the backend for the GPU */
   "hevc", "h265", "h264", "avc",
   /* explicit: forces that backend */
   "hevc_vulkan", "hevc_nvenc", "h264_vulkan", "h264_nvenc", "av1_vulkan", "av1_nvenc",
};

static inline bool
remora_encoder_name_allowed(const char *name)
{
   if (!name)
      return false;
   for (size_t i = 0; i < sizeof(remora_encoder_names) / sizeof(remora_encoder_names[0]); i++)
      if (!strcmp(name, remora_encoder_names[i]))
         return true;
   return false;
}

#endif
