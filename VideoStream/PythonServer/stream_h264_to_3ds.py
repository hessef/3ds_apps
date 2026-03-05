"""
Purpose
-------
- Read a video file on the PC
- Scale/pad it to 400x240 (pillarbox as needed)
- Transpose to 240x400 so the 3DS can write decoded frames directly into the
  "top screen framebuffer layout trick" without CPU rotation
- Encode as raw H.264 Annex-B elementary stream (start codes 00 00 00 01 / 00 00 01)
- Split the bytestream into complete NAL units
- Stream those NAL units over TCP to a client (3DS or a test client)

Protocol
--------
Client connects (TCP) to SERVER_IP:PORT
1) Client -> b"H264" (4 bytes)
2) Server -> b"OKAY" (4 bytes)
3) Loop:
   - Client -> b"N" (1 byte) meaning "send next unit"
   - Server -> [u32 length big-endian] + [NAL bytes]
     - length == 0 means end-of-stream

Why start codes matter
----------------------
H.264 can be carried in several "packaging" formats:
- Annex B byte-stream: start codes separate NAL units.
- AVCC format: length-prefix NAL units (common inside MP4).
The 3DS-side MVD pipeline tends to behave best when you feed it Annex-B NALs
(including the start codes) as a byte-stream.

This server outputs Annex B and sends NALs INCLUDING their start codes.
"""

import socket
import struct
import subprocess
import sys
from typing import List

# ------------------------ Configuration ------------------------

PORT = 5000

# Conservative bitrate while you’re bringing up the pipeline.
# Increase later once it’s stable.
BITRATE = "1200k"

# Keep a constant FPS for easier decoder behavior while learning.
FPS = "30"

# H.264 start codes (Annex B). NAL units are delimited by one of these.
START3 = b"\x00\x00\x01"
START4 = b"\x00\x00\x00\x01"

# Safety cap: if our buffer grows without finding boundaries, trim it.
MAX_ROLLING_BUFFER = 2 * 1024 * 1024  # 2 MiB


# ------------------------ Annex-B parsing helpers ------------------------

def find_start_code(data: bytes, start: int = 0) -> int:
    """
    Find the next occurrence of a H.264 Annex-B start code in `data`,
    searching from index `start`.

    Returns:
        index of the next start code (either 3-byte or 4-byte),
        or -1 if none found.

    Why we search for both:
        Start codes can be 3 bytes (00 00 01) or 4 bytes (00 00 00 01).
        Streams often use 4-byte at the beginning and 3-byte later,
        but you must handle both.
    """
    i = data.find(START4, start)
    j = data.find(START3, start)
    if i == -1:
        return j
    if j == -1:
        return i
    return min(i, j)


def start_code_len(data: bytes, idx: int) -> int:
    """
    Given a buffer `data` and an index `idx` where we KNOW a start code begins,
    return the start code length (3 or 4 bytes).

    We check if it starts with 4-byte code, otherwise assume 3-byte.
    """
    return 4 if data.startswith(START4, idx) else 3


def enqueue_complete_nalus(rolling: bytes, nal_queue: List[bytes]) -> bytes:
    """
    Consume as many COMPLETE (fully delimited) NAL units as possible from `rolling`
    and append them to `nal_queue`.

    IMPORTANT: We queue NAL units INCLUDING their start code bytes.
    That means each queued item looks like:
        b"\\x00\\x00\\x00\\x01" + b"<nal_payload>"
    or
        b"\\x00\\x00\\x01" + b"<nal_payload>"

    We only consider a NAL "complete" if we have:
        [start_i ... start_{i+1})
    i.e., two start codes in the buffer. That guarantees the NAL is bounded.

    Returns:
        The remaining rolling buffer starting at the last start code
        (which begins an incomplete NAL that needs more bytes).
    """
    # Find the first start code in the buffer.
    first = find_start_code(rolling, 0)
    if first == -1:
        # No start code at all, can't parse anything.
        return rolling

    # Find the second start code; without it we can't delimit a full NAL yet.
    second = find_start_code(rolling, first + start_code_len(rolling, first))
    if second == -1:
        return rolling

    # We can now extract full NAL units for each pair of start codes.
    start_i = first
    while True:
        start_next = find_start_code(rolling, start_i + start_code_len(rolling, start_i))
        if start_next == -1:
            # No further delimiter, so the NAL beginning at start_i is incomplete.
            break

        # Complete NAL = bytes from current start code up to (but not including) next start code.
        nal = rolling[start_i:start_next]
        if nal:
            nal_queue.append(nal)

        start_i = start_next

    # Keep the remainder starting from the last start code (incomplete NAL).
    return rolling[start_i:]


# ------------------------ ffmpeg pipeline ------------------------

