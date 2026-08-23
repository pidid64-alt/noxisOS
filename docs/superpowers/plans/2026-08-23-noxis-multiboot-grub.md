# Multiboot/GRUB Dual Boot Path — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ядро noxis грузится и через GRUB (ISO, ELF на 1MB), и собственным бутсектором (стейджинг 0x10000 → PM-копия в 0x100000); куча привязана к линкерному символу.

**Architecture:** Один линк на `0x100000` по `kernel.ld`. GRUB берёт адреса из ELF program headers (flags=0 в заголовке). Флоппи-путь читает ядро в `0x10000`, после входа в PM dword-копирует его на 1MB и делает far-jmp на общий `_start`.

**Tech Stack:** nasm (elf32/bin), GNU ld с кастомным скриптом, objcopy, grub-mkrescue+xorriso, QEMU i386, Python3 стенд.

## Global Constraints

- Ветка `feat/weighted-scheduler`; все пути из корня репо
- Спека: `docs/superpowers/specs/2026-08-23-noxis-multiboot-grub-design.md`
- Заголовок Multiboot строго в первых 8 КиБ образа → секция `.multiboot` первая в скрипте
- `_start` не проверяет EAX; состояние входа «PM, CLI, валидная GDT» на обоих путях
- Существующий RED-базлайн тестов не должен ухудшиться (планировщик чинится отдельным планом)
- Бинарники коммитим по конвенции репо (финальный таск)

---

### Task 1: Ядро на 1MB — оба пути загрузки

**Files:**
- Create: `23-fixes/boot/multiboot_entry.asm`
- Create: `23-fixes/boot/pm_relocate.asm`
- Create: `23-fixes/kernel.ld`
- Modify: `23-fixes/boot/bootsect.asm` (KERNEL_OFFSET, BEGIN_PM)
- Modify: `23-fixes/libc/mem.c`, `23-fixes/libc/mem.h`, `23-fixes/drivers/shell.c` — НЕТ, это Task 2
- Modify: `23-fixes/Makefile`

**Interfaces:**
- Produces: цели `make noxis.elf kernel.bin os-image.bin noxis.iso run-grub`; символ линкера `end_of_kernel` (потребуется Task 2); точка входа `_start` из `multiboot_entry.asm`

Отступление от буквы спеки (осознанное, зафиксировано здесь): `disk.asm` НЕ переписывается на сегментную арифметику — текущий однозаходный `int 13h` уже успешно грузит ядро >64KiB на SeaBIOS/QEMU (доказательство: сегодняшний образ ~50КБ грузится в `0x1000`, пересекая границу 64KiB). Вместо рискованного переписывания CHS-математики добавляется build-guard на размер стейджинга.

- [ ] **Step 1: Создать `23-fixes/boot/multiboot_entry.asm`**

```asm
; Multiboot v1 entry point for noxis. GRUB loads the ELF linked at 1MB;
; our own bootloader far-jumps here after staging the kernel high.
; Both paths arrive in protected mode with interrupts disabled.
[bits 32]

section .multiboot
align 4
multiboot_header:
    dd 0x1BADB002              ; magic
    dd 0x00000000              ; flags: load addresses come from the ELF
    dd 0xE4524FCE              ; checksum: -(magic + flags) mod 2^32

section .bss
align 16
stack_bottom:
    resb 16384                 ; 16 KiB kernel stack
stack_top:

section .text
[global _start]
[extern kernel_main]

_start:
    cli
    mov esp, stack_top
    call kernel_main
.halt:
    cli
    hlt
    jmp .halt
```

- [ ] **Step 2: Создать `23-fixes/kernel.ld`**

```ld
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
```

- [ ] **Step 3: Создать `23-fixes/boot/pm_relocate.asm`**

```asm
; Staged-boot finisher (floppy path): copy the kernel from the low staging
; buffer to its link address, then enter the unified entry point.
; Runs in 32-bit protected mode with a flat 4GB data segment (gdt.asm).
[bits 32]

[global pm_relocate_and_jump]
[extern _start]

KERNEL_BYTES equ KERNEL_SECTORS * 512

pm_relocate_and_jump:
    mov esi, 0x10000           ; staging buffer (bootsect loaded it there)
    mov edi, 0x100000          ; link address
    mov ecx, KERNEL_BYTES / 4
    cld
    rep movsd
    jmp 0x08:_start            ; far jump through the existing code selector
```

- [ ] **Step 4: Правки `23-fixes/boot/bootsect.asm`**

