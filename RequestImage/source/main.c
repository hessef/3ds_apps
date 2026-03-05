// This is just a simple script that connects to a specified IP address and then displays an image that is sent back

// Main 3DS system library (graphics, input, networking helpers, etc.)
#include <3ds.h>

// Standard C libraries
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

// Networking libraries
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

// Libraries for looping
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>

// ------------------------------------------------------------
// stb_image is a lightweight image decoder that supports PNG/JPG
// We use it to decode the image bytes received from the server.
// ------------------------------------------------------------
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

// ------------------------------------------------------------
// Constants describing the 3DS top screen resolution
// ------------------------------------------------------------
#define TOP_W 400
#define TOP_H 240

// TCP port used to communicate with the server
#define SERVER_PORT 6000

// ------------------------------------------------------------
// recv_all()
// Helper function that ensures we receive EXACTLY N bytes.
// TCP may deliver data in chunks, so one recv() call might
// not give the full message. This loops until everything
// arrives or the connection fails.
// ------------------------------------------------------------
static int recv_all(int sock, void* buf, int len) {
    u8* p = (u8*)buf;
    int got = 0;
    while (got < len) {
        int r = recv(sock, p + got, len - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return got;
}

// ------------------------------------------------------------
// Wait until the socket has data to read (or timeout_ms passes).
// Returns: 1 = readable, 0 = timeout, -1 = error
// ------------------------------------------------------------
static int wait_readable(int sock, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int r = select(sock + 1, &rfds, NULL, NULL, &tv);
    return r; // 1, 0, or -1
}

// ------------------------------------------------------------
// blit_rgb_to_top_bgr8()
// Draws an RGB image to the 3DS top screen framebuffer.
//
// The framebuffer format used by libctru is BGR8.
// The memory layout is also rotated relative to normal
// image coordinates, so the index math looks unusual.
// ------------------------------------------------------------
static void blit_rgb_to_top_bgr8(const u8* rgb, int w, int h) {

    // Get pointer to the raw framebuffer memory
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);

    int drawW = (w < TOP_W) ? w : TOP_W;
    int drawH = (h < TOP_H) ? h : TOP_H;

    // Clear screen to black
    memset(fb, 0, TOP_W * TOP_H * 3);

    // Copy pixels from decoded image into framebuffer
    for (int y = 0; y < drawH; y++) {
        for (int x = 0; x < drawW; x++) {
            int src = (y * w + x) * 3;
            u8 r = rgb[src + 0];
            u8 g = rgb[src + 1];
            u8 b = rgb[src + 2];

            // Convert coordinates into framebuffer layout
            int dst = (x * TOP_H + (TOP_H - 1 - y)) * 3;
            fb[dst + 0] = b;
            fb[dst + 1] = g;
            fb[dst + 2] = r;
        }
    }
}

// ------------------------------------------------------------
// prompt_ip()
// Displays the 3DS software keyboard on the touchscreen
// so the user can enter the server's IP address.
// ------------------------------------------------------------
static bool prompt_ip(char* outIp, size_t outSize) {
    SwkbdState swkbd;

    // Initialize keyboard
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 1, (int)outSize);

    // Text shown above the keyboard
    swkbdSetHintText(&swkbd, "Enter server IP (e.g., 192.168.1.50)");

    // Prevent empty input
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);

    // Display keyboard and wait for input
    SwkbdButton btn = swkbdInputText(&swkbd, outIp, outSize);

    // Return true if user pressed OK
    return (btn == SWKBD_BUTTON_RIGHT); // "OK"
}

