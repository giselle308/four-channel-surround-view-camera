#!/usr/bin/env bash
set -euo pipefail

display_sink="${VIDEO_SINK:-ximagesink}"
video_decoder="${VIDEO_DECODER:-}"

if [[ -z "${video_decoder}" ]]; then
  for candidate in nvh265dec vah265dec avdec_h265; do
    if gst-inspect-1.0 "${candidate}" >/dev/null 2>&1; then
      video_decoder="${candidate}"
      break
    fi
  done
fi
if [[ -z "${video_decoder}" ]] || ! gst-inspect-1.0 "${video_decoder}" >/dev/null 2>&1; then
  echo "No usable H.265 decoder found: ${video_decoder:-none}" >&2
  exit 1
fi

echo "Four-grid SRT preview: decoder=${video_decoder} sink=${display_sink} tiles=1440x1080@60 window=1440x1080@60"

exec gst-launch-1.0 \
  compositor name=mix background=black \
    sink_0::xpos=0   sink_0::ypos=0   sink_0::width=720 sink_0::height=540 \
    sink_1::xpos=720 sink_1::ypos=0   sink_1::width=720 sink_1::height=540 \
    sink_2::xpos=0   sink_2::ypos=540 sink_2::width=720 sink_2::height=540 \
    sink_3::xpos=720 sink_3::ypos=540 sink_3::width=720 sink_3::height=540 \
  ! video/x-raw,width=1440,height=1080,framerate=60/1 \
  ! videoconvert ! "${display_sink}" sync=false \
  srtsrc uri="srt://0.0.0.0:5000?mode=listener&latency=250" \
  ! tsdemux ! h265parse ! "${video_decoder}" \
  ! videoconvert ! videoscale ! videorate ! video/x-raw,format=I420,width=1440,height=1080,framerate=60/1 \
  ! queue max-size-buffers=4 max-size-bytes=0 max-size-time=0 leaky=downstream \
  ! mix.sink_0 \
  srtsrc uri="srt://0.0.0.0:5002?mode=listener&latency=250" \
  ! tsdemux ! h265parse ! "${video_decoder}" \
  ! videoconvert ! videoscale ! videorate ! video/x-raw,format=I420,width=1440,height=1080,framerate=60/1 \
  ! queue max-size-buffers=4 max-size-bytes=0 max-size-time=0 leaky=downstream \
  ! mix.sink_1 \
  srtsrc uri="srt://0.0.0.0:5004?mode=listener&latency=250" \
  ! tsdemux ! h265parse ! "${video_decoder}" \
  ! videoconvert ! videoscale ! videorate ! video/x-raw,format=I420,width=1440,height=1080,framerate=60/1 \
  ! queue max-size-buffers=4 max-size-bytes=0 max-size-time=0 leaky=downstream \
  ! mix.sink_2 \
  srtsrc uri="srt://0.0.0.0:5006?mode=listener&latency=250" \
  ! tsdemux ! h265parse ! "${video_decoder}" \
  ! videoconvert ! videoscale ! videorate ! video/x-raw,format=I420,width=1440,height=1080,framerate=60/1 \
  ! queue max-size-buffers=4 max-size-bytes=0 max-size-time=0 leaky=downstream \
  ! mix.sink_3