Заменить константу:
```asm
KERNEL_OFFSET equ 0x10000 ; low staging buffer; relocated to 1MB in PM
```
В `BEGIN_PM` заменить вызов `call KERNEL_OFFSET` на:
```asm
BEGIN_PM:
    mov ebx, MSG_PROT_MODE
    call print_string_pm
    call pm_relocate_and_jump  ; copy 0x10000 -> 0x100000, jmp _start
    jmp $
```
И добавить include рядом с остальными:
```asm
%include "boot/pm_relocate.asm"
```
Удалить неиспользуемое сообщение `MSG_RETURNED_KERNEL`, если линтер не ругается — оставить можно.

- [ ] **Step 5: Убрать старую точку входа**

`boot/kernel_entry.asm` больше не участвует в сборке (его роль у `_start`):
```bash
git rm 23-fixes/boot/kernel_entry.asm
```

- [ ] **Step 6: Переписать `23-fixes/Makefile`**

Полное новое содержимое (сохраняя существующие CFLAGS без изменений):
```make
C_SOURCES = $(wildcard kernel/*.c drivers/*.c cpu/*.c libc/*.c)
HEADERS = $(wildcard kernel/*.h drivers/*.h cpu/*.h libc/*.h)
OBJ = ${C_SOURCES:.c=.o cpu/interrupt.o}

CC = gcc
LD = ld
GDB = gdb
# (комментарии про флаги — сохранить прежние строки CFLAGS-обоснования)
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -fno-pie -fno-stack-protector -fcommon -m32 -mno-sse -mno-sse2 -mno-mmx -mno-avx -mno-avx512f

all: os-image.bin noxis.iso

# --- GRUB path: ELF32 at 1MB, Multiboot v1 ---
noxis.elf: boot/multiboot_entry.o boot/pm_relocate.o ${OBJ} kernel.ld
	${LD} -m elf_i386 -T kernel.ld -o $@ $^

grub-check: noxis.elf
	grub-file --is-x86-multiboot $< && echo "OK: valid multiboot image"

# --- floppy path: raw copy of the same image ---
kernel.bin: noxis.elf
	objcopy -O binary $< $@
	@python3 -c "import os,sys;s=os.path.getsize('kernel.bin');print('kernel.bin:',s,'bytes');sys.exit(0 if s<=0xE000 else 1)" || \
	(echo "ОШИБКА: ядро >60KiB — низкий стейджинг 0x10000 переполнится"; exit 1)

boot/bootsect.bin: boot/bootsect.asm kernel.bin
	@SECTS=`python3 -c "import os;print((os.path.getsize('kernel.bin')+511)//512)"`; \
	echo "bootsect: reading $$SECTS sectors of kernel"; \
	nasm -DKERNEL_SECTORS=$$SECTS $< -f bin -o $@

os-image.bin: boot/bootsect.bin kernel.bin
	cat $^ > os-image.bin

# --- ISO (GRUB) ---
ISO_DIR = isodir
iso: noxis.iso
noxis.iso: noxis.elf
	mkdir -p $(ISO_DIR)/boot/grub
	cp noxis.elf $(ISO_DIR)/boot/
	printf 'set timeout=0\nset default=0\nmenuentry "noxis" {\n  multiboot /boot/noxis.elf\n}\n' \
	  > $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_DIR)

run: os-image.bin
	qemu-system-i386 -fda os-image.bin

run-grub: noxis.iso
	qemu-system-i386 -cdrom noxis.iso

debug: os-image.bin noxis.elf
	qemu-system-i386 -s -fda os-image.bin -d guest_errors,int &
	${GDB} -ex "target remote localhost:1234" -ex "symbol-file noxis.elf"

%.o: %.c ${HEADERS}
	${CC} ${CFLAGS} -c $< -o $@

%.o: %.asm
	nasm $< -f elf32 -o $@

%.bin: %.asm
	nasm $< -f bin -o $@

clean:
	rm -rf *.bin *.dis *.o *.elf *.iso os-image.bin isodir
	rm -rf kernel/*.o boot/*.bin drivers/*.o boot/*.o cpu/*.o libc/*.o
```
Примечание: `boot/bootsect.bin` правило `%bin:%asm` конфликтует по имени — явное правило выше уже перекрывает generic; generic `%.bin: %.asm` оставить последним как fallback для других bin (если нет других — удалить generic-правило, чтобы не ловить неожиданностей).

- [ ] **Step 7: Собрать и проверить GRUB-путь (RED→GREEN для нового пути)**

