/*
 * Fit640 Strip - a Control Strip module for the Fit640 extension.
 *
 * Shows two small buttons:
 *   "640" / "512"   the 640x480 screen is on / off      (click to switch)
 *   "Fit" / "1:1"   scale to fit / crop at full size    (click to switch)
 *
 * It does nothing by itself: it calls the routines the extension
 * publishes through its Gestalt selector. If the extension is not
 * loaded, the module does not appear.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Gestalt.h>
#include <Multiverse.h>
#include "Retro68Runtime.h"
#include "../src/Fit640.h"

/* Control Strip messages */
enum {
    sdevInitModule = 0,
    sdevCloseModule = 1,
    sdevFeatures = 2,
    sdevGetDisplayWidth = 3,
    sdevPeriodicTickle = 4,
    sdevDrawStatus = 5,
    sdevMouseClick = 6,
    sdevSaveSettings = 7,
    sdevShowBalloonHelp = 8
};

/* Feature bits */
enum {
    sdevWantMouseClicks = 0,
    sdevDontAutoTrack = 1,
    sdevHasCustomHelp = 2,
    sdevKeepModuleLocked = 3
};

#define kWidth      60
#define kLeftWidth  30
#define kGeneva     3

static short gShownStatus = -1;
static short gShownMode = -1;

static Fit640Globals *Globals(void)
{
    long response;

    if (Gestalt(kFit640Selector, &response) != noErr || response == 0)
        return 0;
    return (Fit640Globals *)response;
}

static void DrawCentred(ConstStr255Param s, short left, short right,
                        short baseline)
{
    MoveTo(left + (right - left - StringWidth(s)) / 2, baseline);
    DrawString(s);
}

static void Draw(Fit640Globals *g, const Rect *r, GrafPtr port)
{
    GrafPtr save;
    short font, size, mode, mid, baseline;
    Style face;
    Rect global;
    int on = (g->status == kF640Active);
    int scale = (g->mode == kModeScale);

    GetPort(&save);
    SetPort(port);
    font = port->txFont;
    size = port->txSize;
    face = port->txFace;
    mode = port->txMode;

    TextFont(kGeneva);
    TextSize(9);
    TextFace(0);
    TextMode(srcOr);

    EraseRect(r);
    mid = r->left + kLeftWidth;
    baseline = r->top + (r->bottom - r->top + 7) / 2;
    DrawCentred((ConstStr255Param)(on ? "\p640" : "\p512"), r->left, mid, baseline);
    MoveTo(mid, r->top + 2);
    LineTo(mid, r->bottom - 3);
    DrawCentred((ConstStr255Param)(scale ? "\pFit" : "\p1:1"), mid + 1, r->right, baseline);

    /* Tell the extension where we are (its info app uses this). */
    global = *r;
    LocalToGlobal((Point *)&global.top);
    LocalToGlobal((Point *)&global.bottom);
    g->stripRect = global;
    g->stripDraws++;

    TextFont(font);
    TextSize(size);
    TextFace(face);
    TextMode(mode);
    SetPort(save);

    gShownStatus = g->status;
    gShownMode = g->mode;
}

pascal long Fit640Strip(long message, long params, Rect *statusRect,
                        GrafPtr statusPort)
{
    Fit640Globals *g;

    RETRO68_RELOCATE();
    (void)params;

    g = Globals();

    switch (message) {
    case sdevInitModule:
        /* A negative result keeps the module out of the strip. */
        if (g == 0 || g->version < 5 || g->switchProc == 0 ||
            g->setModeProc == 0)
            return -1;
        return 0;

    case sdevFeatures:
        return (1L << sdevWantMouseClicks) | (1L << sdevDontAutoTrack) |
               (1L << sdevKeepModuleLocked);

    case sdevGetDisplayWidth:
        return kWidth;

    case sdevDrawStatus:
        if (g)
            Draw(g, statusRect, statusPort);
        return 0;

    case sdevPeriodicTickle:
        /* Redraw if something else changed the state (hot key, info app). */
        if (g && (g->status != gShownStatus || g->mode != gShownMode))
            Draw(g, statusRect, statusPort);
        return 0;

    case sdevMouseClick:
        if (g) {
            GrafPtr save;
            Point pt;

            GetPort(&save);
            SetPort(statusPort);
            GetMouse(&pt);
            SetPort(save);

            g->stripClicks++;
            if (pt.h < statusRect->left + kLeftWidth)
                ((Fit640SwitchProc)g->switchProc)(g->status != kF640Active);
            else
                ((Fit640SetModeProc)g->setModeProc)(
                    g->mode == kModeScale ? kModeCrop : kModeScale);
            Draw(g, statusRect, statusPort);
        }
        return 0;
    }
    return 0;
}
