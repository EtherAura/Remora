#!/bin/sh
# Exercise VA-API encode with moving content, then verify the complete decoded stream.
set -eu
export LC_ALL=C

node=${1:-/dev/dri/renderD128}
out=$(mktemp -d "${2:-.}/remora-video.XXXXXX")
say() { printf '%s\n' "$*"; }
say "Video probe artifacts: $out"

if ! timeout -k 5 30 vainfo --display drm --device "$node" >"$out/vainfo.log" 2>&1; then
    cat "$out/vainfo.log" >&2
    exit 1
fi
cat "$out/vainfo.log"

rate=60
frames=300
pattern="testsrc2=size=1920x1080:rate=$rate:duration=5"

probe() {
    codec=$1
    profile=$2
    clip="$out/$codec.mkv"

    # Use the regular encode entrypoint and an explicit hardware encoder. FFmpeg
    # fails if VA-API cannot supply it. There is no software encoder in this command.
    if ! grep -E "^[[:space:]]*${profile}[[:space:]]*:[[:space:]]*VAEntrypointEncSlice[[:space:]]*$" \
        "$out/vainfo.log" >/dev/null; then
        say "$codec: no $profile VAEntrypointEncSlice on $node" >&2
        return 1
    fi
    say "Encoding $codec at 1920x1080, $rate fps on $node"
    timeout -k 5 90 ffmpeg -hide_banner -nostdin -xerror -benchmark -init_hw_device "vaapi=va:$node" \
        -filter_hw_device va -f lavfi -i "$pattern" -vf 'format=nv12,hwupload' \
        -c:v "${codec}_vaapi" -profile:v main -bf 0 -qp 24 -frames:v "$frames" "$clip" \
        >"$out/$codec-encode.log" 2>&1 || { cat "$out/$codec-encode.log" >&2; return 1; }

    report="$out/$codec-stream.txt"
    timeout -k 5 90 ffprobe -v error -select_streams v:0 -count_frames \
        -show_entries stream=codec_name,width,height,r_frame_rate,nb_read_frames \
        -of default=noprint_wrappers=1 "$clip" >"$report" || return 1
    cat "$report"
    for expected in "codec_name=$codec" width=1920 height=1080 "r_frame_rate=$rate/1" \
        "nb_read_frames=$frames"; do
        grep -qx "$expected" "$report" || { say "$codec: missing $expected" >&2; return 1; }
    done

    # Check picture content as well as decodability. Compare by frame index to
    # avoid the container's timestamp rounding affecting frame alignment.
    validation="$out/$codec-picture.log"
    timeout -k 5 90 ffmpeg -hide_banner -nostdin -xerror -i "$clip" -f lavfi -i "$pattern" \
        -filter_complex "[0:v]settb=AVTB,setpts=N/($rate*TB)[decoded];\
[1:v]settb=AVTB,setpts=N/($rate*TB)[ref];[decoded][ref]ssim=shortest=1" \
        -an -f null - >"$validation" 2>&1 || { cat "$validation" >&2; return 1; }
    score=$(sed -n 's/.* All:\([0-9.]*\) .*/\1/p' "$validation")
    if ! awk -v score="$score" 'BEGIN { exit !(score + 0 >= 0.90) }'; then
        say "$codec: decoded output failed the source comparison (SSIM ${score:-missing})" >&2
        return 1
    fi
    say "$codec: $frames frames decoded, SSIM $score"
}

failed=0
probe h264 VAProfileH264Main || failed=1
probe hevc VAProfileHEVCMain || failed=1
exit "$failed"
