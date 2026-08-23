# Scheduler Context-Switch Fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Починить первое переключение контекста в планировщике noxis: задачи реально стартуют и вытесняются, шелл не голодает, веса работают.

**Architecture:** Планировщик пишет тройку `[eip|cs|eflags]` на собственный стек целевой задачи и поднимает флаг; эпилог `irq_common_stub` при поднятом флаге прыгает на этот стек перед `iret`. Без флага — обычный выход через нетронутый кадр. Спека: `docs/superpowers/specs/2026-08-23-noxis-scheduler-fix-design.md`.

**Tech Stack:** nasm (elf32/bin), gcc -m32 freestanding, GNU ld, QEMU i386 (headless monitor `sendkey` + `xp` VGA-буфера), Python 3 для тестового стенда.

## Global Constraints

- Ветка: `feat/weighted-scheduler` (текущая); работаем в ней
- `isr_common_stub` НЕ меняется (исключения не планируют)
- Флаги сборки из `23-fixes/Makefile` не трогать (`-mno-sse…`, `-Ttext 0x1000`)
- Имена глобалов строго `sched_resume_esp`, `sched_switch_pending` (asm↔C контракт)
- Слот 0 = kernel/idle, никогда не взвешивается (`quantum_left` логика только для `current_task > 0`)
- Бинарные артефакты (`*.o *.bin`) в этом репо коммитятся — финальный таск включает `make` и коммит пересобранных образов
- Тесты запускаются из корня репо: `python3 tests/qemu_test.py`; каждый тест получает свежий QEMU

---

### Task 1: Headless-стенд и RED-базлайн

**Files:**
- Create: `tests/qemu_test.py`

**Interfaces:**
- Produces: команда `python3 tests/qemu_test.py [имя_теста]`; класс `Noxis` с методами `type_line(str)`, `screen() -> list[str]`, `expect(substr, seconds) -> bool`, `read_task_ticks(name) -> int|None`. Тесты Task 2 зависят только от CLI-выхода (код возврата).

- [ ] **Step 1: Написать стенд**

