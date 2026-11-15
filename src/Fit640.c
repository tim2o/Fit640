/*
 * Fit640 - proof of concept.
 *
 * A system extension (INIT) that gives a Mac with a 512x384 built-in screen
 * a virtual 640x480 main screen. All drawing goes to a buffer in the system
 * heap; a VBL task copies part of it (crop) or a 5:4 reduction of it (scale)
 * to the real video memory.
 *
 * The real video mode is never changed.
 *
 * Hot keys (while running):
 *   Control-Option-S   switch between crop and scale
 *   Control-Option-C   lock the crop window on the centre of the screen /
 *                      go back to following the mouse
 * Hold Control during startup to avoid the 68040 MOVE16 instruction.
 * The crop window starts at the top-left and follows the mouse when the
 * pointer reaches an edge.
 */

#include <Quickdraw.h>
#include <Windows.h>
#include <Memory.h>
#include <Resources.h>
#include <Multiverse.h>
#include <Gestalt.h>
#include <OSUtils.h>
#include "Retro68Runtime.h"
#include "Fit640.h"

/* ------------------------------------------------------------------ */
/* Low memory globals                                                  */

#define LM_ScreenRow   (*(short *)0x0106)
#define LM_Ticks       (*(volatile unsigned long *)0x016A)
#define LM_KeyMap      ((volatile unsigned char *)0x0174)
#define LM_ScrnBase    (*(char **)0x0824)
#define LM_MTemp       (*(Point *)0x0828)
#define LM_RawMouse    (*(Point *)0x082C)
#define LM_Mouse       (*(volatile Point *)0x0830)
#define LM_CrsrPin     (*(Rect *)0x0834)
#define LM_CrsrBase    (*(char **)0x0898)
#define LM_CrsrDevice  (*(GDHandle *)0x089C)
#define LM_MainDevice  (*(GDHandle *)0x08A4)
#define LM_CrsrRow     (*(short *)0x08AC)
#define LM_CrsrNew     (*(volatile char *)0x08CE)
#define LM_CrsrCouple  (*(volatile char *)0x08CF)
#define LM_MMU32Bit    (*(char *)0x0CB2)
#define LM_WMgrPort    (*(GrafPtr *)0x09DE)
#define LM_WMgrCPort   (*(CGrafPtr *)0x0D2C)

/* Virtual key codes */
#define kKeyS       0x01
#define kKeyC       0x08
#define kKeyOption  0x3A
#define kKeyControl 0x3B

#define kBenchTicks 30

/* ------------------------------------------------------------------ */

void CopyRowsMove16(const void *src, void *dst, long blocksPerRow, long rows,
                    long srcSkip, long dstSkip);
void ScaleRow5to4(const void *src, void *dst, long groups);

static Fit640Globals g;
static VBLTask gTask;
static char gKeyWasDown;
static short gNextRow;        /* next screen row to redraw this frame */
static short gFrameTicks;     /* ticks spent on this frame so far */

/* ------------------------------------------------------------------ */
/* Presenting                                                          */

static void CopyRowsLong(const unsigned char *src, unsigned char *dst,
                         long rowBytes, long rows, long srcSkip, long dstSkip)
{
    long longs = rowBytes >> 2;
    long tail = rowBytes & 3;

    while (rows-- > 0) {
        const unsigned long *s = (const unsigned long *)src;
        unsigned long *d = (unsigned long *)dst;
        long n = longs;
        while (n-- > 0)
            *d++ = *s++;
        src = (const unsigned char *)s;
        dst = (unsigned char *)d;
        n = tail;
        while (n-- > 0)
            *dst++ = *src++;
        src += srcSkip;
        dst += dstSkip;
    }
}

/* Redraw real screen rows y0..y1-1 from the crop window. */
static void PresentCropRows(short y0, short y1, int allowMove16)
{
    long xBytes = ((long)g.viewX * g.depth) >> 3;
    long visBytes = ((long)g.realW * g.depth) >> 3;
    unsigned char *src = (unsigned char *)g.virtBase
                       + (long)(g.viewY + y0) * g.virtRow + xBytes;
    unsigned char *dst = (unsigned char *)g.realBase + (long)y0 * g.realRow;

    if (allowMove16 &&
        (((unsigned long)src | (unsigned long)dst | (unsigned long)visBytes |
          (unsigned long)g.virtRow | (unsigned long)g.realRow) & 15) == 0)
        CopyRowsMove16(src, dst, visBytes >> 4, y1 - y0,
                       g.virtRow - visBytes, g.realRow - visBytes);
    else
        CopyRowsLong(src, dst, visBytes, y1 - y0,
                     g.virtRow - visBytes, g.realRow - visBytes);
}

