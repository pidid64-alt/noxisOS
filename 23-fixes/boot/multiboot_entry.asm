; Multiboot v1 entry point for noxis. GRUB loads the ELF linked at 1MB;
; our own bootloader far-jumps here after staging the kernel high.
; Both paths arrive in protected mode with interrupts disabled.
[bits 32]

section .multiboot
align 4
multiboot_header:
    dd 0x1BADB002              ; magic
    dd 0x00000004              ; flags bit 2: request framebuffer (video) info.
                               ; Under UEFI/GOP there is no VGA text mode, so
                               ; asking for a linear framebuffer makes GRUB hand
                               ; us the GOP/VBE mode through the MBI instead.
    dd 0xE4524FFA              ; checksum: -(magic + flags) mod 2^32.
                               ; NB: the task brief said 0xE4524FCE, which sums
                               ; to 0xFFFFFFD0 — GRUB rejects such an image.
    ; --- a.out-kludge fields (offsets 12..28) ---
    ; GRUB's `struct multiboot_header` ALWAYS reserves 20 bytes here for
    ; header_addr/load_addr/load_end_addr/bss_end_addr/entry_addr, so the
    ; graphics fields below must start at offset 32. We leave them zero and
    ; keep bit 16 (AOUT_KLUDGE) CLEAR, so GRUB loads this ELF via its program
    ; headers instead of treating us as an a.out image. Omitting this padding
    ; is what made GRUB read garbage (0x15010ffa) as the graphics type and
    ; reject the kernel with "unsupported graphical mode type".
    dd 0                       ; header_addr
    dd 0                       ; load_addr
    dd 0                       ; load_end_addr
    dd 0                       ; bss_end_addr
    dd 0                       ; entry_addr
    ; --- advanced graphics (framebuffer) request: offset 32, valid when bit 2 set ---
    dd 0                       ; mode_type: 0 = linear graphics (RGB framebuffer)
    dd 0                       ; width:  0 = let GRUB pick the best available
    dd 0                       ; height: 0 = pick best
    dd 0                       ; depth: 0 = pick best

section .bss
align 16
stack_bottom:
    resb 16384                 ; 16 KiB kernel stack
stack_top:

section .rodata
align 8
; Minimal flat GDT mirroring boot/gdt.asm (selector 0x08 = flat code,
; 0x10 = flat data), so _start can install selectors the IDT in cpu/idt.c
; expects. Kept here because gdt.asm itself is assembled only into the
; bootsector (-f bin) and exports no symbols.
mb_gdt:
    dd 0x00000000, 0x00000000          ; null descriptor
    dw 0xffff, 0x0000
    db 0x00, 0x9a, 0xcf, 0x00          ; 0x08: code, base 0, 4GB, 32-bit
    dw 0xffff, 0x0000
    db 0x00, 0x92, 0xcf, 0x00          ; 0x10: data, base 0, 4GB, RW
mb_gdt_end:
mb_gdt_descriptor:
    dw mb_gdt_end - mb_gdt - 1         ; limit (size - 1)
    dd mb_gdt                          ; linear base

section .text
[global _start]
[extern kernel_main]

_start:
    cli
    ; Stash the Multiboot handoff registers BEFORE reloading segments: the
    ; reloads go through AX (`mov ax, 0x10`), and on x86-32 writing a 16-bit
    ; register KEEPS the upper half of the 32-bit one — GRUB's magic
    ; 0x2BADB002 came out of here as 0x2BAD0010, fb_init rejected it, and the
    ; GRUB path booted to a black screen. ESI/EDI are callee-saved (cdecl) and
    ; unused until the push below.
    mov edi, eax               ; stash magic
    mov esi, ebx               ; stash MBI pointer
    ; GRUB enters with ITS OWN GDT active (here: CS=0x10, DS=0x18), but the
    ; IDT built by cpu/idt.c hardcodes selector 0x08 — deliverable only under
    ; the boot/gdt.asm layout the floppy path installs. Install our own GDT
    ; so both paths run identical selectors (code=0x08, data=0x10).
    lgdt [mb_gdt_descriptor]
    jmp 0x08:.reload           ; far jump: CS <- our flat code segment
.reload:
    mov ax, 0x10               ; DATA_SEG layout in boot/gdt.asm
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, stack_top
    ; Forward the Multiboot regs: EAX = 0x2BADB002 magic, EBX = MBI pointer
    ; (fb_init uses them to find the framebuffer GRUB left us). The floppy
    ; path zeroes both before jumping, so kernel_main sees "no MBI" there.
    push esi                   ; arg 2: mbi_addr
    push edi                   ; arg 1: magic
    call kernel_main
    add esp, 8
.halt:
    ; NB: the task brief put `cli` here, but noxis's shell is interrupt-driven:
    ; after kernel_main returns, IRQs (timer/keyboard) must KEEP firing — the
    ; retired boot/kernel_entry.asm did exactly that with a bare `jmp $`.
    ; Halting without touching IF preserves that behaviour.
    hlt
    jmp .halt
