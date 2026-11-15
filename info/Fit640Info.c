/*
 * Fit640 Info - shows what the Fit640 extension did and what programs
 * now see as the main screen. Console app; press Return to quit.
 */
#include <stdio.h>
#include <stdarg.h>
#include <Quickdraw.h>
#include <Gestalt.h>
#include <Events.h>
#include "../src/Fit640.h"

#define LM_MainDevice (*(GDHandle *)0x08A4)
#define LM_GrayRgn    (*(RgnHandle *)0x09EE)
#define LM_Ticks      (*(volatile unsigned long *)0x016A)

/* Everything is printed to the window and also saved to the text file
   "Fit640 Log" next to the app. */
static FILE *gLog;

static void out(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (gLog) {
        va_start(ap, fmt);
        vfprintf(gLog, fmt, ap);
        va_end(ap);
    }
}

static const char *StatusText(short s)
{
    switch (s) {
    case kF640Active:     return "active";
    case kF640NoColorQD:  return "not installed: no Color QuickDraw";
    case kF640Mode24:     return "not installed: 24-bit addressing is on";
    case kF640ScreenSize: return "not installed: screen is not smaller than 640x480";
    case kF640Depth:      return "not installed: more than 16-bit color";
    case kF640NoMemory:   return "not installed: no memory for the virtual screen";
    case kF640NoVBL:      return "not installed: could not start the refresh task";
    case kF640Dormant:    return "dormant: the screen was reset (depth change?)";
    case kF640Installing: return "startup did not finish";
    }
    return "unknown";
}

static void PrintBench(const char *label, short frames, short ticks)
{
    if (frames < 0)
        out("  %-18s not measured\n", label);
    else
        out("  %-18s %d frames in %d ticks = %ld fps\n", label, frames,
               ticks, (long)frames * 60 / ticks);
}

