/*****************************************************************************

Copyright (c) 2022  Michal Necasek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*****************************************************************************/

/* Display driver module initialization. */

/* Display driver mode management. Adapted from boxv9x: any size whose framebuffer fits
 * in VRAM, set through the DISPI registers.
 */

#include "winhack.h"
#include <gdidefs.h>
#include <dibeng.h>
#include <valmode.h>
#include <minivdd.h>
#include "minidrv.h"
#include "dispi.h"


/* Generic DPMI calls, only used in this module. */
extern WORD DPMI_AllocLDTDesc( WORD cSelectors );
#pragma aux DPMI_AllocLDTDesc = \
    "xor    ax, ax"             \
    "int    31h"                \
    "jnc    OK"                 \
    "xor    ax, ax"             \
    "OK:"                       \
    parm [cx];

extern void DPMI_SetSegBase( WORD Selector, DWORD Base );
#pragma aux DPMI_SetSegBase =   \
    "mov    ax, 7"              \
    "int    31h"                \
    parm [bx] [cx dx];

extern void DPMI_SetSegLimit( WORD Selector, DWORD Limit );
#pragma aux DPMI_SetSegLimit =   \
    "mov    ax, 8"              \
    "int    31h"                \
    parm [bx] [cx dx];

/* NB: Compiler insists on CX:BX and DI:SI, DPMI needs it word swapped. */
extern DWORD DPMI_MapPhys( DWORD Base, DWORD Size );
#pragma aux DPMI_MapPhys =      \
    "xchg   cx, bx"             \
    "xchg   si, di"             \
    "mov    ax, 800h"           \
    "int    31h"                \
    "jnc    OK"                 \
    "xor    bx, bx"             \
    "xor    cx, cx"             \
    "OK:"                       \
    "mov    dx, bx"             \
    "mov    ax, cx"             \
    parm [cx bx] [di si];

/* Zeroes dwCount DWORDs from offset 0 of a 32-bit selector. */
extern void FillZeroDwords( WORD wSel, DWORD dwCount );
#pragma aux FillZeroDwords =    \
    ".386"                      \
    "push   es"                 \
    "mov    es, ax"             \
    "shl    ecx, 16"            \
    "mov    cx, dx"             \
    "xor    eax, eax"           \
    "xor    edi, edi"           \
    "cld"                       \
    "db     67h"                \
    "rep    stosd"              \
    "pop    es"                 \
    parm [ax] [cx dx] modify [ax cx dx di];


WORD wScreenX       = 0;
WORD wScreenY       = 0;
WORD wScreenPitchPixels = 0;
WORD BitBltDevProc  = 0;
WORD ScreenSelector = 0;
WORD wPDeviceFlags  = 0;

static WORD     wScreenPitchBytes = 0;  /* Current scanline pitch. */

/* On Entry:
 * EAX   = Function code (VDD_DRIVER_REGISTER)
 * EBX   = This VM's handle
 * ECX   = Size of all visible scanlines in bytes
 * EDX   = Zero to tell VDD to try virtualizing
 * ES:DI = Pointer to function called when switching
 *         back from fullscreen.
 *
 * On Return:
 * EAX   = Amount of video memory used by VDD in bytes,
 *         or function code if VDD call failed.
 */
extern DWORD CallVDDRegister( WORD Function, WORD wPitch, WORD wHeight, void _far *fRHR );
#pragma aux CallVDDRegister =       \
    ".386"                          \
    "movzx  eax, ax"                \
    "movzx  edx, dx"                \
    "mul    edx"                    \
    "mov    ecx, eax"               \
    "movzx  eax, bx"                \
    "movzx  ebx, OurVMHandle"       \
    "xor    edx, edx"               \
    "call   dword ptr VDDEntryPoint"\
    "mov    edx, eax"               \
    "shr    edx, 16"                \
    parm [bx] [ax] [dx] [es di];


#pragma code_seg( _INIT );

/* Returns the reason a mode cannot be set, or VALMODE_YES. The adapter has the last
 * word on the width (DISPI_SetMode); this checks the depth and that the framebuffer at
 * its narrowest DWORD-aligned stride fits in VRAM.
 */
static UINT CheckMode( WORD wXRes, WORD wYRes, WORD wDepth )
{
    DWORD   dwBytes;

    switch( wDepth ) {
    case 8:
    case 16:
    case 24:
    case 32:
        break;
    default:
        return( VALMODE_NO_UNKNOWN );
    }
    if( !wXRes || !wYRes )
        return( VALMODE_NO_UNKNOWN );
    dwBytes = (DWORD)DISPI_AlignedWidth( wXRes, wDepth ) * (wDepth / 8) * wYRes;
    if( dwBytes > dwVideoMemorySize )
        return( VALMODE_NO_NOMEM );
    return( VALMODE_YES );
}


/* Clear the visible screen by setting it to all black (zeros). */
static void ClearVisibleScreen( void )
{
    FillZeroDwords( ScreenSelector, (DWORD)wScreenPitchBytes * wScreenY / 4 );
}


