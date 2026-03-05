"""
render_h264_stream_client.py

Purpose
-------
Connects to the streaming server (same handshake/protocol as the 3DS),
requests H.264 NAL units, and *renders the video* on a laptop.

How it works
------------
1) TCP connect to server
2) Send 4-byte magic b"H264"
3) Receive 4-byte reply b"OKAY"
4) Loop:
   - Send b"N" to request the next chunk (one NAL unit, including start code)
   - Receive u32 length (big-endian)
   - Receive `length` bytes (the NAL unit)
   - Write those bytes into an ffmpeg subprocess stdin

5) ffmpeg decodes the H.264 stream and outputs raw video frames on stdout.
6) We read exactly one frame's worth of bytes per frame and display it with OpenCV.

Why use ffmpeg?
---------------
H.264 decoding is complicated. ffmpeg already handles:
- SPS/PPS parsing
- reordering, reference frames
- bytestream (Annex B) parsing
- error resilience

So this client focuses on verifying your network framing is correct.

Important about orientation
---------------------------
Your server pipeline includes: transpose=1 (rotate 90° clockwise)
to match the 3DS framebuffer trick.

For laptop display, we want the normal orientation:
- Invert transpose=1 by applying transpose=2 (rotate 90° counterclockwise)
- Then we display at 400x240.

So on the ffmpeg decode path, we apply -vf transpose=2 and output 400x240.
"""

import socket
import struct
import subprocess
import sys
import threading
import time

import numpy as np
import cv2

# -------------------- Protocol constants --------------------
REQUEST_BYTE = b"N"
HELLO = b"H264"
OKAY = b"OKAY"

# -------------------- Video dimensions (what we want to DISPLAY) --------------------
# The server scales/pads to 400x240, then transposes to 240x400 for 3DS-friendly layout.
# To display normally, we decode and apply transpose=2 to rotate back to 400x240.
DISPLAY_W = 400
DISPLAY_H = 240
BYTES_PER_PIXEL = 3  # bgr24 (OpenCV's default channel order)
FRAME_BYTES = DISPLAY_W * DISPLAY_H * BYTES_PER_PIXEL

# -------------------- Socket helpers --------------------

def recv_all(sock: socket.socket, n: int) -> bytes:
    """
    Receive exactly n bytes from a TCP socket.
    TCP is a stream: a single recv() might return fewer bytes than requested,
    so we loop until we've got everything or the socket closes.
    """
    out = bytearray()
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise RuntimeError("Socket closed while receiving.")
        out += chunk
    return bytes(out)

def recv_u32_be(sock: socket.socket) -> int:
    """Receive a big-endian unsigned 32-bit integer."""
    b = recv_all(sock, 4)
    return struct.unpack(">I", b)[0]

# -------------------- ffmpeg decode subprocess --------------------

def start_ffmpeg_decoder() -> subprocess.Popen:
    """
    Start ffmpeg such that:
      - Input is raw H.264 Annex-B bytestream from stdin (pipe:0)
      - Decode video
      - Apply transpose=2 to undo the server's transpose=1
      - Output raw frames as bgr24 to stdout (pipe:1)

    Notes:
    - `-fflags nobuffer` and `-flags low_delay` help reduce latency.
    - `-an` disables audio (we're ignoring it here).
    """
    cmd = [
        "ffmpeg",
        "-loglevel", "error",

        # Low-latency-ish flags. Not mandatory, but helpful for streaming.
        "-fflags", "nobuffer",
        "-flags", "low_delay",

        # Input is H.264 elementary stream from stdin.
        "-f", "h264",
        "-i", "pipe:0",

        # Undo server transpose=1 (clockwise) by applying transpose=2 (counterclockwise)
        "-vf", "transpose=2",

        # Output raw frames in BGR24 (matches OpenCV).
        "-f", "rawvideo",
        "-pix_fmt", "bgr24",

        # No audio output
        "-an",

        # Write raw frames to stdout
        "pipe:1",
    ]

    # bufsize=0 to make pipes as immediate as possible
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)

# -------------------- Network → ffmpeg feeder thread --------------------