/* 640x480 -> 512x384, 8-bit. Keeps rows/columns 0,1,3,4 of every 5. */
static void PresentScaleRows(short y0, short y1)
{
    const unsigned char *src = (const unsigned char *)g.virtBase
                             + (long)(y0 + ((y0 + 2) >> 2)) * g.virtRow;
    unsigned char *dst = (unsigned char *)g.realBase + (long)y0 * g.realRow;
    short y;

    for (y = y0; y < y1; y++) {
        ScaleRow5to4(src, dst, g.realW >> 2);
        dst += g.realRow;
        src += g.virtRow;
        if ((y & 3) == 1)
            src += g.virtRow;       /* skip the middle row of each 5 */
    }
}

static int Scaling(void)
{
    return g.mode == kModeScale && g.canScale;
}

static void PresentRows(short y0, short y1)
{
    if (Scaling())
        PresentScaleRows(y0, y1);
    else
        PresentCropRows(y0, y1, g.useMove16);
}

/* Whole-screen versions, for the startup speed test. */
static void PresentCropLong(void)   { PresentCropRows(0, g.realH, 0); }
static void PresentCropMove16(void) { PresentCropRows(0, g.realH, 1); }
static void PresentScale(void)      { PresentScaleRows(0, g.realH); }

/* ------------------------------------------------------------------ */
/* View handling                                                       */

static void CentreView(void)
{
    g.viewX = ((kVirtW - g.realW) / 2) & ~15;
    g.viewY = (kVirtH - g.realH) / 2;
}

/* Keep the mouse inside the visible window (crop mode). */
static void FollowMouse(void)
{
    short mh = LM_Mouse.h;
    short mv = LM_Mouse.v;
    short maxX = (kVirtW - g.realW) & ~15;
    short maxY = kVirtH - g.realH;
    short x = g.viewX;
    short y = g.viewY;

    if (mh < x)
        x = mh & ~15;
    else if (mh > x + g.realW - 16)
        x = (mh - g.realW + 16 + 15) & ~15;
    if (x < 0) x = 0;
    if (x > maxX) x = maxX;

    if (mv < y)
        y = mv;
    else if (mv > y + g.realH - 16)
        y = mv - g.realH + 16;
    if (y < 0) y = 0;
    if (y > maxY) y = maxY;

    g.viewX = x;
    g.viewY = y;
}

static int KeyIsDown(short code)
{
    return (LM_KeyMap[code >> 3] >> (code & 7)) & 1;
}

static void CheckHotKeys(void)
{
    char down = 0;

    if (KeyIsDown(kKeyControl) && KeyIsDown(kKeyOption)) {
        if (KeyIsDown(kKeyS)) down = 1;
        else if (KeyIsDown(kKeyC)) down = 2;
    }
    if (down && !gKeyWasDown) {
        if (down == 1) {
            if (g.canScale)
                g.mode = (g.mode == kModeScale) ? kModeCrop : kModeScale;
        } else {
            g.followMouse = !g.followMouse;
            if (!g.followMouse)
                CentreView();
        }
    }
    gKeyWasDown = down;
}

/* ------------------------------------------------------------------ */
/* Ports that already exist                                            */

#define LM_MemTop      (*(unsigned long *)0x0108)

/* The Window Manager's port pointers are garbage until it has started. */
static int PlausiblePtr(const void *p)
{
    unsigned long a = (unsigned long)p;
    return a >= 0x2000 && a < LM_MemTop && (a & 1) == 0;
}

/* Point one existing drawing port at the virtual screen, if it still
   describes the real one. Only writes fields in place (no Memory Manager
   calls), so it is safe from the refresh task. Returns 1 if changed. */
