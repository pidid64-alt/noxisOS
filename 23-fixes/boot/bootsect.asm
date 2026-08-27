; Identical to lesson 13's boot sector, but the %included files have new paths
[org 0x7c00]
KERNEL_OFFSET equ 0x10000 ; low staging buffer; relocated to 1MB in PM

; How many 512-byte sectors of the kernel to load off the disk. Defaults to 31
; but the real value is computed from kernel.bin's size and passed in via
; `nasm -DKERNEL_SECTORS=...` by the Makefile, so the loader always reads the
; whole kernel no matter how big it grows.
%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 31
%endif

    mov [BOOT_DRIVE], dl ; Remember that the BIOS sets us the boot drive in 'dl' on boot
    mov bp, 0x9000
    mov sp, bp

    mov bx, MSG_REAL_MODE 
    call print
    call print_nl

    call load_kernel ; read the kernel from disk
    call switch_to_pm ; disable interrupts, load GDT,  etc. Finally jumps to 'BEGIN_PM'
    jmp $ ; Never executed

%include "boot/print.asm"
%include "boot/print_hex.asm"
%include "boot/disk.asm"
%include "boot/gdt.asm"
%include "boot/32bit_print.asm"
%include "boot/switch_pm.asm"
%include "boot/pm_relocate.asm"

[bits 16]
load_kernel:
    mov bx, MSG_LOAD_KERNEL
    call print
    call print_nl

    ; Stage the kernel at linear 0x10000. That is above 64KiB, so BX alone
    ; cannot hold it — disk_load reads through ES:BX, so set ES:BX = 0x1000:0.
    mov ax, KERNEL_OFFSET >> 4
    mov es, ax
    xor bx, bx
    mov dh, KERNEL_SECTORS ; number of sectors to read (computed by Makefile)
    mov dl, [BOOT_DRIVE]
    call disk_load
    ret

[bits 32]
BEGIN_PM:
    mov ebx, MSG_PROT_MODE
    call print_string_pm
    call pm_relocate_and_jump  ; copy 0x10000 -> 0x100000, jmp _start
    jmp $


BOOT_DRIVE db 0 ; It is a good idea to store it in memory because 'dl' may get overwritten
MSG_REAL_MODE db "Started in 16-bit Real Mode", 0
MSG_PROT_MODE db "Landed in 32-bit Protected Mode", 0
MSG_LOAD_KERNEL db "Loading kernel into memory", 0

; padding
times 510 - ($-$$) db 0
dw 0xaa55
