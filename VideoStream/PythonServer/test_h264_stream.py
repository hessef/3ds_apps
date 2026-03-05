"""
Purpose
-------
A simple laptop client that speaks the same protocol as the 3DS example:

1) Connect to server
2) Send b"H264"
3) Expect b"OKAY"
4) Repeatedly send b"N" to request one "unit"
5) Receive length-prefixed bytes
6) Validate that each unit begins with an Annex-B start code
7) Print NAL type statistics so you can confirm the stream looks sane

Usage
-----
python test_h264_stream_client.py <server_ip> [port]
"""

import socket
import struct
import sys
from collections import Counter

START3 = b"\x00\x00\x01"
START4 = b"\x00\x00\x00\x01"

def recv_all(sock: socket.socket, n: int) -> bytes:
    """Receive exactly n bytes or raise RuntimeError."""
    out = bytearray()
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise RuntimeError("Socket closed while receiving.")
        out += chunk
    return bytes(out)

def recv_u32_be(sock: socket.socket) -> int:
    """Receive a big-endian u32."""
    b = recv_all(sock, 4)
    return struct.unpack(">I", b)[0]

def nal_type_from_unit(unit: bytes) -> int | None:
    """
    Extract H.264 NAL type (lower 5 bits of first payload byte).
    Works only if unit begins with Annex-B start code.
    """
    if unit.startswith(START4):
        sc_len = 4
    elif unit.startswith(START3):
        sc_len = 3
    else:
        return None
    if len(unit) <= sc_len:
        return None
    return unit[sc_len] & 0x1F

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <server_ip> [port]")
        sys.exit(1)

    ip = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) >= 3 else 5000

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((ip, port))
    print(f"[client] Connected to {ip}:{port}")

    # Handshake
    s.sendall(b"H264")
    reply = recv_all(s, 4)
    if reply != b"OKAY":
        print(f"[client] Bad handshake reply: {reply!r}")
        return
    print("[client] Handshake OK.")

    counts = Counter()
    total_bytes = 0
    n_units = 0

    try:
        while True:
            # Request next unit
            s.sendall(b"N")

            length = recv_u32_be(s)
            if length == 0:
                print("[client] End of stream.")
                break

            unit = recv_all(s, length)
            n_units += 1
            total_bytes += len(unit)

            # Validate start code
            if not (unit.startswith(START4) or unit.startswith(START3)):
                print(f"[client] WARNING: unit {n_units} missing Annex-B start code.")
                print(f"         first 8 bytes: {unit[:8].hex(' ')}")
            else:
                # Extract NAL type
                t = nal_type_from_unit(unit)
                if t is None:
                    counts["unknown"] += 1
                else:
                    counts[t] += 1

            # Print occasional progress
            if n_units % 30 == 0:
                # Show some common types by name
                def c(k): return counts.get(k, 0)
                print(
                    f"[client] units={n_units}, MB={total_bytes/1e6:.2f}, "
                    f"SPS(7)={c(7)}, PPS(8)={c(8)}, IDR(5)={c(5)}, slice(1)={c(1)}, SEI(6)={c(6)}"
                )

    finally:
        s.close()

    print("[client] Done.")
    print(f"[client] total units: {n_units}")
    print(f"[client] total MB: {total_bytes/1e6:.2f}")
    print("[client] NAL type counts:", dict(counts))

if __name__ == "__main__":
    main()