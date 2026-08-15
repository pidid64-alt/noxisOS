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
#define HEAP_START 0x10000
#define HEAP_SIZE  0x100000   /* 1 MiB of kernel heap space */

/* Current bump-pointer into the heap; equals the address of the next free
 * block (and thus the high-water mark of everything allocated so far). */
extern uint32_t free_mem_addr;

#endif
