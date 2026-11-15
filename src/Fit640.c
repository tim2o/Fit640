/*
 * Fit640 - proof of concept.
 *
 * A system extension (INIT) that gives a Mac with a 512x384 (or 560x384)
 * built-in screen a virtual 640x480 main screen. All drawing goes to a buffer in the system
 * heap; a VBL task copies part of it (crop) or a reduction of it (scale)
 * to the real video memory.
 *
 * The real video mode is never changed.
 *
 * Hot keys (while running):
 *   Control-Option-S   switch between scale and crop (until restart; the
 *                      Control Strip module and Fit640Info save the choice)
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
#define LM_Mouse       (*(volatile Point *)0x0830)
#define LM_CrsrPin     (*(Rect *)0x0834)
#define LM_CrsrBase    (*(char **)0x0898)
#define LM_CrsrDevice  (*(GDHandle *)0x089C)
#define LM_MainDevice  (*(GDHandle *)0x08A4)
#define LM_CrsrRow     (*(short *)0x08AC)
#define LM_WindowList  (*(WindowPeek *)0x09D6)
#define LM_WMgrPort    (*(GrafPtr *)0x09DE)
#define LM_MMU32Bit    (*(char *)0x0CB2)
#define LM_WMgrCPort   (*(CGrafPtr *)0x0D2C)
#define LM_PortList    (*(Handle *)0x0D66)
#define LM_GrayRgn     (*(RgnHandle *)0x09EE)
#define LM_MBarHeight  (*(short *)0x0BAA)

/* Virtual key codes */
#define kKeyS       0x01
#define kKeyC       0x08
#define kKeyOption  0x3A
#define kKeyControl 0x3B

#define kBenchTicks 30
#define kMaxDepth   16          /* the buffers are sized for this */
#define kTrapInitGDevice 0xAA2E
#define kTrapDisplayDispatch 0xABEB
#define kTrapUnimplemented 0xA89F

/* Display Manager (not in the open-source headers). */
pascal OSErr DMBeginConfigureDisplays(Handle *displayState)
    M68K_INLINE(0x303C, 0x0206, 0xABEB);
pascal OSErr DMEndConfigureDisplays(Handle displayState)
    M68K_INLINE(0x303C, 0x0207, 0xABEB);

/* ------------------------------------------------------------------ */

void CopyRowsMove16(const void *src, void *dst, long blocksPerRow, long rows,
                    long srcSkip, long dstSkip);
void ScaleRow5to4(const void *src, void *dst, long groups);
void ScaleRow8to7(const void *src, void *dst, long groups);
long FirstDiffLong(const void *a, const void *b, long longs);
long EqualTailLongs(const void *aEnd, const void *bEnd);

typedef pascal void (*InitGDeviceProc)(short refNum, long mode, GDHandle gdh);

static Fit640Globals g;
static VBLTask gTask;
static char *gShadow;         /* copy of what has been put on the screen,
                                 laid out like the virtual screen */
static InitGDeviceProc gOldInitGDevice;
static volatile short gBusy;  /* the screen is being re-initialised */
static char gKeyWasDown;
static short gNextRow;        /* next screen row of the sweep in progress */
static short gFrameTicks;     /* ticks spent on this sweep so far */
static short gForce;          /* this sweep redraws without comparing */
static short gLastViewX, gLastViewY, gLastMode;

/* ------------------------------------------------------------------ */
/* Copying                                                             */

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

static int Scaling(void)
{
    return g.mode == kModeScale && g.canScale;
}

static long VisBytes(void)
{
    return ((long)g.realW * g.depth) >> 3;
}

/* x / 5 for 0 <= x <= 1280, without a division. */
static short Div5(short x)
{
    return (short)(((long)x * 205) >> 10);
}

/*
 * Shrink part of one 640-pixel row onto the real screen row: the source
 * bytes o..o+len-1 and whatever else shares their groups. A 512-pixel
 * screen takes 4 of every 5 pixels, a 560-pixel one 7 of every 8.
 */
