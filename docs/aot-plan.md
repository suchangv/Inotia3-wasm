# AOT: translating `libgame.so` to native aarch64

**Status:** plan + the first stage implemented. `src/aot_predec.c` (pre-decoded
Thumb-1 execution, `--aot=predec`) exists, is verified against the interpreter
instruction by instruction, and has been measured -- see §2. Its result changes
the plan: pre-decoding alone is worth **1.17×**, so the win has to come from
code generation, and the numbers below are revised accordingly.

**Scope:** the PortMaster / SDL2 aarch64 build (`tools/build.sh handheld`). The
browser keeps the interpreter -- it has ~5× headroom already (§1) and no
writable+executable memory worth using. The Vita, Android and native-measurement
hosts are not targets either; they keep the interpreter.

---

## 0. 摘要（中文）

- **问题**：迷你龙（RK3566 / 4×A55）上，portmaster 版每引擎帧要跑 **2.15 M
  guest 指令**，而引擎自己要的是 **66.8 ms/帧（15 fps）**。解释器在这是
  ~27 MIPS 的量级 → 一帧 ~80 ms，比预算超 20%，于是永远是"欠一帧"的状态
  （画面照出，但 12 fps 左右，输入延迟、音频拉扯）。
- **方案**：把 `libgame.so` 的 Thumb/ARM 机器码**整镜像翻译成 aarch64 机器码**，
  在设备上第一次启动时生成、内存里常驻；解释器一行不改，保留为**参照实现、
  差分测试的 oracle、以及任何翻译不了的块的兜底**。
- **为什么这次可行**：guest 是 ELF32/ARMv5TE、`BIND_NOW`、**无自修改代码**、
  地址空间就是一块固定基址的连续内存、没有线程、96% 是 Thumb-1。
  翻译结果一次生成永远有效——这正是"静态 AOT"成立的前提（§5）。
- **预期收益**：一帧的 CPU 时间 80 ms → **20–30 ms（约 3×）**。为什么不是 5–8×：
  已经实测（§2）——**单纯把指令预解码只值 1.17×**，解释器里取指+解码只占约 15%，
  剩下 85% 是分派、寄存器读写和运算本身。真正的收益必须来自代码生成
  （块内直连 + 寄存器分配），规划值取 **3×**，乐观 4.5×。
  开机 15 s → **4–6 s**（开机的纯计算主要就是 LZMA/inflate/PNG 解码）。
- **顺序**：M0 先在设备上量清楚（**有可能根本不是 CPU**）→ M1 翻译器核心
  （Thumb-1 预解码已完成并实测，见 `tools/aotdiff.sh`）→ M2 flags / 分派 /
  寄存器分配 → M3 覆盖率与缓存 → M4 收尾。
- **工期**：全套 **6–8 周**（见 §12）；能玩到 15 fps 的 M1 大约 4 周；
  不写发射器、只做 M0 调优约 3–5 天，能拿回 1.2–1.4×。
- **不做**：wasm 后端的 AOT、预先翻译好二进制随包发布（版权，且每个人
  的 `libgame.so` 是用户自备的）、QEMU/LLVM/RetDec（§6.2）。

---

## 1. What is already measured, and what is not

Everything in `docs/PERFORMANCE.md` is browser and desktop, and it says the
interpreter is *not* the problem there:

| | desktop Chrome, gameplay |
|---|---|
| guest instructions per engine frame | 2,150,383 |
| interpreter time per engine frame | 12.77 ms |
| interpreter throughput | 168 MIPS |
| engine budget (its own 66 ms + fraction) | 66.8 ms |
| share of wall clock | 19 % |

The handheld number in `README.md` is an **estimate, not a measurement**:

> On an RK3566 handheld the same frame costs ~84 ms at ~27 MIPS.

