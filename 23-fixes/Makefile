C_SOURCES = $(wildcard kernel/*.c drivers/*.c cpu/*.c libc/*.c)
HEADERS = $(wildcard kernel/*.h drivers/*.h cpu/*.h libc/*.h)
# Nice syntax for file extension replacement
OBJ = ${C_SOURCES:.c=.o cpu/interrupt.o} 

# Используем системный gcc вместо кросс-компилятора: собираем 32-битный
# freestanding-код напрямую флагами, без i686-elf-gcc.
# Требуется пакет lib32-glibc (Arch: sudo pacman -S lib32-glibc, включив [multilib]).
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

# First rule is run by default
os-image.bin: boot/bootsect.bin kernel.bin
	cat $^ > os-image.bin

# '--oformat binary' deletes all symbols as a collateral, so we don't need
# to 'strip' them manually on this case
kernel.bin: boot/kernel_entry.o ${OBJ}
	${LD} -m elf_i386 -o $@ -Ttext 0x1000 $^ --oformat binary

# Used for debugging purposes
kernel.elf: boot/kernel_entry.o ${OBJ}
	${LD} -m elf_i386 -o $@ -Ttext 0x1000 $^ 

run: os-image.bin
	qemu-system-i386 -fda os-image.bin

# Open the connection to qemu and load our kernel-object file with symbols
debug: os-image.bin kernel.elf
	qemu-system-i386 -s -fda os-image.bin -d guest_errors,int &
	${GDB} -ex "target remote localhost:1234" -ex "symbol-file kernel.elf"

# Generic rules for wildcards
# To make an object, always compile from its .c
%.o: %.c ${HEADERS}
	${CC} ${CFLAGS} -c $< -o $@

%.o: %.asm
	nasm $< -f elf32 -o $@

%.bin: %.asm
	nasm $< -f bin -o $@

clean:
	rm -rf *.bin *.dis *.o os-image.bin *.elf
	rm -rf kernel/*.o boot/*.bin drivers/*.o boot/*.o cpu/*.o libc/*.o
