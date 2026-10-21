/*
 * sound_vorbis.c -- Ogg Vorbis decoded inside the module.
 *
 * The browser backend (tools/sound.js) normally hands each sound to the
 * browser's own decoder through decodeAudioData, which is better than anything
 * here: it runs off the main thread and on whatever the platform uses for
 * audio. It does not, however, exist for Vorbis on Safari -- iOS has no Vorbis
 * decoder at all, so decodeAudioData rejects on every file this game ships and
 * the game is silent there. That is the only reason this file exists.
 *
 * The JS side tries decodeAudioData first and falls back to this on rejection,
 * so Chrome and Firefox never pay for it.
 *
 * stb_vorbis is already vendored for the Android and SDL hosts
 * (src/sound_android.c); this is the same library behind a boundary small
 * enough for JavaScript: bytes in, one heap buffer of float PCM out, and one
 * free call so allocation stays on one side.
 */
#include <stdlib.h>
#include <string.h>

#include "../third_party/stb/stb_vorbis.c"

/* Decode `len` bytes of Ogg Vorbis.
 *
 * Writes through the four out-pointers:
 *   out_channels  interleaved channel count
 *   out_rate      sample rate as stored in the stream
 *   out_frames    frames (samples per channel)
 *   out_pcm       malloc'd float32, channel-major: channel c starts at
 *                 out_pcm + c * out_frames, which is the layout
 *                 AudioBuffer.copyToChannel wants
 *
 * Returns 1 on success, 0 on failure (bad stream, or out of memory). The caller
 * owns *out_pcm and must pass it to spike_vorbis_free.
 */
int spike_vorbis_decode(const unsigned char *data, int len,
                        int *out_channels, int *out_rate, int *out_frames,
                        float **out_pcm)
{
    int channels = 0, rate = 0;
    short *pcm = NULL;

    if (!data || len <= 0 || !out_channels || !out_rate || !out_frames || !out_pcm)
        return 0;

    int frames = stb_vorbis_decode_memory((const uint8_t *)data, len,
                                          &channels, &rate, &pcm);
    if (frames < 0 || !pcm || channels <= 0 || frames == 0) {
        if (pcm) free(pcm);
        return 0;
    }

    size_t n = (size_t)frames * (size_t)channels;
    float *f = (float *)malloc(n * sizeof(float));
    if (!f) {
        free(pcm);
        return 0;
    }
    /* stb_vorbis hands back interleaved samples; AudioBuffer wants planar, so
     * de-interleave while converting. 32768 (not 32767) is exact for the full
     * negative range, the same convention browsers use. */
    for (int c = 0; c < channels; c++) {
        float *dst = f + (size_t)c * (size_t)frames;
        for (int i = 0; i < frames; i++)
            dst[i] = (float)pcm[(size_t)i * (size_t)channels + (size_t)c] / 32768.0f;
    }
    free(pcm);

    *out_channels = channels;
    *out_rate     = rate;
    *out_frames   = frames;
    *out_pcm      = f;
    return 1;
}

void spike_vorbis_free(void *pcm)
{
    free(pcm);
}
