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

//
// ----------- Networking constants -----------
// Change this to match your Python server port.
//
#define SERVER_PORT 5000

//
// We will request the stream one NAL unit at a time.
// This keeps the example simple and avoids buffering logic.
//
#define REQUEST_BYTE 'N'

//
// Max size of a single NAL unit payload we will accept.
// The devkitPro mvd example caps NAL sizes at 0x100000 (1 MiB). :contentReference[oaicite:4]{index=4}
// Keeping the same cap makes it easier to compare behavior.
//
#define NAL_MAX_BYTES 0x100000

//
// SOC (socket) service needs a dedicated buffer.
// 1 MiB is a common “safe” size for homebrew TCP usage.
//
#define SOC_BUFFER_SIZE (1 * 1024 * 1024)

//
// ----------- Helpers -----------
//

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

// Read exactly `len` bytes from a TCP socket (or return <0 on error).
static int recv_all(int sock, void* out, size_t len) {
    u8* p = (u8*)out;
    size_t got = 0;
    while (got < len) {
        int r = recv(sock, p + got, (int)(len - got), 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

// Write exactly `len` bytes to a TCP socket (or return <0 on error).
static int send_all(int sock, const void* data, size_t len) {
    const u8* p = (const u8*)data;
    size_t sent = 0;
    while (sent < len) {
        int r = send(sock, p + sent, (int)(len - sent), 0);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return 0;
}

// Read a big-endian u32 sent by the server.
static int recv_u32_be(int sock, u32* out) {
    u8 b[4];
    if (recv_all(sock, b, 4) < 0) return -1;
    *out = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
    return 0;
}

int main(int argc, char** argv) {
    // --- Basic 3DS init ---
    gfxInit(GSP_RGB565_OES, GSP_BGR8_OES, false);
    consoleInit(GFX_BOTTOM, NULL);

    printf("H.264 stream demo (New 2DS XL / MVD hardware decode)\n");
    printf("A: Enter IP Address");
    printf("B: quit\n\n");

    // --- Init SOC (sockets) ---
    // socInit requires a physically contiguous buffer; memalign(0x1000) is typical.
    u32* socBuffer = (u32*)memalign(0x1000, SOC_BUFFER_SIZE);

    if (!socBuffer) {
        printf("Failed to allocate SOC buffer.\n");
        goto cleanup_gfx;
    }
    Result rc = socInit(socBuffer, SOC_BUFFER_SIZE);

    if (R_FAILED(rc)) {
        printf("socInit failed: 0x%08lX\n", rc);
        goto cleanup_socbuf;
    }

    // --- Allocate decoder input buffer in linear memory ---
    // MVD / GPU-friendly paths often want linear memory.
    // Alignment 0x40 matches what the devkitPro example uses. :contentReference[oaicite:5]{index=5}
    u8* nalBuf = (u8*)linearMemAlign(NAL_MAX_BYTES, 0x40);
    if (!nalBuf) {
        printf("Failed to allocate NAL buffer.\n");
        goto cleanup_soc;
    }
    
    // --- Connect to the server ---
    // We’ll ask the user to type the IP on the bottom screen console,
    // similar to your image receiver flow.
    //char ipStr[64] = {0};
    //printf("Enter server IP (e.g. 192.168.1.25):\n> ");
    //fflush(stdout);
    //scanf("%63s", ipStr);

    //int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    //if (sock < 0) {
    //    printf("socket() failed\n");
    //    goto cleanup_nal;
    //}

    //struct sockaddr_in sa;
    //memset(&sa, 0, sizeof(sa));
    //sa.sin_family = AF_INET;
    //sa.sin_port = htons(SERVER_PORT);
    //sa.sin_addr.s_addr = inet_addr(ipStr);

    //printf("Connecting to %s:%d...\n", ipStr, SERVER_PORT);
    //if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
    //    printf("connect() failed\n");
    //    goto cleanup_sock;
    //}

    int sock = -1; // Socket handle
    char ip[64] = {0}; // Buffer that stores the IP address entered by user

    //loop to get server IP
    while(true){
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

            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (sock < 0) {
                printf("socket() failed\n");
                goto cleanup_nal;
                break;
            }

            // Prepare address structure
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(SERVER_PORT);
            sa.sin_addr.s_addr = inet_addr(ip);

            // Attempt connection
            printf("Connecting to %s:%d...\n", ip, SERVER_PORT);
            if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
                printf("connect() failed\n");
                goto cleanup_sock;
                break;
            }
            //if it gets to the end, continue past the loop
            break;
        }
    }
    // --- Handshake ---
    // 3DS -> "H264"
    // Server -> "OKAY"
    const char hello[4] = {'H','2','6','4'};
    if (send_all(sock, hello, 4) < 0) {
        printf("Failed to send handshake.\n");
        goto cleanup_sock;
    }

    char reply[4];
    if (recv_all(sock, reply, 4) < 0) {
        printf("Failed to read handshake reply.\n");
        goto cleanup_sock;
    }
    if (memcmp(reply, "OKAY", 4) != 0) {
        printf("Bad handshake reply: %c%c%c%c\n", reply[0], reply[1], reply[2], reply[3]);
        goto cleanup_sock;
    }
    printf("Handshake OK.\n");

    // --- Initialize MVD hardware decode ---
    //
    // This is the New 3DS/New 2DS hardware decoder service. :contentReference[oaicite:6]{index=6}
    // If this fails with a permission error, you likely don't have access to mvd:STD. :contentReference[oaicite:7]{index=7}
    //
    // Mode: video processing
    // Input: H.264 elementary stream
    // Output: BGR565 (matches framebuffer pixel format we’re using)
    //
    rc = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, MVD_DEFAULT_WORKBUF_SIZE, NULL);

    printf("mvdstdInit: 0x%08lX\n", rc);
    if (R_FAILED(rc)) {
        printf("\nIf this fails, it's often missing mvd:STD access.\n");
        goto cleanup_sock;
    }

    // --- Configure output dimensions ---
    //
    // We set 240x400, because we will render directly into the TOP framebuffer memory layout
    // (the same trick used by the devkitPro MVD example). :contentReference[oaicite:8]{index=8}
    //
    // Your Python server will:
    //   1) scale/pad to 400x240 (pillarbox),
    //   2) transpose to 240x400,
    // so what the decoder outputs matches what the framebuffer expects.
    //
    MVDSTD_Config cfg;
    mvdstdGenerateDefaultConfig(&cfg,
        240, 400,          // input width/height (what the bitstream represents after transpose)
        240, 400,          // output width/height
        NULL,              // input phys addr is supplied per-frame to mvdstdProcessVideoFrame
        NULL, NULL);

    // --- Decode loop ---
    bool quit = false;
    while (aptMainLoop() && !quit) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_B) quit = true;

        // Ask the server for the next NAL unit (1 byte request).
        char req = REQUEST_BYTE;
        if (send_all(sock, &req, 1) < 0) {
            printf("Socket send failed.\n");
            break;
        }

        // Server sends big-endian u32 length. length=0 means end-of-stream.
        u32 nalLen = 0;
        if (recv_u32_be(sock, &nalLen) < 0) {
            printf("Socket recv length failed.\n");
            break;
        }
        if (nalLen == 0) {
            printf("End of stream.\n");
            break;
        }
        if (nalLen > NAL_MAX_BYTES) {
            printf("NAL too big: %lu bytes\n", (unsigned long)nalLen);
            break;
        }

        // Read the NAL payload bytes.
        if (recv_all(sock, nalBuf, nalLen) < 0) {
            printf("Socket recv payload failed.\n");
            break;
        }

        // Flush CPU cache so the MVD hardware sees the updated input buffer.
        // This matters because the decoder is a hardware unit doing DMA reads.
        GSPGPU_FlushDataCache(nalBuf, nalLen);

        // Feed this NAL to the decoder.
        // flagval = 0 for now (keep simple).
        u32 flagval = 0;
        MVDSTD_ProcessNALUnitOut procOut;
        Result dec = mvdstdProcessVideoFrame(nalBuf, nalLen, flagval, &procOut);

        // Many calls return statuses like:
        // - MVD_STATUS_PARAMSET (SPS/PPS etc)
        // - MVD_STATUS_INCOMPLETEPROCESSING (needs more NALs before a frame is ready)
        // When a frame IS ready, the status is typically not those values, and we render.
        if (MVD_CHECKNALUPROC_SUCCESS(dec) &&
            dec != MVD_STATUS_PARAMSET &&
            dec != MVD_STATUS_INCOMPLETEPROCESSING)
        {
            // Render directly into the top framebuffer to avoid software rotation/copy.
            u8* fbTop = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
            cfg.physaddr_outdata0 = osConvertVirtToPhys(fbTop);

            Result rend = mvdstdRenderVideoFrame(&cfg, true);
            if (rend != MVD_STATUS_OK) {
                printf("Render error: 0x%08lX\n", rend);
                break;
            }

            // Present frame.
            gfxSwapBuffersGpu();
        }
        else if (!MVD_CHECKNALUPROC_SUCCESS(dec)) {
            printf("Decode error: 0x%08lX (remaining=0x%08lX)\n", dec, procOut.remaining_size);
            break;
        }

        // Optional: throttle slightly so the console/UI stays responsive.
        // gspWaitForVBlank() is OK here because we're not trying to max throughput yet.
        gspWaitForVBlank();
    }

    // --- Cleanup ---
    printf("Cleaning up...");
    mvdstdExit();
    
    //for debug
    while(true){
        hidScanInput();
        u32 kd = hidKeysDown();
        if (kd & KEY_START) {
            printf("Stopping stream.\n");
            break;
        }
    }

cleanup_sock:
    close(sock);

cleanup_nal:
    linearFree(nalBuf);

cleanup_soc:
    socExit();

cleanup_socbuf:
    free(socBuffer);

cleanup_gfx:
    gfxExit();
    return 0;
}