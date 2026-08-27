; Staged-boot finisher (floppy path): copy the kernel from the low staging
; buffer to its link address, then enter the unified entry point.
; Runs in 32-bit protected mode with a flat 4GB data segment (gdt.asm).
[bits 32]

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 120 ; fallback so standalone `nasm -f elf32` assembles;
                           ; the real count is baked in when %included from
                           ; bootsect.asm via `nasm -DKERNEL_SECTORS=...`
%endif

; Jump target = unified entry `_start`. Two build contexts:
;  - ELF (noxis.elf): resolved by the linker via [extern _start].
;  - -f bin (inside bootsect.bin): externals are unsupported, so the Makefile
;    bakes _start's linked address in via -DPM_ENTRY_ADDR=0x... (nm noxis.elf).
%ifdef PM_ENTRY_ADDR
%define PM_ENTRY PM_ENTRY_ADDR
%else
[extern _start]
%define PM_ENTRY _start
%endif

[global pm_relocate_and_jump]

KERNEL_BYTES equ KERNEL_SECTORS * 512

pm_relocate_and_jump:
    mov esi, 0x10000           ; staging buffer (bootsect loaded it there)
    mov edi, 0x100000          ; link address
    mov ecx, KERNEL_BYTES / 4
    cld
    rep movsd
    ; No Multiboot here: zero the regs _start forwards to kernel_main so the
    ; kernel takes its legacy VGA-text path instead of parsing garbage as MBI.
    xor eax, eax
    xor ebx, ebx
    jmp 0x08:PM_ENTRY          ; far jump through the existing code selector