static void ScaleRowSpan(const unsigned char *src, unsigned char *dst,
                         long o, long len)
{
    short g0, g1;

    if (g.realW == 560) {
        g0 = (short)(o >> 3);
        g1 = (short)((o + len - 1) >> 3);
        ScaleRow8to7(src + 8 * g0, dst + 7 * g0, g1 - g0 + 1);
    } else {
        g0 = Div5((short)o);
        g1 = Div5((short)(o + len - 1));
        ScaleRow5to4(src + 5 * g0, dst + 4 * g0, g1 - g0 + 1);
    }
}

/* Redraw real screen rows y0..y1-1 from the crop window, unconditionally. */
static void PresentCropRows(short y0, short y1, int allowMove16)
{
    long xBytes = ((long)g.viewX * g.depth) >> 3;
    long visBytes = VisBytes();
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

/* 640x480 -> 512x384 or 560x384, 8-bit. Keeps rows 0,1,3,4 of every 5. */
static void PresentScaleRows(short y0, short y1)
{
    const unsigned char *src = (const unsigned char *)g.virtBase
                             + (long)(y0 + ((y0 + 2) >> 2)) * g.virtRow;
    unsigned char *dst = (unsigned char *)g.realBase + (long)y0 * g.realRow;
    short y;

    for (y = y0; y < y1; y++) {
        ScaleRowSpan(src, dst, 0, g.virtRow);
        dst += g.realRow;
        src += g.virtRow;
        if ((y & 3) == 1)
            src += g.virtRow;       /* skip the middle row of each 5 */
    }
}

static void PresentRows(short y0, short y1)
{
    if (Scaling())
        PresentScaleRows(y0, y1);
    else
        PresentCropRows(y0, y1, g.useMove16);
}

/*
 * Bring real screen rows y0..y1-1 up to date, writing only the part of
 * each row that differs from the shadow copy (or the whole row if `force`).
 * Returns the cost in budget units.
 */
static long UpdateRows(short y0, short y1, int force)
{
    int scale = Scaling();
    long cmpCost = force ? 0 : g.cmpCost[scale];
    long writeCost = g.writeCost[scale];
    long used = 0;
    long off, n, rowBytes;
    const unsigned char *src;
    unsigned char *sh, *dst;
    int move16 = 0;
    short y;

    if (y0 < 0) y0 = 0;
    if (y1 > g.realH) y1 = g.realH;
    if (y0 >= y1)
        return 0;

    dst = (unsigned char *)g.realBase + (long)y0 * g.realRow;
    if (scale) {
        rowBytes = g.virtRow;
        off = (long)(y0 + ((y0 + 2) >> 2)) * g.virtRow;
    } else {
        rowBytes = VisBytes();
        off = (long)(g.viewY + y0) * g.virtRow
            + (((long)g.viewX * g.depth) >> 3);
    }
    src = (const unsigned char *)g.virtBase + off;
    sh = (unsigned char *)gShadow + off;
    n = rowBytes >> 2;
    if (!scale && g.useMove16 &&
        (((unsigned long)src | (unsigned long)dst | (unsigned long)rowBytes |
          (unsigned long)g.virtRow | (unsigned long)g.realRow) & 15) == 0)
        move16 = 1;

    for (y = y0; y < y1; y++) {
        long o = 0, len = rowBytes;
        int changed = 1;

        used += cmpCost;
        if (!force) {
            long i = FirstDiffLong(src, sh, n);
            if (i == n) {
                changed = 0;
            } else {
                long t = EqualTailLongs(src + rowBytes, sh + rowBytes);
                o = i << 2;
                len = (n - i - t) << 2;
            }
        }
        if (changed) {
            CopyRowsLong(src + o, sh + o, len, 1, 0, 0);
            if (scale) {
                ScaleRowSpan(src, dst, o, len);
            } else if (move16 && len == rowBytes) {
                CopyRowsMove16(src, dst, rowBytes >> 4, 1, 0, 0);
            } else {
                CopyRowsLong(src + o, dst + o, len, 1, 0, 0);
            }
            used += writeCost;
            g.rowsWritten++;
        }

        dst += g.realRow;
        src += g.virtRow;
        sh += g.virtRow;
        if (scale && (y & 3) == 1) {
            src += g.virtRow;
            sh += g.virtRow;
        }
    }
    return used;
}

/* Whole-screen versions, for the startup speed test. */
static void PresentCropLong(void)   { PresentCropRows(0, g.realH, 0); }
static void PresentCropMove16(void) { PresentCropRows(0, g.realH, 1); }
static void PresentScale(void)      { PresentScaleRows(0, g.realH); }
static void CompareAll(void)        { UpdateRows(0, g.realH, 0); }

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

/* Pointers found in low memory are garbage until their owner has started.
   RAM and video memory lie below the ROM, which starts at $40000000. (The
   MemTop low-memory global cannot be used for this: it does not cover RAM
   above the first bank.) */
static int PlausiblePtr(const void *p)
{
    unsigned long a = (unsigned long)p;
    return a >= 0x2000 && a < 0x40000000UL && (a & 1) == 0;
}

/*
 * Make one existing screen port describe the virtual screen (toVirtual) or
 * the real one. Ports that are not screen ports are left alone. Only writes
 * fields in place (no Memory Manager calls), so it is safe from the refresh
 * task. Returns 1 if anything changed.
 */
static int PointPort(GrafPtr port, int toVirtual, int wholeScreen)
{
    char *to = toVirtual ? g.virtBase : g.realBase;
    char *other = toVirtual ? g.realBase : g.virtBase;
    long row = toVirtual ? g.virtRow : g.realRow;
    long otherRow = toVirtual ? g.realRow : g.virtRow;
    short w = toVirtual ? kVirtW : g.realW;
    short h = toVirtual ? kVirtH : g.realH;
    int changed = 0;
    Rect *bounds;

    if (!PlausiblePtr(port))
        return 0;

    if ((port->portBits.rowBytes & 0xC000) == 0xC000) {
        PixMapHandle pmh = ((CGrafPtr)port)->portPixMap;
        PixMapPtr pm;
        if (!PlausiblePtr(pmh) || !PlausiblePtr(*pmh))
            return 0;
        pm = *pmh;
        if (pm->baseAddr != to && pm->baseAddr != other)
            return 0;
        if (pm->baseAddr != to) {
            pm->baseAddr = to;
            changed = 1;
        }
        pm->rowBytes = (short)((pm->rowBytes & 0xC000) | row);
        bounds = &pm->bounds;
    } else {
        BitMap *bm = &port->portBits;
        if (bm->baseAddr != to && bm->baseAddr != other)
            return 0;
        if (bm->baseAddr != to) {
            bm->baseAddr = to;
            changed = 1;
        }
        /* An old-style port on a color screen is recognised by its base
           address. Its rowBytes is either the screen's or the 1-bit value;
           keep whichever kind it was. */
        if ((long)bm->rowBytes == otherRow || (long)bm->rowBytes == row)
            bm->rowBytes = (short)row;
        else
            bm->rowBytes = w / 8;
        bounds = &bm->bounds;
    }

    if (bounds->right != bounds->left + w || bounds->bottom != bounds->top + h) {
        bounds->right = bounds->left + w;
        bounds->bottom = bounds->top + h;
        changed = 1;
    }

    if (wholeScreen &&
        (port->portRect.right != w || port->portRect.bottom != h)) {
        port->portRect.top = 0;
        port->portRect.left = 0;
        port->portRect.bottom = h;
        port->portRect.right = w;
        changed = 1;
    }
    return changed;
}

/* The Window Manager's own ports are created from the screen as it was
   before we loaded, so they have to be brought along. So do the ports of
   windows that already exist. */
static int PointWindowManager(int toVirtual)
{
    int fixed = PointPort(LM_WMgrPort, toVirtual, 1) +
                PointPort((GrafPtr)LM_WMgrCPort, toVirtual, 1);
    WindowPeek w = LM_WindowList;
    short guard = 64;

    while (PlausiblePtr(w) && guard-- > 0) {
        fixed += PointPort((GrafPtr)w, toVirtual, 0);
        w = w->nextWindow;
    }
    return fixed;
}

/* Every color port QuickDraw knows about. */
static int PointPortList(int toVirtual)
{
    Handle h = LM_PortList;
    short count, i;
    CGrafPtr *ports;
    int fixed = 0;

    if (!PlausiblePtr(h) || !PlausiblePtr(*h))
        return 0;
    count = *(short *)*h;
    if (count < 0 || count > 2048)
        return 0;
    ports = (CGrafPtr *)(*h + 2);
    for (i = 0; i < count; i++)
        fixed += PointPort((GrafPtr)ports[i], toVirtual, 0);
    return fixed;
}

/* ------------------------------------------------------------------ */
/* Pointing the system at the virtual screen                           */

/* Read the real screen's description from the main device. */
static void ReadRealScreen(GDHandle gd)
{
    PixMapPtr pm = *(*gd)->gdPMap;

    g.realW = pm->bounds.right - pm->bounds.left;
    g.realH = pm->bounds.bottom - pm->bounds.top;
    g.realRow = pm->rowBytes & 0x3FFF;
    g.realBase = pm->baseAddr;
    g.depth = pm->pixelSize;
    g.virtRow = ((long)kVirtW * g.depth) >> 3;
    g.canScale = (g.depth == 8 && g.realH == 384 &&
                  (g.realW == 512 || g.realW == 560));
    /* Comparing works on whole 32-bit words of the visible row. */
    g.useShadow = (gShadow != 0 && (VisBytes() & 3) == 0);
}

/* Make the main device, low memory and the cursor describe the virtual
   screen (or, with toVirtual 0, the real one again). The real one must
   already be recorded in g. */
static void PointScreen(GDHandle gd, int toVirtual)
{
    PixMapPtr pm = *(*gd)->gdPMap;
    char *base = toVirtual ? g.virtBase : g.realBase;
    long row = toVirtual ? g.virtRow : g.realRow;
    short w = toVirtual ? kVirtW : g.realW;
    short h = toVirtual ? kVirtH : g.realH;

    pm->baseAddr = base;
    pm->rowBytes = (short)((pm->rowBytes & 0xC000) | row);
    pm->bounds.right = w;
    pm->bounds.bottom = h;
    (*gd)->gdRect.right = w;
    (*gd)->gdRect.bottom = h;

    LM_ScrnBase = base;
    LM_ScreenRow = (short)row;

    if (LM_CrsrDevice == gd) {
        LM_CrsrBase = base;
        LM_CrsrRow = (short)row;
        LM_CrsrPin.top = 0;
        LM_CrsrPin.left = 0;
        LM_CrsrPin.bottom = h;
        LM_CrsrPin.right = w;
    }
}

static void PointAtVirtualScreen(GDHandle gd)
{
    PointScreen(gd, 1);
}

static short Clamp300(long v)
{
    if (v < 1) return 1;
    if (v > 300) return 300;
    return (short)v;
}

/*
 * Turn the startup speed test into a per-tick redraw budget that amounts
 * to about a third of the processor.
 *
 * With Fc whole-screen compares and Fw whole-screen redraws per test
 * period, take 1/(Fc*Fw*rows) of the period as the unit: checking one row
 * then costs Fw units and redrawing it Fc units, and a third of a tick is
 * Fc*Fw*rows/(3*kBenchTicks) units.
 */
static void ComputeBudgets(void)
{
    long fc, fw;
    short m;

    for (m = 0; m < 2; m++) {
        if (!m)
            fw = g.useMove16 ? g.benchCropMove16 : g.benchCropLong;
        else if (g.benchScale > 0)
            fw = g.benchScale;
        else
            /* Scaling was not possible at the startup depth, so it was
               not timed: assume two thirds of the plain copy's speed. */
            fw = (long)g.benchCropLong * 2 / 3;
        fc = g.benchCompare;
        /* The tests ran at benchDepth; more bits per pixel is more work. */
        if (g.benchDepth > 0 && g.depth > 0) {
            fw = fw * g.benchDepth / g.depth;
            fc = fc * g.benchDepth / g.depth;
        }
        fw = Clamp300(fw);
        fc = Clamp300(fc);
        g.cmpCost[m] = (short)(m ? (fw * 5 + 3) / 4 : fw);
        g.writeCost[m] = (short)fc;
        g.budget[m] = fc * fw * g.realH / (3L * kBenchTicks);
    }
}

/* Without the shadow copy: how many screen rows to redraw per tick so that
   redrawing uses about a third of the processor. */
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

/* ------------------------------------------------------------------ */
/* Surviving a re-initialised screen                                   */

/*
 * InitGDevice is what changes the color depth (SetDepth, the Monitors
 * control panel, games asking for 256 colors). It rewrites the main
 * device from the video driver, which would undo the virtual screen.
 * This patch lets it run, then puts the virtual screen back at the new
 * depth. The buffers never move, so ports that point at them stay valid.
 */
static pascal void MyInitGDevice(short refNum, long mode, GDHandle gdh)
{
    int ours = (gdh == LM_MainDevice) &&
               (g.status == kF640Active || g.status == kF640Dormant ||
                g.status == kF640Off);

    if (!ours) {
        gOldInitGDevice(refNum, mode, gdh);
        return;
    }

    gBusy++;
    HideCursor();
    gOldInitGDevice(refNum, mode, gdh);

    {
        PixMapPtr pm = *(*gdh)->gdPMap;
        short w = pm->bounds.right - pm->bounds.left;
        short h = pm->bounds.bottom - pm->bounds.top;

        if (pm->baseAddr == g.virtBase) {
            /* Nothing was reset. */
        } else if (!g.enabled) {
            /* Switched off: leave the real screen in place and make sure
               nothing still points at the virtual one. */
            ReadRealScreen(gdh);
            g.depthPortsFixed = PointWindowManager(0) + PointPortList(0);
            g.status = kF640Off;
        } else if (pm->pixelSize > kMaxDepth || w > kVirtW || h > kVirtH ||
                   (w == kVirtW && h == kVirtH)) {
            g.status = kF640Dormant;    /* nothing to do on this screen */
        } else {
            ReadRealScreen(gdh);
            PointAtVirtualScreen(gdh);
            g.depthPortsFixed = PointWindowManager(1) + PointPortList(1);
            ComputeBudgets();
            g.cropRowsPerTick = RowsPerTick(
                (short)((long)(g.useMove16 ? g.benchCropMove16
                                           : g.benchCropLong)
                        * g.benchDepth / g.depth));
            g.depthChanges++;
            gNextRow = 0;
            gFrameTicks = 0;
            gForce = 1;
            g.status = kF640Active;
        }
    }

    ShowCursor();
    gBusy--;
}

/* ------------------------------------------------------------------ */
/* Switching on and off while running                                  */

#define kPrefsName "\pFit640 Prefs"

static OSErr PrefsFolder(short *vRef, long *dirID)
{
    return FindFolder((short)0x8000 /* kOnSystemDisk */, 'pref', true,
                      vRef, dirID);
}

/* The prefs file is two 16-bit words: on/off, then the mode. Without a
   file the 640x480 screen is on and scaled to fit. */
static void ReadPrefs(void)
{
    short vRef, ref;
    short words[2];
    long dirID, count = 4;

    g.enabled = 1;
    g.mode = kModeScale;
    if (PrefsFolder(&vRef, &dirID) != noErr)
        return;
    if (HOpenDF(vRef, dirID, (ConstStringPtr)kPrefsName, 1, &ref) != noErr)
        return;
    words[0] = 1;
    words[1] = kModeScale;
    FSRead(ref, &count, words);
    FSClose(ref);
    if (count >= 2)
        g.enabled = (words[0] != 0);
    if (count >= 4)
        g.mode = (words[1] == kModeCrop) ? kModeCrop : kModeScale;
}

static void WritePrefs(void)
{
    short vRef, ref;
    short words[2];
    long dirID, count = 4;

    if (PrefsFolder(&vRef, &dirID) != noErr)
        return;
    HCreate(vRef, dirID, (ConstStringPtr)kPrefsName, 'F640', 'pref');
    if (HOpenDF(vRef, dirID, (ConstStringPtr)kPrefsName, 3, &ref) != noErr)
        return;
    words[0] = g.enabled;
    words[1] = g.mode;
    if (FSWrite(ref, &count, words) == noErr)
        SetEOF(ref, 4);
    FSClose(ref);
}

/* Without the Display Manager: rebuild the desktop for the new size by
   hand and repaint everything. */
static void ReflowByHand(void)
{
    GDHandle gd = LM_MainDevice;
    Rect r = (*gd)->gdRect;
    RgnHandle gray = LM_GrayRgn;
    GrafPtr wp = LM_WMgrPort;
    CGrafPtr wc = LM_WMgrCPort;

    if (!PlausiblePtr(gray) || !PlausiblePtr(*gray))
        return;
    if (PlausiblePtr(wp)) {
        wp->portRect = r;
        RectRgn(wp->visRgn, &r);
    }
    if (PlausiblePtr(wc)) {
        wc->portRect = r;
        RectRgn(wc->visRgn, &r);
    }
    SetRectRgn(gray, r.left, r.top + LM_MBarHeight, r.right, r.bottom);
    DrawMenuBar();
    if (PlausiblePtr(LM_WindowList)) {
        CalcVisBehind(LM_WindowList, gray);
        PaintBehind(LM_WindowList, gray);
    } else {
        PaintOne(0, gray);
    }
}

/*
 * Apply g.enabled now: point the system at the virtual screen or back at
 * the real one. Doing it between the Display Manager's begin/end calls
 * lets the system notice the size change, rebuild the desktop, move
 * windows back on screen and tell applications. If it does not, the
 * desktop is rebuilt by hand.
 */
static OSErr Reconfigure(void)
{
    GDHandle gd = LM_MainDevice;
    Handle state = 0;
    OSErr err = noErr;
    int dm = g.dmPresent;
    Rect want, got;

    if (dm && DMBeginConfigureDisplays(&state) != noErr)
        dm = 0;

    gBusy++;
    HideCursor();
    if (g.enabled) {
        PointScreen(gd, 1);
        PointWindowManager(1);
        PointPortList(1);
        ComputeBudgets();
        g.viewX = 0;
        g.viewY = 0;
        gNextRow = 0;
        gFrameTicks = 0;
        gForce = 1;
        g.status = kF640Active;
    } else {
        g.status = kF640Off;
        PointScreen(gd, 0);
        PointWindowManager(0);
        PointPortList(0);
    }
    ShowCursor();
    gBusy--;

    if (dm)
        err = DMEndConfigureDisplays(state);

    want = (*gd)->gdRect;
    if (PlausiblePtr(LM_GrayRgn) && PlausiblePtr(*LM_GrayRgn)) {
        got = (*LM_GrayRgn)->rgnBBox;
        if (got.right != want.right || got.bottom != want.bottom) {
            ReflowByHand();
            g.reflowsByHand++;
        }
    }
    return err;
}

static pascal short Fit640Switch(short on)
{
    OSErr err;

    on = on ? 1 : 0;
    if (g.status != kF640Active && g.status != kF640Off &&
        g.status != kF640Dormant)
        return -50;                 /* paramErr: not installed */

    if (g.status != kF640Dormant && (g.status == kF640Active) == on) {
        g.enabled = on;
        WritePrefs();
        return noErr;               /* already there */
    }

    g.enabled = on;
    WritePrefs();
    g.switchCount++;
    err = Reconfigure();
    g.lastSwitchErr = err;
    return err;
}

static pascal short Fit640SetMode(short mode)
{
    g.mode = (mode == kModeCrop) ? kModeCrop : kModeScale;
    WritePrefs();
    return noErr;
}

/* ------------------------------------------------------------------ */
/* VBL task                                                            */

/*
 * Runs every 1/60 s. Each tick it first brings the rows around the mouse
 * pointer up to date, then continues a top-to-bottom sweep of the screen
 * for as long as the tick's budget lasts. With the shadow copy, rows that
 * have not changed cost only a comparison and nothing is written to video
 * memory for them.
 */
static void VBLProc(void)
{
    PixMapPtr pm;
    short n, y1;
    long credits;

    gTask.vblCount = 1;
    if (g.status != kF640Active || gBusy)
        return;

    /* If anything re-initialised the screen behind our back, the virtual
       screen is no longer in use: stop touching video memory. */
    pm = *(*LM_MainDevice)->gdPMap;
    if (pm->baseAddr != g.virtBase || pm->pixelSize != g.depth) {
        g.status = kF640Dormant;
        return;
    }

    /* Catch Window Manager ports created after we loaded. */
    n = PointWindowManager(1);
    g.portsFixed += n;
    g.portsFixedLate += n;

    CheckHotKeys();

    if (gNextRow == 0 && g.followMouse && !Scaling())
        FollowMouse();

    /* A moved view or a changed mode means everything has to be redrawn. */
    if (g.viewX != gLastViewX || g.viewY != gLastViewY || g.mode != gLastMode) {
        gLastViewX = g.viewX;
        gLastViewY = g.viewY;
        gLastMode = g.mode;
        gNextRow = 0;
        gFrameTicks = 0;
        gForce = 1;
    }

    if (!g.useShadow) {
        /* Plain slices of the screen, a fixed number of rows per tick. */
        if (gNextRow < g.realH) {
            y1 = gNextRow + (Scaling() ? g.scaleRowsPerTick
                                       : g.cropRowsPerTick);
            if (y1 > g.realH)
                y1 = g.realH;
            PresentRows(gNextRow, y1);
            gNextRow = y1;
        }
    } else {
        short mv = LM_Mouse.v;
        short cy = Scaling() ? (short)(mv - Div5(mv)) : (short)(mv - g.viewY);

        credits = g.budget[Scaling()];
        credits -= UpdateRows(cy - 20, cy + 28, 0);     /* the pointer */
        while (gNextRow < g.realH) {
            y1 = gNextRow + 8;
            if (y1 > g.realH)
                y1 = g.realH;
            credits -= UpdateRows(gNextRow, y1, gForce);
            gNextRow = y1;
            if (credits <= 0)
                break;
        }
    }

    gFrameTicks++;
    if (gNextRow >= g.realH && gFrameTicks >= g.frameDivisor) {
        gNextRow = 0;
        gFrameTicks = 0;
        gForce = 0;
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

static short Bench(void (*work)(void))
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
        work();
        frames++;
    }
    return frames;
}

static char *NewAlignedSysPtr(long size, char **raw)
{
    *raw = NewPtrSysClear(size + 16);
    if (*raw == 0)
        return 0;
    return (char *)(((unsigned long)*raw + 15) & ~15UL);
}

static short Install(void)
{
    long response;
    GDHandle gd;
    PixMapPtr pm;
    long bufSize, visBytes;
    char *raw, *rawShadow;
    short y;

    g.magic = kFit640Selector;
    g.version = kFit640Version;
    g.status = kF640Installing;
    g.frameDivisor = 2;
    g.benchTicks = kBenchTicks;
    g.benchCropLong = g.benchCropMove16 = g.benchScale = -1;
    g.benchCompare = -1;

    if (Gestalt(gestaltQuickdrawVersion, &response) != noErr ||
        response < gestalt8BitQD)
        return kF640NoColorQD;
    if (!LM_MMU32Bit)
        return kF640Mode24;
    if (Gestalt(gestaltProcessorType, &response) == noErr)
        g.cpu = (short)response;

    gd = LM_MainDevice;
    pm = *(*gd)->gdPMap;
    g.mode = kModeCrop;
    ReadRealScreen(gd);

    if (pm->bounds.left != 0 || pm->bounds.top != 0 ||
        g.realW > kVirtW || g.realH > kVirtH ||
        (g.realW == kVirtW && g.realH == kVirtH))
        return kF640ScreenSize;
    if (g.depth > kMaxDepth)
        return kF640Depth;

    /* Both buffers are sized for the deepest supported screen, so they
       never have to move when the depth changes. */
    bufSize = ((long)kVirtW * kMaxDepth >> 3) * kVirtH;
    g.virtBase = NewAlignedSysPtr(bufSize, &raw);
    if (g.virtBase == 0)
        return kF640NoMemory;
    gShadow = NewAlignedSysPtr(bufSize, &rawShadow);
    ReadRealScreen(gd);             /* now that the shadow copy exists */
    visBytes = VisBytes();

    g.followMouse = 1;
    g.viewX = 0;
    g.viewY = 0;

    /* Copy what is on the real screen into the top-left of the virtual
       one, so nothing visibly changes when we switch over. */
    HideCursor();
    for (y = 0; y < g.realH; y++) {
        BlockMoveData(g.realBase + (long)y * g.realRow,
                      g.virtBase + (long)y * g.virtRow, visBytes);
        if (gShadow)
            BlockMoveData(g.realBase + (long)y * g.realRow,
                          gShadow + (long)y * g.virtRow, visBytes);
    }

    /* Measure. The crop tests redraw the same picture; the scale test
       briefly shows the shrunken screen. */
    g.benchDepth = g.depth;
    g.benchCropLong = Bench(PresentCropLong);
    /* Holding Control at startup skips MOVE16 entirely, in case the
       accelerator or video hardware does not cope with it. */
    if (g.cpu >= gestalt68040 && !KeyIsDown(kKeyControl)) {
        g.benchCropMove16 = Bench(PresentCropMove16);
        g.useMove16 = (g.benchCropMove16 > g.benchCropLong);
    }
    if (g.canScale)
        g.benchScale = Bench(PresentScale);
    if (g.useShadow) {
        /* Costs must be set for UpdateRows; nothing differs yet, so this
           times pure comparing. */
        g.cmpCost[0] = g.writeCost[0] = 1;
        g.benchCompare = Bench(CompareAll);
    }
    ComputeBudgets();
    g.cropRowsPerTick = RowsPerTick(g.useMove16 ? g.benchCropMove16
                                                : g.benchCropLong);
    g.scaleRowsPerTick = RowsPerTick(g.benchScale);
    g.rowsWritten = 0;
    PresentCropRows(0, g.realH, g.useMove16);
    gForce = 1;

    /* Start the refresh task first; it does nothing until status is
       kF640Active. */
    gTask.qType = vType;
    gTask.vblAddr = (ProcPtr)VBLProc;
    gTask.vblCount = 1;
    gTask.vblPhase = 0;
    if (VInstall(&gTask) != noErr) {
        ShowCursor();
        DisposePtr(raw);
        if (rawShadow)
            DisposePtr(rawShadow);
        return kF640NoVBL;
    }

    gOldInitGDevice = (InitGDeviceProc)GetToolTrapAddress(kTrapInitGDevice);
    SetToolTrapAddress((ProcPtr)MyInitGDevice, kTrapInitGDevice);
    g.trapPatched = 1;
    g.switchProc = (void *)Fit640Switch;
    g.setModeProc = (void *)Fit640SetMode;
    g.dmPresent =
        GetToolTrapAddress(kTrapDisplayDispatch) !=
            GetToolTrapAddress(kTrapUnimplemented) &&
        Gestalt('dply', &response) == noErr && (response & 1);

    /* Start the way it was left: on or off, scaled or cropped. */
    ReadPrefs();
    if (!g.enabled) {
        ShowCursor();
        return kF640Off;
    }

    PointAtVirtualScreen(gd);
    g.portsFixed = PointWindowManager(1);
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