```python
#!/usr/bin/env python3
"""Headless acceptance tests for the noxis scheduler (spec 2026-08-23).

Boots 23-fixes/os-image.bin in QEMU without a display, drives the shell
through the QEMU monitor (`sendkey`), and asserts on the VGA text buffer
read via `xp /2000xb 0xb8000`.
"""
import re
import socket
import subprocess
import sys
import time

IMAGE = "23-fixes/os-image.bin"
MON = "/tmp/noxis-qmon"


class Noxis:
    def __init__(self):
        self.qemu = subprocess.Popen(
            ["qemu-system-i386", "-m", "32",
             "-drive", f"format=raw,file={IMAGE}",
             "-display", "none", "-no-reboot", "-no-shutdown",
             "-monitor", f"unix:{MON},server,nowait"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 5
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(MON)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise RuntimeError("qemu monitor socket never appeared")
                time.sleep(0.1)
        self.s.settimeout(0.05)
        self.drain()
        if not self.expect("Welcome to noxis", 15):
            raise RuntimeError("kernel banner never appeared")

    def drain(self):
        try:
            while True:
                if not self.s.recv(65536):
                    break
        except socket.timeout:
            pass

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        self.drain()

    _KEYMAP = {" ": "spc", "\n": "ret"}

    def type_line(self, text):
        """Type a command line and press Enter. Lowercase is fine: the
        shell matches commands case-insensitively."""
        for ch in text:
            self.cmd("sendkey " + self._KEYMAP.get(ch, ch))
        self.cmd("sendkey ret")

    def screen(self):
        self.cmd("xp /2000xb 0xb8000")
        # Re-query with a proper blocking read: drain() ate the reply above,
        # so ask again and collect until quiet period.
        self.s.settimeout(0.5)
        self.s.sendall(b"xp /2000xb 0xb8000\n")
        buf = b""
        try:
            while len(buf) < 100000:
                d = self.s.recv(65536)
                if not d:
                    break
                buf += d
        except socket.timeout:
            pass
        vals = []
        for line in buf.decode("latin1").splitlines():
            if ":" not in line:
                continue
            vals += re.findall(r"0x([0-9a-f]{2})\b", line.split(":", 1)[1])
        chars = []
        for i in range(0, len(vals) - 1, 2):
            c = int(vals[i], 16)
            chars.append(chr(c) if 32 <= c < 127 else " ")
        rows = ["".join(chars[r * 80:(r + 1) * 80]).rstrip() for r in range(25)]
        self.s.settimeout(0.05)
        return rows

    def expect(self, substr, seconds=5):
        deadline = time.time() + seconds
        while time.time() < deadline:
            if any(substr in row for row in self.screen()):
                return True
            time.sleep(0.3)
        return False

    def read_task_ticks(self, name):
        """Parse the TASKS table: find the row for `name`, return its ticks."""
        for row in self.screen():
            m = re.match(r"^(\d+)\s+%s\s+\S+\s+(\d+)\s" % re.escape(name), row)
            if m:
                return int(m.group(2))
        return None

    def close(self):
        try:
            self.cmd("quit")
        except Exception:
            pass
        self.s.close()
        self.qemu.wait(timeout=5)


def t1_regress_interactive():
    v = Noxis()
    try:
        v.type_line("help")
        assert v.expect("Available commands"), "HELP printed nothing"
        v.type_line("echo hi noxis")
        assert v.expect("hi noxis"), "ECHO printed nothing"
        v.type_line("time")
        assert v.expect("-20"), "TIME printed no date"
        return True, ""
    finally:
        v.close()


def t2_run_blink_shell_alive():
    v = Noxis()
    try:
        v.type_line("run blink")
        assert v.expect("[blink]", 5), "[blink] never appeared"
        v.type_line("help")
        assert v.expect("Available commands", 5), "shell dead after RUN"
        return True, ""
    finally:
        v.close()


def t3_parallel_tasks():
    v = Noxis()
    try:
        v.type_line("run count")
        v.type_line("run clock")
        assert v.expect("[clock]", 6), "[clock] missing"
        assert v.expect("[count]", 2), "[count] missing"
        v.type_line("tasks")
        assert v.expect("count", 3) and v.expect("clock", 3), "TASKS incomplete"
        return True, ""
    finally:
        v.close()


def t4_cooperative_kill():
    v = Noxis()
    try:
        v.type_line("run blink")
        assert v.expect("[blink]", 5), "[blink] never appeared"
        v.type_line("kill 1")
        time.sleep(2)
        v.type_line("tasks")
        time.sleep(0.5)
        rows = v.screen()
        gone = all(not row.startswith("1 ") or "blink" not in row for row in rows)
        assert gone, "killed task still listed"
        v.type_line("help")
        assert v.expect("Available commands", 5), "shell dead after KILL"
        return True, ""
    finally:
        v.close()


def t5_weights_3to1():
    v = Noxis()
    try:
        v.type_line("run spin")     # default weight 3
        v.type_line("run blink")    # default weight 1
        time.sleep(12)              # let ticks accumulate
        s1, b1 = v.read_task_ticks("spin"), v.read_task_ticks("blink")
        time.sleep(5)
        s2, b2 = v.read_task_ticks("spin"), v.read_task_ticks("blink")
        ds, db = s2 - s1, b2 - b1
        ratio = ds / max(db, 1)
        assert 2.0 <= ratio <= 4.5, f"weight ratio {ratio:.2f} (spin={ds}, blink={db})"
        return True, ""
    finally:
        v.close()


def t6_yield_alive():
    v = Noxis()
    try:
        v.type_line("yield")
        assert v.expect("yielded", 5), "YIELD produced no reply"
        v.type_line("uptime")
        assert v.expect("Uptime:", 5), "system dead after YIELD"
        return True, ""
    finally:
        v.close()


TESTS = [t1_regress_interactive, t2_run_blink_shell_alive,
         t3_parallel_tasks, t4_cooperative_kill,
         t5_weights_3to1, t6_yield_alive]

if __name__ == "__main__":
    wanted = sys.argv[1:]
    failed = []
    for t in TESTS:
        if wanted and t.__name__ not in wanted and t.__name__[2:] not in wanted:
            continue
        try:
            ok, msg = t()
            status = "PASS" if ok else f"FAIL {msg}"
        except Exception as e:
            status = f"FAIL {type(e).__name__}: {e}"
            ok = False
        print(f"[{status}] {t.__name__}")
        if not ok:
            failed.append(t.__name__)
    print(f"\n{len(TESTS)-len(failed)}/{len(TESTS)} passed")
    sys.exit(1 if failed else 0)
```

- [ ] **Step 2: Проверить стенд против ТЕКУЩЕЙ сборки (ожидаем RED)**

Run: `cd ~/github/noxisOS && python3 tests/qemu_test.py`
Expected: как минимум `t2_run_blink_shell_alive` FAIL (первое переключение сломано — см. спеку), `t1_regress_interactive` PASS. Запишите полный вывод в отчёт.
Если t2 вдруг PASSES — остановка и доклад: гипотеза о баге неверна, план пересматривается.

- [ ] **Step 3: mkdir-заглушки не нужны; Commit**

```bash
git add tests/qemu_test.py
git commit -m "test: headless QEMU acceptance suite for scheduler"
```

---

### Task 2: Механизм переключения контекста