static int RetargetPort(GrafPtr port, int wholeScreen)
{
    if (!PlausiblePtr(port))
        return 0;

    if ((port->portBits.rowBytes & 0xC000) == 0xC000) {
        PixMapHandle pmh = ((CGrafPtr)port)->portPixMap;
        PixMapPtr pm;
        if (!PlausiblePtr(pmh) || !PlausiblePtr(*pmh))
            return 0;
        pm = *pmh;
        if (pm->baseAddr != g.realBase)
            return 0;
        pm->baseAddr = g.virtBase;
        pm->rowBytes = (short)((pm->rowBytes & 0xC000) | g.virtRow);
        pm->bounds.right = pm->bounds.left + kVirtW;
        pm->bounds.bottom = pm->bounds.top + kVirtH;
    } else {
        BitMap *bm = &port->portBits;
        if (bm->baseAddr != g.realBase)
            return 0;
        bm->baseAddr = g.virtBase;
        /* An old-style port on a color screen is recognised by its base
           address; its rowBytes is the 1-bit value and stays that way. */
        if ((long)bm->rowBytes == g.realRow)
            bm->rowBytes = (short)g.virtRow;
        else
            bm->rowBytes = kVirtW / 8;
        bm->bounds.right = bm->bounds.left + kVirtW;
        bm->bounds.bottom = bm->bounds.top + kVirtH;
    }

    if (wholeScreen) {
        port->portRect.top = 0;
        port->portRect.left = 0;
        port->portRect.bottom = kVirtH;
        port->portRect.right = kVirtW;
    }
    return 1;
}

/* Mac OS 8 creates the Window Manager's own ports from the screen as it
   was before we loaded, so they have to be brought along. System 7 picks
   up the new size by itself. */
static int RetargetWindowManager(void)
{
    return RetargetPort(LM_WMgrPort, 1) +
           RetargetPort((GrafPtr)LM_WMgrCPort, 1);
}

/* ------------------------------------------------------------------ */
/* VBL task                                                            */

/* Runs every 1/60 s. Redraws the screen a slice at a time, so that on a
   slow machine no single interrupt runs long and redrawing takes at most
   about a third of the processor. On a fast one the slice is the whole
   screen. */
static void VBLProc(void)
{
    PixMapPtr pm;
    short n, y1;

    gTask.vblCount = 1;
    if (g.status != kF640Active)
        return;

    /* If anything re-initialised the screen (a depth change does), our
       virtual screen is no longer in use: stop touching video memory. */
    pm = *(*LM_MainDevice)->gdPMap;
    if (pm->baseAddr != g.virtBase || pm->pixelSize != g.depth) {
        g.status = kF640Dormant;
        return;
    }

    /* Catch Window Manager ports created after we loaded. */
    n = RetargetWindowManager();
    g.portsFixed += n;
    g.portsFixedLate += n;

    CheckHotKeys();

    if (gNextRow == 0 && g.followMouse && !Scaling())
        FollowMouse();

    if (gNextRow < g.realH) {
        y1 = gNextRow + (Scaling() ? g.scaleRowsPerTick : g.cropRowsPerTick);
        if (y1 > g.realH)
            y1 = g.realH;
        PresentRows(gNextRow, y1);
        gNextRow = y1;
    }
    gFrameTicks++;
    if (gNextRow >= g.realH && gFrameTicks >= g.frameDivisor) {
        gNextRow = 0;
        gFrameTicks = 0;
        g.framesPresented++;
    }
}

/* ------------------------------------------------------------------ */
/* Gestalt                                                             */

static pascal OSErr Fit640Gestalt(OSType selector, long *response)
{
    (void)selector;
    *response = (long)&g;
    return noErr;
}

/* ------------------------------------------------------------------ */
/* Installation                                                        */

static short Bench(void (*present)(void))
{
    unsigned long t = LM_Ticks;
    long guard = 4000000;
    short frames = 0;

    while (LM_Ticks == t && --guard > 0)
        ;
    if (guard <= 0)
        return -1;                  /* the clock is not running */
    t = LM_Ticks;
    while ((unsigned long)(LM_Ticks - t) < kBenchTicks && frames < 10000) {
        present();
        frames++;
    }
    return frames;
}

/* How many screen rows to redraw per tick so that redrawing uses about a
   third of the processor: frames-per-tick * rows / 3. */
static short RowsPerTick(short benchFrames)
{
    long rows;

    if (benchFrames <= 0)
        return 32;                  /* not measured: be careful */
    rows = (long)g.realH * benchFrames / (3L * kBenchTicks);
    if (rows < 8)
        rows = 8;
    if (rows > g.realH)
        rows = g.realH;
    return (short)rows;
}

