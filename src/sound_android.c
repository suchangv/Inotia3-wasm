/*
 * sound_android.c -- the Java-side sound player, on OpenSL ES.
 *
 * The chain the engine expects is in sound.h: CS_mdaSoundCreate -> jSoundCreate
 * -> WrapperJinterface.SoundCreate, and on Android that Java side is
 * MediaPlayer (for the looping music) and SoundPool (for effects) reading the
 * game's own .ogg files. The .so's own C2S media layer is stubbed out in this
 * build, so this *is* the audio implementation.
 *
 * Two halves:
 *
 *   decode   everything shipped is Ogg Vorbis (stored in the archive as
 *            "NAME.ogg.jpg"), so stb_vorbis turns the bytes into PCM. Decoding
 *            is lazy and cached with an LRU budget: the engine creates 160
 *            handles at boot, and decoding all of them up front would be tens of
 *            MB and seconds of start-up for sounds the player may never hear.
 *
 *   play     OpenSL ES, which is in the NDK and needs no Java. One player per
 *            sounding handle, a buffer queue holding the whole decoded sound.
 *            Whole-buffer playback is right for this game: effects are short
 *            and the music tracks are a few MB decoded, and streaming would add
 *            a feeder thread for no benefit.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "compat.h"
#include "platform.h"

#include "../third_party/stb/stb_vorbis.c"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

/* The engine's handle space (see sound.c). 0 is not a handle. */
#define SND_MAX 512

/* PCM kept decoded, in bytes. The browser build's budget is the same order and
 * for the same reason: enough for the music and the effects that are actually
 * played, small enough that a phone is not asked for a hundred megabytes. */
#define PCM_BUDGET (48u * 1024u * 1024u)

/* Simultaneous OpenSL players. The engine can ask for more; the oldest is
 * stopped, which is what a phone would do anyway. */
#define VOICES 8

typedef struct {
    int used;
    char name[160];
    unsigned char *ogg;          /* a copy: sound.c frees its buffer */
    unsigned len;
    int loop;
    int volume;                  /* 0..100, the engine's scale */

    /* decoded, if cached */
    short *pcm;
    unsigned pcm_bytes;
    int rate, channels;
    unsigned stamp;              /* LRU */

    SLObjectItf player_obj;
    SLPlayItf   play;
    SLBufferQueueItf queue;
    SLVolumeItf volume_itf;
    int playing;
} snd;

static snd g_snd[SND_MAX];

static SLObjectItf g_engine_obj, g_mix_obj;
static SLEngineItf g_engine;
static SLObjectItf g_mix;
static int g_sl_ready;
static int g_sl_failed;
static unsigned g_stamp;

/* ---- OpenSL bring-up ----------------------------------------------------- */

