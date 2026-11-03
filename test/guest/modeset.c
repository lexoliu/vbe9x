/* Guest side of the boot test. Prints "READY" on COM1, then answers each command line
 * with one line:
 *   SET w h       ChangeDisplaySettings(w x h, 0)         -> "SET <rc> <cx>x<cy>"
 *   ADD w h bpp   lists w x h at bpp in the driver's MODES  -> "ADD <rc>"
 *   ENUM          EnumDisplaySettings                     -> "ENUM <w>x<h>x<bpp> ..."
 *   PATTERN       draws the test pattern over the screen    -> "PATTERN <cx>x<cy>"
 *   REDRAW        drops the pattern, repaints every window  -> "REDRAW"
 * The pattern is a blue screen with red left and right columns and green top and
 * bottom rows, one pixel wide, so a wrong scanline stride shows as borders that drift.
 * The pointer is parked on the bottom right pixel while it shows.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLASS_KEY   "System\\CurrentControlSet\\Services\\Class\\Display"
#define DRIVER_NAME "vbe9x.drv"

static HANDLE   hCom;

static void Reply( const char *lpLine )
{
    DWORD   dwDone;

    WriteFile( hCom, lpLine, strlen( lpLine ), &dwDone, NULL );
    WriteFile( hCom, "\n", 1, &dwDone, NULL );
}

static int ReadLine( char *lpBuf, int cbBuf )
{
    int     n = 0;
    char    c;
    DWORD   dwDone;

    for( ;; ) {
        if( !ReadFile( hCom, &c, 1, &dwDone, NULL ) )
            return( 0 );
        if( !dwDone )
            continue;
        if( c == '\r' || c == '\n' ) {
            if( n ) {
                lpBuf[n] = 0;
                return( 1 );
            }
            continue;
        }
        if( n < cbBuf - 1 )
            lpBuf[n++] = c;
    }
}

/* Finds the display class key whose DEFAULT\drv is this driver. */
static LONG OpenDriverKey( char *lpPath, int cbPath )
{
    HKEY    hClass;
    char    szSub[16];
    DWORD   i;
    LONG    rc;

    rc = RegOpenKey( HKEY_LOCAL_MACHINE, CLASS_KEY, &hClass );
    if( rc != ERROR_SUCCESS )
        return( rc );
    for( i = 0; RegEnumKey( hClass, i, szSub, sizeof( szSub ) ) == ERROR_SUCCESS; ++i ) {
        HKEY    hDefault;
        char    szDrv[64];
        DWORD   cb = sizeof( szDrv );
        DWORD   dwType;
        char    szDef[64];

        sprintf( szDef, "%s\\DEFAULT", szSub );
        if( RegOpenKey( hClass, szDef, &hDefault ) != ERROR_SUCCESS )
            continue;
        rc = RegQueryValueEx( hDefault, "drv", NULL, &dwType, (LPBYTE)szDrv, &cb );
        RegCloseKey( hDefault );
        if( rc == ERROR_SUCCESS && !stricmp( szDrv, DRIVER_NAME ) ) {
            _snprintf( lpPath, cbPath, "%s\\%s", CLASS_KEY, szSub );
            RegCloseKey( hClass );
            return( ERROR_SUCCESS );
        }
    }
    RegCloseKey( hClass );
    return( ERROR_FILE_NOT_FOUND );
}

static LONG AddMode( int w, int h, int bpp )
{
    char    szDrv[96];
    char    szMode[160];
    HKEY    hMode;
    LONG    rc;

    rc = OpenDriverKey( szDrv, sizeof( szDrv ) );
    if( rc != ERROR_SUCCESS )
        return( rc );
    sprintf( szMode, "%s\\MODES\\%d\\%d,%d", szDrv, bpp, w, h );
    rc = RegCreateKey( HKEY_LOCAL_MACHINE, szMode, &hMode );
    if( rc == ERROR_SUCCESS )
        RegCloseKey( hMode );
    return( rc );
}

