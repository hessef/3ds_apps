// This is just a simple script that prints out when a button is pressed

#include <3ds.h>
#include <stdio.h>
#include <math.h>

// Helper: print which keys are in a bitmask.
static void printKeys(u32 k, const char* prefix) {
    if (!k) return;

    printf("%s", prefix);

    if (k & KEY_A)      printf(" A");
    if (k & KEY_B)      printf(" B");
    if (k & KEY_X)      printf(" X");
    if (k & KEY_Y)      printf(" Y");
    if (k & KEY_L)      printf(" L");
    if (k & KEY_R)      printf(" R");
    if (k & KEY_ZL)     printf(" ZL");
    if (k & KEY_ZR)     printf(" ZR");
    if (k & KEY_START)  printf(" START");
    if (k & KEY_SELECT) printf(" SELECT");

    if (k & KEY_DUP)    printf(" D-UP");
    if (k & KEY_DDOWN)  printf(" D-DOWN");
    if (k & KEY_DLEFT)  printf(" D-LEFT");
    if (k & KEY_DRIGHT) printf(" D-RIGHT");

    printf("\n");
}

int main(int argc, char* argv[])
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    printf("Button + Circle Pad demo\n");
    printf("Press START to exit.\n\n");

    circlePosition cp = {0};
    circlePosition lastCp = {0};

    while (aptMainLoop())
    {
        gspWaitForVBlank();
        gfxSwapBuffers();

        hidScanInput();

        u32 kDown = hidKeysDown();
        u32 kUp   = hidKeysUp();
        u32 kHeld = hidKeysHeld();

        // Exit
        if (kDown & KEY_START) break;

        // Print button events
        printKeys(kDown, "DOWN:");
        printKeys(kUp,   "UP:  ");

        // Read circle pad
        // libctru provides hidCircleRead() to read circle pad position. :contentReference[oaicite:4]{index=4}
        hidCircleRead(&cp);

        // Only print if it changed enough to matter
        int dx = cp.dx - lastCp.dx;
        int dy = cp.dy - lastCp.dy;
        if (dx*dx + dy*dy > 25) { // small deadband
            double x = (double)cp.dx;
            double y = (double)cp.dy;

            double angleRad = atan2(y, x);
            double angleDeg = angleRad * (180.0 / 3.14);
            if (angleDeg < 0) angleDeg += 360.0;

            double mag = sqrt(x*x + y*y);

            printf("Circle pad: dx=%d dy=%d | angle=%.1f deg | mag=%.1f\n",
                   cp.dx, cp.dy, angleDeg, mag);

            lastCp = cp;
        }
    }

    gfxExit();
    return 0;
}