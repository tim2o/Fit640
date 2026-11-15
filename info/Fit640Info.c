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
#include <Files.h>
#include <Processes.h>
#include <string.h>
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
    case kF640Off:        return "switched off (real screen in use)";
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

/* Test hook: the file "Fit640 Launch" holds the full path of a program
   to start once the report is done (e.g. a game to try). */
static void LaunchFromHook(void)
{
    FILE *f = fopen("Fit640 Launch", "r");
    Str255 path;
    size_t n;
    FSSpec spec;
    LaunchParamBlockRec lp;
    OSErr err;

    if (!f)
        return;
    n = fread(path + 1, 1, 254, f);
    fclose(f);
    while (n > 0 && (path[n] == '\n' || path[n] == '\r'))
        n--;
    path[0] = (unsigned char)n;

    err = FSMakeFSSpec(0, 0, path, &spec);
    if (err == noErr) {
        memset(&lp, 0, sizeof lp);
        lp.launchBlockID = 0x4C43;          /* extendedBlock */
        lp.launchEPBLength = sizeof lp - 12;
        lp.launchControlFlags = 0x4000 | 0x0800;  /* continue, no file flags */
        lp.launchAppSpec = &spec;
        err = LaunchApplication(&lp);
    }
    printf("Launch: result %d\n", err);
}

/* The PostEvent trap takes its arguments in registers (event kind in A0,
   message in D0) and the open-source headers have no glue for it. */
static void PostAnEvent(short what, long message)
{
    __asm__ volatile("move.w %0,%%a0\n\tmove.l %1,%%d0\n\t.short 0xA02F"
                     : : "d"(what), "d"(message)
                     : "a0", "a1", "d0", "d1", "d2");
}

static void PumpEvents(unsigned long ticks)
{
    EventRecord e;
    unsigned long t = LM_Ticks;

    while (LM_Ticks - t < ticks)
        WaitNextEvent(everyEvent, &e, 2, 0);
}

/* Test hook: the file "Fit640 Click" holds a string of L and R. Each one
   simulates a mouse click on the left or right button of the Control
   Strip module, then reports the result. */
static void ClickFromHook(volatile Fit640Globals *g)
{
    FILE *f = fopen("Fit640 Click", "r");
    char script[32];
    size_t n, i;

    if (!f)
        return;
    n = fread(script, 1, sizeof script, f);
    fclose(f);
    if (g->version < 5)
        return;

    PumpEvents(120);                /* let the strip draw itself first */
    out("\nControl Strip module: drawn %d times at %d,%d-%d,%d\n",
        g->stripDraws, g->stripRect.left, g->stripRect.top,
        g->stripRect.right, g->stripRect.bottom);
    if (g->stripDraws == 0)
        return;

    for (i = 0; i < n; i++) {
        Point p;

        if (script[i] != 'L' && script[i] != 'R')
            continue;
        p.h = (script[i] == 'L') ? g->stripRect.left + 12
                                 : g->stripRect.right - 12;
        p.v = (g->stripRect.top + g->stripRect.bottom) / 2;
        *(Point *)0x0828 = p;       /* MTemp */
        *(Point *)0x082C = p;       /* RawMouse */
        *(Point *)0x0830 = p;       /* Mouse */
        *(char *)0x08CE = *(char *)0x08CF;  /* CrsrNew = CrsrCouple */
        PostAnEvent(mouseDown, 0);
        PostAnEvent(mouseUp, 0);
        PumpEvents(150);
        out("Click %c at %d,%d: %s, mode %s, device %d x %d, clicks seen %d\n",
            script[i], p.h, p.v, StatusText(g->status),
            g->mode == kModeScale ? "scale" : "crop",
            (*LM_MainDevice)->gdRect.right, (*LM_MainDevice)->gdRect.bottom,
            g->stripClicks);
    }
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
    /* "Fit640 Off" switches the 640x480 screen off before reporting;
       "Fit640 OffOn" switches it off and on again. */
    if (g && g->version >= 4 && g->switchProc) {
        Fit640SwitchProc sw = (Fit640SwitchProc)g->switchProc;
        if (HookFile("Fit640 Off"))
            out("Switch off: result %d\n", sw(0));
        if (HookFile("Fit640 OffOn")) {
            out("Switch off: result %d\n", sw(0));
            out("Switch on: result %d\n", sw(1));
        }
    }
    if (g && HookFile("Fit640 Scale"))
        g->mode = kModeScale;
    if (g && HookFile("Fit640 Crop"))
        g->mode = kModeCrop;
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
        LaunchFromHook();
        getchar();
        return 0;
    }

    out("Status: %s\n", StatusText(g->status));
    out("Real screen %d x %d, %d-bit. CPU type %d, MOVE16 %s\n",
        g->realW, g->realH, g->depth, g->cpu, g->useMove16 ? "on" : "off");
    out("Mode: %s%s, view %d,%d", g->mode == kModeScale ? "scale" : "crop",
        (g->mode == kModeScale && !g->canScale)
            ? " (needs 256 colors; showing crop)" : "", g->viewX, g->viewY);
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
        if (g->version >= 4)
            detail("  switches %d (last result %d), desktop rebuilt by hand %d,\n"
                   "  Display Manager %s\n", g->switchCount, g->lastSwitchErr,
                   g->reflowsByHand, g->dmPresent ? "present" : "absent");
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

    ClickFromHook(g);

    if (gLog) {
        fclose(gLog);
        gLog = NULL;
    }

    LaunchFromHook();

    printf("\nS + Return: scale/crop.  C + Return: lock view on\n"
           "centre / follow mouse.  O + Return: 640x480 on/off.\n"
           "Return alone: quit.\n");
    {
        int c;

        while ((c = getchar()) != EOF) {
            if (c == 's' || c == 'S') {
                short m = (g->mode == kModeScale) ? kModeCrop : kModeScale;
                if (g->version >= 5 && g->setModeProc)
                    ((Fit640SetModeProc)g->setModeProc)(m);
                else
                    g->mode = m;
                printf("Mode: %s%s.\n", m == kModeScale ? "scale" : "crop",
                       (m == kModeScale && !g->canScale)
                           ? " (needs 256 colors; showing crop for now)" : "");
            } else if (c == 'c' || c == 'C') {
                g->followMouse = !g->followMouse;
                if (!g->followMouse) {
                    g->viewX = ((kVirtW - g->realW) / 2) & ~15;
                    g->viewY = (kVirtH - g->realH) / 2;
                }
            } else if ((c == 'o' || c == 'O') && g->version >= 4 &&
                       g->switchProc) {
                Fit640SwitchProc sw = (Fit640SwitchProc)g->switchProc;
                short err = sw(g->status != kF640Active);
                printf("640x480 is now %s (result %d).\n",
                       g->status == kF640Active ? "on" : "off", err);
            } else if (c == '\n' || c == '\r') {
                break;
            }
            while (c != '\n' && c != '\r' && c != EOF)
                c = getchar();
        }
    }
    return 0;
}
