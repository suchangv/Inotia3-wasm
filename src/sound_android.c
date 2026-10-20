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
} snd;

static snd g_snd[SND_MAX];

void sound_host_stop(int id);   /* used by open() before its definition */

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

/* ---- voices -------------------------------------------------------------
 *
 * A pool of realised OpenSL players, reused across sounds.
 *
 * The first version created (and destroyed) a player on every play. On a phone
 * that is not a cheap object: CreateAudioPlayer + Realize is a round trip to
 * the audioserver, tens of milliseconds, and it happens *while the frame is
 * being simulated*. A skill that plays a sound therefore stalled the frame it
 * was used on -- audible as a stutter that the browser, which decodes and plays
 * asynchronously, never has.
 *
 * A voice is bound to one PCM format when it is created and reused for every
 * sound that shares it (which is all of them: 44.1 kHz stereo), so after the
 * first few plays nothing but an Enqueue + SetPlayState is left.
 *
 * Also: a play request for a sound that is *already* playing is ignored. The
 * engine re-issues SoundPlay while a sound is running (the Java side it was
 * written against, MediaPlayer/SoundPool, treats a repeat as a no-op), and
 * restarting the buffer on every request is the other half of the stutter.
 */
#define VOICES 10

typedef struct {
    int owner;                 /* sound id, 0 = free */
    int rate, channels;
    int looping;
    int playing;
    unsigned stamp;            /* for "steal the oldest" */
    SLObjectItf obj;
    SLPlayItf play;
    SLBufferQueueItf queue;
    SLVolumeItf vol;
} voice;

static voice g_vo[VOICES];

static void voice_destroy(voice *v)
{
    if (v->obj) {
        (*v->obj)->Destroy(v->obj);
        v->obj = NULL;
        v->play = NULL;
        v->queue = NULL;
        v->vol = NULL;
    }
    v->playing = 0;
    v->owner = 0;
}

/* The callback runs on OpenSL's thread. It only touches this voice. */
static void voice_cb(SLBufferQueueItf bq, void *ctx)
{
    voice *v = (voice *)ctx;
    if (!v)
        return;
    if (v->looping && v->playing) {
        if (v->owner) {
            snd *s = &g_snd[v->owner];
            if (s->pcm && s->pcm_bytes)
                (*bq)->Enqueue(bq, s->pcm, (SLuint32)s->pcm_bytes);
            return;
        }
    }
    /* Finished: free the voice as well as marking it silent, or the pool slowly
     * fills with voices that are not playing but still "owned" and every later
     * play is forced to build a new player (12 ms of audioserver round trip in
     * the middle of a frame -- which is what the stutter was). */
    v->playing = 0;
    v->owner = 0;
}

static int voice_realize(voice *v, int rate, int channels)
{
    if (v->obj && v->rate == rate && v->channels == channels)
        return 1;
    voice_destroy(v);

    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1
    };
    SLDataFormat_PCM fmt = {
        SL_DATAFORMAT_PCM,
        (SLuint32)channels,
        (SLuint32)rate * 1000u,          /* milliHz */
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        channels == 2 ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT)
                      : SL_SPEAKER_FRONT_CENTER,
        SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource src = { &loc_bufq, &fmt };
    SLDataLocator_OutputMix loc_mix = { SL_DATALOCATOR_OUTPUTMIX, g_mix_obj };
    SLDataSink sink = { &loc_mix, NULL };
    const SLInterfaceID ids[2] = { SL_IID_BUFFERQUEUE, SL_IID_VOLUME };
    const SLboolean req[2] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };

    if ((*g_engine)->CreateAudioPlayer(g_engine, &v->obj, &src, &sink,
                                       2, ids, req) != SL_RESULT_SUCCESS)
        return 0;
    if ((*v->obj)->Realize(v->obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
        (*v->obj)->Destroy(v->obj);
        v->obj = NULL;
        return 0;
    }
    (*v->obj)->GetInterface(v->obj, SL_IID_PLAY, &v->play);
    (*v->obj)->GetInterface(v->obj, SL_IID_BUFFERQUEUE, &v->queue);
    (*v->obj)->GetInterface(v->obj, SL_IID_VOLUME, &v->vol);
    if (!v->play || !v->queue) {
        voice_destroy(v);
        return 0;
    }
    (*v->queue)->RegisterCallback(v->queue, voice_cb, v);
    v->rate = rate;
    v->channels = channels;
    return 1;
}

