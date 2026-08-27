# Task 7: End-to-end build + handoff verification

**From plan:** docs/superpowers/plans/2026-08-15-multitasking-core.md

## Context
Final task (7 of 7). Verification only — no source changes except opportunistic
commit of stray files. All code Tasks 1–6 are complete and committed. This task
proves the whole branch builds clean from scratch. QEMU runtime checks are handed
to the user (no automated harness).

## Steps

### Step 1: Clean build from scratch
```bash
cd 23-fixes && make clean && make 2>&1 | tail -25
```
Expected: `os-image.bin` and `kernel.bin` built with no errors.

### Step 2: Hand off QEMU verification to the user
List the manual checks (from the spec's Verification section):
1. `make run` (QEMU) -> `HELP` shows `TASKS PS RUN KILL YIELD`.
2. `RUN blink` then `RUN spin` -> `spin` advances faster (weight 3).
3. `RUN spin 1` vs `RUN spin 3` -> weight 3 updates more often.
4. `TASKS` shows the `W` column with weights.
5. `KILL <id>` then repeat `RUN spin`/`KILL <id>` several times; `MEM` shows
   stack allocations stabilizing (leak fix).
6. `END` halts as before.

### Step 3: Final commit (if any stray tracked files remain)
```bash
git status --short
```
If only build artifacts remain, no commit needed. If any tracked source file is
modified-and-uncommitted from Tasks 1–6, `git add` and commit it with a
descriptive message.

## Report contract
This task is run by the controller directly (not a subagent). No report file.
