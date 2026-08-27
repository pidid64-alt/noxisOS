#include "keyboard.h"
#include "../cpu/ports.h"
#include "../cpu/isr.h"
#include "screen.h"
#include "../libc/string.h"
#include "../libc/function.h"
#include "../kernel/kernel.h"
#include <stdint.h>

#define BACKSPACE 0x0E
#define ENTER 0x1C
/* Modifier scan codes (make codes). They carry no printable character, so they
 * must never be appended to the input buffer. Note: their *break* codes are
 * scancode | 0x80, which is caught by the `scancode & 0x80` check below. */
#define LSHIFT 0x2A
#define RSHIFT 0x36
#define LCTRL  0x1D
#define LALT   0x38

static char key_buffer[256];

/* First scancode NOT in the sc_ascii table. Spacebar is index 57 (0x39),
 * so the bound must be 58, not 57 — otherwise the space key (used in every
 * multi-word command like "echo hi noxis") is silently dropped and the
 * shell sees a concatenated token it cannot match. */
#define SC_MAX 58
const char *sc_name[] = { "ERROR", "Esc", "1", "2", "3", "4", "5", "6", 
    "7", "8", "9", "0", "-", "=", "Backspace", "Tab", "Q", "W", "E", 
        "R", "T", "Y", "U", "I", "O", "P", "[", "]", "Enter", "Lctrl", 
        "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", "`", 
        "LShift", "\\", "Z", "X", "C", "V", "B", "N", "M", ",", ".", 
        "/", "RShift", "Keypad *", "LAlt", "Spacebar"};
const char sc_ascii[] = { '?', '?', '1', '2', '3', '4', '5', '6',     
    '7', '8', '9', '0', '-', '=', '?', '?', 'Q', 'W', 'E', 'R', 'T', 'Y', 
        'U', 'I', 'O', 'P', '[', ']', '?', '?', 'A', 'S', 'D', 'F', 'G', 
        'H', 'J', 'K', 'L', ';', '\'', '`', '?', '\\', 'Z', 'X', 'C', 'V', 
        'B', 'N', 'M', ',', '.', '/', '?', '?', '?', ' '};

static void keyboard_callback(registers_t *regs) {
    /* The PIC leaves us the scancode in port 0x60 */
    uint8_t scancode = port_byte_in(0x60);

    /* Ignore key-release (break code) events: the high bit is set when the key
     * goes up. Without this we would echo the character twice and, worse, the
     * break code of a modifier would otherwise still pass the checks below. */
    if (scancode & 0x80) return;

    /* Modifier keys (Shift/Ctrl/Alt) have no printable form. Drop them so they
     * never end up in the input buffer (a stray '?' before the command name is
     * what used to make "HELP" look like "?HELP" -> "Unknown command"). */
    if (scancode == LSHIFT || scancode == RSHIFT ||
        scancode == LCTRL  || scancode == LALT) return;

    if (scancode == BACKSPACE) {
        backspace(key_buffer);
        kprint_backspace();
    } else if (scancode == ENTER) {
        kprint("\n");
        user_input(key_buffer); /* kernel-controlled function */
        key_buffer[0] = '\0';
    } else if (scancode < SC_MAX) {
        char letter = sc_ascii[(int)scancode];
        /* Remember that kprint only accepts char[] */
        char str[2] = {letter, '\0'};
        append(key_buffer, letter);
        kprint(str);
    }
    UNUSED(regs);
}

void init_keyboard() {
   register_interrupt_handler(IRQ1, keyboard_callback); 
}
