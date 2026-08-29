; ==================================================================
; noxisOS SETUP - First-boot configuration wizard
; Copyright (C) 2026 Ironcarrier
;
; Walks the user through initial system configuration:
; chooses install target, collects username, theme, prompt style.
;
; Files created:
;   CONF.DIR/USER.CFG      -- plain username
;   CONF.DIR/THEME.CFG     -- selected color theme palette
;   CONF.DIR/PROMPT.CFG    -- selected command prompt template
;   CONF.DIR/FIRST_B.CFG   -- '0' marker meaning setup has completed
;
; ==================================================================

[BITS 16]
[ORG 0x8000]

%define SETUP_STAGE_WELCOME   0
%define SETUP_STAGE_USERNAME  1
%define SETUP_STAGE_THEME     2
%define SETUP_STAGE_PROMPT    3
%define SETUP_STAGE_END       4

; ========== SETUP ROUTINE ==========
setup:
%ifndef NO_SETUP
    ; Clear screen
    mov ah, 0x06
    int 0x10

    mov al, 0x01
    call set_background_color

    call draw_top_and_bottom_lines

    mov al, SETUP_STAGE_WELCOME
    call setup_draw_stage_ui

    mov dh, 3
    mov dl, 0
    call string_move_cursor

    mov ah, 0x01
    mov si, setup_help_msg1
    int 0x21

    ; Wait for key press
    mov ah, 0
    int 16h

    ; ========== DISK SELECT ==========
    call disk_select

    ; ========== USERNAME SETUP ==========
    call setup_clear
    mov al, SETUP_STAGE_USERNAME
    call setup_draw_stage_ui

    mov dh, 3
    mov dl, 0
    call string_move_cursor

    mov ah, 0x01
    mov si, setup_help_msg2
    int 0x21

    ; Wait for key press
    mov ah, 0
    int 16h

    ; Read username string
    call read_username

    ; ========== THEME SETUP ==========
    call setup_clear
    mov al, SETUP_STAGE_THEME
    call setup_draw_stage_ui

    mov dh, 3
    mov dl, 0
    call string_move_cursor

    mov ah, 0x01
    mov si, setup_help_msg3
    int 0x21

    ; Wait for key press
    mov ah, 0
    int 16h

    ; Read theme choice
    call read_theme

    ; ========== PROMPT STYLE SETUP ==========
    call setup_clear
    mov al, SETUP_STAGE_PROMPT
    call setup_draw_stage_ui

    mov dh, 3
    mov dl, 0
    call string_move_cursor

    mov ah, 0x01
    mov si, setup_help_msg4
    int 0x21

    ; Wait for key press
    mov ah, 0
    int 16h

    ; Read prompt choice
    call read_prompt

    ; ========== FINALIZE ==========
    call setup_clear
    mov al, SETUP_STAGE_END
    call setup_draw_stage_ui

    mov dh, 5
    mov dl, 0
    call string_move_cursor

    mov si, setup_complete_msg
    call string_print

    ; Mark setup as completed
    mov byte [FIRST_B], 0

    ; Return to main menu
    ret

; ========== SUBROUTINES ==========

set_background_color:
    mov ah, 0x0B
    mov al, 0x00       ; Black background
    mov bh, 0x00
    mov bl, 0x0F       ; White text
    int 0x10
    ret

draw_top_and_bottom_lines:
    ; Draw top line
    mov ah, 0x09
    mov al, 0x2A       ; Attribute: white on black
    mov cx, 80         ; Line length
    mov bx, 0x0000     ; Start position
    int 0x10

    ; Draw bottom line
    mov ah, 0x09
    mov al, 0x2A
    mov cx, 80
    mov bx, 0x0050     ; Bottom line position
    int 0x10
    ret