// ------------------------------------------------------------
// MAIN PROGRAM
// ------------------------------------------------------------
int main(int argc, char* argv[]) {

    // Initialize graphics system
    gfxInitDefault();

    // Enable text console on bottom screen
    consoleInit(GFX_BOTTOM, NULL);

    // --------------------------------------------------------
    // Initialize networking system
    //
    // The 3DS network stack requires a buffer that is
    // aligned to 4096 bytes (0x1000).
    // --------------------------------------------------------
    static u32* socBuffer = NULL;
    socBuffer = (u32*)memalign(0x1000, 0x100000); // 1 MB
    if (!socBuffer) {
        printf("Failed to alloc SOC buffer.\n");
        printf("Press START.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        gfxExit();
        return 0;
    }

    // Initialize SOC service
    if (socInit(socBuffer, 0x100000) != 0) {
        printf("socInit failed.\n");
        printf("Press START.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        free(socBuffer);
        gfxExit();
        return 0;
    }

    // Buffer that stores the IP address entered by user
    char ip[64] = {0};

    printf("3DS Image Client\n");
    printf("Press A to enter IP.\n");
    printf("Press START to exit.\n\n");

    // Socket handle
    int sock = -1;

    // --------------------------------------------------------
    // Main application loop
    // --------------------------------------------------------
    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown();

        if (kd & KEY_START) break;

        // ----------------------------------------------------
        // Press A → ask for IP and connect to server
        // ----------------------------------------------------
        if ((kd & KEY_A) && sock < 0) {
            if (!prompt_ip(ip, sizeof(ip))) {
                printf("IP entry cancelled.\n");
                continue;
            }
            printf("IP: %s\n", ip);
            printf("Connecting to %s:%d...\n", ip, SERVER_PORT);

            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                printf("socket() failed.\n");
                sock = -1;
                continue;
            }

            // Prepare address structure
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(SERVER_PORT);

            // Convert IP string to binary format
            if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
                printf("Invalid IP format.\n");
                close(sock);
                sock = -1;
                continue;
            }

            // Attempt connection
            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
                printf("connect() failed.\n");
                close(sock);
                sock = -1;
                continue;
            }

            // ------------------------------------------------
            // Handshake with server
            // ------------------------------------------------

            // Send HELLO
            const char* hello = "HELLO\n";
            send(sock, hello, (int)strlen(hello), 0);

            // Wait for "OK\n"
            char okbuf[8] = {0};
            int r = recv(sock, okbuf, sizeof(okbuf)-1, 0);
            if (r <= 0 || strstr(okbuf, "OK") == NULL) {
                printf("Handshake failed. Got: %s\n", okbuf);
                close(sock);
                sock = -1;
                continue;
            }

            printf("Connected. Waiting for image...\n");

            printf("Streaming mode: Press START to exit streaming.\n");

            // Stream loop: keep receiving frames until exit/disconnect
            while (aptMainLoop()) {
                // Allow user to exit
                hidScanInput();
                u32 kd = hidKeysDown();
                if (kd & KEY_START) {
                    printf("Stopping stream.\n");
                    break;
                }

                // Wait briefly for incoming data so we can keep checking input
                int wr = wait_readable(sock, 100); // 100ms
                if (wr < 0) {
                    printf("select() error. Disconnecting.\n");
                    break;
                }
                if (wr == 0) {
                    // no data yet, keep looping
                    gspWaitForVBlank();
                    continue;
                }

                // 1) Read 4-byte length
                u32 netLen = 0;
                if (recv_all(sock, &netLen, 4) != 4) {
                    printf("Server disconnected (len).\n");
                    break;
                }
                u32 imgLen = ntohl(netLen);

                if (imgLen == 0 || imgLen > 5 * 1024 * 1024) {
                    printf("Bad frame length: %lu\n", (unsigned long)imgLen);
                    break;
                }

                // 2) Read image bytes
                u8* imgData = (u8*)malloc(imgLen);
                if (!imgData) {
                    printf("malloc failed for frame.\n");
                    break;
                }

                if (recv_all(sock, imgData, (int)imgLen) != (int)imgLen) {
                    printf("Server disconnected (frame).\n");
                    free(imgData);
                    break;
                }

                // 3) Decode
                int w = 0, h = 0, comp = 0;
                u8* rgb = stbi_load_from_memory(imgData, (int)imgLen, &w, &h, &comp, 3);
                free(imgData);

                if (!rgb) {
                    printf("Decode failed: %s\n", stbi_failure_reason());
                    // Don’t necessarily kill the stream; you can continue if you want:
                    // continue;
                    break;
                }

                // 4) Display
                blit_rgb_to_top_bgr8(rgb, w, h);
                stbi_image_free(rgb);

                gfxFlushBuffers();
                gfxSwapBuffers();

                gspWaitForVBlank();
            }

            printf("Leaving streaming mode.\n");

            printf("Done. Press B to disconnect.\n");
        }

        // ----------------------------------------------------
        // Press B → close connection
        // ----------------------------------------------------
        if ((kd & KEY_B) && sock >= 0) {
            printf("Disconnecting...\n");
            close(sock);
            sock = -1;
            printf("Disconnected.\n");
        }

        // Wait for next frame (VBlank)
        gspWaitForVBlank();
    }

    // Cleanup
    if (sock >= 0) close(sock);
    socExit();
    free(socBuffer);
    gfxExit();
    return 0;
}