/* A voice for this format: a free one with the right format, else any free one,
 * else the oldest playing one (stopped and reused). */
static voice *voice_acquire(int rate, int channels)
{
    voice *free_same = NULL, *free_any = NULL;
    for (int i = 0; i < VOICES; i++) {
        voice *v = &g_vo[i];
        if (v->owner || v->playing)
            continue;
        if (!free_any)
            free_any = v;
        if (!free_same && v->obj && v->rate == rate && v->channels == channels)
            free_same = v;
    }
    if (free_same)
        return free_same;
    if (free_any)
        return free_any;

    /* Everything is busy: take the one that has been sounding longest. */
    voice *victim = NULL;
    for (int i = 0; i < VOICES; i++)
        if (!victim || g_vo[i].stamp < victim->stamp)
            victim = &g_vo[i];
    if (!victim)
        return NULL;
    if (victim->play)
        (*victim->play)->SetPlayState(victim->play, SL_PLAYSTATE_STOPPED);
    if (victim->queue)
        (*victim->queue)->Clear(victim->queue);
    victim->playing = 0;
    victim->owner = 0;
    return victim;
}

static int voice_start(int id)
{
    snd *s = &g_snd[id];
    if (!sl_init())
        return 0;
    if (!decode(s))
        return 0;
    if (s->rate <= 0 || s->channels <= 0)
        return 0;

    /* Already sounding? Ignore. See the comment at the top of this section. */
    for (int i = 0; i < VOICES; i++) {
        voice *v = &g_vo[i];
        if (v->owner == id && v->playing) {
            s->stamp = ++g_stamp;
            return 1;
        }
    }

    voice *v = voice_acquire(s->rate, s->channels);
    if (!v)
        return 0;
    if (!voice_realize(v, s->rate, s->channels))
        return 0;

    v->owner = id;
    v->looping = s->loop;
    v->playing = 1;
    v->stamp = ++g_stamp;
    if (v->vol)
        (*v->vol)->SetVolumeLevel(v->vol, (SLmillibel)((s->volume - 100) * 60));
    if ((*v->queue)->Enqueue(v->queue, s->pcm, (SLuint32)s->pcm_bytes) !=
            SL_RESULT_SUCCESS ||
        (*v->play)->SetPlayState(v->play, SL_PLAYSTATE_PLAYING) !=
            SL_RESULT_SUCCESS) {
        v->playing = 0;
        v->owner = 0;
        return 0;
    }
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
    sound_host_stop(id);
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
    g_snd[id].loop = loop ? 1 : 0;
    voice_start(id);
}

void sound_host_stop(int id)
{
    if (id <= 0 || id >= SND_MAX)
        return;
    for (int i = 0; i < VOICES; i++) {
        voice *v = &g_vo[i];
        if (v->owner != id)
            continue;
        if (v->play)
            (*v->play)->SetPlayState(v->play, SL_PLAYSTATE_STOPPED);
        if (v->queue)
            (*v->queue)->Clear(v->queue);
        v->playing = 0;
        v->owner = 0;
    }
}

void sound_host_pause(int id)
{
    if (id <= 0 || id >= SND_MAX)
        return;
    for (int i = 0; i < VOICES; i++) {
        voice *v = &g_vo[i];
        if (v->owner == id && v->play)
            (*v->play)->SetPlayState(v->play, SL_PLAYSTATE_PAUSED);
    }
}

void sound_host_resume(int id)
{
    if (id <= 0 || id >= SND_MAX)
        return;
    for (int i = 0; i < VOICES; i++) {
        voice *v = &g_vo[i];
        if (v->owner == id && v->play)
            (*v->play)->SetPlayState(v->play, SL_PLAYSTATE_PLAYING);
    }
}

void sound_host_volume(int id, int volume)
{
    if (id <= 0 || id >= SND_MAX)
        return;
    snd *s = &g_snd[id];
    s->volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    for (int i = 0; i < VOICES; i++) {
        voice *v = &g_vo[i];
        if (v->owner == id && v->vol)
            (*v->vol)->SetVolumeLevel(v->vol, (SLmillibel)((s->volume - 100) * 60));
    }
}

void sound_host_close(int id)
{
    if (id <= 0 || id >= SND_MAX)
        return;
    snd *s = &g_snd[id];
    sound_host_stop(id);
    drop_pcm(s);
    free(s->ogg);
    s->ogg = NULL;
    s->len = 0;
    s->used = 0;
}