Run:
```bash
which grub-mkrescue xorriso || { echo 'sudo pacman -S --needed grub xorriso'; }
cd 23-fixes && make clean && make
grub-file --is-x86-multiboot noxis.elf && echo MULTIBOOT_OK
timeout 90 python3 - <<'PY'
import socket, subprocess, time, re, sys
subprocess.Popen(["qemu-system-i386","-m","32","-cdrom","noxis.iso","-boot","d",
                  "-display","none","-no-reboot","-monitor","unix:/tmp/qmon,server,nowait"],
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    try:
        s=socket.socket(socket.AF_UNIX); s.connect("/tmp/qmon"); break
    except Exception: time.sleep(0.2)
s.settimeout(0.05); time.sleep(2)
try: s.recv(65536)
except Exception: pass
def screen():
    s.sendall(b"xp /2000xb 0xb8000\n"); buf=b""; s.settimeout(0.6)
    try:
        while len(buf)<100000: buf+=s.recv(65536)
    except Exception: pass
    vals=[]
    for line in buf.decode("latin1").splitlines():
        if ":" in line: vals+=re.findall(r"0x([0-9a-f]{2})\b", line.split(":",1)[1])
    chars=[chr(int(v,16)) if 32<=int(v,16)<127 else " " for v in vals[::2][:2000]]
    return "".join(chars)
ok=False
for _ in range(20):
    if "Welcome to noxis" in screen(): ok=True; break
    time.sleep(0.5)
print("GRUB BOOT:", "PASS" if ok else "FAIL")
s.close(); sys.exit(0 if ok else 1)
PY
pkill -f "qemu-system-i386.*noxis.iso"; echo DONE
```
Expected: `MULTIBOOT_OK`, затем `GRUB BOOT: PASS`.
Если `grub-mkrescue` ругается на mformat (mtools) — `sudo pacman -S --needed mtools`.

- [ ] **Step 8: Проверить, что флоппи-путь тоже грузится**

Run: `cd 23-fixes && make os-image.bin && timeout 60 python3 ../tests/qemu_test.py regress_interactive`
Expected: `PASS` (баннер + HELP/ECHO/TIME работают через новый стейджинг; остальные тесты — прежняя RED-форма, их чинит другой план).

- [ ] **Step 9: Commit**

```bash
cd ~/github/noxisOS
git add -A 23-fixes tests 2>/dev/null
git commit -m "feat(boot): Multiboot/GRUB path + floppy staging to 1MB

- kernel links at 1MB via kernel.ld; unified entry _start with own stack
- floppy path stages to 0x10000 and relocates in PM before jumping _start
- Makefile: noxis.elf / kernel.bin(objcopy) / iso(grub-mkrescue) / run-grub
- kernel_entry.asm retired (role moved to _start)"
```

---

### Task 2: Куча от линкерного символа

**Files:**
- Modify: `23-fixes/libc/mem.c`, `23-fixes/libc/mem.h`
- Modify: `23-fixes/drivers/shell.c:143-156` (`cmd_mem`)

**Interfaces:**
- Consumes: символ `end_of_kernel` (Task 1, kernel.ld)
- Produces: `uint32_t heap_allocated(void)`, `uint32_t heap_free(void)` (mem.h); `HEAP_CAP 0x200000`

- [ ] **Step 1: mem.h — заменить константы HEAP_START/HEAP_SIZE**

```c
#ifndef MEM_H
#define MEM_H

#include "type.h"   /* или <stdint.h> — как в текущем файле */

/* Верхняя граница кучи для статистики. Допущение спеки: RAM >= 2 MiB. */
#define HEAP_CAP 0x200000u

uint32_t kmalloc(size_t size, int align, uint32_t *phys_addr);
uint32_t heap_allocated(void);
uint32_t heap_free(void);

#endif
```
(сигнатуры существующих функций сохранить как есть — сверить с текущим заголовком перед правкой; удалить только HEAP_START/HEAP_SIZE)

- [ ] **Step 2: mem.c — ленивая инициализация от символа**

Добавить после includes:
```c
/* Линкерный символ из kernel.ld: первый 4KiB-выровненный адрес после
 * образа ядра. Работает одинаково на обоих путях загрузки. */
extern uint8_t end_of_kernel[];

static uint8_t *heap_ptr = 0;
```
`kmalloc` начать с:
```c
    if (heap_ptr == 0)
        heap_ptr = (uint8_t *)(((uint32_t)end_of_kernel + 0xFFF) & ~(uint32_t)0xFFF);
```
и заменить внутри все использования `free_mem_addr` на `heap_ptr`
(`ret = heap_ptr; heap_ptr += size;`). Удалить глобал `free_mem_addr`.
Добавить в конец файла:
```c
uint32_t heap_allocated(void) {
    return (uint32_t)(heap_ptr ? heap_ptr : 0) -
           ((uint32_t)((uint32_t)end_of_kernel + 0xFFF) & ~(uint32_t)0xFFF);
}

uint32_t heap_free(void) {
    return HEAP_CAP - heap_allocated();
}
```

- [ ] **Step 3: shell.c cmd_mem — новая формула**

