# Peng Pong — Sega Master System port
# Requires: sdcc, python3
# Sources: main.c (game), smslib_sprites.c (VDP sprite/tile routines)
# Libraries: SMSlib_base.rel, crt0_sms.rel  (devkitSMS subset, included)

PRJNAME := pengpong_sms
CC      := sdcc
CFLAGS  := -c -mz80 --peep-file peep-rules.txt \
           --disable-warning 110 --disable-warning 126

OBJS := pengpong.rel smslib_sprites.rel

all: $(PRJNAME).sms

%.rel : %.c
	$(CC) $(CFLAGS) $<

$(PRJNAME).sms: $(PRJNAME).ihx
	python3 tools/ihx2sms.py $(PRJNAME).ihx $(PRJNAME).sms

$(PRJNAME).ihx: $(OBJS)
	sdcc -o $@ -mz80 --no-std-crt0 --data-loc 0xC000 \
	     crt0_sms.rel $(OBJS) SMSlib_base.rel

clean:
	rm -f *.sms *.sav *.asm *.sym *.rel *.noi *.map *.lst *.lk *.ihx
