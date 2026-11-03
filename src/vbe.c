/* Framebuffer discovery through the VBE BIOS, so the driver needs no PCI enumeration. */

#include "winhack.h"
#include <gdidefs.h>
#include <dibeng.h>
#include "minidrv.h"
#include <string.h>

/* DPMI real-mode call structure (DPMI 0.9, function 0300h). */
typedef struct {
    DWORD   edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    WORD    flags, es, ds, fs, gs, ip, cs, sp, ss;
} RMREGS;

/* Simulates a real-mode interrupt; returns zero on success. SS:SP of zero lets the DPMI
 * host supply the real-mode stack.
 */
extern WORD DPMI_SimulateRMInt( BYTE bInt, RMREGS __far *lpRegs );
#pragma aux DPMI_SimulateRMInt =    \
    "mov    ax, 0300h"              \
    "xor    bh, bh"                 \
    "xor    cx, cx"                 \
    "int    31h"                    \
    "sbb    ax, ax"                 \
    parm [bl] [es di] value [ax] modify [bx cx];

/* Maps a real-mode segment to a selector (DPMI 0002h); returns zero on failure. */
extern WORD DPMI_SegToDesc( WORD wSegment );
#pragma aux DPMI_SegToDesc =        \
    "mov    ax, 0002h"              \
    "int    31h"                    \
    "jnc    OK"                     \
    "xor    ax, ax"                 \
    "OK:"                           \
    parm [bx] value [ax];

#define VBE_OK              0x004F
#define VBE_MODE_SUPPORTED  0x0001
#define VBE_MODE_LFB        0x0080

/* Layout of the DOS buffer: the VBE controller or mode information at 0, and the copy of
 * the mode list at MODE_LIST, since the BIOS may keep the list in the buffer itself.
 */
#define DOS_BUF_SIZE        1024
#define MODE_LIST           512
#define MODE_LIST_MAX       ((DOS_BUF_SIZE - MODE_LIST) / 2 - 1)

#pragma code_seg( _INIT )

static WORD CallVBE( WORD wFunction, WORD wCx, WORD wSegment )
{
    RMREGS  regs;

    _fmemset( &regs, 0, sizeof( regs ) );
    regs.eax = wFunction;
    regs.ecx = wCx;
    regs.es  = wSegment;
    if( DPMI_SimulateRMInt( 0x10, &regs ) )
        return( 0 );
    return( (WORD)regs.eax );
}

/* Finds the physical address of the linear framebuffer, from the mode information of
 * the first mode the BIOS lists with a linear framebuffer, and the VRAM size. Returns
 * zero if the BIOS offers neither.
 */
int VBE_FindFramebuffer( DWORD FAR *lpdwPhys, DWORD FAR *lpdwVRAM )
{
    DWORD       dwDos;
    WORD        wSeg, wSel, wListSel;
    LPBYTE      lpBuf;
    WORD FAR    *lpList;
    WORD FAR    *lpSrc;
    WORD        i, wModes;
    int         rc = 0;

    dwDos = GlobalDosAlloc( DOS_BUF_SIZE );
    if( !dwDos ) {
        dbg_printf( "VBE: GlobalDosAlloc failed\n" );
        return( 0 );
    }
    wSeg  = HIWORD( dwDos );
    wSel  = LOWORD( dwDos );
    lpBuf = wSel :> 0;
    lpList = (WORD FAR *)(lpBuf + MODE_LIST);

    _fmemset( lpBuf, 0, DOS_BUF_SIZE );
    _fmemcpy( lpBuf, "VBE2", 4 );
    if( CallVBE( 0x4F00, 0, wSeg ) != VBE_OK || _fmemcmp( lpBuf, "VESA", 4 ) ) {
        dbg_printf( "VBE: 4F00 failed\n" );
        goto done;
    }
    *lpdwVRAM = (DWORD)*(WORD FAR *)(lpBuf + 18) << 16;

    /* Copy the mode list out of the way of the mode information. */
    wListSel = DPMI_SegToDesc( *(WORD FAR *)(lpBuf + 16) );
    if( !wListSel ) {
        dbg_printf( "VBE: no selector for the mode list\n" );
        goto done;
    }
    lpSrc = wListSel :> *(WORD FAR *)(lpBuf + 14);
    for( wModes = 0; wModes < MODE_LIST_MAX && lpSrc[wModes] != 0xFFFF; ++wModes )
        lpList[wModes] = lpSrc[wModes];

    for( i = 0; i < wModes; ++i ) {
        WORD    wAttr;
        DWORD   dwPhys;

        _fmemset( lpBuf, 0, 256 );
        if( CallVBE( 0x4F01, lpList[i], wSeg ) != VBE_OK )
            continue;
        wAttr  = *(WORD FAR *)lpBuf;
        dwPhys = *(DWORD FAR *)(lpBuf + 40);
        if( (wAttr & (VBE_MODE_SUPPORTED | VBE_MODE_LFB)) == (VBE_MODE_SUPPORTED | VBE_MODE_LFB) && dwPhys ) {
            dbg_printf( "VBE: mode %X LFB at %lX, VRAM %lX\n", lpList[i], dwPhys, *lpdwVRAM );
            *lpdwPhys = dwPhys;
            rc = *lpdwVRAM != 0;
            goto done;
        }
    }
    dbg_printf( "VBE: none of %u modes has a linear framebuffer\n", wModes );

done:
    GlobalDosFree( wSel );
    return( rc );
}
