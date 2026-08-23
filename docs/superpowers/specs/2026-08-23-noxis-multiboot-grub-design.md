# Дизайн: Multiboot-загрузка noxis через GRUB (два пути)

**Дата:** 2026-08-23
**Статус:** одобрено пользователем (подход A — канонический, ядро на 1MB)
**Ветка:** `feat/weighted-scheduler`

## Цель

Ядро noxis становится Multiboot-совместимым и грузится двумя независимыми
путями из одного образа кода:

```
GRUB-путь:   BIOS → GRUB (ISO) ──────────────► ELF @ 0x100000 ─┐
                                                               ├─► _start → kernel_main
Флоппи-путь: BIOS → bootsect → RAM 0x10000 → PM-копия ─────────┘
                              в 0x100000 → jmp _start
```

Существующий флоппи-путь сохраняется как рабочий (не музей).

## Контракт точки входа

`_start` приходит в состоянии «protected mode, прерывания выключены,
GDT валиден» на ОБОИХ путях: GRUB гарантирует это по спецификации
Multiboot; флоппи-путь доводит состояние сам (сегодняшний `switch_pm`
плюс новая PM-релокация). `_start` не проверяет magic в EAX — оба пути
эквивалентны для запуска. Сигнатура `kernel_main` не меняется.

## Компоненты

### 1. `23-fixes/boot/multiboot_entry.asm` (новый)
- Секция `.multiboot`: заголовок Multiboot v1 — magic `0x1BADB002`,
  flags `0x00000000`, checksum `0xE4524FCE`. Адреса загрузки не задаются:
  payload отдаётся GRUB'у как ELF32, адреса он берёт из program headers.
- `_start`: `cli`; `mov esp, stack_top`; `call kernel_main`;
  вечный `hlt`-цикл после возврата.
- Стек 16 KiB в `.bss` (`stack_bottom`/`stack_top` метки).
- Файл объявляет `[global _start]`.

### 2. `23-fixes/kernel.ld` (новый)
- `ENTRY(_start)`; база `. = 1M`.
- Порядок секций: `.multiboot` первым (`KEEP(*(.multiboot))`),
  затем `.text .rodata .data .bss`.
- Стек из `multiboot_entry.asm` попадает в `.bss` автоматически.
- Экспорт: `PROVIDE(end_of_kernel = ALIGN(ADDR(.bss) + SIZEOF(.bss), 4096));`

### 3. Флоппи-путь: стейджинг через 0x10000 → 0x100000
- `boot/disk.asm`: посекторное чтение с сегментной арифметикой — BX не
  выходит за границы 64KiB, при переполнении инкрементируется ES
  (сейчас BX тупо инкрементится и заворачивается на границе 64 KiB,
  портя память при ядре >64 KiB).
- `boot/bootsect.asm`: `KERNEL_OFFSET equ 0x10000` (вместо 0x1000).
- Новый `boot/pm_relocate.asm` (вызывается из `BEGIN_PM` вместо
  `call KERNEL_OFFSET`): dword-копия `esi=0x10000 → edi=0x100000`,
  счётчик `(KERNEL_SECTORS*512)/4` (символ уже выпекается Makefile'ом),
  затем `jmp 0x08:_start`.
- Размер бинарника не меняется от релокации: `objcopy -O binary`
  пишет LMA-компактно (~десятки KiB), формула секторов в Makefile
  остаётся корректной.

### 4. Куча от линкерного символа
- `libc/mem.c`: `free_mem_addr` лениво инициализируется выравненным
  на 4KiB значением символа `end_of_kernel` при первом `kmalloc`
  (статическая инициализация адресом линкерного символа невозможна).
- `mem.h`: константы `HEAP_START` удаляются; вводится `HEAP_CAP = 0x200000`
  как верхняя граница для статистики. `cmd_mem` в шелле считает
  `allocated = free_mem_addr - aligned_end_of_kernel`,
  `free = HEAP_CAP - allocated`. Документируется допущение: объём RAM
  ≥ 2 MiB (без probe memory map — вне скоупа).
- Оба пути получают идентичную раскладку кучи.

### 5. Makefile
- `noxis.elf`: `ld -m elf_i386 -T kernel.ld` всех объектов (замена
  текущего `kernel.elf` без `-Ttext`).
- `kernel.bin` (флоппи): `objcopy -O binary noxis.elf kernel.bin`;
  bootsect по-прежнему выпекается с `-DKERNEL_SECTORS=ceil(size/512)`.
- `iso:` сборка `isodir/boot/grub/{grub.cfg,noxis.elf}` →
  `grub-mkrescue -o noxis.iso isodir`;
  `grub.cfg`: единственный `menuentry "noxis" { multiboot /boot/noxis.elf }`.
- `run-grub:` `qemu-system-i386 -cdrom noxis.iso`; `run:` — как раньше.
- Предусловие: `grub-mkrescue` и `xorriso` установлены; если нет — цель
  печатает подсказку `sudo pacman -S --needed grub xorriso` и падает явно.

## Обработка ошибок / границы

- Несовпадение magic/checksum заголовка → GRUB скажет
  «no multiboot header found»; ловится тестом загрузки ISO.
- Забытая PM-релокация на флоппи-пути → тройной fault сразу после
  прыжка; тоже ловится headless-тестом флоппи-пути.
- Вложенность со сломанным планировщиком: задача планировщика стоит
  паузой в SDD-ledger; GRUB-работа не зависит от неё и идёт первой
  по решению пользователя.

## План тестирования

Расширение существующего стенда `tests/qemu_test.py`:
1. **t7_grub_iso (новый)**: QEMU с `-cdrom noxis.iso` → баннер
   «Welcome to noxis» → `HELP` печатает список. Свежий VM, как остальные.
2. **Регрессия флоппи-пути**: все существующие тесты остаются на
   `os-image.bin`; их зелёность подтверждает, что стейджинг не сломал
   старый путь (сейчас они красные из-за известных багов планировщика/
   клавиатуры — критерий только «не хуже RED-базлайна»).
3. **Ручная проверка**: `grub-file --is-x86-multiboot noxis.elf` → exit 0;
   `make run-grub` глазами пользователя.

Критерий готовности: t7 зелёный; флоппи-базлайн не ухудшился;
`make run-grub` показывает тот же шелл, что и `make run`.
