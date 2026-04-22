/**
 * gaming_bios.c
 * RNFS Mini Core - Gaming BIOS UI for UEFI
 *
 * Build with EDK II or gnu-efi (see comments per section).
 * NO stdio.h / stdlib.h / string.h - pure UEFI services only.
 *
 * EDK II compile:  add to your .inf INF file:
 *   [LibraryClasses]
 *     UefiLib | MdePkg/Library/UefiLib/UefiLib.inf
 *
 * gnu-efi compile example:
 *   gcc -I/usr/include/efi -I/usr/include/efi/x86_64 \
 *       -ffreestanding -fno-stack-protector -fpic -fshort-wchar \
 *       -mno-red-zone -DEFI_FUNCTION_WRAPPER \
 *       -c gaming_bios.c -o gaming_bios.o
 */

/* ─────────────────────────────────────────────
   HEADERS  (EDK II path shown; gnu-efi is similar)
   ───────────────────────────────────────────── */
#include <efi.h>
#include <efilib.h>



/* ── If using gnu-efi instead, swap the block above for:
#include <efi.h>
#include <efilib.h>
   ── */


/* ═══════════════════════════════════════════════════════════════
   1. GRAPHICS INITIALIZATION  (GOP + Framebuffer)
   ═══════════════════════════════════════════════════════════════ */

/* Target resolution */
#define SCREEN_W  1024
#define SCREEN_H   768

/* Colour helpers – GOP uses BGRA (Blue-Green-Red-Reserved) */
typedef struct { UINT8 Blue; UINT8 Green; UINT8 Red; UINT8 Reserved; } PIXEL;

#define RGB(r,g,b)  ((PIXEL){(b),(g),(r),0})

/* Palette */
#define COL_BLACK       RGB(0x00, 0x00, 0x00)
#define COL_DARK_RED    RGB(0x4b, 0x00, 0x00)   /* #4b0000 – gradient start */
#define COL_ACCENT      RGB(0xcc, 0x00, 0x00)   /* #cc0000 – bright red     */
#define COL_PANEL       RGB(0x14, 0x00, 0x00)   /* panel background         */
#define COL_WHITE       RGB(0xff, 0xff, 0xff)
#define COL_TOGGLE_ON   RGB(0xcc, 0x00, 0x00)
#define COL_TOGGLE_OFF  RGB(0x44, 0x44, 0x44)

/* Global GOP state */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *gGop  = NULL;
static UINT32                        gPPSL  = 0;   /* pixels-per-scan-line  */
static EFI_GRAPHICS_OUTPUT_BLT_PIXEL *gFB  = NULL; /* framebuffer base ptr  */

/**
 * GopInit
 * Locates GOP, sets 1024×768 mode, and caches the framebuffer pointer.
 * Returns EFI_SUCCESS or a GOP error.
 */
EFI_STATUS GopInit(VOID)
{
    EFI_STATUS  Status;
    EFI_GUID    GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    UINTN       ModeIndex, NumModes;

    /* Locate GOP – LocateProtocol finds the first handle */
    Status = gBS->LocateProtocol(&GopGuid, NULL, (VOID **)&gGop);
    if (EFI_ERROR(Status)) return Status;

    NumModes = gGop->Mode->MaxMode;

    /* Walk modes looking for 1024×768 */
    for (ModeIndex = 0; ModeIndex < NumModes; ModeIndex++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
        UINTN InfoSize;

        Status = gGop->QueryMode(gGop, (UINT32)ModeIndex, &InfoSize, &Info);
        if (EFI_ERROR(Status)) continue;

        if (Info->HorizontalResolution == SCREEN_W &&
            Info->VerticalResolution   == SCREEN_H) {
            Status = gGop->SetMode(gGop, (UINT32)ModeIndex);
            if (!EFI_ERROR(Status)) {
                gPPSL = gGop->Mode->Info->PixelsPerScanLine;
                /* Framebuffer is a linear memory-mapped region */
                gFB   = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)
                        (UINTN)gGop->Mode->FrameBufferBase;
                return EFI_SUCCESS;
            }
        }
    }
    return EFI_UNSUPPORTED;   /* 1024×768 not available */
}


/* ═══════════════════════════════════════════════════════════════
   2. UI RENDERING LOGIC
   ═══════════════════════════════════════════════════════════════ */

