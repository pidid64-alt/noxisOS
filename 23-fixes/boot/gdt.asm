gdt_start: ; don't remove the labels, they're needed to compute sizes and jumps
    ; the GDT starts with a null 8-byte
    dd 0x0 ; 4 byte
    dd 0x0 ; 4 byte

; GDT for code segment. base = 0x00000000, length = 0xfffff
; for flags, refer to os-dev.pdf document, page 36
gdt_code: 
    dw 0xffff    ; segment length, bits 0-15
    dw 0x0       ; segment base, bits 0-15
    db 0x0       ; segment base, bits 16-23
    db 10011010b ; flags (8 bits)
    db 11001111b ; flags (4 bits) + segment length, bits 16-19
    db 0x0       ; segment base, bits 24-31

; GDT for data segment. base and length identical to code segment
; some flags changed, again, refer to os-dev.pdf
gdt_data:
    dw 0xffff
    dw 0x0
    db 0x0
    db 10010010b
    db 11001111b
    db 0x0

; 16-bit code segment for the real-mode thunk. base=0, limit=0xffff,
; present, ring0, code, 16-bit default operand size (D bit = 0).
gdt_rmcode:
    dw 0xffff
    dw 0x0
    db 0x0
    db 10011010b ; 0x9A: present, ring0, code, readable
    db 00001111b ; limit bits 16-19 = 0xF; D bit (bit 6 of flags) = 0 => 16-bit
    db 0x0

; real-mode data segment. base=0, limit=0xffff, present, ring0, writable.
gdt_rmdata:
    dw 0xffff
    dw 0x0
    db 0x0
    db 10010010b ; 0x92: present, ring0, data, writable
    db 00001111b
    db 0x0

gdt_end:

; GDT descriptor
gdt_descriptor:
    dw gdt_end - gdt_start - 1 ; size (16 bit), always one less of its true size
    dd gdt_start ; address (32 bit)

; define some constants for later use
CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start
RM_CODE_SEG equ gdt_rmcode  - gdt_start
RM_DATA_SEG equ gdt_rmdata  - gdt_start
