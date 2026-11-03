# Builds build/vbe9x.drv, the display driver, and build/modeset.exe, the guest side of
# the boot test. Run through tools/build.sh, which provides the pinned Open Watcom.
#   wmake           release build
#   wmake DEBUG=1   keeps a driver log in guest memory, which the test harnesses print

!ifdef DEBUG
O = build/debug
!else
O = build
!endif

DRV_OBJS = $(O)/dibcall.obj $(O)/dibthunk.obj $(O)/enable.obj $(O)/init.obj &
	$(O)/palette.obj $(O)/scrsw.obj $(O)/sswhook.obj $(O)/modes.obj &
	$(O)/dispi.obj $(O)/vbe.obj

DEFS =
!ifdef DEBUG
DEFS = -DDBGPRINT
DRV_OBJS += $(O)/dbgprint.obj
!endif

INCS = -I"$(%WATCOM)/h/win" -I"$(%WATCOM)/h" -Isrc/ddk -Isrc
CFLAGS = -q -wx -we -wcd=303 -s -zu -zls -3 -zW -bt=windows $(INCS) $(DEFS)
RES_BINS = $(O)/config.bin $(O)/colortab.bin $(O)/fonts.bin $(O)/fonts120.bin

all : $(O)/vbe9x.drv $(O)/modeset.exe .symbolic

$(O)/vbe9x.drv : $(DRV_OBJS) $(O)/display.res $(O)/dibeng.lib src/vbe9x.lnk
	wlink libpath $(O) name $@ option resource=$(O)/display.res file { $(DRV_OBJS) } @src/vbe9x.lnk

.c : src
.asm : src

.c.obj : .autodepend
	wcc $(CFLAGS) -fo=$@ $<

.asm.obj :
	wasm -q $(DEFS) -fo=$@ $<

$(O)/display.res : src/res/display.rc src/res/display.rcv $(RES_BINS)
	wrc -q -r -bt=windows -fo=$@ -I$(O) -Isrc/res -I"$(%WATCOM)/h/win" src/res/display.rc

# Resource data: C structures linked into raw binaries.
$(O)/config.bin : src/res/config.c src/winhack.h
	wcc -q -bt=windows $(INCS) -fo=$(O)/config.obj src/res/config.c
	wlink op quiet disable 1014,1023 name $@ sys dos output raw file $(O)/config.obj

$(O)/colortab.bin : src/res/colortab.c src/winhack.h
	wcc -q -bt=windows $(INCS) -fo=$(O)/colortab.obj src/res/colortab.c
	wlink op quiet disable 1014,1023 name $@ sys dos output raw file $(O)/colortab.obj

$(O)/fonts.bin : src/res/fonts.c src/res/fonttmpl.c src/winhack.h
	wcc -q -bt=windows $(INCS) -fo=$(O)/fonts.obj src/res/fonts.c
	wlink op quiet disable 1014,1023 name $@ sys dos output raw file $(O)/fonts.obj

$(O)/fonts120.bin : src/res/fonts120.c src/res/fonttmpl.c src/winhack.h
	wcc -q -bt=windows $(INCS) -fo=$(O)/fonts120.obj src/res/fonts120.c
	wlink op quiet disable 1014,1023 name $@ sys dos output raw file $(O)/fonts120.obj

$(O)/dibeng.lib : src/ddk/dibeng.lbc
	wlib -b -q -n -fo -ii $@ @src/ddk/dibeng.lbc

$(O)/modeset.exe : test/guest/modeset.c
	wcl386 -q -wx -we -wcd=303 -bt=nt -l=nt_win -I"$(%WATCOM)/h/nt" -I"$(%WATCOM)/h" -fo=$(O)/modeset.obj -fe=$@ test/guest/modeset.c

clean : .symbolic
	rm -rf $(O)