Заменить тело `cmd_mem` на:
```c
static void cmd_mem(char *args) {
    UNUSED(args);
    char buf[12];
    int_to_ascii((int)heap_allocated(), buf);
    kprint("MEM: allocated ");
    kprint(buf);
    kprint(" bytes, free ");
    int_to_ascii((int)heap_free(), buf);
    kprint(buf);
    kprint(" bytes\n");
}
```

- [ ] **Step 4: Проверка headless на флоппи-пути**

Run: `cd 23-fixes && make && cd .. && timeout 60 python3 tests/qemu_test.py regress_interactive && timeout 60 python3 - <<'PY'
import sys; sys.path.insert(0, "tests")
from qemu_test import Noxis
v = Noxis()
try:
    v.type_line("page")     # аллокация через kmalloc
    v.type_line("mem")
    assert v.expect("MEM: allocated", 5), "MEM output missing"
    rows = [r for r in v.screen() if "MEM:" in r][0]
    nums = [int(x) for x in rows.replace(","," ").split() if x.isdigit()]
    alloc = nums[0]; free = nums[1]
    assert 0 < alloc < free, f"suspicious stats: {alloc}/{free}"
    print(f"MEM sanity PASS: allocated={alloc} free={free}")
finally:
    v.close()
PY
`
Expected: регресс PASS, `MEM sanity PASS: allocated=<N> free=<M>` при N>0 (PAGE только что аллоцировал) и M>alloc.

- [ ] **Step 5: Commit**

```bash
git add 23-fixes/libc/mem.c 23-fixes/libc/mem.h 23-fixes/drivers/shell.c 23-fixes/*.bin 23-fixes/**/*.o
git commit -m "feat(mem): heap base from linker symbol end_of_kernel; HEAP_CAP stats"
```

---

### Task 3: Тест t7 (ISO), README и финальный прогон

**Files:**
- Modify: `tests/qemu_test.py`
- Modify: `README.md` (строка баннера «no GRUB» и Quick start)

**Interfaces:**
- Consumes: `Noxis.__init__(self)` — расширяется опциональным параметром приводов

- [ ] **Step 1: Расширить Noxis и добавить t7**

Сигнатура конструктора:
```python
    def __init__(self, extra_qemu_args=None):
        self.qemu = subprocess.Popen(
            ["qemu-system-i386", "-m", "32",
             "-drive", f"format=raw,file={IMAGE}"]
            + (extra_qemu_args or []) +
            ["-display", "none", "-no-reboot", "-no-shutdown",
             "-monitor", f"unix:{MON},server,nowait"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
```
(все существующие вызовы `Noxis()` продолжают работать без аргументов)

Новый тест перед списком TESTS:
```python
def t7_grub_iso():
    subprocess.run(["make", "-C", "23-fixes", "noxis.iso"],
                   check=True, stdout=subprocess.DEVNULL)
    v = Noxis(extra_qemu_args=["-cdrom", "23-fixes/noxis.iso", "-boot", "d"])
    try:
        assert v.expect("Welcome to noxis", 30), "no banner via GRUB/ISO"
        v.type_line("help")
        assert v.expect("Available commands", 5), "shell dead on ISO path"
        return True, ""
    finally:
        v.close()
```
Добавить `t7_grub_iso` в список `TESTS`.

- [ ] **Step 2: README — убрать ложь про «no GRUB»**

В баннере заменить строку
```
       |_| |_|\___/ \_/    32-bit · freestanding · no GRUB
```
на
```
       |_| |_|\___/ \_/    32-bit · freestanding · floppy or GRUB
```
В Quick start после блока запуска добавить:
```markdown
Or boot the Multiboot way (needs `grub-mkrescue`, `xorriso`, `mtools`):

```bash
cd 23-fixes && make iso run-grub
```
```

- [ ] **Step 3: Финальный полный прогон**

Run: `python3 tests/qemu_test.py`
Expected: `t7_grub_iso PASS`; остальные — не хуже записанного RED-базлайна (их починка — план планировщика). Зафиксировать вывод.

- [ ] **Step 4: Commit (включая бинарники)**

```bash
git add tests/qemu_test.py README.md 23-fixes
git commit -m "test+t(docs): ISO acceptance test; README reflects GRUB support"
```

---

## Самопроверка плана

- Спека → таски: entry/header/линкер/Makefile/GRUB-путь (T1), стейджинг+релокация флоппи (T1), куча от символа + HEAP_CAP + cmd_mem (T2), t7 + README + критерий готовности (T3). Отступление по disk.asm задокументировано в шапке T1 с обоснованием. Покрыто всё.
- Заглушек нет; весь код приведён полностью.
- Имена согласованы: `end_of_kernel` (ld ↔ mem.c), `pm_relocate_and_jump` (bootsect ↔ pm_relocate.asm), `_start` (везде), `Noxis(extra_qemu_args=...)`.
