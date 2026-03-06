"""
Convenience launcher:
- starts the H.264 streaming server
- waits briefly
- starts the rendering client pointing at localhost

Usage:
python run_local_test.py "C:\\path\\to\\video.mp4"
"""

import subprocess
import sys
import time

PORT = "5000"

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <video_file>")
        return

    video_path = sys.argv[1]

    # Start server in a separate process
    server = subprocess.Popen(
        [sys.executable, "PythonServer/stream_h264_to_3ds.py", video_path],
        stdout=None, stderr=None
    )

    # Give server a moment to bind and listen
    time.sleep(0.5)

    try:
        # Run client in the foreground (so Ctrl+C stops it)
        subprocess.run(
            [sys.executable, "PythonServer/test_h264_stream.py", "localhost", PORT],
            check=False
        )
    finally:
        # Clean up server if it's still running
        server.terminate()
        server.wait(timeout=2)

if __name__ == "__main__":
    main()