static WORD AllocLinearSelector( DWORD dwPhysAddr, DWORD dwSize )
{
    WORD    wSel;
    DWORD   dwLinear;

    wSel = DPMI_AllocLDTDesc( 1 );
    if( !wSel )
        return( 0 );

    dwLinear = DPMI_MapPhys( dwPhysAddr, dwSize );
    if( !dwLinear ) {
        FreeSelector( wSel );
        return( 0 );
    }

    DPMI_SetSegBase( wSel, dwLinear );
    DPMI_SetSegLimit( wSel, dwSize - 1 );

    return( wSel );
}


/* Set the given mode in hardware. If bFullSet is non-zero, then also
 * reinitialize globals. When re-establishing a previously set mode (e.g.
 * coming back from fullscreen), bFullSet will be zero.
 * NB: BPP won't change at runtime.
 */
static int SetDisplayMode( WORD wXRes, WORD wYRes, int bFullSet )
{
    WORD    wHw;

    dbg_printf( "SetDisplayMode: wXRes=%u wYRes=%u\n", wXRes, wYRes );

    /* Inform the VDD that the mode is about to change. */
    CallVDD( VDD_PRE_MODE_CHANGE );

    wHw = DISPI_SetMode( wXRes, wYRes, wBpp );
    if( !wHw )
        return( 0 );

    if( bFullSet ) {
        wScreenX = wXRes;
        wScreenY = wYRes;
        wScreenPitchPixels = wHw;
        wScreenPitchBytes  = wHw * (wBpp / 8);
        BitBltDevProc      = NULL;      /* No acceleration implemented. */
        wPDeviceFlags      = MINIDRIVER | VRAM;
        if( wBpp == 16 ) {
            wPDeviceFlags |= FIVE6FIVE; /* Needed for 16bpp modes. */
        }
    }
    return( 1 );
}


/* Forward declaration. */
void __far RestoreDesktopMode( void );

/* Sets the configured mode (wScrX, wScrY, wBpp). Returns zero on failure. */
int PhysicalEnable( void )
{
    UINT    rc;

    rc = CheckMode( wScrX, wScrY, wBpp );
    if( rc != VALMODE_YES ) {
        dbg_printf( "PhysicalEnable: %ux%ux%u not valid (%u)\n", wScrX, wScrY, wBpp, rc );
        return( 0 );
    }

    if( !SetDisplayMode( wScrX, wScrY, 1 ) ) {
        dbg_printf( "PhysicalEnable: SetDisplayMode failed! wScrX=%u wScrY=%u wBpp=%u\n", wScrX, wScrY, wBpp );
        return( 0 );
    }

    if( !ScreenSelector ) {
        ScreenSelector = AllocLinearSelector( dwPhysVRAM, dwVideoMemorySize );
        if( !ScreenSelector ) {
            dbg_printf( "PhysicalEnable: cannot map %lX bytes at %lX\n", dwVideoMemorySize, dwPhysVRAM );
            return( 0 );
        }
    }

    /* NB: It's not fatal if the VDD does not take the registration. */
    CallVDDRegister( VDD_DRIVER_REGISTER, wScreenPitchBytes, wScreenY, RestoreDesktopMode );

    /* Let the VDD know that the mode changed. */
    CallVDD( VDD_POST_MODE_CHANGE );
    CallVDD( VDD_SAVE_DRIVER_STATE );

    ClearVisibleScreen();

    return( 1 );
}


/* Check if the requested mode can be set. Return yes, no (with a reason),
 * or maybe. Must be exported by name, recommended ordinal 700.
 * NB: Can be called when the driver is not the current display driver.
 */
#pragma aux ValidateMode loadds;    /* __loadds not operational due to prototype in valmode.h */
UINT WINAPI __loadds ValidateMode( DISPVALMODE FAR *lpValMode )
{
    UINT    rc;

    rc = CheckMode( lpValMode->dvmXRes, lpValMode->dvmYRes, lpValMode->dvmBpp );
    dbg_printf( "ValidateMode: %ux%ux%u rc=%u\n", lpValMode->dvmXRes, lpValMode->dvmYRes, lpValMode->dvmBpp, rc );
    return( rc );
}

#pragma code_seg( _TEXT );

/* Called by the VDD in order to restore the video state when switching back
 * to the system VM. Function is in locked memory (_TEXT segment).
 * NB: Must reload DS, but apparently need not save/restore DS.
 */
#pragma aux RestoreDesktopMode loadds;
void __far RestoreDesktopMode( void )
{
    dbg_printf( "RestoreDesktopMode: %ux%u, wBpp=%u\n", wScreenX, wScreenY, wBpp );

    /* Set the current desktop mode again. */
    SetDisplayMode( wScreenX, wScreenY, 0 );

    /* Reprogram the DAC if relevant. */
    if( wBpp == 8 )
        SetRAMDAC_far( 0, 256, lpColorTable );

    /* Clear the busy flag. Quite important. */
    lpDriverPDevice->deFlags &= ~BUSY;

    /* Poke the VDD now that everything is restored. */
    CallVDD( VDD_SAVE_DRIVER_STATE );

    ClearVisibleScreen();
}
