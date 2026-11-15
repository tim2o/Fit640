/*
 * Fit640 - shared definitions between the extension and the Info app.
 *
 * The extension registers a Gestalt selector 'F640' whose response is a
 * pointer to this structure (it lives in the system heap).
 */
#ifndef FIT640_H
#define FIT640_H

#define kFit640Selector 0x46363430L /* 'F640' */
#define kFit640Version  2

#define kVirtW 640
#define kVirtH 480

/* Why the extension did or did not install. */
enum {
    kF640Active = 0,       /* installed and presenting */
    kF640NoColorQD = 1,    /* no Color QuickDraw */
    kF640Mode24 = 2,       /* machine is in 24-bit addressing mode */
    kF640ScreenSize = 3,   /* main screen is already >= 640x480, or bigger */
    kF640Depth = 4,        /* unsupported depth (more than 16 bits) */
    kF640NoMemory = 5,     /* could not allocate the virtual screen */
    kF640NoVBL = 6,        /* could not install the screen refresh task */
    kF640Dormant = 7,      /* something reset the screen (e.g. depth change) */
    kF640Installing = 8    /* startup did not finish */
};

enum {
    kModeCrop = 0,         /* show a 1:1 window of the virtual screen */
    kModeScale = 1         /* shrink the whole virtual screen (5:4)   */
};

typedef struct Fit640Globals {
    unsigned long magic;       /* kFit640Selector */
    short version;
    short status;              /* kF640... */
    short mode;                /* kModeCrop / kModeScale */
    short canScale;            /* scaling is available (8-bit, 512x384) */
    short depth;               /* bits per pixel at install time */
    short useMove16;           /* crop uses the 68040 MOVE16 copy */
    short cpu;                 /* Gestalt 'proc' value (5 = 68040) */

    short realW, realH;        /* the real screen */
    long  realRow;             /* its rowBytes */
    char *realBase;            /* its video memory */

    long  virtRow;             /* rowBytes of the virtual 640x480 screen */
    char *virtBase;            /* the virtual screen buffer */

    short viewX, viewY;        /* crop mode: top-left of the visible window */
    short frameDivisor;        /* shortest time per frame, in ticks */

    /* Startup benchmark: full frames completed in benchTicks ticks (1/60 s).
       -1 = not measured. */
    short benchTicks;
    short benchCropLong;       /* crop, plain 32-bit copy */
    short benchCropMove16;     /* crop, MOVE16 copy */
    short benchScale;          /* 5:4 scale */

    unsigned long framesPresented;

    /* Added in version 2 */
    short followMouse;         /* crop: 1 = view follows the mouse,
                                  0 = view locked on the centre */
    short wmExisted;           /* the Window Manager was already running when
                                  we loaded (Mac OS 8), so its ports were
                                  redirected */
    short portsFixed;          /* how many existing ports were redirected */
    short portsFixedLate;      /* ...of which after startup, by the refresh task */
    short cropRowsPerTick;     /* screen rows redrawn per 1/60 s, from the */
    short scaleRowsPerTick;    /* startup speed test (whole screen = realH) */
} Fit640Globals;

#endif
