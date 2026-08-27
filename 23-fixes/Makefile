C_SOURCES = $(wildcard kernel/*.c drivers/*.c cpu/*.c libc/*.c)
HEADERS = $(wildcard kernel/*.h drivers/*.h cpu/*.h libc/*.h)
# Nice syntax for file extension replacement
OBJ = ${C_SOURCES:.c=.o cpu/interrupt.o}

CC = gcc
LD = ld
GDB = gdb
# -g: Use debugging symbols in gcc
# -fno-pie: отключаем Position Independent Executable (по умолчанию на многих
#           дистрибутивах), иначе линковка по фиксированному адресу 0x1000 не сработает
# -fno-stack-protector: убираем canary-код, которому неоткуда взять __stack_chk_fail на голом железе
# -mno-sse/-mno-sse2/-mno-mmx/-mno-avx: хостовый gcc (Arch) для -m32 по умолчанию
#   разрешает SSE2, и генерирует movdqu/movups для копирования структур (напр.
#   registers_t в scheduler_tick). Ядро не включает FPU/SSE (CR0/CR4 не трогаются),
#   поэтому любая SSE-инструкция даёт #UD (int 6). Запрещаем их на уровне кода.
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -fno-pie -fno-stack-protector -fcommon -m32 -mno-sse -mno-sse2 -mno-mmx -mno-avx -mno-avx512f

all: os-image.bin noxis.iso

# --- GRUB path: ELF32 at 1MB, Multiboot v1 ---
noxis.elf: boot/multiboot_entry.o boot/pm_relocate.o ${OBJ} kernel.ld
	${LD} -m elf_i386 -T kernel.ld -o $@ $(filter-out kernel.ld,$^)

grub-check: noxis.elf
	grub-file --is-x86-multiboot $< && echo "OK: valid multiboot image"

# --- floppy path: raw copy of the same image ---
kernel.bin: noxis.elf
	objcopy -O binary $< $@
	@python3 -c "import os,sys;s=os.path.getsize('kernel.bin');print('kernel.bin:',s,'bytes');sys.exit(0 if s<=0xE000 else 1)" || \
	(echo "ОШИБКА: ядро >60KiB — низкий стейджинг 0x10000 переполнится"; exit 1)

boot/bootsect.bin: boot/bootsect.asm kernel.bin
	@SECTS=`python3 -c "import os;print((os.path.getsize('kernel.bin')+511)//512)"`; \
	ENTRY=`nm noxis.elf | awk '$$3=="_start" {print "0x"$$1}'`; \
	echo "bootsect: reading $$SECTS sectors of kernel, entry $${ENTRY:-MISSING}"; \
	test -n "$$ENTRY"; \
	nasm -DKERNEL_SECTORS=$$SECTS -DPM_ENTRY_ADDR=$$ENTRY $< -f bin -o $@

os-image.bin: boot/bootsect.bin kernel.bin
	cat $^ > os-image.bin

# --- ISO (GRUB) ---
ISO_DIR = isodir
iso: noxis.iso
noxis.iso: noxis.elf
	mkdir -p $(ISO_DIR)/boot/grub
	cp noxis.elf $(ISO_DIR)/boot/
	printf 'set timeout=0\nset default=0\nmenuentry "noxis" {\n  multiboot /boot/noxis.elf\n}\n' \
	  > $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_DIR)

run: os-image.bin
	qemu-system-i386 -fda os-image.bin

run-grub: noxis.iso
	qemu-system-i386 -cdrom noxis.iso

debug: os-image.bin noxis.elf
	qemu-system-i386 -s -fda os-image.bin -d guest_errors,int &
	${GDB} -ex "target remote localhost:1234" -ex "symbol-file noxis.elf"

%.o: %.c ${HEADERS}
	${CC} ${CFLAGS} -c $< -o $@

%.o: %.asm
	nasm $< -f elf32 -o $@

clean:
	rm -rf *.bin *.dis *.o *.elf *.iso os-image.bin isodir
	rm -rf kernel/*.o boot/*.bin drivers/*.o boot/*.o cpu/*.o libc/*.o