/**
 * PutPixel  –  direct framebuffer write (fastest path, no BLT overhead)
 */
static VOID PutPixel(UINTN X, UINTN Y, PIXEL C)
{
    /* gFB is EFI_GRAPHICS_OUTPUT_BLT_PIXEL* which is {Blue,Green,Red,Rsvd}
       – identical layout to our PIXEL typedef so a cast is safe.           */
    if (X < SCREEN_W && Y < SCREEN_H)
        gFB[Y * gPPSL + X] = *(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)&C;
}

/**
 * DrawFilledRect  –  draw a solid-colour filled rectangle.
 * Used for buttons, panels, toggle tracks, selection highlights.
 *
 * @param X      left edge (pixels)
 * @param Y      top edge  (pixels)
 * @param W      width     (pixels)
 * @param H      height    (pixels)
 * @param Color  fill colour
 */
VOID DrawFilledRect(UINTN X, UINTN Y, UINTN W, UINTN H, PIXEL Color)
{
    UINTN Row, Col;
    for (Row = Y; Row < Y + H; Row++)
        for (Col = X; Col < X + W; Col++)
            PutPixel(Col, Row, Color);
}

/**
 * DrawBackground
 * Vertical gradient: dark red (#4b0000) at the top blending to black at
 * the bottom, matching the volcanic RNFS desktop wallpaper aesthetic.
 *
 * Formula: for row r in [0, SCREEN_H)
 *   t    = r / (SCREEN_H - 1)          -- 0.0 at top, 1.0 at bottom
 *   red  = 0x4b * (1 - t)              -- lerp from 0x4b → 0x00
 *   blue = 0x00,  green = 0x00
 *
 * Integer-only: multiply by 0x4b (= 75) then divide by SCREEN_H.
 */
VOID DrawBackground(VOID)
{
    UINTN Row, Col;
    for (Row = 0; Row < SCREEN_H; Row++) {
        /* Integer lerp: red = 0x4b * (SCREEN_H - 1 - Row) / (SCREEN_H - 1) */
        UINT8 Red = (UINT8)( (UINTN)0x4b * (SCREEN_H - 1 - Row)
                             / (SCREEN_H - 1) );
        PIXEL C = RGB(Red, 0, 0);
        for (Col = 0; Col < SCREEN_W; Col++)
            PutPixel(Col, Row, C);
    }
}

/**
 * DrawBorder  –  1-pixel outline of a rectangle (for icon tiles / buttons)
 */
VOID DrawBorder(UINTN X, UINTN Y, UINTN W, UINTN H, PIXEL Color)
{
    DrawFilledRect(X,       Y,       W, 1, Color);  /* top    */
    DrawFilledRect(X,       Y+H-1,   W, 1, Color);  /* bottom */
    DrawFilledRect(X,       Y,       1, H, Color);  /* left   */
    DrawFilledRect(X+W-1,   Y,       1, H, Color);  /* right  */
}

/**
 * DrawToggle  –  a rounded-ish toggle button (simplified rectangular version)
 * @param X, Y   top-left of the 40×20 toggle widget
 * @param On     TRUE = red (enabled), FALSE = grey (disabled)
 */
VOID DrawToggle(UINTN X, UINTN Y, BOOLEAN On)
{
    PIXEL TrackColor = On ? COL_TOGGLE_ON : COL_TOGGLE_OFF;
    PIXEL KnobColor  = COL_WHITE;
    UINTN KnobX      = On ? (X + 22) : X;       /* knob slides left/right */

    DrawFilledRect(X,     Y,     40, 20, TrackColor);
    DrawFilledRect(KnobX, Y + 2, 16, 16, KnobColor);
}


/* ═══════════════════════════════════════════════════════════════
   3. ASSET HANDLING  –  BMP icon decoder + header-array guide
   ═══════════════════════════════════════════════════════════════

   OPTION A (Recommended for small icon sets): embed icons as C byte arrays.
   ─────────────────────────────────────────────────────────────────────────
   Steps:
     1. Export each icon as a raw BGRA bitmap (no file header):
          $ convert icon.png -depth 8 BGRA:icon_raw.bin
     2. Convert to a C array:
          $ xxd -i icon_raw.bin > icon_web_browser.h
        This produces:
          unsigned char icon_raw_bin[] = { 0x1f, 0x8b, ... };
          unsigned int  icon_raw_bin_len = 1024;
     3. Include the header and blit:
          #include "icon_web_browser.h"
          BlitIcon(icon_raw_bin, 32, 32, X, Y);

   OPTION B: Minimal BMP decoder (handles 24-bit uncompressed BMP).
   ─────────────────────────────────────────────────────────────────
   Store BMP files inside the EFI system partition and load them with
   EFI_FILE_PROTOCOL (SimpleFileSystem).  The decoder below converts
   the BMP pixel rows (bottom-up, BGR) to top-down BGRA for the GOP.
   ═══════════════════════════════════════════════════════════════ */