2.15 M instructions at 27 MIPS is 79.6 ms, i.e. 120 % of the 66.8 ms budget. That
matches the reported behaviour of the port ("runs, but not at the game's own
rate"), and it is the whole reason for this plan. But nobody has put a counter
on the device, so the first milestone is to confirm it (M0). Three outcomes, and
the plan changes shape for each:

| M0 says | then |
|---|---|
| CPU ≫ budget, GL/present small | proceed with AOT (this document) |
| CPU and GL/present both large | fix the host side first (§10.6); AOT alone will not reach 15 fps |
| CPU within budget, but stutter | the frame pacing in `spike_tick()` (`src/main.c`) is the suspect, not the CPU |

---

## 2. Measured: how much of the interpreter's cost is decoding?

This is the first question the plan turns on, and it is now answered by
measurement rather than by the estimate that was here before.

**1.17×.** Pre-decoding every guest instruction once -- so execution never
fetches, never extracts a bitfield, never re-decodes a halfword -- buys 17 % on
real Thumb-1 code. Fetch and decode are about **15 % of the interpreter's
per-instruction cost**, not the half that a threaded-code design traditionally
assumed.

### 2.1 Method

The workload is `tools/aot-bench.c`: 54,449,405 guest Thumb-1 instructions in
one run, built as an ELF32/ARMv5TE shared object with
`arm-linux-musleabi-gcc -march=armv5te -mthumb -O2` and run under `build/spike`.
Both runners execute **the same machine code on the same host**, so the ratio is
the runner's, not the compiler's or the CPU's.

| | |
|---|---|
| host | Intel i5-13600KF, 3.5 GHz base, pinned with `taskset -c 4` |
| build | `x86_64-linux-musl-gcc 11.2.1 -O2 -static` for both runners |
| workload | `tools/aot-bench.c`, 54,449,405 guest instructions |
| runner A | `--aot=off`, `src/cpu.c`'s interpreter |
| runner B | `--aot=predec`, `src/aot_predec.c` |

What runner B keeps, deliberately, so the number means something: the guest
register file still lives in `g_cpu` (a load and a store per operand), the
dispatch is still one indirect jump per instruction, and the budget, stop-at and
trace checks are still per instruction. It only removes the fetch and the
decode.

### 2.2 Result

Five runs each, alternating, pinned to one core (MIPS as reported by
`main.c`'s per-run timer):

| runner | runs | median | vs interpreter |
|---|---|---|---|
| interpreter | 352.7 357.2 358.2 361.2 363.5 | **358.2 MIPS** | 1.00× |
| pre-decoded | 414.7 419.6 420.6 422.7 424.7 | **420.6 MIPS** | **1.17×** |

Correctness, which is the other half of the result: with `--trace-insns`, the
two runners produce **byte-identical traces** -- pc, r0-r14 and cpsr after every
retired instruction -- for as far as the comparison is run (2,000,000
instructions by default in `tools/aotdiff.sh`), and the whole run's instruction
count is identical to the instruction (54,449,405). A wrong decode would show up
as the first differing line, with the register and the PC that differ.

The dynamic mix of that run, from the same trace (`tools/aot-mix.mjs`):

| class | share | class | share |
|---|---|---|---|
| ALU register | 33.6 % | shift immediate | 21.6 % |
| compare immediate | 11.1 % | load/store register offset | 9.2 % |
| conditional branch | 6.1 % | load/store SP-relative | 5.3 % |
| multiply | 4.4 % | compare register | 3.3 % |
| mov immediate | 1.6 % | halfword access | 1.5 % |
| ldmia | 1.1 % | everything else | < 1 % |

That is a register-to-register and load/store mix, which is also what the
engine's GRP_* blitter looks like (`docs/PERFORMANCE.md`: 36 % of a frame). It
is **not** call- or flag-heavy, so it is the mix least favourable to the AOT:
with no calls to elide and one conditional branch per six instructions, the
AOT's advantage is its most limited here. The projected emit for it is **3.6–3.8
host instructions per guest instruction** (3.63 on the fully traced sample,
3.82 on a 1.5 M-instruction window early in the run; §10.2).

### 2.3 What it means for the plan

Per guest instruction, the interpreter's time goes roughly like this. Only one
row is measured; the rest are the model the measurement calibrated, and they are
what M2's `--aot=stats` is meant to replace with numbers.

| part | share | removed by |
|---|---|---|
| fetch + decode | **15 % (measured)** | pre-decode -- 1.17× measured, §2.2 |
| dispatch (one indirect jump per instruction) | ~15–25 % | block chaining (an AOT emits one branch per *block*) |
| operands: load/store `g_cpu.r[n]` per operand | ~20–30 % | register allocation (§8.7) |
| flags (`cpsr` read-modify-write) | ~10–20 % | PSTATE mapping (§8.4) |
| the operation itself | remainder | unavoidable |
| budget / stop-at / trace / profiler checks | ~5 % | per-block accounting |

So the plan's original "5–8×, optimistic 15×" (§10.2) was too optimistic: it
assumed the decode was the bulk of the cost, which the measurement refutes. The
revised planning numbers:

| stage | expected gain vs today's interpreter | how derived |
|---|---|---|
| M0: hoisted checks, `-O3 -flto` | 1.1–1.3× | the checks are ~5 % of the frame, plus codegen flags |
| pre-decode only | **1.17×** | measured, §2.2 |
| AOT, memory-resident registers | 1.8–2.5× | decode + dispatch + per-block accounting removed |
| AOT + register allocation (M2) | **2.5–4.5×, planning on 3×** | adds the operand share; emitted ~3.7 host insns/guest insn at IPC 1–2 |
| A55 relative to this host | likely higher than 3× | in-order, no OoO to hide the interpreter's larger instruction count |

**3× is enough.** The engine wants a frame every 66.8 ms and the frame's CPU
cost today is ~80 ms, so anything from ~1.3× upwards stops the game from being
late; 3× leaves ~27 ms of CPU per frame and puts even the worst observed frame
(4.4 M instructions) inside the budget. The plan does not need the optimistic
case to be correct -- it only needs ≥ 2×, and the measurement says the
codegeneration path should reach 3×.

### 2.4 Reproducing it

```sh
# toolchain: musl.cc arm-linux-musleabi (the guest is soft-float ARMv5TE)
arm-linux-musleabi-gcc -march=armv5te -mthumb -O2 -fno-builtin \
    -fno-tree-loop-distribute-patterns -nostdlib -shared \
    -Wl,--hash-style=both -o /tmp/bench.so tools/aot-bench.c

tools/build.sh native            # build/spike, the interpreter
tools/aotdiff.sh /tmp/bench.so bench 900000000 5
#  -> trace differential (2 M instructions) + full-run instruction count
#  -> 5 timed runs of each runner, pinned to one core

# the mix and the projected emit cost of a trace:
./build/spike /tmp/bench.so --entry=0x1000111 --budget=1500000 \
    --no-profile --trace-insns=/tmp/trace.txt >/dev/null
node tools/aot-mix.mjs /tmp/bench.so /tmp/trace.txt
```

For the real image the same gate is `--boot` plus the six scripted taps of
`docs/PERFORMANCE.md`, once the translator covers ARM and Thumb-2 (§12, M1).

---

## 3. Why the interpreter cannot be made fast enough by tuning

### 3.1 The interpreter's per-instruction cost, from `src/cpu.c` `cpu_run()`

Every guest instruction currently pays, in `cpu_run`'s loop body
(`src/cpu.c:471` onward):

1. `g_insn_count >= g_budget` -- a 64-bit global load and compare.
2. `g_trace_insn_on && g_have_prev` -- two globals, and a call when the trace is
   on (`host_trace_insn`).
3. `g_stop_at && pc == g_stop_at` -- a global load and compare.
4. the executable-range check -- two ranges, `text_lo/hi` and the stub page,
   four compares plus two loads.
5. `g_prev_pc`, `g_have_prev`.
6. `g_prof.on && g_prof.cur >= 0` -- two globals and a register-indexed
   `counts[cur]++`.
7. `g_tr_on` and the ring buffer.
8. `g_thumb` test, then `LD16(pc)` -- a `g_mem + pc` add plus a halfword load.
9. `switch (ins >> 12)` into a 16-way jump table, then bitfield extraction.
10. flag updates, each a read-modify-write of `g_cpu.cpsr`, and the arithmetic
    helpers `add_c`/`sub_c` which always write all four flags.
11. each guest memory access is `*(g_mem + addr)` -- a base add per access.
12. `R[15] = pc + 2` plus, on every transfer, `prof_transfer(pc)` (a binary
    search over `nsorted` when leaving the current function).

That is on the order of 30–70 host instructions per guest instruction depending
on the opcode. At 27 MIPS on a 1.8 GHz in-order core it is ~66 host cycles per
guest instruction, which is consistent.

Tuning can remove items 1–8 (~15-25 %), and `-O3 -flto` another few percent.
That is **1.2–1.4×, not 5×**. The decode (9) and the flag/memory work (10–12)
are inherent to interpreting and only disappear if the decode happens once,
ahead of time, and the result is executable.

### 3.2 What AOT deletes outright

- the dispatch (one `br` per basic block instead of a 16-way switch per
  instruction);
- the fetch and decode (done once, at translation time);
- items 1–8 above, replaced by one budget/counter update per block;
- the `g_thumb` test (a block is Thumb or ARM by construction);
- PC bookkeeping as arithmetic (`R[15] = pc + 2`, `add rN, pc` becomes a
  constant, `ldr rN, [pc, #imm]` becomes a known address);
- the "is this PC in the image" check (translation-time, per block);
- for the common `cmp`/`adds`/`subs` + `b<cond>` pair, the *whole* flags
  materialisation, because ARM and AArch64 condition codes and their
  add/subtract flag semantics are identical (§8.4).

---

## 4. The guest, and what it guarantees

These are facts from `src/elf.h`, `src/elf_loader.c`, `src/guest.h`,
`docs/SPIKE.md` and `README.md`. Each one is load-bearing for the design; if one
turns out false, the design changes.

| Property | Source | Why it matters |
|---|---|---|
| ELF32 / ARMv5TE, `armeabi`, soft-float ABI | `src/elf.h`, `elf_loader.c` | VFP is unused (`Tag_ABI_VFP_args` absent); no NEON, no IT blocks. `host_hooks.c` already answers the soft-float libgcc helpers. |
| `BIND_NOW`, `SYMBOLIC`, no `mprotect` import | `src/elf.h` comment | all relocations resolvable up front; the guest never asks for `PROT_EXEC`, so it cannot generate code at runtime. |
| no self-modifying code | same, and 10 months of running | a translation stays valid for the whole process. The only writers of `.text` are our own `patches.c` and `host_hooks.c`, both of which run **before** any guest instruction. |
| one flat 144 MB guest space, fixed layout | `src/guest.h` | guest address → host pointer is `g_mem + a`, and a *32-bit* guest address always fits `[xBase, wIdx, uxtw]` -- one host instruction per access, no bounds check. |
| `.text` ≈ 745 KB, 96 % Thumb-1 | `docs/SPIKE.md` | the whole image is ~370 K instructions; translating all of it is a fraction of a second (§10.3). |
| `.dynsym` has 5,962 FUNC symbols; `.symtab` stripped | `docs/SPIKE.md` | exact entry points for 5,904 of them are enumerable (that is what `tools/difftest.sh --all` already sweeps). Static functions are anonymous and need the `.ARM.exidx` walk, which **does not exist yet** -- `elf_loader.c` reads no unwind tables at all. |
| single-threaded guest | `imports.h` has `pthread_create`; nothing creates a guest thread | `g_cpu` is one global; the port already cannot run two guest threads. AOT inherits this. |
| no interrupts, no MMU, no mode changes | `cpu.c` never emulates an exception | blocks have exactly one way in and out. |

### 4.1 The contracts an AOT runner must honour

`cpu_run()` is not just "run code": four tools and the frame loop depend on its
behaviour. The AOT entry point must produce the same observable results.

| Contract | Where | Requirement |
|---|---|---|
| `g_insn_count` is exact | `cpu.c`, `main.c` run results, oracle | count per block; a block's count is known at translation time. Totals at any block boundary are identical to the interpreter's. |
| `HALT_*` reasons | `src/cpu.h` | `BUDGET`, `STOPPED`, `IMPORT` (oracle mode), `BAD_PC`, `TRAP`, `UNDEFINED`, `HALTED`, `BAD_MEM`. |
| `g_fault_pc`, `g_fault_insn`, `g_fault_width`, `g_fault_r[]` | `cpu.c:trap()` | identical report on an untranslatable encoding. |
| `g_cpu` is the whole machine state | `src/cpu.h` | including `cpsr` and `r[15]` at any host-visible point. |
| `cpu_call_guest()` re-entrancy | `cpu.c:1585` | `qsort`'s comparator runs guest code from *inside* a host import, then restores `g_cpu`, `g_budget`, `g_thumb`, the profiler window and the trace ring. The AOT must be re-enterable, or qsort breaks. |
| `setjmp`/`longjmp` change LR/SP from a shim | `host_libc.c:310-335` | after **every** host call the next PC must be taken from `g_cpu.r[14]`, not from a compile-time constant. A shim may also rewrite r4-r11, SP and r0. |
| profiler | `cpu.c:109-165` | per-function instruction counts, charged per block (exact, since the block's function is fixed at translation time *unless* the block spans a function boundary -- split blocks at symbol boundaries). |
| `--stop-at=ADDR` and `--dump-guest` | `main.c:518-540` | split the block at ADDR so the stop is exact. |
| `GUEST_CHECK` | `src/guest.h` | a checked-access build for the second verification gate (§9.2). |
| trace | `g_trace_insn_on` | block-boundary state is enough for the main gate; per-instruction trace is a debug emitter option (§9.3). |

---

## 5. Decision

**Translate the whole image, eagerly, on the device, at first run; keep the
interpreter as the reference, the oracle and the fallback.**

Concretely:

1. After `elf_load()` + `patches_apply()` + `host_hooks_install()`, and before
   the first guest instruction, walk the image and translate every reachable
   basic block to aarch64 machine code in a code arena.
2. Seed the walk with: `e_entry`, every `DT_INIT_ARRAY` entry, every defined
   FUNC symbol in `.dynsym` (5,962), every `R_ARM_ABS32`/`RELATIVE` value that
   lands inside `.text` (function-pointer tables in `.data`), and the addresses
   `patches.c`/`host_hooks.c` wrote.
3. Follow direct branches and fall-through inside each function; stop at
   terminators. This is why literal pools are never decoded as instructions:
   the sweep is *reachability*, not a linear disassembly.
4. Anything reached through an indirect branch that the static walk missed
   (jump tables, function pointers computed at runtime) is **translated lazily
   on first hit** and patched into the dispatch table. So "AOT" means "no decode
   in the hot path", not "nothing is ever translated at runtime".
5. Anything the translator does not implement (an encoding `cpu.c` traps on, an
   address outside `.text` and the stub page) falls back to `cpu_run()` for that
   block, because the interpreter *is* the specification.

Why eager rather than lazy-only: boot is 414 M guest instructions of LZMA and
inflate, so translation cost is paid back within the first second of boot even
if it takes 300 ms. It also makes the translation set deterministic, which makes
the differential tests meaningful.

Why on-device and not "ship a translated blob": `libgame.so` is the player's own
copy (Com2uS copyright, `game/` is gitignored); a translated blob is a derived
work and must never be distributed. Generating it locally, keyed by a hash of
the player's file, is the only acceptable form.

---

## 6. Alternatives, and why they are not this

### 6.1 Considered, kept as fallbacks

| Option | Verdict |
|---|---|
| **Pre-decoded execution** (decode once into a struct with its operands split out, execute that) | **Built and measured at 1.17×** (§2.2) -- so it is kept for its correctness scaffolding (decoder, stable instruction cache, successor resolution, host-call plumbing, trace differential), not as a performance strategy. The measured number replaced the 1.8–3× guess this row used to carry. |
| **JIT only, never eager** | Same emitter, worse boot, less deterministic. Rejected as the primary mode; kept as the cache-miss path. |
| **Disk cache of the translated blob** | Optional (M4). Retranslating 370 K instructions is estimated at 0.2–0.5 s on A55 (§11.2); if M3 measures it as noticeable, cache it under `data/aot/<hash>.a64`. Adds a version/corruption failure mode, which is why it is not in M1. |

### 6.2 Rejected (do not re-litigate without new measurements)

- **QEMU's TCG, or QEMU-in-wasm** -- already rejected in `docs/REUSE.md`; for an
  aarch32 guest under wasm it only gets TCI, which is slower than this
  interpreter.
- **LLVM / RetDec / McSema / remill as the lifter** -- needs an LLVM toolchain
  *on the device* to compile the lifted IR, or a host-side compile of a binary
  the host does not have. Latency to first frame, binary size and licence all
  lose to a 2,500-line direct emitter for a 19-format instruction set.
- **Decompiling to C and compiling on device** -- no compiler on the device, and
  `docs/HANDOFF.md` already records why Ghidra C is not a path.
- **A wasm32 backend + AOT in the browser** -- the browser is at 19 % of its
  budget; and WebAssembly code cannot be generated without a compile step whose
  cost dwarfs the win. Not a target.
- **Dropping the interpreter entirely** -- the interpreter is the oracle. Every
  AOT milestone is verified against it (§9). It stays linked in every build;
  `--aot=off` must always work.

---

## 7. Target ABI: registers, memory, calls

All host-visible state stays where it is today, so shims, `jni.c`, `gl.c`,
`text.c`, `sound.c`, `timers.c` and `host_libc.c` need **no changes at all**.

### 7.1 Pinned registers

AArch64's x19–x28 are callee-saved under AAPCS64, so a C helper preserves them
for free and translated code never has to spill them around a host call.

| Register | Holds | Notes |
|---|---|---|
| `x27` | `g_mem` (guest address space base) | every guest load/store is `[x27, wAddr, uxtw]` |
| `x28` | `&g_cpu.r[0]` | guest `r0`..`r15` at `[x28, #0]`..`[x28, #60]` |
| `x26` | `&g_aot_host[0]` | helper table: import dispatch, lookup-miss, traps, div, etc. |
| `x25` | code arena base | dispatch table entries are offsets from here |
| `x23` | materialised NZCV nibble (CPSR bits 31..28) | only when flags are live at a block boundary (§8.4) |
| `x22` | `g_insn_count` as a live 64-bit counter (M2) | spilled to the global at a halt or host call; in M1 it is the global, updated per block |
| `x21` | dispatch table base | guest PC → translated block, §7.5 |
| `x19` | `.text` low address | indirect-branch range check, §7.5 |
| `x20` | dispatch table entry count | the other half of that range check |
| `x24` | `g_budget` (M2) | loaded once per run |
| `x0`–`x18`, `x30` | host scratch / link register | as AAPCS64 says |
| `sp` | *host* stack | never the guest SP; guest SP is `g_cpu.r[13]` |

Guest registers live in memory in M1 (load/store per access). M2 adds
within-block register allocation (§8.7); that is the main performance dial, and
it is deliberately *not* on the critical path to correctness.

### 7.2 Guest memory access

```
        ldr  w9, [x28, #12]        ; guest r3
        add  w9, w9, #8            ; + the instruction's immediate
        ldr  w10, [x27, w9, uxtw]  ; the guest load itself
        str  w10, [x28, #8]        ; guest r2
```

`uxtw` zero-extends the 32-bit guest address, which is exactly the flat-memory
model in `src/guest.h`. No bounds check: the release build has none today
(`GUEST_CHECK` is a test build), and the guest is a statically validated image.
The `GUEST_CHECK` variant of the emitter uses the same `guest_ld32()` accessors
as the interpreter, which is what makes the checked build a usable second gate.

### 7.3 Block structure

```
        ; ---- block prologue (only when needed)
        ldr  w23, [x28, #64]        ; g_cpu.cpsr if flags are live-in (see 7.4)
        str  wPc, [x28, #60]        ; g_cpu.r[15] = this block's PC, so a crash
                                    ; report and --trace name the right place
        ; ---- body
        ...
        ; ---- accounting, once per block (M2: x22, a pinned 64-bit counter)
        add  x22, x22, #<block insns>
        cmp  x22, x24               ; x24 = g_budget, loaded once per run
        b.hs budget_exit
        ; ---- exit
        b    <next block>           ; direct chain: one instruction
```

`g_insn_count` is a `uint64_t` global (`src/cpu.c:29`), not part of `g_cpu`, and
`g_budget` is a `static uint64_t` in `cpu.c`. In M1 both are read/written from
memory at each block boundary; in M2 they move to `x22`/`x24` and are spilled
only at a halt or a host call. The offsets in the snippets (`#0`..`#60`, `#64`)
are `offsetof(cpu_state, r[n])` and `offsetof(cpu_state, cpsr)` -- the emitter
computes them once, so a field added to `cpu_state` cannot silently shift the
register file. Storing `g_cpu.r[15]` per block is what keeps the host-side crash
report (`platform_native.c`) and `--trace` honest -- the interpreter updates
`r[15]` per instruction, and a fault report that names the block instead of the
instruction is still useful, but only if the block is current.

### 7.4 Host calls

A guest `bl` to the ARM PLT stub (the path of *every* import, `docs/SPIKE.md`)
ends with an ARM `ldr pc, [ip]!` of a GOT slot holding `stub|1`; the target is
in the stub page, i.e. `GUEST_STUB_BASE + idx*4` (`src/imports.h`). Translated,
that becomes:

```
        ldr  wT, [x28, #64]         ; g_cpu.cpsr -- flush the flag nibble
        str  w23, [x28, #64]        ;   (only if a carrier is live)
        mov  w0, #<idx>
        bl   aot_host_import        ; C: import_dispatch(idx)
        cbz  x0, block_halt         ; x0 = 0 -> driver takes over (halt)
        br   x0                     ; else x0 = host code for g_cpu.r[14]&~1
```

(The `ldr` is the other direction: a shim may have changed `cpsr`; the nibble is
reloaded after the call. Flags are dead after a call unless the image-wide scan
in §8.4 proved otherwise.)

Two things make this robust:

* the *return* goes through `aot_lookup(g_cpu.r[14] & ~1)` inside the helper, so
  `longjmp` and any shim that rewrites LR/SP/registers works unchanged;
* `import_dispatch()` and every shim see `g_cpu` exactly as they do today,
  including `g_cpu.r[0..3]` and `g_cpu.r[14]`, so `host_libc.c`, `jni.c` and
  `gl4es_bridge.c` are untouched.

The same shape serves the `host_hooks.c` BKPT-patched function entries (index in
imm8/imm12): `aot_host_hook(idx)` then resume at LR.

Cost per crossing stays what it is today (a `bl` into a C function), so AOT does
not change the crossing economics -- it only removes the ~30 interpreter
iterations that used to surround the crossing. The crossing histogram
(`imports_report`) remains valid.

### 7.5 Indirect branches and the dispatch table

The guest has three sources of indirect control flow: `bx`/`blx Rm` (calls,
returns, function pointers), Thumb-1 `add pc, rN` / `mov pc, rN` (switch jump
tables), and the ARM PLT's `ldr pc, [ip]!`.

Dispatch table: one **32-bit word per 2 bytes** of `.text`
(`(text_hi - text_lo) / 2` entries, ~380 K entries, ~1.5 MB), holding an offset
from `x25` to the translated block, or 0 for "not translated yet".

```
        ; wPc = target (Thumb bit masked off)
        ; x19 = .text low address, x20 = (text_hi - text_lo)/2, x21 = table
        sub  w9, wPc, w19            ; unsigned: also rejects targets below .text
        cmp  w9, w20
        b.hs indirect_out_of_text
        lsr  w9, w9, #1              ; two bytes per entry
        ldr  w10, [x21, w9, uxtw #2] ; table entry: offset from x25, 0 = miss
        cbz  w10, indirect_miss
        add  x10, x25, w10, uxtw     ; Xd, Xn, Wm, UXTW -- x25 plus a 32-bit
        br   x10                     ; offset, zero-extended into x10
```

Eight instructions, with no helper call on the hit path. Indirect branches are
a few percent of the mix, so what matters here is that a miss is rare, not that
the sequence is short. (`x19`/`x20` could be folded away with a *biased* table
base -- `x21 = table - (text_lo >> 1) * 4` -- but then a target below `.text`
would index outside the allocation, so the range check stays until M2 has
measured that this path is worth micro-optimising.)

`indirect_miss` is a call to `aot_indirect_miss(pc)` in C, which:

1. if `pc` is in the stub page → returns the host trampoline for
   `import_index_of_stub(pc)` (the same `import_dispatch` shape as §7.4);
2. if `pc` is `RETURN_SENTINEL` → returns "halt, `HALT_BAD_PC`", exactly what
   the interpreter's PC range check does;
3. if `pc` is inside `.text` → translates the block lazily, fills the table
   entry, returns it;
4. otherwise → returns "halt, `HALT_BAD_PC`" (or runs one interpreter block in
   verify mode, §9.1).

A table miss must converge to zero on a normal run; the run report prints the
count, and a non-zero steady-state count is a bug in the seed list (§5 step 2), not a
performance question.

---

## 8. Translation: blocks, flags, and the instruction set

### 8.1 Block formation

A block starts at:

* an entry point (the §5 step 2 seeds),
* any branch target (direct),
* the instruction after a `bl`/`blx` (the return point must be a block start,
  because the callee returns to `g_cpu.r[14]`, which the dispatcher resolves),
* `g_stop_at` (`--stop-at`),
* a `.dynsym` symbol boundary (so the profiler's per-function attribution is
  exact),
* an instruction whose successor is not fall-through (terminators).

Block ends at: conditional/unconditional branch, `bx`/`blx` register, `bl`
(direct: chain, but the fall-through is a separate block), any instruction
accessing `r15` as destination, `BKPT`, an unknown encoding, or 64 instructions
(a cap that keeps translation, liveness and debugging tractable; the cap costs
one dispatch per 64 instructions, unmeasurable).

### 8.2 Decoding

The decoder is a *separate implementation*, not a refactor of `cpu.c`. Reasons:

* `cpu.c` is bit-exact against Unicorn and is the acceptance oracle; touching it
  invalidates that evidence (`docs/HANDOFF.md`: "rerun `tools/difftest.sh` after
  any change to `cpu.c`").
* The interpreter's decode is a `switch` in a hot loop; extracting an IR would
  slow it down for the browser builds that must keep working.
* Two independent implementations of the same spec, compared per block over the
  whole image (§9.1), is a stronger check than one shared decoder with no
  independent cross-check.

The price is duplication: every encoding in `src/cpu.c` needs a translator
counterpart. The mitigation is that coverage is *enumerable*: the translator
reports any encoding it does not handle with its raw word (the same "fail loudly
with the encoding" rule `trap()` follows), and `--aot=verify` turns any
divergence into a named block and register.

### 8.3 Instruction mapping (the worklist)

Counts are from `src/cpu.c`'s own coverage list (`docs/SPIKE.md`) and the real
binary's mix. "M1" = required to boot and play; "M2" = required for the oracle
sweep to be clean.

| Thumb-1 format | Instructions | aarch64 emit | Milestone |
|---|---|---|---|
| 1 | LSL/LSR/ASR `imm` | `lslv`/`lsrv`/`asrv` + C-bit computation when flags live | M1 |
| 2 | ADD/SUB `reg`/`imm3` | `add`/`sub` (+`adds`/`subs` when flags live) | M1 |
| 3 | MOV/CMP/ADD/SUB `imm8` | `mov`/`cmp`/`add`/`sub` | M1 |
| 4 | AND/EOR/ORR/BIC/MVN/MUL/TST/NEG/RSB/CMP/CMN/LSL..ROR | `and`/`eor`/`orr`/`bic`/`mvn`/`mul`/`neg`/`subs`/`cmn`; NZ-only flag updates deferred (§8.4) | M1 |
| 5 | hi-reg ADD/CMP/MOV, `BX`, `BLX` | `add`/`subs`/`mov`; BX → dispatch (§7.5); BLX → set LR + dispatch | M1 |
| 6 | LDR `[pc, #imm]` | address known at translation time → `ldr` from a constant offset of `x27` | M1 |
| 7/8 | LDR/STR/LDRSB/LDRSH/LDRH/STRH `[rN, rM]` | `add` + `ldr`/`str`/`ldrsb`/`ldrsh`/`ldrh` | M1 |
| 9/10/11 | LDR/STR/LDRB/STRB/LDRH/STRH `[rN,#imm]`, `[sp,#imm]` | `add` (imm) + access; SP-relative is `[x28,#52]` | M1 |
| 12 | ADD `rd, pc/sp, #imm` | `mov`/`add` immediate -- **this is the GOT-base idiom** (`add rN, pc`), see the `cpu.c` note about the +4 | M1 |
| 13 | ADD `sp, #imm` | `add` + store to `r13` | M1 |
| 14 | PUSH/POP | sequence of `str`/`ldr` with the SP write-back computed at translation time | M1 |
| 15 | LDMIA/STMIA | sequence of accesses; write-back rule (`LDMIA` with base in list) as in `cpu.c` | M1 |
| 16 | B`<cond>` | `b.cond` (identical condition encoding) when flags are in PSTATE; else `tbz`/`tbnz` on the nibble (§8.4) | M1 |
| 17 | SWI | trap (same as `cpu.c`) | M1 |
| 18 | B | `b` | M1 |
| 19 | BL / BLX (long) | `bl`-equivalent: set `r14` then direct chain or dispatch; **H bit, not J2**, decides Thumb/ARM | M1 |
| Thumb-2 | `movw`/`movt`, wide data-processing, wide `ldm/stm`, `push.w/pop.w`, `ldrd/strd` | same as the 32-bit forms above; `movw/movt` → `movz/movk` | M1 (movw/movt are on the boot path) |
| ARM | data processing (imm and reg-shifted), single/block transfer, B/BL/BLX imm, multiply, `BX`, `CLZ`, BKPT | same emitter, plus **conditional execution**: ARM's `cond` field means the instruction may be skipped -- emit `b.<!cond>` around it (the .plt and libgcc stubs are ARM and are on every import path) | M1 |
| ARM | `MRS`/`MSR`, coprocessor/VFP, `SWI`, `SWP` | trap, exactly as `cpu.c` does | M2 (only if reached) |
| — | `NOP`, `PLD/PLI` | nothing | M1 |
| — | BKPT outside the stub page | host hook call (§7.4) | M1 |
| — | BKPT inside the stub page | host import call (§7.4) | M1 |

### 8.4 Flags: the part that decides whether this is 2× or 10×

ARM's 32-bit condition codes and AArch64's are **the same encoding**
(`EQ..LE` = 0..13), and `ADDS/SUBS` set N, Z, C, V identically (C is "not
borrow" for both subtract forms). So:

* `cmp r0, r1` + `bne` → `subs wzr, w0, w1` + `b.ne`. Two host instructions for
  two guest instructions, no flag materialisation at all. This pair is the
  single most common pattern in Thumb-1 code (loop counters, comparisons).
* `adds r0, r0, #1` + `bmi` → `adds w0, w0, #1` + `b.mi`.

Everything else needs a model, because Thumb-1's *partial* flag updates are not
aarch64's: the ALU ops (AND/EOR/ORR/BIC/MVN/MUL) update N and Z and **leave C
and V alone**; the shifts update N, Z, C and leave V alone; `ADC`/`SBC` read C.
`cpu.c` encodes exactly these rules (`set_nz`, `add_c`, `sub_c`, `sh_*`,
`F_N/F_Z/F_C/F_V`), and they are the specification.

Design: a **compile-time `cc_op` state machine** in the translator, with three
runtime carriers:

| State | Meaning | Runtime cost |
|---|---|---|
| `CC_PSTATE` | the last flag-setting instruction had an exact aarch64 mapping and was just emitted; PSTATE holds N,Z,C,V | 0 |
| `CC_NIBBLE` | `x23` holds the CPSR flag nibble (bits 31..28) | one OR/AND sequence when the state is produced |
| `CC_CPSR` | `g_cpu.cpsr` holds the nibble (block entry/exit, host call) | one load/store |

Transitions are decided at translation time, so there is no per-instruction
branch on state:

* exact setters (`adds`, `subs`, `cmn`, `cmp`, `neg`) → `CC_PSTATE`;
* partial setters → emit the result, then either stay `CC_PSTATE` if the
  consumer only needs N/Z (emit `tst wRes, wRes`) or build the nibble in `x23`:
  * C (shifts, when the following instruction needs it): `lsls wzr, wValue,
    #<31-k>` -- this sets PSTATE.C from bit `k` of the shifted value, i.e. the
    bit ARM leaves in C. It also clobbers PSTATE.N/Z, so it must be emitted
    **before** the N/Z bits are placed and must not be used when PSTATE already
    holds something the block still needs;
  * C, V (unchanged): `and wT, w23, #0x30000000` (old bits 29, 28);
  * N: `and wT, wRes, #0x80000000`;
  * Z: `cmp wRes, #0; cset wT2, eq; lsl wT2, wT2, #30`;
  i.e. ~4–8 instructions, but only for the minority of instructions that both
  set flags *and* are not exact.
* a flag consumer (conditional branch, `ADC/SBC`, `MRS`) materialises whichever
  carrier the state needs, at the cheapest point.

Two more rules make this exact without pessimising the hot path:

1. **Blocks that do not read flags do not care what arrives.** The translator
   computes `flags_in` per block by backward dataflow over the *whole image's*
   CFG (does the block read flags before writing them, or does a successor?).
   `flags_in == false` is the overwhelming majority -- a block starts with a
   load or a compare -- so those blocks materialise nothing at all, and a
   predecessor edge into them costs zero. A block with `flags_in == true` is
   entered with `x23` valid and uses the `tbz`/`tbnz` forms above. Edges from a
   *conditional branch inside the same block* keep PSTATE and use `b.cond`; that
   path never reaches a block boundary at all.
2. **Calls and returns**: a function is allowed to clobber flags, and no
   compiler-generated code relies on flags across a call. That is verifiable for
   *this* binary: the translator scans every `BL`/`BLX` and checks the
   fall-through block's `flags_in`; if any call site needs flags preserved, the
   translator marks that edge "flags live" and materialises before the call. If
   the count is non-zero, print the sites (they are few, or zero). This is the
   same "prove it for the one binary in hand" move the rest of the port uses.

`x23` is the canonical cross-block carrier because it avoids the one
instruction the emitter must not rely on: reading PSTATE. `MRS Xt, NZCV`
requires FEAT_FlagM (ARMv8.4), which Cortex-A55 does *not* have. Writing
(`MSR NZCV, Xt`) exists at v8.0 but is deprecated and, more importantly, is not
needed if conditions are tested with `tbz`/`tbnz`:

| ARM condition | from nibble `x23` | instructions |
|---|---|---|
| EQ / NE | Z = bit 30 | `tbnz` / `tbz` |
| MI / PL | N = bit 31 | `tbnz` / `tbz` |
| CS / CC | C = bit 29 | `tbnz` / `tbz` |
| VS / VC | V = bit 28 | `tbnz` / `tbz` |
| HI (C && !Z) | bits 29,30 | 3: `and wT, w23, #0x60000000` + `cmp wT, #0x20000000` + `b.eq` |
| LS (!C \|\| Z) | bits 29,30 | 3: mirror of HI (`b.ne`) |
| GE / LT (N == V) | `eor wT, w23, w23, lsl #3` moves V to bit 31, where it XORs N | 2: then `tbz`/`tbnz` on bit 31 |
| GT / LE (N == V, Z) | as GE, plus a Z test folded into the same `and`/`cmp` | 3–4 |

So a condition test from the nibble is 1–4 instructions and never reads PSTATE.
The PSTATE fast path (`b.cond`, 1 instruction) is used when the producer is in
the same block, which is the common case.

Objective for M2: **≥ 90 % of conditional branches take the `b.cond` path**, and
`flags_in == false` for ≥ 90 % of blocks. Both are printed by `--aot=stats`.

### 8.5 Data that the translator resolves at build time

Because the image is relocated and patched before translation, these become
constants, and each one is an interpreter instruction sequence removed:

| Guest | Becomes |
|---|---|
| `add rN, pc, #imm` (GOT base) | `mov wN, #<absolute guest address>` (or a `movz`/`movk` pair) |
| `ldr rN, [pc, #imm]` | `ldr wN, [x27, wT]` where `wT` is the literal's guest address |
| `b`/`bl` to a known target | `b` / set `r14` + `b` |
| `push {r4-r7, lr}` | `str` sequence with fixed offsets and a `sub sp, sp, #16` |
| Thumb-2 `movw/movt` pair | `movz`/`movk` |

### 8.6 What must *not* be resolved at build time

* Any load whose address depends on a guest value, obviously.
* Literal-pool *contents*: a GOT slot may be rewritten by the loader and by
  shims. Load them at runtime; only the *address* is a constant.
* Anything after a `patches.c`/`host_hooks.c` write: the write happens before
  translation, and the translator reads the patched bytes, so the patch *is*
  part of the input. The cache key (§11.2) includes a hash of the patched
  `.text`, never of the file on disk.

### 8.7 Register allocation (M2, the main performance dial)

M1 keeps every guest register in `g_cpu` (a load/store per operand). That is
already a large win (§10) and, crucially, it makes host calls and nested
`cpu_call_guest()` trivially correct: memory *is* the state.

M2 adds, per block: load the registers the block uses into `x9`-`x17` at entry,
keep them live, and store back at the block exit and before a host call.
`x9`-`x17` are caller-saved, which is the right choice: the block already has to
synchronise `g_cpu` before a call, so a spill is free there, and a shim that
re-enters the guest (`cpu_call_guest`) cannot see stale host registers. The
range-check constants (`x19`/`x20`, §7.5) can be folded away in the same pass if
`--aot=stats` says indirect branches are hot, freeing two more.

Forbidden optimisations:

* Do **not** keep guest state live in host registers *across* a host call: a
  shim may call `cpu_call_guest()` (qsort), which replaces `g_cpu` wholesale.
* Do **not** cache across an indirect branch (the target block's live set is
  unknown at the branch).
* Do **not** cache `r15`/`r13`/`cpsr` in host registers (host callbacks read
  them; `g_cpu.r[15]` must name the current block for crash reports).

Expected M2 gain: 1.3–1.8× on top of M1, by removing 1–2 loads per operand from
the hottest (ALU and address-computation) instructions.

---

## 9. Verification: how a wrong translation is found before the player finds it

The port's existing answer to "is the CPU right" is `tools/difftest.sh` (this
interpreter vs Unicorn, per instruction, over 5,904 function entries). AOT needs
three gates in addition, and they are the reason this plan is credible.

### 9.1 Gate 1: block self-verification against the interpreter

`--aot=verify` runs a translated block, then **from a snapshot taken at the
block entry**, runs the same block under `cpu_run()` and compares
`g_cpu` (68 bytes), `g_insn_count` and the halt reason.

```
        memcpy(&save, &g_cpu, sizeof save)   ; 68 bytes
        save_insns = g_insn_count
        ... translated block ...
        g_cpu = save; g_insn_count = save_insns
        run the same block with cpu_run()
        compare -> on mismatch: report guest PC, instruction, both register files,
                   and permanently mark this block "interpreter only"
```

Properties that make this the right first gate:

* it is *exhaustive*: every executed block, not a sample;
* it is *self-locating*: the report names the guest PC and the first differing
  register, which is exactly what a decoder bug needs;
* it is *safe*: a diverging block is demoted to the interpreter at runtime, so a
  verify build cannot be less correct than the interpreter;
* it costs 2–3×, which is fine for a test build (`tools/build.sh check` already
  ships a half-speed variant).

After a clean full-image run (boot + menu + gameplay + saves), ship with
`--aot=on`; keep `--aot=verify` as the gate for any change to the emitter.

### 9.2 Gate 2: `GUEST_CHECK`

Build the AOT with checked accesses (`guest_ld32()` etc., the same accessors
`-DGUEST_CHECK` uses). Every guest access the translator emits then reports the
guest address and PC, and `HALT_BAD_MEM` must occur at the same instruction as
under the interpreter. This catches the class of bug the flags model cannot: a
wrong base register or a wrong write-back producing a plausible-but-wrong
address.

### 9.3 Gate 3: existing oracle and pixel A/B

* `tools/difftest.sh --all` with `AOT=on`: 5,904 entries, ~4 M instructions,
  interpreter replaced by the AOT runner. This is the check that covers
  untranslated/unexecuted corners (each function entry, from a real snapshot).
* The deterministic frame run: `--boot --frames=6500` with the six scripted taps
  from `docs/PERFORMANCE.md` must report the same **1271 engine frames** and the
  same final `pc=0badf00c`, and `--frames=6500` without taps must still report
  `average over 1271 frame(s): 390385 instructions` -- the AOT may not change
  the instruction count at all.
* Pixel A/B: same frame index, `--shot` with AOT on and off, byte-compared.
* A per-instruction trace emitter (`--aot-trace`) for debugging a single block:
  it emits the same `host_trace_insn` lines as the interpreter, so
  `tools/difftrace.py` works unchanged. Not for shipping (it is 5–10× slower).

### 9.4 Non-negotiable other gates

* `--aot=off` remains bit-identical to today's build: the interpreter path must
  not change (keep `cpu.c` untouched; the only edit to it is the one-line
  "choose the runner" in `cpu_run()`/`cpu_call_guest()`).
* The browser, Android and native builds must still compile: the AOT lives
  behind `#if defined(AOT_AARCH64)`, and `tools/build.sh` gains one target.
* The cache (M4) must be splittable: a corrupt/absent blob falls back to
  translation, then to the interpreter. Never fail to start because of a cache.

---

## 10. Performance model, targets, and how each is measured

### 10.1 Where the device's 66.8 ms goes (to be filled in by M0)

| Component | Today (estimated) | After AOT | How measured |
|---|---|---|---|
| guest CPU (`cpu_run`) | ~80 ms | **8–20 ms** | `run_result.seconds` / `insns` already logged per frame |
| gl4es + driver (GLES2) | unknown | unchanged | `host_sdl_present` timing, add a per-phase timer |
| swap / vsync wait | unknown | unchanged | same |
| input + timers + logging | < 1 ms | unchanged | same |
| audio | not wired | n/a | `docs/PORTMASTER.md` |

### 10.2 Instruction-count model

Per guest instruction, the emitter produces (this is the table the pre-decoder's
decode was checked against; the measured dynamic mix of §2.2 projects to
**~3.7 host instructions per guest instruction**):

| Class | Example | Host instructions |
|---|---|---|
| ALU, 2 regs + dest | `add r0, r1, r2` | 4 (2 loads, op, store) |
| compare + branch | `cmp r0,#0; beq` | 2 |
| load/store, imm offset | `ldr r2,[r3,#8]` | 4 |
| load/store, reg offset | `ldr r2,[r3,r4]` | 5 |
| push/pop, 5 regs | `push {r4-r7,lr}` | 6 |
| direct call | `bl foo` | 3 (set LR, branch) |
| indirect call | `blx r3` | 8+ (dispatch table) |
| host crossing | import call | 4 + the C shim |

At ~3.7 host instructions per guest instruction and IPC between 1 and 2 -- the
range this core actually delivers on dependent load/ALU chains -- the AOT should
be **2.5–4.5×** today's interpreter, i.e. 3× is the planning number (§2.3).
The earlier version of this table claimed 5–8×; that assumed the interpreter's
decode was most of its cost, and §2 measured it at 15%.

A note on why the *operand* rows dominate: they are exactly what register
allocation (§8.7) removes, which is why M2 is not optional if the 3× has to be
reached.

### 10.3 Boot

```
the decode alone (LZMA + inflate + PNG) = 414 M guest instructions,
37 % of a full run's total (docs/PERFORMANCE.md §5), so boot-to-menu is
roughly 1-2 G instructions.
at 2.5-4.5x of ~27 MIPS: 11-6x, i.e. 1.5-4 s of CPU,
plus asset I/O and the loader's own stages.
```

Today: ~15 s on device (estimated from 27 MIPS), ~17 s in the browser (measured).
Target after AOT: **≤ 6 s to the menu**. If boot must go below that, the
orthogonal (and larger) step is already scoped in `docs/PERFORMANCE.md`: replace
LZMA/inflate/PNG with host libraries through `patches_replace_with_import()` --
the guest's own `File::GetFullPathName` already uses that mechanism. That is a
semantic change where the guest no longer does its own decoding; AOT is the
version that keeps the guest faithful, so it goes first.

### 10.4 Targets (definition of done)

| Metric | Target |
|---|---|
| engine frames per second, gameplay, handheld | **15.0** (the engine's own 66.8 ms), min 14.5 |
| guest CPU time per engine frame on device | **≤ 30 ms** (today ~80; 3× gives ~27) |
| worst observed gameplay frame (4.4 M instructions) | **≤ 55 ms** (today ~163; 3× gives ~54) |
| boot to main menu | **≤ 6 s** (today ~15) |
| table misses in steady state | 0 |
| verify-mode divergences over a full run | 0 |
| `--frames=6500` instruction total | unchanged: 390385 average, 1271 frames |
| translation time at first run | ≤ 1 s (measure; if > 1 s, add the disk cache) |

### 10.5 How to measure on the device

`SPIKE_DEBUG=1` in `env.txt` already prints `perf: ... ms/frame` in
`host_sdl_present` and the per-run `instrs / ms / MIPS` from `main.c`. M0 adds:

* a per-phase breakdown (tick / cpu / present / swap / audio) in the same block;
* a translation-time line at startup (`aot: 131072 blocks, 4.2 MB, 0.31 s`);
* `--aot=stats` printing block count, mean block length, `b.cond` share,
  `flags_in` share, table misses, fallback-block count, code size.

The deterministic native build stays the reference for correctness; **the device
is the only place a performance number means anything.**

### 10.6 If M0 shows GL is a large part of the frame

Then AOT alone will not fix it, and the order changes: profile the draw path
(`gl4es_bridge.c` is ~1,100 lines of per-call adapters; `gl.c` tracks state),
look for per-frame texture uploads and redundant state changes, and consider
`-O3 -flto` for gl4es too. Record the numbers here before starting M1 so the
decision is not lost.

---

## 11. Where the code goes

### 11.1 Files

Already in the tree (M1 step 0 -- Thumb-1 only, measurement scope):

```
src/aot.h                the mode switch and the aot_predec_run() contract
src/aot_predec.c         decoder (all 19 Thumb-1 formats), instruction cache with
                         stable addresses, successor resolution, host-call
                         plumbing, --aot=predec runner, stats
tools/aot-bench.c        54 M-instruction Thumb-1 workload (§2.1)
tools/aot-sym.mjs        symbol -> guest entry address (veneer-aware)
tools/aot-mix.mjs        dynamic instruction mix + projected emit cost
tools/aotdiff.sh         the gate: identical per-instruction traces + instruction
                         count, then the timing A/B
src/cpu.c                one hook in cpu_run(): hand the run to aot_predec_run()
src/main.c               --aot=off|predec, the AOT environment variable, stats
```

To be written (M1 -- the emitter, then M2):

```
src/aot_decode.c         the same decoder extended to ARM mode and the Thumb-2
                         encodings the image uses (or aot_predec.c renamed, if
                         the split is not worth it)
src/aot_emit_arm64.c     the encoder (movz/movk, ldr/str forms, b.cond, tbz/tbnz)
src/aot_translate.c      block finding, CFG, flags liveness, code arena, dispatch table
src/aot_run.c            entry/exit, budget, host-call trampolines, interpreter fallback
src/aot_cache.c          (M4) disk blob: header, hash, mmap RX, versioning
tools/build.sh           new target: `aot` (aarch64 CLI, no window) and
                         `handheld` gains -DAOT_AARCH64
```

Integration points, deliberately tiny:

* `src/cpu.c`: one `if` at the top of `cpu_run()`, after the state setup, so
  there is one place that decides what a run looks like and `cpu_call_guest()`
  inherits the AOT for free. The file is otherwise untouched (it is the oracle).
* `src/main.c`: `--aot=off|on|verify|predec`, the `AOT` environment variable, stats.
* `src/platform_sdl.c`: nothing.

### 11.2 Cache format (M4 only, if measured as needed)

```
struct aot_cache_hdr {
    char     magic[8];        /* "AOT64\0\1"                       */
    uint32_t version;         /* translator version, bumped on any change */
    uint32_t flags;           /* hook set, patch set, GUEST_CHECK   */
    uint64_t text_hash;       /* hash of the *relocated, patched* .text */
    uint32_t text_lo, text_hi;
    uint32_t nblocks;
    uint32_t code_size;
    uint32_t table_size;
};                             /* followed by table[], blocks[], code[] */
```

The blob is loaded with `mmap(PROT_READ|PROT_EXEC)`, the code arena is
`mmap(PROT_READ|PROT_WRITE)` → emit → `__builtin___clear_cache` →
`mprotect(PROT_READ|PROT_EXEC)`. If any of that fails (hardened kernel, no
exec permission, hash mismatch), fall back to in-memory translation. `data/`
is the only writable location (`host_app_data_dir()`).

Code must be position independent (blocks referenced by offsets from `x25`,
host helpers through `x26`, guest memory through `x27`) so the blob can be
mapped wherever the kernel likes.

---

## 12. Work plan

Effort is calendar estimate for one experienced engineer, with the verification
gates included (they are not optional).

### M0 -- measure the handheld, and take the cheap wins (2–3 days)

1. Add the per-phase timer and `aot:`-style summary line to the SDL host.
2. Run on the device with `SPIKE_DEBUG=1`; record frame ms, per-frame
   instructions, `instrs/ms/MIPS`, and the GL/present split in §10.1.
3. Cheap, independent wins, measured one at a time:
   * `-O3 -flto` for `handheld` (today `-O2`, `tools/build.sh`);
   * move the per-instruction checks in `cpu_run` out of the inner loop
     (`g_prof.on`, `g_trace_insn_on`, `g_stop_at`, PC-range) -- they are
     loop-invariant per `cpu_run()` call, i.e. hoist into locals and a
     `for (;;)`/`for (; budget left;)` split;
   * verify `g_do_profile` is off (it already is in the SDL host:
     `getenv("SPIKE_DEBUG")`).
   Expected: **1.1–1.3×**, revised down from 1.3–1.6× after the measurement in
   §2 showed the per-instruction bookkeeping is ~5 % and the decode 15 %, not
   half the frame. Not enough on its own, but it buys headroom and it benefits
   the interpreter paths that stay (browser, fallback).
4. **Gate:** if the CPU is already within budget and the stutter is elsewhere,
   stop and fix that instead (§10.6). Otherwise continue.

### M1 -- translator core (3–4 weeks, step 0 already done)

Order matters: each step is runnable, and step 0 is in the tree.

0. **Done:** `src/aot_predec.c` -- the Thumb-1 decoder, a stable instruction
   cache, successor resolution, host-call plumbing, the identical
   `HALT_*`/counter contract, `--aot=predec`, and `tools/aotdiff.sh` as its
   gate. Measured at 1.17×, which is why the emitter is next and not the
   threaded ladder (Appendix B).
1. Extend the decoder to ARM mode and the Thumb-2 encodings the image uses
   (`.plt` is ARM and every import goes through it; `movw/movt` is on the boot
   path). The predec runner's fallback (`PD_UNSUP` -> `HALT_TRAP`) becomes a
   per-block fallback to the interpreter once the translator owns blocks.
2. `aot_emit_arm64` for the arithmetic/load/branch subset; blocks, code arena,
   dispatch table, direct chaining.
3. `aot_run` + the host-call trampolines. **Milestone M1a:** the static
   constructors (`--init-array`) and `JNI_OnLoad` run under the AOT with
   `--aot=verify` clean.
4. Flags state machine (§8.4) completed; the emitter covers everything the
   image executes. **Milestone M1b:** boot to the main menu under the AOT, zero
   verify divergences.
5. **Milestone M1c:** gameplay under the AOT, on the device, 15.0 fps -- the
   first point at which the plan's 3× claim can be checked against hardware.
6. `tools/difftest.sh --all` with `AOT=on` clean; `tools/aotdiff.sh` clean on
   the boot path.

**Gate:** the four deterministic checks in §9.3 pass; no block falls back to the
interpreter except addresses outside `.text`.

### M2 -- the performance pass (1–2 weeks)

1. Flags liveness dataflow + the `b.cond` fast path; `--aot=stats` targets in
   §8.4.
2. Keep `g_insn_count` in a pinned register; batch the profiler charge.
3. Within-block register allocation (§8.7).
4. Inline the hot libgcc helpers as *host-hooks on the translated side*:
   `__aeabi_uidiv`/`idiv` → `udiv`/`sdiv` (+ `msub` for the remainder) instead
   of a call into `host_hooks.c`. `src/host_hooks.h` records the pre-hook
   profile: `__divsi3` 39.6 %, `__divdf3` 6.6 %, `__udivsi3` 5.6 % of the
   steady-state frame -- ~52 %. Those instructions are already gone (the hook
   replaces the entry), so AOT's win here is only the call, not the loop; A55
   has a hardware divider, so the call can become `udiv`/`sdiv` + `msub`.
5. Cheap peepholes: `str`+`ldr` of the same guest register, `add`+`add` folds,
   constant-offset fold into the load.

**Gate:** device frame CPU ≤ 30 ms (3×; ≤ 20 ms would be the good end of the
range), or a documented reason why not.

### M3 -- coverage and completeness (1 week)

1. `.ARM.exidx` walk: recover static function ranges (also improves the
   profiler, which today drops anonymous ranges -- `docs/SPIKE.md`'s "Recover
   function ranges from `.ARM.exidx`" item).
2. Seed the walk from `.ARM.exidx` + relocation targets; drive table misses to 0
   over a full boot-to-play run.
3. Translation-time and code-size report; decide on the disk cache with numbers.
4. Implement the cache only if translation > 1 s or if M0's boot target needs it.

**Gate:** full run with `--aot=stats` showing 0 misses, 0 fallbacks, 0
divergences.

### M4 -- ship, document, protect (1 week)

1. `docs/PERFORMANCE.md` and `docs/PORTMASTER.md` get the device numbers
   (measured, with the command lines); `docs/MINILOONG.md` §0/§6 updated to point
   here -- its "AOT is not needed" conclusion is now wrong.
2. `README.md` device paragraph updated (it currently states the 84 ms estimate).
3. Keep `--aot=off` as the escape hatch in `env.txt`; document it.
4. Regression checklist in this file: the checks in §9.3.

### 12.1 Effort and gain, in one table

One experienced engineer, calendar estimates including the verification gates
(the gates are what make the estimates real -- they are not optional).

| Milestone | Effort | Gain, from today's interpreter | Confirmed by |
|---|---|---|---|
| M1 step 0: pre-decode (done) | ~1 day for the Thumb-1 half | **1.17× measured** | `tools/aotdiff.sh` |
| M0: device measurement + cheap wins | 2–3 days | 1.1–1.3× | device log |
| M1a: emitter + constructors/JNI under AOT | 1.5–2 weeks | -- (no gameplay yet) | verify mode |
| M1b: ARM + Thumb-2 + boot to menu | +1 week | ~1.8–2.5× (memory-resident regs) | verify + pixels |
| M1c: gameplay, device, 15 fps | +3–5 days | **2.5–3×** | frame CPU ms |
| M2: register allocation + flags fast path | 1–2 weeks | **3–4.5×** | frame CPU ms |
| M3: coverage (`.ARM.exidx`), misses to 0, cache decision | 1 week | -- | `--aot=stats` |
| M4: docs, device tuning, escape hatch | 1 week | -- | regression run |

**Total: 6–8 weeks** from here to a shipped PortMaster build, of which the first
playable AOT build (M1c) is about **4 weeks**. Add ~30 % contingency for the
debugging this document cannot predict -- the flag model and the decoder drift
are the two risks most likely to spend it (§13).

What it buys: frame CPU **~80 ms → ~27 ms** (3×; 20 ms if M2 lands well), boot
**~15 s → ~5 s**, and enough headroom that the engine's own 15 fps is met with
margin on the worst frame instead of on the average one. If the answer turns out
to be at the bottom of the range (2.5×), the game still meets 15 fps; if M0 finds
the bottleneck is not the CPU, none of it is needed and the plan stops at M0 for
3 days.


---

## 13. Risks, ranked by "how likely is this to sink the schedule"

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| **Flag-semantics bug** (partial updates, ADC/SBC, shifts) | high | wrong gameplay, hard to localise | §8.4 state machine driven by `cpu.c`'s rules; `--aot=verify` demotes a bad block automatically; block-level report names PC + register |
| **A missed instruction encoding** | medium | a crash at one address, late | coverage is enumerable (`trap()`-style report with the word); the interpreter is the fallback so it is a slowdown, never a hang |
| **Indirect-target coverage** (`add pc`, jump tables) | medium | table miss, silent slowdown | lazy translation on miss + a miss counter that must reach 0; a miss in verify mode is immediate and loud |
| **Host-call/return contract** (LR rewritten by `longjmp`, nested `cpu_call_guest`) | medium | subtle wrong-PC crash | resume always via `aot_lookup(g_cpu.r[14])`; no guest state in host registers across a call (§8.7) |
| **Decoder duplication drift** (translator vs `cpu.c`) | medium | a block that diverges only on an input the tests did not hit | the oracle sweep + exhaustive verify mode; the emitter may only *narrow* what it handles (fall back), never guess |
| **W^X / exec permission on device** | low | AOT cannot start | probe at init, fall back to interpreter, report it in the log |
| **Code size blow-up** (arena too small) | low | allocation failure mid-translation | 24 MB arena (≈6× the image), measured; abort translation and fall back cleanly |
| **Pacing**: with CPU inside budget the tick loop can actually sleep on `timers_next_in()` (`src/main.c`), so the frame rate becomes the engine's 15 fps instead of "whatever the CPU manages" | low | the goal, but it changes the *feel*, including input latency | no change to `APPINFO_nTimer`; `docs/PERFORMANCE.md` §3's rule stands. Check the first 30 frames after boot, where `dt` is clamped (250 ms) and the clock catches up |
| **The device is GL-bound, not CPU-bound** | medium | the plan's lever is the wrong one | M0 gates it (§10.6) |
| **qemu-user's timing is meaningless** (IPC, branch prediction, cache all differ) | certain | a device number cannot be predicted off-device | qemu-aarch64 is used for correctness only; every performance figure in this document comes from the device or from the desktop build and is labelled as such |

Rollback is trivial by design: the interpreter stays, and `--aot=off` is one
env variable.

---

## 14. Open questions for the device (answer before/at M0)

1. What is the interpreter's real MIPS on the RK3566, in gameplay, at the
   engine's own pacing? (`SPIKE_DEBUG=1`: per-frame `instrs / ms / MIPS`.)
2. What does a frame's 66.8 ms consist of: CPU, gl4es, driver, swap, idle?
3. Does the port hit 15.0 engine fps today, and if not, what is it?
4. Boot: seconds to the menu, and instructions in that window (the loader
   stages and the 12 LZMA assets are the bulk).
5. Which firmware/kernel: does it allow `mmap(PROT_READ|WRITE)` +
   `mprotect(PROT_READ|EXEC)` (some hardened kernels refuse)? Does
   `mmap(MAP_EXEC)` of a file in `data/` work?
6. Does the KMSDRM path present only when the engine drew (`g_tick_drew`, the
   flicker fix) and does that leave the display side out of the frame budget?

---

## Appendix A -- what the interpreter spends per instruction, and what replaces it

| `cpu_run` step | per instruction | in AOT |
|---|---|---|
| budget compare | 2 | 1 per block |
| trace check | 2 (+ call) | 0 (debug emitter only) |
| `--stop-at` check | 2 | 0 (block split) |
| PC-range check | ~6 | 0 (translation-time) |
| `g_prev_pc`/`g_have_prev` | 2 | 0 |
| profiler check + `counts[cur]++` | 3 | 1 per block |
| trace ring | 1 branch | 0 |
| `g_thumb` test | 1 | 0 (implicit) |
| fetch + switch + decode | ~15–25 | 0 |
| flags work | ~4–10 | 0–6, mostly 0 (§8.4) |
| guest access base add | 1 per access | 1 per access (unavoidable) |
| `R[15] = pc+2` | 1 | 0 (block accounting instead) |
| `prof_transfer` | on transfers | 0 (fixed per block) |

## Appendix B -- the fallback ladder, measured against

The ladder this document originally proposed was: threaded code first, native
emitter only if needed. The first rung has now been built and measured, and it
settles the question the other way round.

1. **Pre-decoded / threaded execution** -- built as `src/aot_predec.c`, measured
   at **1.17×** (§2.2). The original guess here was 1.8–3×, so the guess is dead:
   removing fetch and decode removes ~15 % of the interpreter's cost, because the
   dominant costs are the per-instruction dispatch, the memory-resident operand
   accesses, and the operation itself.
   What that stage *is* good for is everything except the speed, and those things
   were worth having: the Thumb-1 decoder, the instruction cache with stable
   addresses, successor resolution, the host-call/return plumbing, the
   `HALT_*`/counter contract, and the per-instruction trace differential that
   gates it. That is exactly the front end the emitter needs, so it stays as
   **M1 step 0** -- not as a performance strategy.
2. **Superblock / trace linking on top of (1)** -- one indirect jump per block
   instead of per instruction, and a cached edge per branch. This removes the
   dispatch share (~15–25 %) and no more: 1.3–1.6× in total, still short of what
   the frame needs with margin, and it cannot touch the operand share. Not worth
   two weeks.
3. **The emitter** -- the only rung that removes the dispatch *and* (with §8.7)
   the operand traffic. That is where the 2.5–4.5× lives, and it is what the rest
   of this document is about.

Consequence: **skip to the emitter.** The measurement removed the main argument
for starting with the cheap ladder, which was "you might not need the expensive
one".

Order of decision: **M0's numbers decide whether M1 is justified at all; M2's
numbers decide whether register allocation or the escape hatch ("the interpreter
is only 1.2× over budget, tune it instead") comes next.** No step in this plan
requires a later step to be worth its cost.

---

## 15. Built: the aarch64 translator (M1 in progress)

`src/aot_arm64.c` is the emitter and its runtime, written after the measurement
in §2. This section records what it does, what it has been proven to do, and
where it still falls short -- measured, not hoped.

### 15.1 What it is

* **Encoder** for the aarch64 subset the translation needs. Every encoding is
  checked against the assembler: 41 instruction forms emitted by
  `aot_arm64.c` are compared word-for-word with `as` output, and all 41 match.
  Three of them were wrong on the first attempt (`ldr` register-offset's fixed
  `10` bits, the `rev` opcode field, sign-extending loads' `opc`), which is why
  that check exists.
* **Decoder + emitter** for Thumb-1 (all 19 formats), ARM mode (data
  processing, single/halfword/doubleword transfers, ldm/stm, mul, clz, b/bl,
  bx/blx, conditional execution) and the Thumb-2 encodings the image uses
  (`movw`/`movt`, wide data processing). Anything else becomes a
  **fallback block**: the instruction is interpreted, one at a time, and the
  count is reported -- so coverage is a number, not a guess.
* **Blocks**, not instructions: a conditional branch is emitted as a real
  `b.cond` out of the block and the fall-through continues in the same block
  (a superblock), which is what keeps flags in PSTATE across
  `cmp; bgt L1; blt L2`. Flags that a block cannot produce make the block
  untranslatable and it is interpreted instead -- precise, and rare.
* **Lazy, whole-image** translation: the dispatch table holds one entry per two
  bytes of `.text`; a block is translated on first arrival and cached, so a
  jump-table target or a function pointer needs no seed list.
* **Host crossings** go through `aot_host_call()`, which runs the same
  `import_dispatch()`/`host_hook_dispatch()` the interpreter does and resumes at
  `g_cpu.r[14]` -- so `longjmp`, `qsort`'s re-entrant comparator and every shim
  keep working unchanged.
* **Accounting**: one `add x25, x25, #n` per instruction (a 32-bit Thumb
  encoding retires two, an ARM one retires one, exactly as `cpu.c` counts them)
  and a budget check at block exits, with the counter written back to
  `g_insn_count` at every halt and host call.

### 15.2 What is proven

| Check | Result |
|---|---|
| Encoder vs `as` | 41/41 instruction forms identical |
| Benchmark instruction count, AOT vs interpreter | **54,449,405 = 54,449,405**, byte-identical |
| Benchmark per-instruction comparison (`AOT_CHECK=1`) | **52,240,357 instructions compared**, zero divergences |
| Real image: the four static constructors | **byte-identical 96 MB guest dump** (all registers + memory) |
| Real image: `nativeInit` | **not yet** -- diverges 260 instructions in |

### 15.3 Speed

`tools/aot-bench.c` (54 M instructions), under `qemu-aarch64` on the same host,
so both runners are emulated the same way:

| runner | MIPS | vs interpreter |
|---|---|---|
| interpreter (aarch64, under qemu) | 40.8 | 1.0× |
| **AOT (aarch64, under qemu)** | **760** | **18.6×** |
| interpreter (x86-64, native, for scale) | 360 | -- |

**18.6× is not the number to plan on.** qemu inflates it: it translates the
interpreter's own switch loop into host code, so the interpreter pays an
emulator tax the generated code does not. The plan's honest planning number
stays **3×** (§2.3): the AOT emits ~3.7 host instructions per guest instruction
against the interpreter's ~10-15, and the rest is IPC. What the 18.6× does say
is that the generated code is *shaped* right -- straight-line, no per-instruction
dispatch -- and that nothing in the emitter is accidentally quadratic. The
device number is the only one that counts, and it needs M1 finished.

### 15.4 What is left (in order)

1. **`nativeInit` diverges.** 260 instructions in, the guest calls
   `NewGlobalRef` with a null object and then jumps to 0. The constructors (which
   are pure computation) are exact, so this is inside the JNI/ARM path:
   `__divsi3` and friends are being interpreted (they take the "flags live
   across a call" fallback), and the failure is downstream of a
   JNI call returning 0 where the interpreter gets a real handle. Next step is
   the same per-instruction checker, with the shadow re-synced across host
   calls (already implemented) to find the first instruction that differs
   rather than the first *symptom*.
2. **Thumb-2 wide `ldm`/`stm`, `push.w`/`pop.w`, `ldrd`/`strd`** are not
   emitted yet; they fall back and are interpreted. The fallback is correct but
   costs the whole instruction's speed, so coverage matters for the 3×.
3. **The gameplay measurement** (`--boot --frames=6500` with the scripted taps)
   once the boot is clean, then the device.

### 15.5 The bugs that cost the most, for the next session

Each of these was found by a measurement, and each would have looked like
"the emulator is wrong somewhere":

* **Guest register offsets are scaled by the access size** (`ldr w, [x28, #g]`
  means byte offset `g*4`). Writing `#g*4` reads four registers too far.
* **`ldr`/`str` register-offset has a fixed `10` in bits 11..10**, and
  sign-extending loads use `opc = 11` in that form but `opc = 10` in the
  immediate form.
* **A conditional ARM branch must not end the block** -- the fall-through
  continues, exactly like Thumb's `bcond`, or the next instruction is whatever
  happens to follow in the arena.
* **`add rX, pc, #imm` is how the ARM PLT finds the GOT**; PC as a source operand
  must be `instruction + 8`, a translation-time constant, and `bits 27..25 = 001`
  (immediate data processing) is a different class from `000`.
* **An exit target held in a scratch register does not survive the checker's
  call** (`w0`-`w18` are caller-saved): park it in `w23`.
* **`R[15]` is not the next PC in ARM mode** -- it is `instruction + 8` so that
  PC-relative operands read correctly. Anything that needs where a run stopped
  must ask `cpu_last_pc()`.
* **Count a 32-bit Thumb encoding as two instructions and an ARM one as one.**
  `len >> 1` is neither.
* **`getenv()` on a hot path costs more than the translation saves** (it walked
  `environ` once per block: 760 MIPS → 61 MIPS until it was removed).

### 15.6 Status after the ARM/Thumb-2 pass (2026-11-02, second session)

The ARM-mode and Thumb-2 support went in, and with it a long tail of semantic
bugs that only the real image exposes. Each was found by an exact comparison,
and each is now fixed:

| Bug | Symptom it produced |
|---|---|
| guest register offsets are scaled by the access size | four registers read/written for every one |
| `add rX, pc, #imm` (bits 27..25 = 001) not decoded | the ARM PLT never resolved: every import call jumped to code |
| ARM PC-as-source is instruction+8 | the PLT's GOT base was 8 bytes off |
| bit 5 of BX/BLX register form masked out | `blx r6` decoded as `teq r15, r6`: no call, LR stale, flags clobbered |
| ARM `stmdb`/`ldmdb` writeback computed from the running address | `push {r4-r9}` left sp 24 bytes high; the stack drifted and a later `malloc` got a garbage size |
| ARM `ldmia`/`stmia` started at base+4 | every ARM pop read four bytes too high |
| post-indexed loads/stores did not write back | `str rX, [sp], #4` wrote the same slot forever |
| ARM register-shifted operand's amount read from the immediate field | `lsl r1, r1, r0` shifted by the wrong amount |
| conditional ARM instruction's guard used the *checker's* clobbered PSTATE | `subcs`/`orrcs` in `__divsi3` branched on garbage |
| logical-with-S never computed N/Z | `movs`/`lsrs` left stale flags; loops ran forever |
| `clz` had `sf=1` and used the guest register index as a host register | `clz` returned nonsense, so `__divsi3` was wrong |
| `--stop-at` not implemented in the AOT | no exact state snapshot to compare against |

Where that leaves it, measured:

* **Benchmark** (`tools/aot-bench.c`, 54 M instructions): identical instruction
  count (54,449,405), 52 M per-instruction comparisons clean, **676–760 MIPS
  under qemu vs 40.8–42.4 for the interpreter (16–18.6×)**.
* **Real image, static constructors**: 96 MB guest dump (all registers + all
  memory) **byte-identical**.
* **Real image, boot**: identical through the constructors, `JNI_OnLoad`,
  `nativePreInit` and the first ~320 instructions of `nativeInit` (verified with
  exact `--stop-at` snapshots at a dozen addresses up to 0x10dc550), then it
  diverges inside `getCurrentTime`'s loop (0xddc78–0xdddc08): the AOT's
  `__divsi3` result is 848 where the interpreter's is 929, so the guest reads a
  different clock value and its `operator new` then asks for a zero-sized block
  and bails out.

So the emitter is right for everything the constructors exercise, and the
remaining work is `__divsi3`'s loop (six conditional `subcs`/`orrcs` steps with
shifted operands) — one function, the same shape as the bugs in the table.

Two verification notes for whoever picks this up:

1. **`AOT_CHECK` cannot see memory bugs.** Its "shadow" runs on the *same*
   `g_mem`, so every store happens twice and a `push`/`pop` pair drifts the
   shadow's stack. It is exact for register-only code (which is how the
   benchmark was verified) and misleading for stack-touching code.
2. **`--stop-at` + `--dump-guest` is the exact oracle**: both runners stop *at*
   the same instruction, so the 96 MB state can be diffed. That is what localised
   the last three bugs, and it now works in AOT mode too (implemented in
   `aot_stop_check`).

### 15.7 Third pass: `--args=`, single-function A/B, and what it found

The decisive tool this round was `--args=<r0,r1,r2,r3>` plus the `result:` line:
an ARM helper can be called *by address* in both runners and the results diffed
in one second, instead of hunting a divergence inside a 18 M-instruction boot.

```
./build/spike-aarch64 game/libgame.so --entry=10d1cd0 --args=64,5d \
    --budget=200000 --aot=arm64
  result: r0=00000001 r1=00000007 r2=0000005d r3=0000005d
```

With it, `__divsi3`, `__aeabi_idivmod`, `cmp64` and `__aeabi_ddiv` became
individually testable, and each wrong answer pointed at one instruction. Bugs
found and fixed this pass:

| Bug | Symptom |
|---|---|
| ARM `MUL`/`MLA` took Rd from bits 15..12 (it is 19..16) | `mul r3, r2, r0` wrote **r0**; every division's remainder was garbage |
| ARM `UMULL`/`UMLAL`/`SMULL`/`SMLAL` were not decoded at all | 64-bit multiplies fell into the data-processing class; `__aeabi_ddiv` returned nonsense |
| `ADC`/`SBC` were emulated with `cset` + two subtracts | the first subtract's borrow was lost, so `0 - 1` with C=1 reported C=1 and **every 64-bit compare branched the wrong way** -- the single most damaging bug of the three passes |
| a conditional ARM *terminator* (`pople`, `bxeq`) had no fall-through exit | the skip landed one past the emitted block: the CPU ran zeroes and trapped as SIGILL |

AArch64 **does** have `ADCS`/`SBCS`, which is the correct one-instruction mapping;
`docs/REUSE.md`'s "no adc on aarch64" note was wrong and cost a session.

New debug aids, all env-gated and off by default:

| Variable | What it does |
|---|---|
| `AOT_BLKCHECK=1` | after each block is emitted, decode every branch in it and check the target is inside the arena. This named the SIGILL in one run. |
| `AOT_SEQ=1` | emit a log call per translated instruction, so the generated code's own guest-PC stream can be diffed against `--trace-insns`. |
| `SPIKE_TRACE_MALLOC=1` | log every `malloc(size, result)` from the shim. |
| `SPIKE_TRACE_LIMIT=n` | stop the instruction trace after n lines (a full boot is ~10 GB of trace). |
| `--args=` / `result:` | call one guest function with chosen arguments and print what it returned. |

Also fixed: `aot_enter()` now range-checks every code address it returns, so a
translation bug halts with `BLOCK ADDRESS OUT OF RANGE: guest <pc>` instead of
letting the CPU execute zeroes.

**Where it stands:** the benchmark is still exact (54,449,405 instructions,
52 M per-instruction comparisons clean, **619 MIPS vs 40.3 under qemu = 15.4×**),
the constructors are byte-identical, and the boot now reaches 168,874
instructions into `nativeInit` before diverging. The remaining divergence is one
function: `__aeabi_ddiv` still returns `inf` where it should return `0.667` for
`1.0 / 1.5`, i.e. there is at least one more instruction in the soft-float double
path that is translated wrongly. `cmp64` (the subs/sbcs/cond pattern it is built
from) is now exact, so the fault is in the 64-bit multiply/shift normalisation
around it.

### 15.5 Two more exactness bugs (soft-float division)

`__aeabi_ddiv` disagreed with the interpreter on every divisor whose mantissa is
not a power of two (`1.0/1.5` came back as `0.5`). Two independent bugs, both
found by the `--stop-at` + `--dump-guest` sweep:

1. **`ADC`/`SBC` emitter ignored the S bit.** `enc_adcsbc(..., 1, ...)` hardcoded
   S=1, so an ARM `adc r4, r4, #253` (no S) *clobbered the carry* that the next
   `bcs` was about to read. Now the S bit comes from the instruction, and
   `cc_kind` only becomes `CC_EXACT` when it is actually set. Fixed the divergence
   at `__aeabi_ddiv+0x60`.
2. Same base-constant mistake in `enc_adcsbc`: the `0xd0` prefix was shifted by
   24 instead of 21. The encoder self-test (`enctest`, compared word-for-word
   with `as`) now covers `adcs`/`sbcs`/`adc` too.

Also fixed: `ADC`/`SBC` were the *only* place where a fallback block could start
with a flag-consuming instruction, and AOT code keeps the flags in PSTATE while
the interpreter reads `g_cpu.cpsr`. `emit_flags_to_cpsr()` now recomputes the
nibble from the last exact setter (`g_last_cc`) at any exit whose target begins
by reading flags, so an interpreted block never branches on a stale `cpsr`.

After both fixes, `__aeabi_ddiv` agrees with the interpreter on 40/40 random
double pairs (including `1.0/1.5`, denormals, and negative divisors).
`__divsi3`, `__aeabi_idivmod`, `__aeabi_uidivmod` remain exact.

Remaining: the boot still faults at 168,874 instructions (pc `0x010dc742`,
straight after the first `blx __aeabi_ddiv`), where the caller's callee-saved
registers arrive corrupted (`r3`, `r7`, `r12` hold what look like arena offsets),
so the double-divide result is written through a bad pointer. Next step is the
sweep over `0xd2300..0xd2440` with the fixed build to find the first differing
instruction on the real call path.
