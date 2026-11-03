/* Bochs VBE display interface (DISPI), as QEMU -vga std and v86 emulate it. */

#define DISPI_PORT_INDEX        0x01CE
#define DISPI_PORT_DATA         0x01CF

#define DISPI_INDEX_ID          0x0
#define DISPI_INDEX_XRES        0x1
#define DISPI_INDEX_YRES        0x2
#define DISPI_INDEX_BPP         0x3
#define DISPI_INDEX_ENABLE      0x4
#define DISPI_INDEX_BANK        0x5
#define DISPI_INDEX_VIRT_WIDTH  0x6
#define DISPI_INDEX_VIRT_HEIGHT 0x7
#define DISPI_INDEX_X_OFFSET    0x8
#define DISPI_INDEX_Y_OFFSET    0x9

#define DISPI_ID0               0xB0C0
#define DISPI_ID5               0xB0C5

#define DISPI_DISABLED          0x00
#define DISPI_ENABLED           0x01
#define DISPI_8BIT_DAC          0x20
#define DISPI_LFB_ENABLED       0x40

/* Returns non-zero if a DISPI adapter answers at the DISPI ports. */
extern int DISPI_Detect( void );

/* Sets a wWidth x wHeight x wBpp mode and returns the width in pixels the adapter
 * displays, or zero if it cannot show the mode. See dispi.c.
 */
extern WORD DISPI_SetMode( WORD wWidth, WORD wHeight, WORD wBpp );

/* Returns the width in pixels DISPI_SetMode starts probing from: wWidth rounded up so
 * that a scanline is a whole number of DWORDs.
 */
extern WORD DISPI_AlignedWidth( WORD wWidth, WORD wBpp );

/* The widest padding DISPI_SetMode adds beyond DISPI_AlignedWidth. */
#define DISPI_MAX_PAD           16