/* BMP file / info-header layout (packed, no padding) */
#pragma pack(1)
typedef struct {
    UINT16 Signature;       /* 'BM' = 0x424D */
    UINT32 FileSize;
    UINT32 Reserved;
    UINT32 DataOffset;      /* offset to pixel data */
    UINT32 HeaderSize;      /* 40 for BITMAPINFOHEADER */
    INT32  Width;
    INT32  Height;          /* positive = bottom-up storage */
    UINT16 Planes;
    UINT16 BitsPerPixel;    /* must be 24 for this decoder */
    UINT32 Compression;     /* 0 = BI_RGB */
    UINT32 ImageSize;
    UINT32 XPixelsPerMeter;
    UINT32 YPixelsPerMeter;
    UINT32 ColorsUsed;
    UINT32 ColorsImportant;
} BMP_HEADER;
#pragma pack()

/**
 * BlitRawIcon
 * Draw a pre-decoded BGRA icon (width × height bytes × 4 channels)
 * at screen position (DestX, DestY).
 * 'IconData' points to the raw pixel array (no file headers).
 */
VOID BlitRawIcon(
    CONST UINT8 *IconData,
    UINTN        Width,
    UINTN        Height,
    UINTN        DestX,
    UINTN        DestY)
{
    UINTN Row, Col, Idx;
    for (Row = 0; Row < Height; Row++) {
        for (Col = 0; Col < Width; Col++) {
            Idx = (Row * Width + Col) * 4;
            PIXEL P;
            P.Blue     = IconData[Idx + 0];
            P.Green    = IconData[Idx + 1];
            P.Red      = IconData[Idx + 2];
            P.Reserved = 0;
            PutPixel(DestX + Col, DestY + Row, P);
        }
    }
}

/**
 * DecodeBmp24
 * Minimal 24-bit uncompressed BMP → framebuffer blit.
 * 'BmpData' points to the raw file bytes already loaded into RAM.
 */
VOID DecodeBmp24(
    CONST UINT8 *BmpData,
    UINTN        DestX,
    UINTN        DestY)
{
    CONST BMP_HEADER *H = (CONST BMP_HEADER *)BmpData;
    if (H->Signature != 0x4D42)  return;  /* not 'BM' */
    if (H->BitsPerPixel != 24)   return;  /* only 24-bit supported here */
    if (H->Compression  != 0)    return;  /* only BI_RGB */

    INT32  W          = H->Width;
    INT32  ImgH       = H->Height;         /* may be negative (top-down) */
    UINT32 PixelStart = H->DataOffset;
    BOOLEAN TopDown   = (ImgH < 0);
    UINTN   AbsH      = (UINTN)(TopDown ? -ImgH : ImgH);

    /* BMP rows are padded to 4-byte boundaries */
    UINTN RowBytes = (((UINTN)W * 3) + 3) & ~(UINTN)3;

    UINTN Row;
    for (Row = 0; Row < AbsH; Row++) {
        /* BMP bottom-up: row 0 in file = last row on screen */
        UINTN SrcRow = TopDown ? Row : (AbsH - 1 - Row);
        CONST UINT8 *Src = BmpData + PixelStart + SrcRow * RowBytes;
        UINTN Col;
        for (Col = 0; Col < (UINTN)W; Col++) {
            PIXEL P;
            P.Blue     = Src[Col * 3 + 0];
            P.Green    = Src[Col * 3 + 1];
            P.Red      = Src[Col * 3 + 2];
            P.Reserved = 0;
            PutPixel(DestX + Col, DestY + Row, P);
        }
    }
}


/* ═══════════════════════════════════════════════════════════════
   4. SYSTEM INFO  –  Total RAM  +  Uptime
   ═══════════════════════════════════════════════════════════════ */

