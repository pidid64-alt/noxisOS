# commands/Makefile

#ENTRYPOINT	= 0x1000
HD		= ../100m.img

ASM		= nasm
DASM	= objdump
CC		= gcc
LD		= ld
ASMFLAGS	= -I ../include/ -f elf
# -m32: user programs are 32-bit like the kernel; the host gcc is x86_64.
# -fno-pic/-fno-pie: static flat binaries at -Ttext 0x1000, no GOT.
CFLAGS		= -m32 -I ../include/ -c -fno-builtin -fno-stack-protector -fno-pic -fno-pie -fno-omit-frame-pointer -Wall
LDFLAGS		= -m elf_i386 -Ttext 0x1000
DASMFLAGS	= -D
LIB		= ../lib/orangescrt.a
BIN		= echo pwd demo desktop

# All Phony Targets
.PHONY : everything final clean realclean disasm all install

# Default starting position
everything : $(BIN)

install : all clean
	cp ../boot/hdldr.bin ./ -v
	cp ../kernel.bin ./ -v
	tar vcf cmd.tar kernel.bin $(BIN) hdldr.bin
	dd if=cmd.tar of=$(HD) seek=`echo "obase=10;ibase=16;(\`egrep -e '^ROOT_BASE' ../boot/include/load.inc | sed -e 's/.*0x//g'\`+\`egrep -e '#define[[:space:]]*INSTALL_START_SECT' ../include/sys/config.h | sed -e 's/.*0x//g'\`)*200" | bc` bs=1 count=`ls -l cmd.tar | awk -F " " '{print $$5}'` conv=notrunc

all : realclean everything

final : all clean

clean :
	rm -f *.o

realclean :
	rm -f $(BIN) *.o

kernel.bin :
	cp ../kernel.bin ./

start.o : start.asm
	$(ASM) $(ASMFLAGS) -o $@ $<

echo.o: echo.c ../include/type.h ../include/stdio.h
	$(CC) $(CFLAGS) -o $@ $<

echo : echo.o start.o $(LIB)
	$(LD) $(LDFLAGS) -o $@ $^

pwd.o: pwd.c ../include/type.h ../include/stdio.h
	$(CC) $(CFLAGS) -o $@ $<

pwd : pwd.o start.o $(LIB)
	$(LD) $(LDFLAGS) -o $@ $^

demo.o: demo.c ../include/type.h ../include/stdio.h
	$(CC) $(CFLAGS) -o $@ $<

demo : demo.o start.o $(LIB)
	$(LD) $(LDFLAGS) -o $@ $^

desktop.o: desktop.c ../include/type.h ../include/stdio.h
	$(CC) $(CFLAGS) -o $@ $<

desktop : desktop.o start.o $(LIB)
	$(LD) $(LDFLAGS) -o $@ $^