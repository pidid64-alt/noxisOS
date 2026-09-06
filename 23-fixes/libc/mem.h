#ifndef MEM_H
#define MEM_H

#include <stdint.h>
#include <stddef.h>

void memory_copy(uint8_t *source, uint8_t *dest, int nbytes);
void memory_set(uint8_t *dest, uint8_t val, uint32_t len);

/* At this stage there is no 'free' implemented. */
uint32_t kmalloc(size_t size, int align, uint32_t *phys_addr);

/* The bump allocator hands out memory starting at HEAP_START and may grow up
 * to HEAP_START + HEAP_SIZE. These let the MEM command report how much of the
 * heap has been consumed without touching the allocator internals. */
/* The kernel is linked at 1 MiB (see kernel.ld: '. = 1M'), and the floppy
 * path stages kernel.bin at 0x10000 before relocating it, so that staging
 * area is free once _start runs. The heap therefore starts there -- but it
 * must stop at 1 MiB, or kmalloc eventually hands back memory that the
 * running kernel itself lives in (with the old 1 MiB size the heap reached
 * 0x110000, i.e. 64 KiB past the kernel's base). */
#define HEAP_START 0x10000
#define HEAP_SIZE  (0x100000 - HEAP_START)   /* 960 KiB, ends exactly at 1 MiB */

/* Current bump-pointer into the heap; equals the address of the next free
 * block (and thus the high-water mark of everything allocated so far). */
extern uint32_t free_mem_addr;

#endif