static int sl_init(void)
{
    if (g_sl_ready)
        return 1;
    if (g_sl_failed)
        return 0;

    if (slCreateEngine(&g_engine_obj, 0, NULL, 0, NULL, NULL) != SL_RESULT_SUCCESS ||
        (*g_engine_obj)->Realize(g_engine_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
        (*g_engine_obj)->GetInterface(g_engine_obj, SL_IID_ENGINE, &g_engine) != SL_RESULT_SUCCESS) {
        host_log("[snd] OpenSL: no engine\n");
        g_sl_failed = 1;
        return 0;
    }
    if ((*g_engine)->CreateOutputMix(g_engine, &g_mix_obj, 0, NULL, NULL) != SL_RESULT_SUCCESS ||
        (*g_mix_obj)->Realize(g_mix_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
        (*g_mix_obj)->GetInterface(g_mix_obj, SL_IID_OUTPUTMIX, &g_mix) != SL_RESULT_SUCCESS) {
        host_log("[snd] OpenSL: no output mix\n");
        g_sl_failed = 1;
        return 0;
    }
    g_sl_ready = 1;
    host_log("[snd] OpenSL ES ready\n");
    return 1;
}

/* ---- decode / cache ------------------------------------------------------ */

static void drop_pcm(snd *s)
{
    free(s->pcm);
    s->pcm = NULL;
    s->pcm_bytes = 0;
}

/* Evict least-recently-used decoded sounds until `need` more bytes fit. */
static void evict(unsigned need)
{
    for (;;) {
        unsigned total = 0;
        snd *oldest = NULL;
        for (int i = 0; i < SND_MAX; i++) {
            if (!g_snd[i].used || !g_snd[i].pcm)
                continue;
            total += g_snd[i].pcm_bytes;
            if (!oldest || g_snd[i].stamp < oldest->stamp)
                oldest = &g_snd[i];
        }
        if (total + need <= PCM_BUDGET || !oldest)
            return;
        drop_pcm(oldest);
    }
}

static int decode(snd *s)
{
    if (s->pcm)
        return 1;
    if (!s->ogg || !s->len)
        return 0;
    int channels = 0, rate = 0;
    short *pcm = NULL;
    int frames = stb_vorbis_decode_memory(s->ogg, (int)s->len, &channels,
                                          &rate, &pcm);
    if (frames <= 0 || !pcm) {
        host_log("[snd] cannot decode %s\n", s->name);
        return 0;
    }
    unsigned bytes = (unsigned)frames * (unsigned)channels * 2u;
    evict(bytes);
    s->pcm = pcm;
    s->pcm_bytes = bytes;
    s->rate = rate;
    s->channels = channels;
    s->stamp = ++g_stamp;
    return 1;
}

/* ---- players ------------------------------------------------------------- */

static void player_destroy(snd *s)
{
    if (s->player_obj) {
        (*s->player_obj)->Destroy(s->player_obj);
        s->player_obj = NULL;
        s->play = NULL;
        s->queue = NULL;
        s->volume_itf = NULL;
    }
    s->playing = 0;
}

/* OpenSL has no "loop this buffer" flag: looping is a re-enqueue from the
 * buffer-queue callback, which is the documented pattern and the only one that
 * works for a whole-buffer player. The callback runs on OpenSL's thread, so it
 * only touches the slot's own fields. */
static void queue_cb(SLBufferQueueItf bq, void *ctx)
{
    snd *s = (snd *)ctx;
    if (!s)
        return;
    if (s->loop && s->playing && s->pcm && s->pcm_bytes) {
        (*bq)->Enqueue(bq, s->pcm, (SLuint32)s->pcm_bytes);
        return;
    }
    s->playing = 0;
}

static void stop_oldest_voice(void)
{
    snd *oldest = NULL;
    for (int i = 0; i < SND_MAX; i++) {
        if (g_snd[i].used && g_snd[i].playing) {
            if (!oldest || g_snd[i].stamp < oldest->stamp)
                oldest = &g_snd[i];
        }
    }
    if (oldest) {
        if (oldest->play)
            (*oldest->play)->SetPlayState(oldest->play, SL_PLAYSTATE_STOPPED);
        oldest->playing = 0;
    }
}

static int voices_playing(void)
{
    int n = 0;
    for (int i = 0; i < SND_MAX; i++)
        if (g_snd[i].used && g_snd[i].playing)
            n++;
    return n;
}

static int player_start(snd *s, int loop)
{
    if (!sl_init())
        return 0;
    if (!decode(s))
        return 0;

    player_destroy(s);

    if (voices_playing() >= VOICES)
        stop_oldest_voice();

    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1
    };
    SLDataFormat_PCM fmt = {
        SL_DATAFORMAT_PCM,
        (SLuint32)s->channels,
        (SLuint32)s->rate * 1000u,        /* milliHz */
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        s->channels == 2 ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT)
                         : SL_SPEAKER_FRONT_CENTER,
        SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource src = { &loc_bufq, &fmt };
    SLDataLocator_OutputMix loc_mix = { SL_DATALOCATOR_OUTPUTMIX, g_mix_obj };
    SLDataSink sink = { &loc_mix, NULL };

    const SLInterfaceID ids[2] = { SL_IID_BUFFERQUEUE, SL_IID_VOLUME };
    const SLboolean req[2] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };

    if ((*g_engine)->CreateAudioPlayer(g_engine, &s->player_obj, &src, &sink,
                                       2, ids, req) != SL_RESULT_SUCCESS ||
        (*s->player_obj)->Realize(s->player_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
        host_log("[snd] cannot create a player for %s\n", s->name);
        player_destroy(s);
        return 0;
    }
    (*s->player_obj)->GetInterface(s->player_obj, SL_IID_PLAY, &s->play);
    (*s->player_obj)->GetInterface(s->player_obj, SL_IID_BUFFERQUEUE, &s->queue);
    (*s->player_obj)->GetInterface(s->player_obj, SL_IID_VOLUME, &s->volume_itf);
    if (!s->play || !s->queue) {
        player_destroy(s);
        return 0;
    }
    s->loop = loop ? 1 : 0;
    (*s->queue)->RegisterCallback(s->queue, queue_cb, s);

    if (s->volume_itf) {
        /* 0 mB is full scale; the engine's 0..100 maps to -60..0 dB. */
        SLmillibel level = (SLmillibel)((s->volume - 100) * 60);
        (*s->volume_itf)->SetVolumeLevel(s->volume_itf, level);
    }

    if ((*s->queue)->Enqueue(s->queue, s->pcm, (SLuint32)s->pcm_bytes) != SL_RESULT_SUCCESS ||
        (*s->play)->SetPlayState(s->play, SL_PLAYSTATE_PLAYING) != SL_RESULT_SUCCESS) {
        host_log("[snd] cannot start %s\n", s->name);
        player_destroy(s);
        return 0;
    }
    s->playing = 1;
    s->stamp = ++g_stamp;
    return 1;
}

/* ---- the hooks sound.c calls -------------------------------------------- */

int sound_host_open(int id, const char *name, int loop,
                     const unsigned char *data, unsigned len)
{
    if (id <= 0 || id >= SND_MAX)
        return 0;
    snd *s = &g_snd[id];
    /* A handle can be destroyed and recreated; start clean. */
    player_destroy(s);
    drop_pcm(s);
    free(s->ogg);
    s->used = 1;
    snprintf(s->name, sizeof(s->name), "%s", name ? name : "?");
    s->loop = loop;
    s->volume = 100;
    s->len = len;
    s->ogg = NULL;
    if (data && len) {
        s->ogg = (unsigned char *)host_alloc(len);
        memcpy(s->ogg, data, len);
    }
    return s->ogg != NULL;
}

void sound_host_prepare(int id)
{
    /* Decoding here rather than at play time would pay for 160 sounds the
     * player may never trigger. Nothing to do: the engine's "prepare" is a
     * state transition, and the first play decodes. */
    (void)id;
}

void sound_host_play(int id, int loop)
{
    if (id <= 0 || id >= SND_MAX || !g_snd[id].used)
        return;
    player_start(&g_snd[id], loop);
}

void sound_host_stop(int id)
{
    if (id <= 0 || id >= SND_MAX)
        return;
    snd *s = &g_snd[id];
    if (s->play)
        (*s->play)->SetPlayState(s->play, SL_PLAYSTATE_STOPPED);
    s->playing = 0;
}

void sound_host_pause(int id)
{
    if (id <= 0 || id >= SND_MAX)
        return;
    snd *s = &g_snd[id];
    if (s->play)
        (*s->play)->SetPlayState(s->play, SL_PLAYSTATE_PAUSED);
}

void sound_host_resume(int id)
{
    if (id <= 0 || id >= SND_MAX)
        return;
    snd *s = &g_snd[id];
    if (s->play)
        (*s->play)->SetPlayState(s->play, SL_PLAYSTATE_PLAYING);
}

void sound_host_volume(int id, int volume)
{
    if (id <= 0 || id >= SND_MAX)
        return;
    snd *s = &g_snd[id];
    s->volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    if (s->volume_itf) {
        SLmillibel level = (SLmillibel)((s->volume - 100) * 60);
        (*s->volume_itf)->SetVolumeLevel(s->volume_itf, level);
    }
}

void sound_host_close(int id)
{
    if (id <= 0 || id >= SND_MAX)
        return;
    snd *s = &g_snd[id];
    player_destroy(s);
    drop_pcm(s);
    free(s->ogg);
    s->ogg = NULL;
    s->len = 0;
    s->used = 0;
}
