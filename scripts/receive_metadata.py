#!/usr/bin/env python3
import argparse
import socket
import struct

PACKET = struct.Struct("!IHHQQqII" + "QQq" * 4)
MAGIC = 0x53564D44
NAMES = ("Front", "Rear", "Left", "Right")


def main() -> None:
    parser = argparse.ArgumentParser(description="Receive surround-view FrameGroup metadata")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5100)
    arguments = parser.parse_args()

    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind((arguments.bind, arguments.port))
    print(f"metadata receiver listening on {arguments.bind}:{arguments.port}", flush=True)
    while True:
        payload, source = receiver.recvfrom(2048)
        if len(payload) != PACKET.size:
            print(f"invalid packet size={len(payload)} source={source}", flush=True)
            continue
        fields = PACKET.unpack(payload)
        magic, version, packet_size, group_id, cycle, group_ts, rtp_ts, _ = fields[:8]
        if magic != MAGIC or version != 1 or packet_size != PACKET.size:
            print("invalid metadata header", flush=True)
            continue
        camera_fields = fields[8:]
        frames = ", ".join(
            f"{NAMES[index]}(frame={camera_fields[index * 3]},dev={camera_fields[index * 3 + 1]})"
            for index in range(4)
        )
        print(
            f"group={group_id} cycle={cycle} group_ts={group_ts} rtp_ts={rtp_ts} {frames}",
            flush=True,
        )


if __name__ == "__main__":
    main()
