/*
 * vorbis_wasm.c -- the Ogg Vorbis decoder, as its own wasm module.
 *
 * Safari has no Vorbis decoder, so decodeAudioData rejects on all 180 sounds
 * this game ships and the browser port is silent on every iOS device. The fix
 * cannot live in the main module: the build that carried stb_vorbis there
 * (and an AudioContext created during boot) black-screened on launch, and no
 * amount of correctness in the decoder makes that trade worth it.
 *
 * So the decoder is a separate module, fetched only when decodeAudioData has
 * already failed. If it does not load, the game still runs and is silent --
 * which is exactly the state iOS was in before, and far better than a page that
 * does not start.
 *
 * Built by tools/build_vorbis.sh, which uses emscripten's standalone-wasm mode
 * rather than a freestanding link: stb_vorbis needs ldexp/exp/log/pow/sin/cos
 * (the IMDCT window), and borrowing musl's libm is both correct and free. The
 * resulting module imports nothing but WASI stubs, which the loader supplies.
 *
 * The ABI is deliberately trivial -- three calls, no callbacks, and JS never
 * hands in a pointer of its own:
 *
 *   void   *spike_scratch(void)      buffer to write the compressed bytes into
 *   int32_t *spike_out(void)         where the result header is written
 *   int     spike_decode(int len)    1 on success, 0 on a bad stream
 *
 * The header is five int32s: channels, sample rate, frames, pcm pointer, and a
 * reserved word. pcm is float32, channel-major, and lives until the next
 * spike_decode() call -- one sound is decoded at a time, so there is nothing for
 * JS to free or track.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "stb_vorbis.c"

/* Where JS writes the compressed bytes: a buffer this module owns, carved out
 * of its own heap. A fixed *address* would be wrong -- stb_vorbis' working set
 * grows past any small constant, so a hand-picked offset would eventually be
 * handed out by malloc and the decode would read its own scratch. 4 MB is far
 * more than the largest file the game has (a few hundred KB). */
#define SPIKE_IN_CAP (4u * 1024u * 1024u)

static float *g_pcm;
static unsigned char *g_in;
static int32_t g_out[5];

void *spike_scratch(void)
{
    if (!g_in)
        g_in = (unsigned char *)malloc(SPIKE_IN_CAP);
    return g_in;
}

int32_t *spike_out(void)
{
    return g_out;
}

/* Decode what is in the scratch buffer. Leaves the PCM in a heap block that the
 * next call releases; returns 0 on a bad stream. */
int spike_decode(int len)
{
    int channels = 0, rate = 0;
    short *pcm = NULL;

    if (g_pcm) {
        free(g_pcm);
        g_pcm = NULL;
    }
    if (!g_in || len <= 0 || len > (int)SPIKE_IN_CAP)
        return 0;

    int frames = stb_vorbis_decode_memory(g_in, len, &channels, &rate, &pcm);
    if (frames <= 0 || !pcm || channels <= 0)
        return 0;

    size_t n = (size_t)frames * (size_t)channels;
    float *f = (float *)malloc(n * sizeof(float));
    if (!f) {
        free(pcm);
        return 0;
    }
    /* stb_vorbis hands back interleaved int16; AudioBuffer wants planar float,
     * so de-interleave and convert in one pass. /32768 is exact for the whole
     * negative range. */
    for (int c = 0; c < channels; c++) {
        float *dst = f + (size_t)c * (size_t)frames;
        for (int i = 0; i < frames; i++)
            dst[i] = (float)pcm[(size_t)i * (size_t)channels + (size_t)c] / 32768.0f;
    }
    free(pcm);

    g_pcm = f;
    g_out[0] = channels;
    g_out[1] = rate;
    g_out[2] = frames;
    g_out[3] = (int)(uintptr_t)f;
    g_out[4] = 0;
    return 1;
}
