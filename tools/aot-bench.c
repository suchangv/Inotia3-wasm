/*
 * aot-bench.c -- synthetic guest workload for the pre-decode / AOT measurement.
 *
 * Not part of the port. Compiled to ELF32/ARMv5TE Thumb-1 and run under
 * build/spike --entry=<bench>, so the interpreter and any alternative runner
 * execute the *same* machine code on the same host and the only difference is
 * how that code is executed.
 *
 * The mix resembles the engine's hot paths (docs/PERFORMANCE.md: the GRP_*
 * software blitter is ~36% of instructions): 16-bit pixel loops with
 * fixed-point multiply and saturation, 32-bit fills and read-modify-write
 * copies, integer arithmetic chains with loop-carried dependencies, conditional
 * branches, a switch (jump table), pointer chasing, and a software division
 * loop. Every pass of the outer loop touches every shape once, so any window of
 * the instruction trace contains the whole mix -- which is what makes a short
 * traced sample usable as a dynamic instruction histogram.
 *
 * The symbol is deliberately hidden: an exported Thumb function gets an ARM
 * interworking veneer from the linker, and a measurement that starts in ARM
 * mode is not the measurement. As a local symbol it starts where it looks like
 * it starts, and tools/aot-sym.mjs finds it in .symtab.
 *
 * Build (musl.cc arm-linux-musleabi toolchain; no libc, no libgcc, so the whole
 * benchmark is Thumb-1 in .text and the loader sees no imports):
 *
 *   arm-linux-musleabi-gcc -march=armv5te -mthumb -O2 -fno-builtin \
 *       -fno-tree-loop-distribute-patterns -nostdlib -shared \
 *       -Wl,--hash-style=both -o /tmp/bench.so tools/aot-bench.c
 *
 * Measure and verify:
 *
 *   tools/aotdiff.sh /tmp/bench.so bench 900000000
 */

#include <stdint.h>

#ifndef W
#define W 160
#endif
#ifndef H
#define H 120
#endif
#define OUTER_DEFAULT 40

static uint16_t fb[H][W];
static uint16_t src[H][W];
static uint8_t  mask[H][W];
static uint32_t words[1024];
static uint8_t  bytes[4096];
static uint32_t lcg = 12345u;

/* 16-bit RGB565 alpha blend: per pixel two loads, a fixed-point multiply chain,
 * shifts and a 16-bit store -- the GRP_FilterAlpha / GRP_GetPixelFromRGB_565
 * shape. */