**Files:**
- Modify: `23-fixes/cpu/interrupt.asm` (шапка externs + эпилог `irq_common_stub`)
- Modify: `23-fixes/cpu/task.c` (глобалы, `task_create_prio`, `scheduler_tick`)

**Interfaces:**
- Consumes: `registers_t` (isr.h), `KERNEL_CS` (idt.h), существующие поля PCB (`weight`, `quantum_left`, `stack_top`, `stack_base`)
- Produces: `volatile uint32_t sched_resume_esp`, `volatile uint8_t sched_switch_pending` (asm читает оба по имени)

- [ ] **Step 1: interrupt.asm — externs**

Сразу после `[extern irq_handler]` добавить:
```asm
[extern sched_switch_pending]
[extern sched_resume_esp]
```

- [ ] **Step 2: interrupt.asm — новый эпилог irq_common_stub**

Заменить хвост стаба (от `popa` до `iret` включительно) на:
```asm
    popa
    add esp, 8
    cmp byte [sched_switch_pending], 0
    jne .switch_stack
    iret                        ; обычный выход: кадр планировщиком не тронут
.switch_stack:
    mov byte [sched_switch_pending], 0
    mov esp, [sched_resume_esp] ; прыжок на стек целевой задачи
    iret                        ; снимаем eip|cs|eflags уже оттуда
```
`isr_common_stub` не трогать.

- [ ] **Step 3: task.c — глобалы**

После `static int current_task = -1;` добавить:
```c
/* Контракт с interrupt.asm: когда планировщик решил переключиться, он кладёт
 * адрес тройки [eip|cs|eflags] НА СТЕКЕ ЦЕЛЕВОЙ задачи в sched_resume_esp и
 * поднимает флаг; эпилог irq_common_stub тогда делает mov esp,[resume_esp]
 * перед iret. Без флага любой IRQ (клавиатура!) улетал бы на чужой стек. */
volatile uint32_t sched_resume_esp = 0;
volatile uint8_t sched_switch_pending = 0;
```

- [ ] **Step 4: task.c — кадр новой задачи в task_create_prio**

Удалить блок построения синтетического iret-кадра (сейчас строки ~89-98:
комментарий «Build a synthetic interrupt frame…» вместе с пятью `*--sp = …`).
Заменить заполнение контекста (сейчас ~строки 111-113) на:
```c
    /* Стартовая тройка [eip|cs|eflags] на верхушке СОБСТВЕННОГО стека задачи:
     * iret эпилога снимет её оттуда и оставит ESP == esp0. */
    uint32_t esp0 = stack_top - 16;
    uint32_t *start_blk = (uint32_t *)(esp0 - 12);
    start_blk[0] = (uint32_t)task_trampoline; /* eip */
    start_blk[1] = KERNEL_CS;                 /* cs  */
    start_blk[2] = 0x202;                     /* eflags: IF=1 */

    task_t *t = &tasks[slot];
    t->id = slot;
    set_name(t, name);
    t->state = TASK_READY;
    t->ticks = 0;
    t->kill_req = 0;
    t->wake_tick = 0;
    t->entry = entry;
    t->stack_top = stack_top;
    t->weight = weight;
    t->quantum_left = weight;
    memory_set((uint8_t *)&t->regs, 0, sizeof(registers_t));
    t->regs.ds = 0x10;          /* стаб грузит это значение во все сегменты */
    t->regs.ss = 0x10;          /* informational: ring0 iret ss не читает */
    t->regs.cs = KERNEL_CS;
    t->regs.eflags = 0x202;
    t->regs.eip = (uint32_t)task_trampoline;
    t->regs.esp = esp0;
```
(переменная `uint32_t *sp = (uint32_t *)stack_top;` из старого блока удаляется за ненадобностью)

- [ ] **Step 5: task.c — переписать scheduler_tick целиком**