def feeder_thread(sock: socket.socket, ff: subprocess.Popen, stop_flag: threading.Event):
    """
    Runs in a background thread.

    Job:
    - Request NAL units from the server
    - Push them into ffmpeg's stdin as a continuous bytestream

    Why a separate thread?
    - ffmpeg decoding and frame display loop runs in main thread
    - this thread continuously feeds compressed data to keep the decoder happy

    IMPORTANT:
    - We send NAL units INCLUDING start codes. This makes a valid Annex-B bytestream.
    - If server sends length=0, that's end-of-stream.
    """
    assert ff.stdin is not None

    try:
        while not stop_flag.is_set():
            # Request the next chunk from server
            sock.sendall(REQUEST_BYTE)

            # Read length prefix
            length = recv_u32_be(sock)
            if length == 0:
                # End-of-stream
                break

            # Read NAL bytes
            nal = recv_all(sock, length)

            # Write into ffmpeg input pipe
            # This turns "NAL packets" into a continuous H.264 bytestream for ffmpeg.
            ff.stdin.write(nal)

            # Flush occasionally so data doesn't sit in Python buffer too long.
            # (Flushing every time is okay for a demo; for max performance you might batch.)
            ff.stdin.flush()

    except Exception as e:
        # If something breaks (server disconnect etc.), signal main thread to stop.
        print(f"[feeder] stopped: {e}")
    finally:
        stop_flag.set()
        try:
            # Close stdin to tell ffmpeg "no more input"
            if ff.stdin:
                ff.stdin.close()
        except Exception:
            pass

# -------------------- Main: connect, handshake, render --------------------

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <server_ip> [port]")
        sys.exit(1)

    server_ip = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) >= 3 else 5000

    # ---- Connect to server ----
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((server_ip, port))
    print(f"[client] Connected to {server_ip}:{port}")

    # ---- Handshake ----
    sock.sendall(HELLO)
    reply = recv_all(sock, 4)
    if reply != OKAY:
        print(f"[client] Bad handshake reply: {reply!r}")
        sock.close()
        return
    print("[client] Handshake OK.")

    # ---- Start ffmpeg decoder ----
    ff = start_ffmpeg_decoder()
    assert ff.stdout is not None
    assert ff.stdin is not None

    # ---- Start feeder thread ----
    stop_flag = threading.Event()
    t = threading.Thread(target=feeder_thread, args=(sock, ff, stop_flag), daemon=True)
    t.start()

    # ---- Display loop ----
    cv2.namedWindow("H264 Stream", cv2.WINDOW_NORMAL)
    cv2.resizeWindow("H264 Stream", DISPLAY_W * 2, DISPLAY_H * 2)

    frames = 0
    last_time = time.time()

    try:
        while not stop_flag.is_set():
            # Read exactly one frame worth of raw bytes from ffmpeg stdout.
            # If we get fewer bytes, ffmpeg likely ended or is starved.
            raw = ff.stdout.read(FRAME_BYTES)
            if not raw or len(raw) < FRAME_BYTES:
                # If feeder stopped, this is expected. Otherwise, might be decode starvation.
                break

            # Convert raw bytes to numpy image:
            # - shape: (H, W, 3)
            # - dtype: uint8
            frame = np.frombuffer(raw, dtype=np.uint8).reshape((DISPLAY_H, DISPLAY_W, 3))

            # Show
            cv2.imshow("H264 Stream", frame)

            # Handle UI events. Press 'q' to quit.
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                stop_flag.set()
                break

            # FPS stats (optional)
            frames += 1
            now = time.time()
            if now - last_time >= 2.0:
                fps = frames / (now - last_time)
                print(f"[client] approx FPS: {fps:.1f}")
                frames = 0
                last_time = now

    finally:
        stop_flag.set()
        try:
            sock.close()
        except Exception:
            pass

        try:
            # Terminate ffmpeg cleanly
            ff.terminate()
        except Exception:
            pass

        cv2.destroyAllWindows()
        print("[client] Done.")

if __name__ == "__main__":
    main()