static void blit_alpha(int passes)
{
    for (int p = 0; p < passes; p++) {
        for (int y = 0; y < H; y++) {
            uint16_t *d = fb[y];
            const uint16_t *s = src[y];
            const uint8_t *m = mask[y];
            for (int x = 0; x < W; x++) {
                uint32_t a = m[x];
                uint32_t dv = d[x], sv = s[x];
                uint32_t dr = (dv >> 11) & 0x1f, dg = (dv >> 5) & 0x3f, db = dv & 0x1f;
                uint32_t sr = (sv >> 11) & 0x1f, sg = (sv >> 5) & 0x3f, sb = sv & 0x1f;
                uint32_t r = (dr * (32 - (a >> 3)) + sr * (a >> 3)) >> 5;
                uint32_t g = (dg * (64 - (a >> 2)) + sg * (a >> 2)) >> 6;
                uint32_t b = (db * (32 - (a >> 3)) + sb * (a >> 3)) >> 5;
                d[x] = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
    }
}

/* 32-bit fill plus a byte-wise read-modify-write copy: GRP_FillRect's shape. */
static void fill_and_copy(int passes)
{
    for (int p = 0; p < passes; p++) {
        for (int i = 0; i < 1024; i++)
            words[i] = (uint32_t)(i * 2654435761u + lcg);
        for (int i = 0; i < 4096; i++)
            bytes[i] = (uint8_t)(bytes[(i + 7) & 4095] ^ (uint8_t)(i & 0xff));
    }
}

/* Fixed-point chain with a loop-carried dependency, compares and short
 * branches: the per-frame game-logic shape. */
static int game_math(int passes)
{
    int32_t pos = 0, vel = 3, acc = 0;
    for (int p = 0; p < passes * 64; p++) {
        vel += ((acc & 7) - 3);
        if (vel > 97) vel = 97;
        if (vel < -97) vel = -97;
        pos += vel;
        if (pos > 12000) { pos = 12000; vel = -vel; }
        if (pos < -12000) { pos = -12000; vel = -vel; }
        acc += (pos >> 3) * 3 - (acc >> 4);
        lcg = lcg * 1103515245u + 12345u;
    }
    return pos + acc;
}

/* A dense switch: GCC emits a jump table, i.e. the Thumb-1 `add pc, rN`
 * indirect branch the AOT dispatch table has to cover. */
static int state_machine(int passes)
{
    int state = 0, out = 0;
    for (int i = 0; i < passes * 256; i++) {
        switch (state) {
        case 0: state = (i & 1) ? 1 : 2; break;
        case 1: state = 3; out += i; break;
        case 2: state = 4; out ^= i; break;
        case 3: state = (i & 3) ? 5 : 0; break;
        case 4: state = 5; out += 1; break;
        case 5: state = (i & 7) ? 6 : 1; break;
        case 6: state = 7; out -= 2; break;
        case 7: state = 0; out += 3; break;
        default: state = 0; break;
        }
    }
    return out;
}

/* Pointer chasing through a table, i.e. loads whose address comes from the
 * previous load -- the dependency chain AOT's register allocation is for. */
static uint32_t chase(int passes)
{
    static uint32_t next[256];
    for (int i = 0; i < 256; i++)
        next[i] = (uint32_t)((i * 37 + 11) & 255);
    uint32_t idx = 0, sum = 0;
    for (int p = 0; p < passes * 512; p++) {
        uint32_t slot = next[idx & 255] & 255;
        sum += next[slot] + slot;
        idx = slot;
    }
    return sum;
}

/* Software division loop, the shape of libgcc's Thumb __udivsi3 (shift,
 * compare, subtract, 32 iterations). Written out rather than using `/` so the
 * whole benchmark stays Thumb-1 in .text, with no ARM-mode libgcc veneer in the
 * middle of the measurement. */
static uint32_t udiv_soft(uint32_t n, uint32_t d)
{
    if (!d)
        return 0;
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    return q;
}

static int divides(int passes)
{
    int32_t acc = 0;
    for (int i = 1; i <= passes * 8; i++) {
        acc += (int32_t)udiv_soft(1000000u, (uint32_t)(i | 1));
        acc -= (int32_t)udiv_soft((uint32_t)i, (uint32_t)(i + 3));
    }
    return acc;
}

/* Not exported: an exported Thumb function gets an ARM interworking veneer
 * from the linker, and the measurement then starts in ARM mode. As a local
 * symbol it starts where it looks like it starts, and tools/aot-sym.mjs finds
 * it in .symtab. */
__attribute__((visibility("hidden"))) int bench(void)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            src[y][x] = (uint16_t)((x * 31 + y * 17) & 0xffff);
            mask[y][x] = (uint8_t)((x * 7 + y * 3) & 0xff);
            fb[y][x]  = (uint16_t)((x * 13) ^ (y * 29));
        }

    int r = 0;
#ifndef OUTER
#define OUTER OUTER_DEFAULT
#endif
    for (int n = 0; n < OUTER; n++) {
        blit_alpha(1);
        fill_and_copy(1);
        r += game_math(1);
        r += state_machine(1);
        r += (int)chase(1);
        r += divides(1);
    }

    uint32_t sum = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            sum = sum * 31 + fb[y][x] + src[y][x] + mask[y][x];
    for (int i = 0; i < 1024; i++)
        sum = sum * 17 + words[i];
    return (int)(sum + (uint32_t)r);
}