/**
 * GetTotalRam
 * Walks the UEFI memory map and sums all usable conventional memory.
 * Returns the total in bytes (caller converts to MB/GB for display).
 *
 * NOTE: gBS->AllocatePool is used for the map buffer; we free it after.
 */
UINT64 GetTotalRam(VOID)
{
    EFI_STATUS              Status;
    EFI_MEMORY_DESCRIPTOR  *MemMap       = NULL;
    UINTN                   MemMapSize   = 0;
    UINTN                   MapKey, DescSize;
    UINT32                  DescVersion;
    UINT64                  TotalBytes   = 0;

    /* First call: get required buffer size */
    Status = gBS->GetMemoryMap(&MemMapSize, MemMap,
                               &MapKey, &DescSize, &DescVersion);
    if (Status != EFI_BUFFER_TOO_SMALL) return 0;

    /* Add slack for descriptors that may appear during AllocatePool */
    MemMapSize += 2 * DescSize;
    Status = gBS->AllocatePool(EfiLoaderData, MemMapSize, (VOID **)&MemMap);
    if (EFI_ERROR(Status)) return 0;

    Status = gBS->GetMemoryMap(&MemMapSize, MemMap,
                               &MapKey, &DescSize, &DescVersion);
    if (!EFI_ERROR(Status)) {
        UINT8 *Entry = (UINT8 *)MemMap;
        UINT8 *End   = Entry + MemMapSize;
        while (Entry < End) {
            EFI_MEMORY_DESCRIPTOR *D = (EFI_MEMORY_DESCRIPTOR *)Entry;
            /* EfiConventionalMemory = free RAM usable by OS */
            if (D->Type == EfiConventionalMemory)
                TotalBytes += D->NumberOfPages * EFI_PAGE_SIZE;
            Entry += DescSize;
        }
    }

    gBS->FreePool(MemMap);
    return TotalBytes;
}

/**
 * GetUptimeSeconds
 * UEFI exposes a monotonic counter via gBS->GetNextMonotonicCount().
 * However the simplest reliable uptime source is EFI_TIME:
 *   – Record boot time once at startup, compare with current time.
 * For a rolling tick counter use gBS->Stall (microseconds) instead.
 *
 * Here we use the Runtime GetTime approach and store the boot epoch.
 * Call RecordBootTime() once at startup, then call GetUptimeSeconds()
 * whenever you need to refresh the uptime display.
 */
static EFI_TIME gBootTime;

VOID RecordBootTime(VOID)
{
    gRT->GetTime(&gBootTime, NULL);
}

/* Very simplified: only handles uptime < 1 hour correctly.
   For production, implement a full HH:MM:SS delta. */
UINT32 GetUptimeSeconds(VOID)
{
    EFI_TIME Now;
    gRT->GetTime(&Now, NULL);

    INT32 Seconds =
        ((INT32)Now.Hour   - gBootTime.Hour)   * 3600 +
        ((INT32)Now.Minute - gBootTime.Minute) *   60 +
        ((INT32)Now.Second - gBootTime.Second);

    if (Seconds < 0) Seconds += 86400;   /* midnight rollover */
    return (UINT32)Seconds;
}

/**
 * FormatUptime
 * Converts seconds → "HH:MM:SS" into caller-supplied 9-char buffer.
 * Pure integer arithmetic – no sprintf / printf.
 */
VOID FormatUptime(UINT32 Seconds, CHAR8 *Buf /* must be ≥9 bytes */)
{
    UINT32 H = Seconds / 3600;
    UINT32 M = (Seconds % 3600) / 60;
    UINT32 S = Seconds % 60;

    /* Manual two-digit formatting */
    Buf[0] = '0' + (H / 10);  Buf[1] = '0' + (H % 10);  Buf[2] = ':';
    Buf[3] = '0' + (M / 10);  Buf[4] = '0' + (M % 10);  Buf[5] = ':';
    Buf[6] = '0' + (S / 10);  Buf[7] = '0' + (S % 10);  Buf[8] = '\0';
}


/* ═══════════════════════════════════════════════════════════════
   5. EVENT LOOP  –  Mouse (EFI_SIMPLE_POINTER_PROTOCOL)
   ═══════════════════════════════════════════════════════════════ */

