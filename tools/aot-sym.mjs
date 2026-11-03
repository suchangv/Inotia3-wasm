#!/usr/bin/env node
// aot-sym.mjs -- guest entry address for a symbol in a guest .so.
//
//   node tools/aot-sym.mjs build/bench.so bench      -> 0x1000111
//
// Why this exists: the interpreter's --entry= takes a *number*, and the guest
// image is loaded at GUEST_IMAGE_BASE (0x01000000), so the entry is the symbol's
// link-time address rebased and with the Thumb bit set.
//
// Two details are not obvious, and both have cost time already:
//
//  * A Thumb function exported in .dynsym gets an ARM interworking veneer
//    (`ldr ip,[pc,#4]; add ip,ip,pc; bx ip; .word target-pc'), because the
//    linker must assume an ARM caller. Jumping to the veneer with the Thumb bit
//    set decodes it as Thumb and the run ends somewhere unrelated. This script
//    recognises the veneer and follows it.
//  * .symtab is tried first: it has the local symbols (.dynsym does not), and
//    for a non-exported function it holds the real address with no veneer.
//
// ELF32 little-endian only, which is what the guest is (docs/SPIKE.md).

import fs from 'node:fs';

const [elfPath, want] = process.argv.slice(2);
if (!elfPath || !want) {
  console.error('usage: aot-sym.mjs <elf> <symbol>');
  process.exit(2);
}

const BASE = 0x01000000;
const buf = fs.readFileSync(elfPath);
const u16 = o => buf.readUInt16LE(o);
const u32 = o => buf.readUInt32LE(o);

const shoff = u32(32), shentsize = u16(46), shnum = u16(48);
const sh = [];
let symtab = -1, dynsym = -1;
for (let i = 0; i < shnum; i++) {
  const o = shoff + i * shentsize;
  sh.push({ type: u32(o + 4), off: u32(o + 16), size: u32(o + 20), link: u32(o + 24) });
  if (u32(o + 4) === 2 && symtab < 0) symtab = i;   // SHT_SYMTAB
  if (u32(o + 4) === 11 && dynsym < 0) dynsym = i;  // SHT_DYNSYM
}
if (symtab < 0 && dynsym < 0) throw new Error('no symbol table');

const phoff = u32(28), phentsize = u16(42), phnum = u16(44);
const ph = [];
for (let i = 0; i < phnum; i++) {
  const o = phoff + i * phentsize;
  ph.push({ type: u32(o), off: u32(o + 4), vaddr: u32(o + 8), filesz: u32(o + 16) });
}
function read(addr, n) {
  for (const p of ph)
    if (p.type === 1 && addr >= p.vaddr && addr + n <= p.vaddr + p.filesz)
      return buf.subarray(p.off + (addr - p.vaddr), p.off + (addr - p.vaddr) + n);
  throw new Error('address ' + addr.toString(16) + ' is not in a PT_LOAD segment');
}
const rd32 = a => read(a, 4).readUInt32LE(0);

function lookup(sectionIndex) {
  if (sectionIndex < 0) return 0;
  const ds = sh[sectionIndex], st = sh[ds.link];
  for (let o = ds.off; o < ds.off + ds.size; o += 16) {
    const nameOff = u32(o);
    const end = buf.indexOf(0, st.off + nameOff);
    const nm = buf.toString('utf8', st.off + nameOff, end);
    if (nm === want) return u32(o + 4);            // st_value
  }
  return 0;
}

let value = lookup(symtab) || lookup(dynsym);
if (!value) throw new Error('symbol not found: ' + want);

let addr = value & ~1;
let thumb = (value & 1) === 1;

// ARM interworking veneer: ldr ip,[pc,#4] / add ip,ip,pc / bx ip / .word rel
if (!thumb && rd32(addr) === 0xe59fc004 && rd32(addr + 4) === 0xe08cc00f &&
    rd32(addr + 8) === 0xe12fff1c) {
  const target = (rd32(addr + 12) + (addr + 12)) >>> 0;
  console.error(`aot-sym: ${want} is an interworking veneer at ${addr.toString(16)}` +
                ` -> ${target.toString(16)}${target & 1 ? ' (thumb)' : ' (arm)'}`);
  addr = target & ~1;
  thumb = (target & 1) === 1;
}

console.log('0x' + ((BASE + addr) | (thumb ? 1 : 0)).toString(16));
