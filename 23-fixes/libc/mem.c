#include "mem.h"

void memory_copy(uint8_t *source, uint8_t *dest, int nbytes) {
    int i;
    for (i = 0; i < nbytes; i++) {
        *(dest + i) = *(source + i);
    }
}

void memory_set(uint8_t *dest, uint8_t val, uint32_t len) {
    uint8_t *temp = (uint8_t *)dest;
    for ( ; len != 0; len--) *temp++ = val;
}

/* This should be computed at link time, but a hardcoded
 * value is fine for now. Remember that our kernel starts
 * at 0x1000 as defined on the Makefile. The bump pointer
 * begins at the start of the kernel heap region. */
uint32_t free_mem_addr = HEAP_START;
/* Implementation is just a pointer to some free memory which
 * keeps growing */
uint32_t kmalloc(size_t size, int align, uint32_t *phys_addr) {
    /* Round up to the next 4K page when asked to align. The test has to be
     * against the page *offset* (0xFFF), not the page number (0xFFFFF000):
     * with the old mask the condition was true for every address outside the
     * first 4KiB, so an already-aligned pointer was pushed a whole page
     * further and every aligned allocation leaked 4KiB. */
    if (align == 1 && (free_mem_addr & 0x00000FFF)) {
        free_mem_addr &= 0xFFFFF000;
        free_mem_addr += 0x1000;
    }
    /* Save also the physical address */
    if (phys_addr) *phys_addr = free_mem_addr;

    uint32_t ret = free_mem_addr;
    free_mem_addr += size; /* Remember to increment the pointer */
    return ret;
}
