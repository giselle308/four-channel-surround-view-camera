#!/usr/bin/env python3
"""Verify that local RTP streams carry the timestamps announced by metadata."""

import argparse
import selectors
import socket
import struct
import time


METADATA_PACKET = struct.Struct("!IHHQQqII" + "QQq" * 4)
METADATA_MAGIC = 0x53564D44
CAMERA_NAMES = ("Front", "Rear", "Left", "Right")
DEFAULT_RTP_PORTS = (5000, 5002, 5004, 5006)


def bind_udp(address: str, port: int) -> socket.socket:
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    receiver.bind((address, port))
    receiver.setblocking(False)
    return receiver


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare FrameGroup metadata timestamps with local RTP marker packets"
    )
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--metadata-port", type=int, default=5100)
    parser.add_argument("--rtp-ports", type=int, nargs=4, default=DEFAULT_RTP_PORTS)
    parser.add_argument("--active-streams", type=int, choices=range(1, 5), default=4)
    parser.add_argument("--duration", type=float, default=8.0)
    parser.add_argument("--minimum-match", type=float, default=0.95)
    arguments = parser.parse_args()

    selector = selectors.DefaultSelector()
    sockets = []
    metadata_socket = bind_udp(arguments.bind, arguments.metadata_port)
    sockets.append(metadata_socket)
    selector.register(metadata_socket, selectors.EVENT_READ, ("metadata", -1))
    for index, port in enumerate(arguments.rtp_ports[: arguments.active_streams]):
        rtp_socket = bind_udp(arguments.bind, port)
        sockets.append(rtp_socket)
        selector.register(rtp_socket, selectors.EVENT_READ, ("rtp", index))

    metadata_timestamps = set()
    metadata_packets = 0
    invalid_metadata = 0
    rtp_packets = [0] * arguments.active_streams
    rtp_marker_timestamps = [set() for _ in range(arguments.active_streams)]
    first_metadata = None
    first_rtp = [None] * arguments.active_streams
    deadline = time.monotonic() + arguments.duration

    print(
        f"listening metadata={arguments.bind}:{arguments.metadata_port} "
        f"rtp={list(arguments.rtp_ports[:arguments.active_streams])}",
        flush=True,
    )
    try:
        while time.monotonic() < deadline:
            remaining = max(0.0, deadline - time.monotonic())
            for key, _ in selector.select(min(0.25, remaining)):
                payload, _ = key.fileobj.recvfrom(65535)
                packet_type, index = key.data
                if packet_type == "metadata":
                    if len(payload) != METADATA_PACKET.size:
                        invalid_metadata += 1
                        continue
                    fields = METADATA_PACKET.unpack(payload)
                    magic, version, packet_size = fields[:3]
                    if (
                        magic != METADATA_MAGIC
                        or version != 1
                        or packet_size != METADATA_PACKET.size
                    ):
                        invalid_metadata += 1
                        continue
                    metadata_packets += 1
                    metadata_timestamps.add(fields[6])
                    if first_metadata is None:
                        first_metadata = fields[6]
                elif len(payload) >= 12 and payload[0] >> 6 == 2:
                    rtp_packets[index] += 1
                    timestamp = struct.unpack_from("!I", payload, 4)[0]
                    if first_rtp[index] is None:
                        first_rtp[index] = timestamp
                    if payload[1] & 0x80:
                        rtp_marker_timestamps[index].add(timestamp)
    finally:
        selector.close()
        for receiver in sockets:
            receiver.close()

    success = metadata_packets > 0 and invalid_metadata == 0
    print(
        f"metadata packets={metadata_packets} unique_ts={len(metadata_timestamps)} "
        f"invalid={invalid_metadata} first_ts={first_metadata}"
    )
    for index in range(arguments.active_streams):
        marker_timestamps = rtp_marker_timestamps[index]
        # Metadata is emitted before encoding, so the last metadata item may not
        # have reached the RTP socket when a finite-duration test stops.
        matched = len(metadata_timestamps & marker_timestamps)
        denominator = min(len(metadata_timestamps), len(marker_timestamps))
        ratio = matched / denominator if denominator else 0.0
        print(
            f"{CAMERA_NAMES[index]} port={arguments.rtp_ports[index]} "
            f"rtp_packets={rtp_packets[index]} marker_frames={len(marker_timestamps)} "
            f"matched={matched}/{denominator} ({ratio:.2%}) first_ts={first_rtp[index]}"
        )
        success = success and denominator > 0 and ratio >= arguments.minimum_match

    print("RESULT=PASS" if success else "RESULT=FAIL")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