```c
void scheduler_tick(registers_t *r) {
    /* 1. Сохранить контекст прерванной задачи. */
    if (current_task >= 0 && current_task < MAX_TASKS) {
        tasks[current_task].regs = *r;
        if (tasks[current_task].state == TASK_RUNNING)
            tasks[current_task].state = TASK_READY;
    }

    /* 2. Взвешенное продолжение: та же задача держит CPU, кадр НЕ трогаем —
     *    флаг остаётся сброшен, обычный iret продолжит её. Слот 0 не взвешен. */
    if (current_task > 0 && current_task < MAX_TASKS) {
        task_t *cur = &tasks[current_task];
        if (cur->state == TASK_READY && cur->quantum_left > 0) {
            cur->quantum_left--;
            cur->ticks++;
            cur->state = TASK_RUNNING;
            return;
        }
    }

    /* 3. Ротация по ВСЕМ слотам, включая kernel/idle (0). Спящие просыпаются. */
    int next = -1, i;
    for (i = 1; i <= MAX_TASKS; i++) {
        int idx = (current_task + i) % MAX_TASKS;
        task_t *t = &tasks[idx];
        if (t->state == TASK_SLEEPING && tick >= t->wake_tick)
            t->state = TASK_READY;
        if (t->state == TASK_READY) { next = idx; break; }
    }

    /* 4а. Никого другого или снова мы сами: обычный iret продолжает текущую
     *     (она READY — иначе сюда не попали бы). Кадр не трогаем. */
    if (next < 0 || next == current_task) {
        tasks[current_task].state = TASK_RUNNING;
        return;
    }

    /* 4б. Переключение на N: тройка iret уходит на ЕЁ стек. */
    task_t *nt = &tasks[next];
    nt->state = TASK_RUNNING;
    nt->ticks++;
    nt->quantum_left = nt->weight - 1;

    uint32_t *blk = (uint32_t *)(nt->regs.esp - 12);
    blk[0] = nt->regs.eip;
    blk[1] = nt->regs.cs;
    blk[2] = nt->regs.eflags;
    sched_resume_esp = (uint32_t)blk;
    sched_switch_pending = 1;

    *r = nt->regs;      /* GPR и сегменты восстановит popa/mov в стабе */
    current_task = next;
}
```

- [ ] **Step 6: Собрать и прогнать GREEN**

Run: `cd 23-fixes && make && cd .. && python3 tests/qemu_test.py`
Expected: все 6 тестов PASS. Если падает t5 (веса) при зелёных остальных — допустимо расширить окно ratio до 1.8–5.0 и перезапустить один раз; зафиксировать фактические числа.

- [ ] **Step 7: Commit (включая пересобранные бинарники — конвенция репо)**

```bash
git add 23-fixes/cpu/task.c 23-fixes/cpu/interrupt.asm 23-fixes/*.bin 23-fixes/**/*.o
git commit -m "fix(sched): working context switch via target-stack iret block

- task_create fills cs/eflags/ds; startup triple lives on the task's own stack
- irq epilogue jumps to sched_resume_esp when scheduler raised the flag
- rotation includes idle slot 0; sleeping tasks can no longer starve the shell"
```

---

### Task 3: Документация и финальный прогон

**Files:**
- Modify: `docs/scheduler-design.md`

**Interfaces:** только документация.

- [ ] **Step 1: Переписать секции про переключение**

В `docs/scheduler-design.md` заменить диаграмму и текст «Context switch flow» на:
```markdown
## Context switch flow

`scheduler_tick(registers_t *r)` receives the *interrupted* frame:

1. `cur->regs = *r;` — save the live frame into the PCB.
2. Pick the next READY task by weighted round-robin over ALL slots
   (slot 0 included; SLEEPING tasks wake when `tick >= wake_tick`).
3. If the winner is somebody else, the scheduler writes the triple
   `[eip | cs | eflags]` of the winner at `winner->regs.esp - 12`
   (dead slots of its own old interrupt frame, on its own stack),
   stores that address in `sched_resume_esp` and raises
   `sched_switch_pending`.
4. The `irq_common_stub` epilogue sees the flag, does
   `mov esp, [sched_resume_esp]` and `iret` — popping the triple from
   the TARGET's stack, leaving ESP exactly where the task was
   interrupted (or at its fresh startup pointer).

When the winner is the current task, the flag stays low and the stub
returns through the untouched frame. All other IRQs also leave the flag
low — that is why the conditional jump exists: an unconditional stack
switch would hijack every interrupt onto a foreign stack.
```
И в «Initial-task stack frame» заменить схему на:
```markdown
A freshly `task_create()`'d task owns a startup triple written at the top
of ITS OWN stack: `[trampoline | KERNEL_CS | 0x202]`, with
`regs.esp = stack_top - 16`. The first `iret` into the task pops that
triple and begins execution with a clean stack below it.
```
Удалить строку ограничения `- **No stack recycling** — …` (стеки переиспользуются с commit 6aced4d, `task.c:78-86`).

- [ ] **Step 2: Финальный полный прогон**

Run: `python3 tests/qemu_test.py`
Expected: `6/6 passed`.

- [ ] **Step 3: Commit**

```bash
git add docs/scheduler-design.md
git commit -m "docs: scheduler design reflects target-stack iret switching"
```

---

## Самопроверка плана

- Спека → таски: механизм (T2), ротация+слот 0+спящие (T2 шаг 5), документация (T3), критерий готовности 6/6 (T2 шаг 6, T3 шаг 2). RED-подтверждение бага до фикса (T1 шаг 2). Покрыто всё.
- Заглушек нет: весь код стенда и правок приведён целиком.
- Контракт имён согласован: `sched_resume_esp`/`sched_switch_pending` одинаковы в asm и C; `Noxis.read_task_ticks` используется в t5 так же, как объявлен.
