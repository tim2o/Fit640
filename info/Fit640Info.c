/*
 * Fit640 Info - shows what the Fit640 extension did and what programs
 * now see as the main screen. Console app.
 *
 * The window shows a short report. The same report plus extra detail is
 * saved to the text file "Fit640 Log" next to the app.
 */
#include <stdio.h>
#include <stdarg.h>
#include <Quickdraw.h>
#include <Gestalt.h>
#include <Events.h>
#include <Windows.h>
#include <OSUtils.h>
#include "../src/Fit640.h"

#define LM_MainDevice (*(GDHandle *)0x08A4)
#define LM_GrayRgn    (*(RgnHandle *)0x09EE)
#define LM_Ticks      (*(volatile unsigned long *)0x016A)

static FILE *gLog;

/* To the window and the log file. */
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

/* To the log file only. */
static void detail(const char *fmt, ...)
{
    va_list ap;

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
    case kF640Dormant:    return "dormant: the screen was reset";
    case kF640Installing: return "startup did not finish";
    }
    return "unknown";
}

static void PrintBench(const char *label, short frames, short ticks)
{
    if (frames < 0)
        out("  %-17s not measured\n", label);
    else
        out("  %-17s %d in %d ticks\n", label, frames, ticks);
}

static int HookFile(const char *name)
{
    FILE *f = fopen(name, "r");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

/* Extra detail about the window system's drawing ports, for the log. */
static void LogWindowSystem(GDHandle gd)
{
    GrafPtr wp = *(GrafPtr *)0x09DE;
    CGrafPtr wc = *(CGrafPtr *)0x0D2C;
    WindowPeek w = *(WindowPeek *)0x09D6;
    short n = 0;
    Rect r;

    detail("\nWindow system detail:\n");
    if (LM_GrayRgn) {
        r = (*LM_GrayRgn)->rgnBBox;
        detail("  desktop region %d,%d - %d,%d, %d bytes, menu bar %d\n",
               r.left, r.top, r.right, r.bottom, (*LM_GrayRgn)->rgnSize,
               *(short *)0x0BAA);
    }
    if (wp) {
        r = wp->portRect;
        detail("  WMgrPort  rect %d,%d-%d,%d base $%08lX rowBytes $%04X\n",
               r.left, r.top, r.right, r.bottom,
               (unsigned long)wp->portBits.baseAddr,
               (unsigned short)wp->portBits.rowBytes);
    }
    if (wc) {
        PixMapPtr wpm = *wc->portPixMap;
        r = wc->portRect;
        detail("  WMgrCPort rect %d,%d-%d,%d base $%08lX rowBytes $%04X\n",
               r.left, r.top, r.right, r.bottom,
               (unsigned long)wpm->baseAddr, (unsigned short)wpm->rowBytes);
    }
    while (w && n++ < 8) {
        detail("  window $%08lX: base $%08lX rowBytes $%04X kind %d\n",
               (unsigned long)w, (unsigned long)w->port.portBits.baseAddr,
               (unsigned short)w->port.portBits.rowBytes, w->windowKind);
        w = w->nextWindow;
    }
    detail("  main device pixmap base $%08lX\n",
           (unsigned long)(*(*gd)->gdPMap)->baseAddr);
}

int main(void)
{
    long response;
    GDHandle gd = LM_MainDevice;
    PixMapPtr pm;
    Rect r, d;
    volatile Fit640Globals *g = 0;

    gLog = fopen("Fit640 Log", "w");
    out("Fit640 Info");
    if (Gestalt(gestaltSystemVersion, &response) == noErr)
        out(" - System %lx.%lx.%lx", (response >> 8) & 0xFF,
            (response >> 4) & 0xF, response & 0xF);
    out("\n");

    if (Gestalt(kFit640Selector, &response) == noErr && response != 0)
        g = (volatile Fit640Globals *)response;

    /* Test hooks: empty files next to the app.
       "Fit640 Depth" switches the color depth before reporting, the way a
       game might. "Fit640 Scale" switches to scale mode. "Fit640 Slow"
       shrinks the redraw budget, as on a slow machine. */
    if (HookFile("Fit640 Depth")) {
        short want = ((*(*gd)->gdPMap)->pixelSize == 8) ? 16 : 8;
        OSErr err = SetDepth(gd, want, 1, 1);
        out("Depth change to %d bits: result %d\n", want, err);
    }
    if (g && HookFile("Fit640 Scale") && g->canScale)
        g->mode = kModeScale;
    if (g && HookFile("Fit640 Slow")) {
        if (g->version >= 2)
            g->cropRowsPerTick = g->scaleRowsPerTick = 32;
        if (g->version >= 3) {
            g->budget[0] = (long)g->cmpCost[0] * 100;
            g->budget[1] = (long)g->cmpCost[1] * 100;
        }
    }

    pm = *(*gd)->gdPMap;
    r = qd.screenBits.bounds;
    d = (*gd)->gdRect;
    out("Programs see: %d x %d (device %d x %d), %d-bit\n",
        r.right - r.left, r.bottom - r.top, d.right - d.left,
        d.bottom - d.top, pm->pixelSize);
    LogWindowSystem(gd);

    if (!g) {
        out("Fit640 is not loaded.\n\nPress Return to quit.\n");
        if (gLog)
            fclose(gLog);
        getchar();
        return 0;
    }

    out("Status: %s\n", StatusText(g->status));
    out("Real screen %d x %d, %d-bit. CPU type %d, MOVE16 %s\n",
        g->realW, g->realH, g->depth, g->cpu, g->useMove16 ? "on" : "off");
    out("Mode: %s%s, view %d,%d", g->mode == kModeScale ? "scale" : "crop",
        g->canScale ? "" : " (scale needs 256 colors)", g->viewX, g->viewY);
    if (g->version >= 2)
        out(g->followMouse ? ", follows mouse" : ", locked on centre");
    out("\n");
    detail("  real base $%08lX rowBytes %ld, virtual base $%08lX rowBytes %ld\n",
           (unsigned long)g->realBase, g->realRow,
           (unsigned long)g->virtBase, g->virtRow);
    if (g->version >= 2)
        detail("  window system ports redirected: %d at startup, %d later\n",
               g->portsFixed - g->portsFixedLate, g->portsFixedLate);

    out("\nStartup speed test (whole screens done):\n");
    PrintBench("crop, plain copy", g->benchCropLong, g->benchTicks);
    PrintBench("crop, MOVE16", g->benchCropMove16, g->benchTicks);
    PrintBench("scale", g->benchScale, g->benchTicks);
    if (g->version >= 3) {
        PrintBench("compare only", g->benchCompare, g->benchTicks);
        out("Change-only redrawing: %s\n", g->useShadow ? "on" : "off");
        out("Depth changes survived: %d\n", g->depthChanges);
        detail("  budget per tick %ld (crop) %ld (scale); row costs %d to check,\n"
               "  %d more to redraw (crop), %d and %d (scale)\n",
               g->budget[0], g->budget[1], g->cmpCost[0], g->writeCost[0],
               g->cmpCost[1], g->writeCost[1]);
        detail("  depth-change patch %s, %d ports fixed by the last change\n",
               g->trapPatched ? "installed" : "not installed",
               g->depthPortsFixed);
    }
    if (g->version >= 2 && !(g->version >= 3 && g->useShadow))
        out("Redrawing %d of %d rows per tick (crop), %d (scale)\n",
            g->cropRowsPerTick, g->realH, g->scaleRowsPerTick);

    {
        unsigned long f0 = g->framesPresented;
        unsigned long r0 = g->version >= 3 ? g->rowsWritten : 0;
        unsigned long t0 = LM_Ticks;

        while (LM_Ticks - t0 < 60)
            ;
        out("\nNow: whole screen checked %lu times a second",
            g->framesPresented - f0);
        if (g->version >= 3)
            out(",\n%lu rows redrawn a second", g->rowsWritten - r0);
        out(".\n");
    }

    if (gLog) {
        fclose(gLog);
        gLog = NULL;
    }

    printf("\nS + Return: crop/scale.  C + Return: lock view on\n"
           "centre / follow mouse.  Return alone: quit.\n");
    {
        int c;

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
    }
    return 0;
}
