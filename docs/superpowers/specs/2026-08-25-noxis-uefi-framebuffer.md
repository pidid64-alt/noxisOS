# Спека: UEFI-видео для noxis — фреймбуфер-консоль

**Дата:** 2026-08-25
**Статус:** одобрено пользователем (фреймбуфер-консоль; серийный порт и
BIOS-only отклонены)
**Ветка:** `feat/weighted-scheduler`

## Проблема

Загрузка `noxis.iso` на реальном ПК в режиме UEFI падает с ошибкой GRUB:

```
WARNING: no console will be available to OS
error: video/video.c:grub_video_set_mode:782: no suitable video mode found.
```

Корневая причина (воспроизведена в QEMU/OVMF, скриншоты и дампы регистров):
Multiboot v1-заголовок ядра просит текстовый режим (`flags=0`), под UEFI
текстового VGA-режима нет (только GOP-графика). Ошибка установки режима
фатальна для пункта меню: ядро не получает управление (CPU остаётся в
64-битном коде GRUB). Под BIOS/CSM ISO полностью рабочий. Правка grub.cfg
(`insmod all_video`, `gfxpayload=...`) ядро не спасает — проверено.

## Решение

Ядро запрашивает у GRUB линейный фреймбуфер и рисует текст само.

### Multiboot-заголовок (`boot/multiboot_entry.asm`)
- `flags = 0x00000004` (bit 2 — запрос видео-информации).
- Дополнительные поля: `mode_type=0` (linear graphics), `width=height=depth=0`
  (режим выбирает GRUB: под UEFI — GOP, под BIOS — VBE).
- Checksum: `-(0x1BADB002 + 0x4) = 0xE4524FFA`.

### Контракт точки входа (ОТКЛОНЕНИЕ от спеки 2026-08-23)
Ранее: «`_start` не проверяет magic в EAX, сигнатура `kernel_main` не меняется».
Теперь magic нужен, чтобы отличить GRUB-путь (есть MBI) от флоппи-пути:

```
kernel_main(uint32_t magic, uint32_t mbi_addr)
```

- GRUB-путь: `EAX=0x2BADB002`, `EBX=MBI` → `_start` делает
  `push ebx; push eax; call kernel_main`.
- Флоппи-путь: `boot/pm_relocate.asm` обнуляет `EAX`/`EBX` перед прыжком →
  ядро видит `magic != 0x2BADB002` и остаётся на легаси VGA-тексте
  (поведение флоппи не меняется).

### Разбор MBI (`drivers/fb.c`)
Multiboot v1 info при `flags bit 12`: `framebuffer_addr` (u64 @88),
`framebuffer_pitch` @96, `framebuffer_width` @100, `framebuffer_height` @104,
`framebuffer_bpp` @108, `framebuffer_type` @112. Активируем fb-консоль только
при `framebuffer_type == 0` (пиксельный RGB) и bpp 24/32. `type==1` (EGA text)
или отсутствие MBI → легаси VGA-текст.

### Рендерер (`drivers/fb.c`, `drivers/font8x16.c`)
- Шрифт 8x16, ASCII 32..126, извлекается из системного PSF-шрифта
  (kbd/consolefonts) скриптом при генерации файла — в репо лежит готовый C-массив.
- Вывод: глифы в пиксели по `pitch` (24 и 32 bpp), `\n`, скролл построчным
  `memory_copy`, блочный курсор-инверсия.
- Публичный API `drivers/screen.h` не меняется: `kprint*`, `clear_screen`,
  `kprint_backspace` диспетчеризуются на fb при активном фреймбуфере.
  `shell.c` не трогается.

## Границы

- Цвета фиксируем: белый на чёрном; ошибка координат в `print_char` — красный
  на белом. Палитр/конфигов нет.
- Точное позиционирование `kprint_at(col,row)` на fb — те же ячейки 80x25
  (умножаются на 8x16); при разрешении меньше 640x400 ячейки за экраном
  обрезаются (не поддерживаем тайлинг разрешений).
- Аппаратный курсор CRTC на fb не существует — только блочный.

## План тестирования

1. `make`; `grub-file --is-x86-multiboot noxis.elf` → OK.
2. BIOS-ISO (`-cdrom noxis.iso -boot d`, SeaBIOS): `screendump` → баннер
   пикселями (PIL-анализ: экран не чёрный, есть строки текста).
3. UEFI-ISO (OVMF `OVMF_CODE.4m.fd`): `screendump` t≈8s → баннер виден;
   `sendkey`-ввод `HELP` → список команд отрисован. До фикса: чёрный экран.
4. Флоппи-регрессия: `tests/qemu_test.py` (VGA-текст через `xp 0xb8000`)
   не хуже базлайна.
5. Ручной критерий: ISO на флешке грузится с терминалом на UEFI-машине
   пользователя.

Критерий готовности: (2) и (3) зелёные, (4) не хуже базлайна.
