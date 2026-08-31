#!/usr/bin/env bash
set -euo pipefail

bind_address="${1:-0.0.0.0}"
display_sink="${VIDEO_SINK:-xvimagesink}"
rtp_caps='application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000'

video_decoder="${VIDEO_DECODER:-}"
if [[ -z "${video_decoder}" ]]; then
  for candidate in nvh264dec vah264dec avdec_h264; do
    if gst-inspect-1.0 "${candidate}" >/dev/null 2>&1; then
      video_decoder="${candidate}"
      break
    fi
  done
fi
if [[ -z "${video_decoder}" ]] || ! gst-inspect-1.0 "${video_decoder}" >/dev/null 2>&1; then
  echo "No usable H.264 decoder found: ${video_decoder:-none}" >&2
  exit 1
fi

if [[ "${FOUR_GRID_DIAG_DECODE_ONLY:-0}" == "1" ]]; then
  echo "Decode-only diagnostic: four RTP streams in one pipeline, sink=fakesink"
  exec gst-launch-1.0 \
    udpsrc address="${bind_address}" port=5000 caps="${rtp_caps}" \
    ! rtpjitterbuffer latency=50 drop-on-latency=true \
    ! rtph264depay ! h264parse ! "${video_decoder}" ! fakesink sync=false \
    udpsrc address="${bind_address}" port=5002 caps="${rtp_caps}" \
    ! rtpjitterbuffer latency=50 drop-on-latency=true \
    ! rtph264depay ! h264parse ! "${video_decoder}" ! fakesink sync=false \
    udpsrc address="${bind_address}" port=5004 caps="${rtp_caps}" \
    ! rtpjitterbuffer latency=50 drop-on-latency=true \
    ! rtph264depay ! h264parse ! "${video_decoder}" ! fakesink sync=false \
    udpsrc address="${bind_address}" port=5006 caps="${rtp_caps}" \
    ! rtpjitterbuffer latency=50 drop-on-latency=true \
    ! rtph264depay ! h264parse ! "${video_decoder}" ! fakesink sync=false
fi

echo "Four-grid preview: decoder=${video_decoder} sink=${display_sink} output=1280x960@60"

exec gst-launch-1.0 \
  compositor name=mix background=black \
    sink_0::xpos=0   sink_0::ypos=0   sink_0::width=640 sink_0::height=480 \
    sink_1::xpos=640 sink_1::ypos=0   sink_1::width=640 sink_1::height=480 \
    sink_2::xpos=0   sink_2::ypos=480 sink_2::width=640 sink_2::height=480 \
    sink_3::xpos=640 sink_3::ypos=480 sink_3::width=640 sink_3::height=480 \
  ! video/x-raw,width=1280,height=960,framerate=60/1 \
  ! videoconvert ! "${display_sink}" sync=false \
  udpsrc address="${bind_address}" port=5000 caps="${rtp_caps}" \
  ! rtpjitterbuffer latency=50 drop-on-latency=true \
  ! rtph264depay ! h264parse ! "${video_decoder}" \
  ! videoconvert ! videoscale ! video/x-raw,width=640,height=480 \
  ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream \
  ! mix.sink_0 \
  udpsrc address="${bind_address}" port=5002 caps="${rtp_caps}" \
  ! rtpjitterbuffer latency=50 drop-on-latency=true \
  ! rtph264depay ! h264parse ! "${video_decoder}" \
  ! videoconvert ! videoscale ! video/x-raw,width=640,height=480 \
  ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream \
  ! mix.sink_1 \
  udpsrc address="${bind_address}" port=5004 caps="${rtp_caps}" \
  ! rtpjitterbuffer latency=50 drop-on-latency=true \
  ! rtph264depay ! h264parse ! "${video_decoder}" \
  ! videoconvert ! videoscale ! video/x-raw,width=640,height=480 \
  ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream \
  ! mix.sink_2 \
  udpsrc address="${bind_address}" port=5006 caps="${rtp_caps}" \
  ! rtpjitterbuffer latency=50 drop-on-latency=true \
  ! rtph264depay ! h264parse ! "${video_decoder}" \
  ! videoconvert ! videoscale ! video/x-raw,width=640,height=480 \
  ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream \
  ! mix.sink_3
