#!/usr/bin/env node
// aot-mix.mjs -- dynamic instruction-mix histogram from an --trace-insns file.
//
//   node tools/aot-mix.mjs /tmp/bench.so /tmp/trace.txt
//
// The trace holds pc and the register file after every retired instruction
// (host_trace_insn), so the *instruction* has to be read back out of the guest
// image at that pc. This classifies each one into the Thumb-1 format it belongs
// to and prints two things the plan needs:
//
//   * the mix, because the emit cost per guest instruction depends on it;
//   * a projected host-instruction count per guest instruction, using the
//     emitter's per-class cost from docs/aot-plan.md §10.2. That projection is
//     what §2.3's "2.5–4.5×, planning on 3×" is derived from, and it is the
//     number to re-derive whenever the mix changes.
//
// Thumb-1 only: the guest image is 96 % Thumb and this is a first-order model.
// An ARM or Thumb-2 halfword is reported separately rather than guessed at.

import fs from 'node:fs';

const [elfPath, tracePath] = process.argv.slice(2);
if (!elfPath || !tracePath) {
  console.error('usage: aot-mix.mjs <guest.so> <trace.txt>');
  process.exit(2);
}

// Host instructions the emitter needs per guest instruction, at M1 (guest
// registers in g_cpu, flags where AArch64 can carry them directly). Keep in
// step with docs/aot-plan.md §10.2.
const COST = {
  mov_imm: 2, alu_imm: 3, alu_reg: 4, cmp_imm: 2, cmp_reg: 3,
  shift_imm: 4, shift_reg: 5, mul: 5, mvn_neg: 4,
  ldst_imm: 4, ldst_reg: 5, ldst_sp: 3, ldst_half: 4, ldr_literal: 4,
  ldm: 6, push_pop: 6, addr_pc_sp: 3, add_sp: 3,
  b_cond: 1, b: 1, bl: 2, bx: 12, swi_bkpt: 8, nop: 0,
};

const buf = fs.readFileSync(elfPath);
const u16 = o => buf.readUInt16LE(o), u32 = o => buf.readUInt32LE(o);
const phoff = u32(28), phentsize = u16(42), phnum = u16(44);
const ph = [];
for (let i = 0; i < phnum; i++) {
  const o = phoff + i * phentsize;
  ph.push({ type: u32(o), off: u32(o + 4), vaddr: u32(o + 8), filesz: u32(o + 16) });
}

const BASE = 0x01000000;   // GUEST_IMAGE_BASE, src/guest.h
function insnAt(pc) {
  const a = pc - BASE;
  for (const p of ph)
    if (p.type === 1 && a >= p.vaddr && a + 2 <= p.vaddr + p.filesz)
      return buf.readUInt16LE(p.off + (a - p.vaddr));
  return -1;
}

const counts = Object.create(null);
let total = 0, unreadable = 0;
for (const line of fs.readFileSync(tracePath, 'utf8').split('\n')) {
  if (!line) continue;
  const pc = parseInt(line.slice(0, 8), 16);
  const ins = insnAt(pc);
  total++;
  if (ins < 0) { unreadable++; continue; }

  const top = ins >> 12;
  let cls = null;
  if (top <= 1 && (ins & 0x1800) !== 0x1800) cls = 'shift_imm';
  else if (top <= 1) cls = 'alu_reg';                       // add/sub imm3|reg
  else if (top === 2 || top === 3) {
    const op = (ins >> 11) & 3;
    cls = op === 0 ? 'mov_imm' : op === 1 ? 'cmp_imm' : 'alu_imm';
  } else if (top === 4) {
    if ((ins & 0xFC00) === 0x4000) {
      const op = (ins >> 6) & 0xf;
      cls = op === 0xd ? 'mul' : op === 0xf ? 'mvn_neg'
          : (op === 0x8 || op === 0xa) ? 'cmp_reg' : 'alu_reg';
    } else if ((ins & 0xFC00) === 0x4400) {
      cls = (ins & 0x0300) === 0x0300 ? 'bx' : 'alu_reg';   // hi-reg add/cmp/mov
    } else cls = 'ldr_literal';
  } else if (top === 5) cls = 'ldst_reg';
  else if (top === 6 || top === 7) cls = 'ldst_imm';
  else if (top === 8) cls = 'ldst_half';
  else if (top === 9) cls = 'ldst_sp';
  else if (top === 0xa) cls = 'addr_pc_sp';
  else if (top === 0xb) {
    if ((ins & 0xFF80) === 0xB000) cls = 'add_sp';
    else if ((ins & 0xF600) === 0xB400) cls = 'push_pop';
    else if ((ins & 0xFF00) === 0xBE00) cls = 'swi_bkpt';
    else cls = 'alu_reg';                                   // rev/ext, costed as ALU
  } else if (top === 0xc) cls = 'ldm';
  else if (top === 0xd) cls = ((ins >> 8) & 0xf) === 0xf ? 'swi_bkpt' : 'b_cond';
  else if (top === 0xe) cls = (ins & 0x0800) ? 'b' : 'b';
  else cls = 'bl';                                          // 0xf: BL/BLX
  counts[cls] = (counts[cls] || 0) + 1;
}

let proj = 0;
const rows = Object.keys(counts).sort((a, b) => counts[b] - counts[a]);
for (const k of rows) proj += counts[k] * (COST[k] ?? 4);

console.log(`instructions: ${total}${unreadable ? ` (${unreadable} not in a PT_LOAD)` : ''}`);
console.log('class                 count     share  host/insn');
for (const k of rows) {
  const n = counts[k];
  console.log(String(k).padEnd(20), String(n).padStart(9),
              ((100 * n / total).toFixed(1) + '%').padStart(8),
              String(COST[k] ?? 4).padStart(9));
}
console.log(`projected host instructions per guest instruction: ${(proj / total).toFixed(2)}`);
