#the server portion of of the image request system

import socket
import struct
from pathlib import Path
import time

HOST = "0.0.0.0"
PORT = 6000

IMAGE_PATH = Path("test.png")  # Put a PNG or JPG next to this script
IMAGE_PATH2 = Path("test2.png")  # Put a PNG or JPG next to this script

def main():
    if not IMAGE_PATH.exists():
        raise FileNotFoundError(f"Image not found: {IMAGE_PATH.resolve()}")

    img_bytes = IMAGE_PATH.read_bytes()
    print(f"Loaded {IMAGE_PATH} ({len(img_bytes)} bytes)")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(1)
        print(f"Listening on {HOST}:{PORT} ...")

        conn, addr = s.accept()
        with conn:
            print(f"Client connected from {addr}")

            data = conn.recv(1024)
            print(f"Handshake recv: {data!r}")

            conn.sendall(b"OK\n")

            #repeatedly send images, flipping between test and test2
            while True:
                img_bytes = IMAGE_PATH.read_bytes()
                conn.sendall(struct.pack("!I", len(img_bytes)))
                conn.sendall(img_bytes)
                time.sleep(0.1)  # 10 fps-ish

                #again for the second image
                img_bytes = IMAGE_PATH2.read_bytes()
                conn.sendall(struct.pack("!I", len(img_bytes)))
                conn.sendall(img_bytes)
                time.sleep(0.1)  # 10 fps-ish

if __name__ == "__main__":
    main()