static EFI_SIMPLE_POINTER_PROTOCOL *gMouse = NULL;

/* Signed accumulator for absolute cursor position */
static INT32 gCursorX = SCREEN_W / 2;
static INT32 gCursorY = SCREEN_H / 2;

/**
 * MouseInit
 * Locates the first available Simple Pointer (mouse/touchpad) protocol.
 */
EFI_STATUS MouseInit(VOID)
{
    EFI_GUID MouseGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    return gBS->LocateProtocol(&MouseGuid, NULL, (VOID **)&gMouse);
}

/**
 * PollMouse
 * Reads relative movement from the mouse and updates the global cursor.
 * @param OutX / OutY   current absolute cursor position after update
 * @param OutClick      set to TRUE if left button is pressed
 *
 * EFI_SIMPLE_POINTER_STATE.RelativeMovementX is in "counts"; divide by
 * Mode->ResolutionX to convert to pixels (1 count = 1 micron by spec).
 * A divisor of 500–2000 works well for typical touchpads.
 */
VOID PollMouse(INT32 *OutX, INT32 *OutY, BOOLEAN *OutClick)
{
    EFI_SIMPLE_POINTER_STATE State;

    if (gMouse == NULL) { *OutX = gCursorX; *OutY = gCursorY; return; }

    EFI_STATUS Status = gMouse->GetState(gMouse, &State);
    if (Status == EFI_SUCCESS) {
        /* RelativeMovementX is INT64 in microns; scale to pixels */
        gCursorX += (INT32)(State.RelativeMovementX / 1000);
        gCursorY += (INT32)(State.RelativeMovementY / 1000);

        /* Clamp to screen bounds */
        if (gCursorX < 0)           gCursorX = 0;
        if (gCursorX >= SCREEN_W)   gCursorX = SCREEN_W - 1;
        if (gCursorY < 0)           gCursorY = 0;
        if (gCursorY >= SCREEN_H)   gCursorY = SCREEN_H - 1;

        *OutClick = State.LeftButton;
    }
    *OutX = gCursorX;
    *OutY = gCursorY;
}

/**
 * DrawCursor  –  simple 8×8 cross-hair cursor
 */
VOID DrawCursor(INT32 X, INT32 Y)
{
    DrawFilledRect((UINTN)X - 4, (UINTN)Y,     8, 1, COL_WHITE);  /* horiz */
    DrawFilledRect((UINTN)X,     (UINTN)Y - 4, 1, 8, COL_WHITE);  /* vert  */
}

/**
 * HitTest  –  axis-aligned bounding box check for click regions
 */
BOOLEAN HitTest(INT32 CX, INT32 CY,
                UINTN RX, UINTN RY, UINTN RW, UINTN RH)
{
    return (CX >= (INT32)RX && CX < (INT32)(RX + RW) &&
            CY >= (INT32)RY && CY < (INT32)(RY + RH));
}


/* ═══════════════════════════════════════════════════════════════
   DEMO ICON TILE  (without real font rendering)
   ═══════════════════════════════════════════════════════════════
   Full text rendering in UEFI requires either:
     a) EFI_HII / HII_FONT_PROTOCOL  (complex, available in full UEFI)
     b) A hand-rolled bitmap font (embed a 8×8 or 16×16 bitmap font as
        a C array – see Tom Thumb or Unscii for permissively-licensed
        bitmap fonts that are trivially embedded)
   The tile below draws the border and fill only as a placeholder.     */

VOID DrawIconTile(UINTN X, UINTN Y, CONST UINT8 *Icon32x32 /* may be NULL */)
{
    /* Dark panel background */
    DrawFilledRect(X, Y, 80, 80, COL_PANEL);
    /* Red border */
    DrawBorder(X, Y, 80, 80, COL_ACCENT);
    /* Icon bitmap (32×32 BGRA) centred in the 80×80 tile */
    if (Icon32x32)
        BlitRawIcon(Icon32x32, 32, 32, X + 24, Y + 10);
}


/* ═══════════════════════════════════════════════════════════════
   EFI_MAIN  –  ties everything together
   ═══════════════════════════════════════════════════════════════ */

