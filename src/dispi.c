/* Mode setting through the Bochs VBE display interface. */

#include "winhack.h"
#include <gdidefs.h>
#include <dibeng.h>
#include <conio.h>
#include "dispi.h"
#include "minidrv.h"

static WORD DISPI_Read( WORD wIndex )
{
    outpw( DISPI_PORT_INDEX, wIndex );
    return( inpw( DISPI_PORT_DATA ) );
}

static void DISPI_Write( WORD wIndex, WORD wValue )
{
    outpw( DISPI_PORT_INDEX, wIndex );
    outpw( DISPI_PORT_DATA, wValue );
}

int DISPI_Detect( void )
{
    WORD    wId = DISPI_Read( DISPI_INDEX_ID );

    dbg_printf( "DISPI_Detect: id=%X\n", wId );
    return( wId >= DISPI_ID0 && wId <= DISPI_ID5 );
}

WORD DISPI_AlignedWidth( WORD wWidth, WORD wBpp )
{
    /* Pixels per DWORD: 4 at 8 bpp, 2 at 16 bpp, 4 at 24 bpp (three DWORDs), 1 at 32 bpp. */
    WORD    wAlign;

    switch( wBpp ) {
    case 16:
        wAlign = 2;
        break;
    case 32:
        wAlign = 1;
        break;
    default:
        wAlign = 4;
        break;
    }
    return( (wWidth + wAlign - 1) & ~(wAlign - 1) );
}

/* Programs one mode and reports whether the adapter took every value as written. QEMU
 * rounds XRES down to a multiple of 8 and clamps YRES to the VRAM once DISPI is enabled,
 * so the values are read back after enabling.
 */
static int DISPI_TryMode( WORD wWidth, WORD wHeight, WORD wBpp )
{
    WORD    wEnable = DISPI_ENABLED | DISPI_LFB_ENABLED;

    if( wBpp == 8 )
        wEnable |= DISPI_8BIT_DAC;

    DISPI_Write( DISPI_INDEX_ENABLE, DISPI_DISABLED );
    DISPI_Write( DISPI_INDEX_BPP, wBpp );
    DISPI_Write( DISPI_INDEX_XRES, wWidth );
    DISPI_Write( DISPI_INDEX_YRES, wHeight );
    DISPI_Write( DISPI_INDEX_VIRT_WIDTH, wWidth );
    DISPI_Write( DISPI_INDEX_VIRT_HEIGHT, wHeight );
    DISPI_Write( DISPI_INDEX_BANK, 0 );
    DISPI_Write( DISPI_INDEX_X_OFFSET, 0 );
    DISPI_Write( DISPI_INDEX_Y_OFFSET, 0 );
    DISPI_Write( DISPI_INDEX_ENABLE, wEnable );

    return( DISPI_Read( DISPI_INDEX_XRES ) == wWidth
         && DISPI_Read( DISPI_INDEX_YRES ) == wHeight
         && DISPI_Read( DISPI_INDEX_BPP ) == wBpp );
}

/* The adapter's scanline stride is its width times the bytes per pixel: v86 ignores
 * VIRT_WIDTH, and VIRT_WIDTH is set to the width for QEMU. The DIB Engine needs that
 * stride to be a whole number of DWORDs, so the displayed width is the smallest width
 * from DISPI_AlignedWidth onwards, in DWORD steps, that the adapter accepts exactly.
 * Columns past wWidth stay outside the desktop. Zero means no such width exists.
 */
WORD DISPI_SetMode( WORD wWidth, WORD wHeight, WORD wBpp )
{
    WORD    wFirst = DISPI_AlignedWidth( wWidth, wBpp );
    WORD    wStep = DISPI_AlignedWidth( 1, wBpp );
    WORD    wHw;

    for( wHw = wFirst; wHw <= wFirst + DISPI_MAX_PAD; wHw += wStep ) {
        if( DISPI_TryMode( wHw, wHeight, wBpp ) ) {
            dbg_printf( "DISPI_SetMode: %ux%ux%u displayed %u wide\n", wWidth, wHeight, wBpp, wHw );
            return( wHw );
        }
    }
    dbg_printf( "DISPI_SetMode: adapter rejects %ux%ux%u\n", wWidth, wHeight, wBpp );
    DISPI_Write( DISPI_INDEX_ENABLE, DISPI_DISABLED );
    return( 0 );
}
