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

/* Display driver module initialization. Adapted from boxv9x for the Bochs VBE adapter:
 * the framebuffer comes from the VBE BIOS instead of PCI, and the mode only from the
 * Registry through the VDD.
 */

#include "winhack.h"
#include <gdidefs.h>
#include <dibeng.h>
#include <minivdd.h>
#include "minidrv.h"
#include "dispi.h"
#include <string.h>

WORD    wScrX       = 0;    /* Configured X resolution. */
WORD    wScrY       = 0;    /* Configured Y resolution. */
WORD    wDpi        = 96;   /* Configured DPI. */
WORD    wBpp        = 0;    /* Configured bits per pixel. */
WORD    wPalettized = 0;    /* Non-zero if palettized. */

WORD    OurVMHandle   = 0;  /* The current VM's ID. */
DWORD   VDDEntryPoint = 0;  /* The VDD entry point. */
DWORD   dwPhysVRAM    = 0;  /* Physical address of the linear framebuffer. */
DWORD   dwVideoMemorySize = 0;  /* VRAM size in bytes. */

/* On Entry:
 * EAX    = Function code (VDD_GET_DISPLAY_CONFIG)
 * EBX    = This VM's handle
 * ECX    = Size of DISPLAYINFO structure
 * EDX    = Zero to tell VDD to try virtualizing
 * ES:EDI = Pointer to DISPLAYINFO structure to be
 *          filled
 *
 * On Return:
 * EAX    = Return code (0 = succeess, -1 = failed)
 */
extern DWORD CallVDDGetDispConf( WORD Function, WORD wDInfSize, LPVOID pDInf );
#pragma aux CallVDDGetDispConf =    \
    ".386"                          \
    "movzx  eax, ax"                \
    "movzx  ecx, cx"                \
    "movzx  ebx, OurVMHandle"       \
    "movzx  edi, di"                \
    "xor    edx, edx"               \
    "call   dword ptr VDDEntryPoint"\
    "mov    edx, eax"               \
    "shr    edx, 16"                \
    parm [ax] [cx] [es di] modify [bx];


#pragma code_seg( _INIT )

/* Reads the configured display mode from the Registry through the VDD into wScrX,
 * wScrY, wBpp and wDpi. Returns zero if the VDD cannot supply it.
 */
int ReadDisplayConfig( void )
{
    DISPLAYINFO DispInfo;
    DWORD       dwRc;

    _fmemset( &DispInfo, 0, sizeof( DispInfo ) );
    dwRc = CallVDDGetDispConf( VDD_GET_DISPLAY_CONFIG, sizeof( DispInfo ), &DispInfo );
    if( dwRc == VDD_GET_DISPLAY_CONFIG || dwRc ) {
        dbg_printf( "ReadDisplayConfig: VDD_GET_DISPLAY_CONFIG failed, dwRc=%lX\n", dwRc );
        return( 0 );
    }
    dbg_printf( "ReadDisplayConfig: %ux%u %ubpp %udpi flags=%X\n", DispInfo.diXRes, DispInfo.diYRes,
                DispInfo.diBpp, DispInfo.diDPI, DispInfo.diInfoFlags );
    if( DispInfo.diInfoFlags & (REGISTRY_BPP_NOT_VALID | REGISTRY_RESOLUTION_NOT_VALID) ) {
        dbg_printf( "ReadDisplayConfig: no valid mode in the Registry\n" );
        return( 0 );
    }
    wScrX = DispInfo.diXRes;
    wScrY = DispInfo.diYRes;
    wBpp  = DispInfo.diBpp;
    /* Windows leaves the DPI unset until it is changed from its 96 DPI default. */
    wDpi  = DispInfo.diDPI ? DispInfo.diDPI : 96;
    wPalettized = wBpp == 8;
    return( 1 );
}

#define VDD_ID      10  /* Virtual Display Driver ID. */

/* Get Device API Entry Point. */
void __far *int_2F_GetEP( unsigned ax, unsigned bx );
#pragma aux int_2F_GetEP =  \
    "int    2Fh"            \
    parm [ax] [bx] value [es di];

/* Get "magic number" (current Virtual Machine ID) for VDD calls. */
WORD int_2F_GetVMID( unsigned ax );
#pragma aux int_2F_GetVMID =    \
    "int    2Fh"                \
    parm [ax] value [bx];

/* This is a standard DLL entry point. Note that DS is already set
 * to point to this DLL's data segment on entry. A zero return makes Windows
 * refuse the driver.
 */
#pragma aux DriverInit parm [cx] [di] [es si]
UINT FAR DriverInit( UINT cbHeap, UINT hModule, LPSTR lpCmdLine )
{
    /* The VDD calls RestoreDesktopMode at any time; keep its segment resident. The
     * segment comes from the function itself: a __segname("_TEXT") based pointer gets a
     * fixup to an empty segment that wlink drops, and the loader then refuses the module.
     */
    GlobalPageLock( SELECTOROF( RestoreDesktopMode ) );

    VDDEntryPoint = (DWORD)int_2F_GetEP( 0x1684, VDD_ID );
    OurVMHandle = int_2F_GetVMID( 0x1683 );
    dbg_printf( "DriverInit: VDDEntryPoint=%WP, OurVMHandle=%x\n", VDDEntryPoint, OurVMHandle );
    if( !VDDEntryPoint ) {
        dbg_printf( "DriverInit: no VDD\n" );
        return( 0 );
    }

    if( !DISPI_Detect() ) {
        dbg_printf( "DriverInit: no Bochs VBE adapter\n" );
        return( 0 );
    }
    if( !VBE_FindFramebuffer( &dwPhysVRAM, &dwVideoMemorySize ) ) {
        dbg_printf( "DriverInit: no linear framebuffer from the VBE BIOS\n" );
        return( 0 );
    }
    if( !ReadDisplayConfig() )
        return( 0 );

    return( 1 );
}
