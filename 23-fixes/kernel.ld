/* noxis kernel layout: the Multiboot header must live within the first
 * 8 KiB of the image, so its section goes first. Base is 1MB — where
 * GRUB places the ELF and where our bootloader stages the raw copy. */
ENTRY(_start)
SECTIONS
{
    . = 1M;

    .multiboot : { KEEP(*(.multiboot)) }

    .text   : { *(.text*)   }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*)   }

    .bss : {
        *(COMMON)
        *(.bss*)
    }

    PROVIDE(end_of_kernel = ALIGN(ADDR(.bss) + SIZEOF(.bss), 4096));
}