int main(void)
{
    long response;
    GDHandle gd = LM_MainDevice;
    PixMapPtr pm = *(*gd)->gdPMap;
    Rect r;

    gLog = fopen("Fit640 Log", "w");
    out("Fit640 Info\n");
    if (Gestalt(gestaltSystemVersion, &response) == noErr)
        out("System %lx.%lx.%lx\n", (response >> 8) & 0xFF,
            (response >> 4) & 0xF, response & 0xF);
    out("\n");

    out("What programs see:\n");
    r = qd.screenBits.bounds;
    out("  screenBits.bounds  %d x %d\n", r.right - r.left, r.bottom - r.top);
    r = (*gd)->gdRect;
    out("  main device rect   %d x %d\n", r.right - r.left, r.bottom - r.top);
    out("  depth %d, rowBytes %d, base $%08lX\n", pm->pixelSize,
           pm->rowBytes & 0x3FFF, (unsigned long)pm->baseAddr);
    if (LM_GrayRgn) {
        r = (*LM_GrayRgn)->rgnBBox;
        out("  desktop region     %d,%d - %d,%d\n", r.left, r.top, r.right, r.bottom);
        out("  menu bar height    %d\n", *(short *)0x0BAA);
    }

    {
        GrafPtr wp = *(GrafPtr *)0x09DE;
        CGrafPtr wc = *(CGrafPtr *)0x0D2C;
        RgnHandle gray = LM_GrayRgn;

        out("\nWindow system:\n");
        if (wp) {
            r = wp->portRect;
            out("  WMgrPort  rect %d,%d-%d,%d base $%08lX rowBytes $%04X\n",
                r.left, r.top, r.right, r.bottom,
                (unsigned long)wp->portBits.baseAddr,
                (unsigned short)wp->portBits.rowBytes);
            r = (*wp->visRgn)->rgnBBox;
            out("            vis %d,%d-%d,%d", r.left, r.top, r.right, r.bottom);
            r = (*wp->clipRgn)->rgnBBox;
            out(" clip %d,%d-%d,%d\n", r.left, r.top, r.right, r.bottom);
        }
        if (wc) {
            PixMapPtr wpm = *wc->portPixMap;
            r = wc->portRect;
            out("  WMgrCPort rect %d,%d-%d,%d base $%08lX rowBytes $%04X\n",
                r.left, r.top, r.right, r.bottom,
                (unsigned long)wpm->baseAddr, (unsigned short)wpm->rowBytes);
            r = wpm->bounds;
            out("            bounds %d,%d-%d,%d", r.left, r.top, r.right, r.bottom);
            r = (*wc->visRgn)->rgnBBox;
            out(" vis %d,%d-%d,%d\n", r.left, r.top, r.right, r.bottom);
        }
        if (gray)
            out("  desktop region size %d bytes\n", (*gray)->rgnSize);
    }

    out("\nExtension:\n");
    if (Gestalt(kFit640Selector, &response) != noErr || response == 0) {
        out("  Fit640 is not loaded.\n");
    } else {
        volatile Fit640Globals *g = (volatile Fit640Globals *)response;
        FILE *hook;
        unsigned long f0, t0;

        /* Test hook: an (empty) file named "Fit640 Scale" next to the app
           switches to scale mode. */
        hook = fopen("Fit640 Scale", "r");
        if (hook) {
            fclose(hook);
            if (g->canScale)
                g->mode = kModeScale;
        }

        /* Test hook: a file named "Fit640 Slow" makes the extension redraw
           in small slices, as it would on a slow machine. */
        hook = fopen("Fit640 Slow", "r");
        if (hook) {
            fclose(hook);
            if (g->version >= 2)
                g->cropRowsPerTick = g->scaleRowsPerTick = 32;
        }

        out("  status: %s\n", StatusText(g->status));
        out("  CPU type %d (5 = 68040), MOVE16 copy %s\n", g->cpu,
               g->useMove16 ? "on" : "off");
        out("  real screen %d x %d, %d-bit, rowBytes %ld, base $%08lX\n",
               g->realW, g->realH, g->depth, g->realRow,
               (unsigned long)g->realBase);
        out("  virtual screen at $%08lX, rowBytes %ld\n",
               (unsigned long)g->virtBase, g->virtRow);
        out("  mode: %s%s, view at %d,%d\n",
               g->mode == kModeScale ? "scale" : "crop",
               g->canScale ? "" : " (scale unavailable)", g->viewX, g->viewY);
        if (g->version >= 2) {
            out("  view %s\n", g->followMouse ? "follows the mouse"
                                               : "locked on the centre");
            out("  window system ports redirected: %d at startup, %d later\n",
                g->portsFixed - g->portsFixedLate, g->portsFixedLate);
        }

        out("\nStartup speed test (whole-screen redraws):\n");
        PrintBench("crop, plain copy", g->benchCropLong, g->benchTicks);
        PrintBench("crop, MOVE16", g->benchCropMove16, g->benchTicks);
        PrintBench("scale 5:4", g->benchScale, g->benchTicks);

        if (g->version >= 2)
            out("  redrawing %d of %d rows per tick (crop), %d (scale)\n",
                g->cropRowsPerTick, g->realH, g->scaleRowsPerTick);

        f0 = g->framesPresented;
        t0 = LM_Ticks;
        while (LM_Ticks - t0 < 60)
            ;
        out("\nRefreshing now at %lu frames per second.\n",
               g->framesPresented - f0);
    }

    if (gLog) {
        fclose(gLog);
        gLog = NULL;
    }
    if (Gestalt(kFit640Selector, &response) == noErr && response != 0) {
        volatile Fit640Globals *g = (volatile Fit640Globals *)response;
        int c;

        printf("\nType S then Return to switch crop/scale,\n"
               "C then Return to lock the view on the centre (again to\n"
               "follow the mouse), Return alone to quit.\n");
        while ((c = getchar()) != EOF) {
            if (c == 's' || c == 'S') {
                if (g->canScale)
                    g->mode = (g->mode == kModeScale) ? kModeCrop : kModeScale;
                else
                    printf("Scale needs 256 colors on a 512x384 screen.\n");
            } else if (c == 'c' || c == 'C') {
                g->followMouse = !g->followMouse;
                if (!g->followMouse) {
                    g->viewX = ((kVirtW - g->realW) / 2) & ~15;
                    g->viewY = (kVirtH - g->realH) / 2;
                }
            } else if (c == '\n' || c == '\r') {
                break;
            }
            while (c != '\n' && c != '\r' && c != EOF)
                c = getchar();
        }
        return 0;
    }

    printf("\nPress Return to quit.\n");
    getchar();
    return 0;
}