EFI_STATUS
EFIAPI
UefiMain(
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS Status;

    /* ── 0. Hide the text cursor / clear screen ── */
    SystemTable->ConOut->EnableCursor(SystemTable->ConOut, FALSE);
    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);

    /* ── 1. GOP init ── */
    Status = GopInit();
    if (EFI_ERROR(Status)) return Status;

    /* ── 4. Record boot time for uptime counter ── */
    RecordBootTime();

    /* ── 5. Mouse init (non-fatal if absent) ── */
    MouseInit();   /* ignore error – many VMs have no pointer device */

    /* ── Gather system info ── */
    UINT64 TotalRamBytes = GetTotalRam();
    UINT64 TotalRamMB    = TotalRamBytes / (1024 * 1024);

    /* ══════════════════════════════════════════
       MAIN RENDER + EVENT LOOP
       ══════════════════════════════════════════ */
    INT32   MouseX = SCREEN_W / 2, MouseY = SCREEN_H / 2;
    BOOLEAN Clicked = FALSE;
    BOOLEAN Running = TRUE;

    /* Keyboard input event (for ESC to exit) */
    EFI_INPUT_KEY Key;

    while (Running) {

        /* ── Redraw frame ── */
        DrawBackground();

        /* Left sidebar panel */
        DrawFilledRect(0, 0, 150, SCREEN_H, COL_PANEL);
        DrawFilledRect(148, 0, 2, SCREEN_H, COL_ACCENT);   /* separator */

        /* Icon grid (positions match the RNFS layout roughly) */
        /* Pass NULL for Icon32x32 until you embed real icon arrays */
        DrawIconTile(175, 145, NULL);   /* Web Browser  */
        DrawIconTile(270, 145, NULL);   /* Text Editor  */
        DrawIconTile(365, 145, NULL);   /* Media Player */
        DrawIconTile(175, 240, NULL);   /* Image Viewer */
        DrawIconTile(270, 240, NULL);   /* Disk Utility */
        DrawIconTile(365, 240, NULL);   /* Network      */
        DrawIconTile(175, 335, NULL);   /* Pkg Manager  */
        DrawIconTile(270, 335, NULL);   /* Sys Monitor  */
        DrawIconTile(365, 335, NULL);   /* Tools        */

        /* Right settings panel */
        DrawFilledRect(670, 0, SCREEN_W - 670, SCREEN_H, COL_PANEL);
        DrawFilledRect(670, 0, 2, SCREEN_H, COL_ACCENT);

        /* Toggle examples */
        DrawToggle(SCREEN_W - 60, 160, TRUE);   /* High Performance: ON  */
        DrawToggle(SCREEN_W - 60, 210, TRUE);   /* Memory Opt:       ON  */
        DrawToggle(SCREEN_W - 60, 260, TRUE);   /* I/O Boost:        ON  */
        DrawToggle(SCREEN_W - 60, 310, FALSE);  /* Power Efficiency: OFF */

        /* Status bar at bottom */
        DrawFilledRect(0, SCREEN_H - 48, SCREEN_W, 48, COL_PANEL);
        DrawFilledRect(0, SCREEN_H - 50, SCREEN_W, 2, COL_ACCENT);

        /* ── Uptime overlay (tile in status bar) ── */
        UINT32 Uptime = GetUptimeSeconds();
        CHAR8  UptimeBuf[9];
        FormatUptime(Uptime, UptimeBuf);
        /* TODO: render UptimeBuf with your bitmap font renderer at (600, SCREEN_H-30) */

        /* ── RAM display (status bar) ── */
        /* TODO: render TotalRamMB with bitmap font at (300, SCREEN_H-30) */
        (VOID)TotalRamMB;   /* suppress unused-variable warning for now */

        /* ── Mouse polling + cursor ── */
        PollMouse(&MouseX, &MouseY, &Clicked);
        DrawCursor(MouseX, MouseY);

        /* ── Hit-test: Web Browser tile ── */
        if (Clicked && HitTest(MouseX, MouseY, 175, 145, 80, 80)) {
            DrawFilledRect(175, 145, 80, 80, COL_ACCENT); /* highlight */
            /* TODO: launch sub-menu or load next .efi payload */
        }

        /* ── Keyboard: ESC to exit ── */
        Status = SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key);
        if (!EFI_ERROR(Status) && Key.ScanCode == SCAN_ESC)
            Running = FALSE;

        /* Throttle: ~30 fps (33 ms per frame) */
        gBS->Stall(33000);
    }

    return EFI_SUCCESS;
}