def ffmpeg_h264_stream(path: str) -> subprocess.Popen:
    """
    Launch ffmpeg to output a raw H.264 Annex-B stream to stdout.

    Filters:
      scale=400:240:force_original_aspect_ratio=decrease
        - scale to fit within 400x240 without stretching
      pad=400:240:(400-iw)/2:(240-ih)/2
        - pad with black to exactly 400x240, centered (pillarbox/letterbox as needed)
      transpose=1
        - rotate 90 degrees clockwise, resulting in 240x400

    Why transpose?
      The 3DS top framebuffer memory layout is "rotated-ish".
      A classic trick is to transpose/rotate the video so the decoder writes
      directly into the framebuffer without CPU-side rotation.
    """
    vf = (
        "scale=400:240:force_original_aspect_ratio=decrease,"
        "pad=400:240:(400-iw)/2:(240-ih)/2,"
        "transpose=1"
    )

    cmd = [
        "ffmpeg",
        "-loglevel", "error",

        # -re makes ffmpeg emit frames in real-time-ish instead of as fast as possible.
        # Helpful for streaming demos and makes bandwidth more consistent.
        "-re",

        "-i", path,

        "-vf", vf,
        "-r", FPS,                 # constant FPS

        # H.264 encoding
        "-c:v", "libx264",
        "-pix_fmt", "yuv420p",     # widely supported by decoders
        "-profile:v", "baseline",  # simplest profile, best compatibility
        "-level:v", "3.0",
        "-tune", "zerolatency",    # reduce buffering
        "-g", str(int(FPS)),       # GOP length ~1 second (more frequent keyframes)
        "-b:v", BITRATE,

        # Repeat SPS/PPS headers more often (typically with keyframes).
        # This makes streaming more robust if the client starts mid-stream or drops data.
        "-x264-params", "repeat-headers=1",

        # Output raw Annex-B H.264 elementary stream on stdout
        "-f", "h264",
        "pipe:1",
    ]

    return subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=0)


# ------------------------ Server main loop ------------------------

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <video_file>")
        sys.exit(1)

    video_path = sys.argv[1]
    proc = ffmpeg_h264_stream(video_path)
    assert proc.stdout is not None

    # Create TCP server socket
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(1)

    print(f"[server] Listening on 0.0.0.0:{PORT}")
    print("[server] Waiting for client...")

    conn, addr = srv.accept()
    print(f"[server] Client connected from {addr}")

    # ---------------- handshake ----------------
    # Expect exactly 4 bytes: b"H264"
    hello = conn.recv(4)
    if hello != b"H264":
        print(f"[server] Bad handshake: got {hello!r}, expected b'H264'")
        conn.close()
        srv.close()
        proc.kill()
        return

    conn.sendall(b"OKAY")
    print("[server] Handshake OK (H264 <-> OKAY)")

    # Rolling buffer accumulates ffmpeg bytes until we can split complete NALs.
    rolling = b""
    # Queue of complete NAL units ready to send. Each item INCLUDES start code.
    nal_queue: List[bytes] = []

    try:
        while True:
            # Wait for 1-byte request from client.
            req = conn.recv(1)
            if not req:
                print("[server] Client disconnected.")
                break

            if req != b"N":
                print(f"[server] Unexpected request byte: {req!r}")
                break

            # Ensure at least one complete NAL is available.
            while not nal_queue:
                chunk = proc.stdout.read(8192)
                if not chunk:
                    # ffmpeg ended => send end-of-stream marker
                    conn.sendall(struct.pack(">I", 0))
                    print("[server] End-of-stream sent (length=0).")
                    return

                rolling += chunk

                # Parse as many complete NALs as we can from rolling.
                rolling = enqueue_complete_nalus(rolling, nal_queue)

                # Safety: if we haven't found start codes in a while, keep memory bounded.
                if len(rolling) > MAX_ROLLING_BUFFER:
                    # Keep only the tail; if start codes exist, they should show up soon.
                    rolling = rolling[-MAX_ROLLING_BUFFER:]

            nal = nal_queue.pop(0)

            # Optional debug: print NAL type for learning.
            # NAL type is in the first byte of payload, AFTER the start code.
            sc_len = 4 if nal.startswith(START4) else 3
            if len(nal) > sc_len:
                nal_type = nal[sc_len] & 0x1F
                # Common types:
                # 7=SPS, 8=PPS, 5=IDR (keyframe), 1=non-IDR slice, 6=SEI
                # We log occasionally without being too spammy.
                # print(f"[server] Sending NAL type={nal_type}, bytes={len(nal)}")

            # Send length-prefixed NAL (big-endian u32), then send bytes.
            conn.sendall(struct.pack(">I", len(nal)))
            conn.sendall(nal)

    finally:
        conn.close()
        srv.close()
        proc.kill()
        print("[server] Clean shutdown.")


if __name__ == "__main__":
    main()