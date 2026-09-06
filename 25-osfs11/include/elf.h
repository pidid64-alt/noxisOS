/* ELF32 on-disk structures. A freestanding kernel must not depend on the
 * host libc's <elf.h> (which pulls in unavailable 32-bit libc headers). */
#ifndef _NOXIS_ELF_H_
#define _NOXIS_ELF_H_

#define ELFMAG "\177ELF"
#define SELFMAG 4
#define PT_LOAD 1
#define SHF_ALLOC 2

typedef struct {
	unsigned char e_ident[16];
	unsigned short e_type, e_machine;
	unsigned int e_version, e_entry, e_phoff, e_shoff, e_flags;
	unsigned short e_ehsize, e_phentsize, e_phnum;
	unsigned short e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;

typedef struct {
	unsigned int p_type, p_offset, p_vaddr, p_paddr;
	unsigned int p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;

typedef struct {
	unsigned int sh_name, sh_type, sh_flags, sh_addr, sh_offset;
	unsigned int sh_size, sh_link, sh_info, sh_addralign, sh_entsize;
} Elf32_Shdr;

#endif