static void Fill( HDC hdc, int x, int y, int w, int h, COLORREF clr )
{
    RECT    rc;
    HBRUSH  hbr = CreateSolidBrush( clr );

    SetRect( &rc, x, y, x + w, y + h );
    FillRect( hdc, &rc, hbr );
    DeleteObject( hbr );
}

static void DrawPattern( int cx, int cy )
{
    HWND    hDesk = GetDesktopWindow();
    HDC     hdc;

    /* Park the pointer on the bottom right pixel; its image falls past the desktop. */
    SetCursorPos( cx - 1, cy - 1 );
    /* Keep every window, the taskbar clock included, from drawing over the pattern. */
    LockWindowUpdate( hDesk );
    hdc = GetDCEx( hDesk, NULL, DCX_WINDOW | DCX_CACHE | DCX_LOCKWINDOWUPDATE );
    Fill( hdc, 0, 0, cx, cy, RGB( 0, 0, 255 ) );
    Fill( hdc, 0, 0, 1, cy, RGB( 255, 0, 0 ) );
    Fill( hdc, cx - 1, 0, 1, cy, RGB( 255, 0, 0 ) );
    Fill( hdc, 1, 0, cx - 2, 1, RGB( 0, 255, 0 ) );
    Fill( hdc, 1, cy - 1, cx - 2, 1, RGB( 0, 255, 0 ) );
    GdiFlush();
    ReleaseDC( hDesk, hdc );
}

int PASCAL WinMain( HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow )
{
    char    szLine[128];
    char    szOut[2048];
    DCB     dcb;

    hCom = CreateFile( "COM1", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL );
    if( hCom == INVALID_HANDLE_VALUE )
        return( 1 );
    memset( &dcb, 0, sizeof( dcb ) );
    dcb.DCBlength = sizeof( dcb );
    GetCommState( hCom, &dcb );
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    SetCommState( hCom, &dcb );

    Reply( "READY" );
    while( ReadLine( szLine, sizeof( szLine ) ) ) {
        int     w, h, bpp;

        if( sscanf( szLine, "SET %d %d", &w, &h ) == 2 ) {
            DEVMODE dm;
            LONG    rc;

            memset( &dm, 0, sizeof( dm ) );
            dm.dmSize       = sizeof( dm );
            dm.dmPelsWidth  = w;
            dm.dmPelsHeight = h;
            dm.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT;
            rc = ChangeDisplaySettings( &dm, 0 );
            sprintf( szOut, "SET %ld %dx%d", rc, GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
            Reply( szOut );
        } else if( sscanf( szLine, "ADD %d %d %d", &w, &h, &bpp ) == 3 ) {
            sprintf( szOut, "ADD %ld", AddMode( w, h, bpp ) );
            Reply( szOut );
        } else if( !strcmp( szLine, "ENUM" ) ) {
            DEVMODE dm;
            DWORD   i;
            int     n = sprintf( szOut, "ENUM" );

            memset( &dm, 0, sizeof( dm ) );
            dm.dmSize = sizeof( dm );
            for( i = 0; EnumDisplaySettings( NULL, i, &dm ) && n < (int)sizeof( szOut ) - 24; ++i )
                n += sprintf( szOut + n, " %lux%lux%lu", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel );
            Reply( szOut );
        } else if( !strcmp( szLine, "PATTERN" ) ) {
            int     cx = GetSystemMetrics( SM_CXSCREEN );
            int     cy = GetSystemMetrics( SM_CYSCREEN );

            DrawPattern( cx, cy );
            sprintf( szOut, "PATTERN %dx%d", cx, cy );
            Reply( szOut );
        } else if( !strcmp( szLine, "REDRAW" ) ) {
            LockWindowUpdate( NULL );
            RedrawWindow( NULL, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW );
            Reply( "REDRAW" );
        } else {
            sprintf( szOut, "ERR %s", szLine );
            Reply( szOut );
        }
    }
    return( 0 );
}