static short Install(void)
{
    long response;
    GDHandle gd;
    PixMapPtr pm;
    long bufSize, visBytes;
    char *raw;
    short y;

    g.magic = kFit640Selector;
    g.version = kFit640Version;
    g.status = kF640Installing;
    g.frameDivisor = 2;
    g.benchTicks = kBenchTicks;
    g.benchCropLong = g.benchCropMove16 = g.benchScale = -1;

    if (Gestalt(gestaltQuickdrawVersion, &response) != noErr ||
        response < gestalt8BitQD)
        return kF640NoColorQD;
    if (!LM_MMU32Bit)
        return kF640Mode24;
    if (Gestalt(gestaltProcessorType, &response) == noErr)
        g.cpu = (short)response;

    gd = LM_MainDevice;
    pm = *(*gd)->gdPMap;

    g.realW = pm->bounds.right - pm->bounds.left;
    g.realH = pm->bounds.bottom - pm->bounds.top;
    g.realRow = pm->rowBytes & 0x3FFF;
    g.realBase = pm->baseAddr;
    g.depth = pm->pixelSize;

    if (pm->bounds.left != 0 || pm->bounds.top != 0 ||
        g.realW > kVirtW || g.realH > kVirtH ||
        (g.realW == kVirtW && g.realH == kVirtH))
        return kF640ScreenSize;
    if (g.depth > 16)
        return kF640Depth;

    g.virtRow = ((long)kVirtW * g.depth) >> 3;
    bufSize = g.virtRow * kVirtH;
    raw = NewPtrSysClear(bufSize + 16);
    if (raw == 0)
        return kF640NoMemory;
    g.virtBase = (char *)(((unsigned long)raw + 15) & ~15UL);

    g.canScale = (g.depth == 8 && g.realW == 512 && g.realH == 384);
    g.mode = kModeCrop;
    g.followMouse = 1;
    g.viewX = 0;
    g.viewY = 0;

    /* Copy what is on the real screen into the top-left of the virtual
       one, so nothing visibly changes when we switch over. */
    HideCursor();
    visBytes = ((long)g.realW * g.depth) >> 3;
    for (y = 0; y < g.realH; y++)
        BlockMoveData(g.realBase + (long)y * g.realRow,
                      g.virtBase + (long)y * g.virtRow, visBytes);

    /* Measure. The crop tests redraw the same picture; the scale test
       briefly shows the shrunken screen. */
    g.benchCropLong = Bench(PresentCropLong);
    /* Holding Control at startup skips MOVE16 entirely, in case the
       accelerator or video hardware does not cope with it. */
    if (g.cpu >= gestalt68040 && !KeyIsDown(kKeyControl)) {
        g.benchCropMove16 = Bench(PresentCropMove16);
        g.useMove16 = (g.benchCropMove16 > g.benchCropLong);
    }
    if (g.canScale)
        g.benchScale = Bench(PresentScale);
    g.cropRowsPerTick = RowsPerTick(g.useMove16 ? g.benchCropMove16
                                                : g.benchCropLong);
    g.scaleRowsPerTick = RowsPerTick(g.benchScale);
    PresentRows(0, g.realH);

    /* Start the refresh task first; it does nothing until status is
       kF640Active. */
    gTask.qType = vType;
    gTask.vblAddr = (ProcPtr)VBLProc;
    gTask.vblCount = 1;
    gTask.vblPhase = 0;
    if (VInstall(&gTask) != noErr) {
        ShowCursor();
        DisposePtr(raw);
        return kF640NoVBL;
    }

    /* Switch the main screen over to the virtual buffer. */
    pm = *(*gd)->gdPMap;
    pm->baseAddr = g.virtBase;
    pm->rowBytes = (short)((pm->rowBytes & 0xC000) | g.virtRow);
    pm->bounds.right = kVirtW;
    pm->bounds.bottom = kVirtH;
    (*gd)->gdRect.right = kVirtW;
    (*gd)->gdRect.bottom = kVirtH;

    LM_ScrnBase = g.virtBase;
    LM_ScreenRow = (short)g.virtRow;

    if (LM_CrsrDevice == gd) {
        LM_CrsrBase = g.virtBase;
        LM_CrsrRow = (short)g.virtRow;
        LM_CrsrPin.top = 0;
        LM_CrsrPin.left = 0;
        LM_CrsrPin.bottom = kVirtH;
        LM_CrsrPin.right = kVirtW;
    }

    g.portsFixed = RetargetWindowManager();
    g.wmExisted = (g.portsFixed != 0);

    ShowCursor();

    g.status = kF640Active;
    return kF640Active;
}

void _start(void)
{
    Handle self;

    RETRO68_RELOCATE();

    /* Stay in memory after the extension file is closed. The INIT resource
       is marked sysHeap + locked, so it is already in the system heap. */
    self = Get1Resource('INIT', 128);
    if (self)
        DetachResource(self);

    g.status = Install();
    NewGestalt(kFit640Selector, (SelectorFunctionUPP)Fit640Gestalt);
}