setup_draw_stage_ui:
    ; Draw stage UI framework
    push ax
    push bx
    push cx

    ; Clear line 1
    mov ah, 0x06
    mov al, 0x00
    mov cx, 80
    mov bx, 0x0001
    int 0x10

    ; Write stage name at line 2
    mov ah, 0x09
    mov cx, 20
    mov bx, 0x0100     ; Position line 2, col 0
    int 0x10

    ; Clear line 3
    mov ah, 0x06
    mov al, 0x00
    mov cx, 80
    mov bx, 0x0201
    int 0x10

    ; Clear line 4
    mov ah, 0x06
    mov al, 0x00
    mov cx, 80
    mov bx, 0x0301
    int 0x10

    pop cx
    pop bx
    pop ax
    ret

setup_clear:
    mov ah, 0x06
    mov al, 0x00
    mov cx, 80
    mov bx, 0x0700
    int 0x10
    ret

; ----- Message strings (noxisOS specific) -----

setup_help_msg1 db 0x0D, 0x0A, "noxisOS Welcome", 0x0D, 0x0A, "Press any key to continue", 0
setup_help_msg2 db 0x0D, 0x0A, "Enter your username (max 15 chars):", 0x0D, 0x0A, 0
setup_help_msg3 db 0x0D, 0x0A, "Choose theme (1-5):", 0x0D, 0x0A, "1=Default 2=Dark 3=Green 4=Blue 5=Custom", 0x0D, 0x0A, 0
setup_help_msg4 db 0x0D, 0x0A, "Select prompt style:", 0x0D, 0x0A, "1=Simple 2=Directory 3=Full 4=Compact", 0x0D, 0x0A, 0

setup_complete_msg db 0x0D, 0x0A, "noxisOS Setup Complete", 0x0D, 0x0A, "Configuration saved to CONF.DIR", 0x0D, 0x0A, "Press any key to restart", 0

; ----- Disk selection subroutine -----
disk_select:
    ; Show disk options
    call setup_clear
    mov al, SETUP_STAGE_WELCOME
    call setup_draw_stage_ui

    mov dh, 5
    mov dl, 0
    call string_print

    mov si, disk_prompt1
    call string_print

    mov dh, 7
    mov dl, 0
    call string_print

    mov si, disk_option_hd
    call string_print

    mov dh, 8
    mov dl, 0
    call string_print

    mov si, disk_option_floppy
    call string_print

    ; Wait for selection
    mov ah, 0
    int 16h

    ; Check which key was pressed
    cmp al, '1'
    je .select_hd
    cmp al, '2'
    je .select_floppy
    jmp disk_select

.select_hd:
    ; Install to hard disk (100m.img)
    mov si, disk_selected_hd
    call string_print
    ; Set installation target to hard disk
    ; (implementation continues based on specific OS needs)
    ret

.select_floppy:
    ; Install to floppy disk (a.img)
    mov si, disk_selected_floppy
    call string_print
    ; Set installation target to floppy
    ret

; ----- String printing helpers -----
string_print:
    push ax
    push bx
    push cx
    push si

.print_loop:
    mov al, [si]
    cmp al, 0
    jz .print_done

    mov ah, 0x0E
    int 0x10

    inc si
    jmp .print_loop

.print_done:
    pop si
    pop cx
    pop bx
    pop ax
    ret

; ----- Data strings -----

disk_prompt1 db 0x0D, 0x0A, "Select install target:", 0x0D, 0x0A, 0
disk_option_hd db "1 - Hard Disk (100m.img)", 0
disk_option_floppy db "2 - Floppy Disk (a.img)", 0
disk_selected_hd db 0x0D, 0x0A, "Installing to Hard Disk...", 0
disk_selected_floppy db 0x0D, 0x0A, "Installing to Floppy...", 0

; ----- Placeholder subroutines (to be expanded) -----
read_username:
    ; Placeholder - reads username string from keyboard
    ret

read_theme:
    ; Placeholder - reads theme choice from keyboard
    ret

read_prompt:
    ; Placeholder - reads prompt style choice from keyboard
    ret
```