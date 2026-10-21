/*
 * sound.c -- be the Java side of the engine's sound playback.
 *
 * The chain, for the record:
 *
 *   SOUNDSYSTEM_Create
 *     -> CS_mdaSoundCreate(&handle, path, loop)     (guest 0xe8f18)
 *       -> jSoundCreate(path, loop)                 (guest 0xd8df8)
 *         -> WrapperJinterface.SoundCreate(String, boolean)
 *
 * and then SoundPrepare/SoundPlay/SoundStop/SoundSetVolume/SoundDestroy the
 * same way. On Android that Java side is MediaPlayer (the looping ambience and
 * BGM) and SoundPool (the effects) reading the game's own .ogg files.
 *
 * The .so also carries a native C2S media layer (CS_mdaClipCreate and
 * friends), but in this build every one of those is a stub returning 0, so
 * SOUNDPLAYER_Play's CS_mdaClipCreate fails and the native path is dead. The
 * Java path above is the live one -- the native run calls SoundCreate 160
 * times at boot -- which makes this file the whole audio implementation.
 *
 * Format: everything shipped is Ogg Vorbis, stored in the archive as
 * "NAME.ogg.jpg" (the packer renames already-compressed files so aapt stores
 * them instead of deflating them again). Nothing is decoded here: the browser
 * has a Vorbis decoder built in, so the bytes go to the host as they are and
 * come back as a playable buffer. See tools/sound.js.
 */
#include <stdint.h>

#include "asset_zip.h"
#include "compat.h"     /* the string helpers: no libc freestanding */
#include "guest.h"
#include "falsojni.h"
#include "platform.h"
#include "sound.h"

/* The archive is read through stdio; the freestanding throughput build has no
 * filesystem at all, so everything below that opens a file is guarded. */
#ifndef WASM_FREESTANDING
#include <stdio.h>
#include <stdlib.h>
#endif

/* The engine creates one handle per distinct sound asset it preloads and keeps
 * them for the life of the process (160 of them at boot), so this is sized for
 * the assets, not for the voices -- the host decides how many can sound at
 * once. Index 0 is not a handle, matching the Java side's "0 means failure". */
#define SOUND_MAX       512
#define SOUND_NAME_MAX  160

typedef struct {
    int  used;
    char name[SOUND_NAME_MAX];
    int  loop;
    int  volume;            /* the engine's own scale: 0..100 */
} sound_slot;

static sound_slot g_sound[SOUND_MAX];

/* ---- the host backend ----------------------------------------------------
 *
 * The browser implements these in tools/sound.js (WebAudio; the decoder is the
 * browser's). Everywhere else -- the native runner and the freestanding wasm
 * throughput build -- they are no-ops that report success, so the engine's own
 * state machine still runs and the two hosts stay behaviourally identical
 * everywhere except the audio hardware itself. */
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
extern int  sound_host_open(int id, const char *name, int loop,
                            const unsigned char *data, unsigned len);
extern void sound_host_prepare(int id);
extern void sound_host_play(int id, int loop);
extern void sound_host_stop(int id);
extern void sound_host_pause(int id);
extern void sound_host_resume(int id);
extern void sound_host_volume(int id, int volume);
extern void sound_host_close(int id);
/* Not tied to any one sound: create the audio context early, and resume it from
 * a user gesture. iOS only unlocks audio from inside a gesture, and the page's
 * "start" click is the one it gets; see tools/sound.js and web/index.html. */
extern int  sound_host_audio_prime(void);
extern int  sound_host_audio_unlock(void);
#else
static int  sound_host_open(int id, const char *name, int loop,
                            const unsigned char *data, unsigned len)
{ (void) id; (void) name; (void) loop; (void) data; (void) len; return 1; }
static void sound_host_prepare(int id) { (void) id; }
static void sound_host_play(int id, int loop) { (void) id; (void) loop; }
static void sound_host_stop(int id) { (void) id; }
static void sound_host_pause(int id) { (void) id; }
static void sound_host_resume(int id) { (void) id; }
static void sound_host_volume(int id, int volume) { (void) id; (void) volume; }
static void sound_host_close(int id) { (void) id; }
static int  sound_host_audio_prime(void) { return 1; }
static int  sound_host_audio_unlock(void) { return 1; }
#endif

#ifndef WASM_FREESTANDING
/* A logical sound name's bytes, freshly allocated; the caller frees. The
 * archive lookup is the same one the engine's own resource loads go through,
 * so a name it can open is a name this finds. */
static unsigned char *read_sound(const char *name, unsigned *out_len)
{
    uint32_t off = 0, len = 0;
    if (asset_lookup(name, &off, &len) || len == 0)
        return NULL;

    FILE *f = fopen(host_apk_path(), "rb");
    if (!f)
        return NULL;
    unsigned char *buf = malloc(len);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, (long) off, SEEK_SET) != 0 ||
        fread(buf, 1, len, f) != len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = len;
    return buf;
}
#endif

static int slot_of(int handle)
{
    return (handle >= 1 && handle < SOUND_MAX && g_sound[handle].used) ? handle
                                                                      : 0;
}

int sound_create(const char *name, int loop)
{
    int id = 0;
    for (int i = 1; i < SOUND_MAX; i++) {
        if (!g_sound[i].used) {
            id = i;
            break;
        }
    }
    if (!id) {
        /* Once, not once per attempt: the engine creates sounds as it meets
         * new monsters and maps, and a full table would otherwise turn every
         * such encounter into another log line. */
        static int said;
        if (!said) {
            said = 1;
            host_log("[snd] out of handle slots (%d) -- further sounds are "
                     "dropped\n", SOUND_MAX);
        }
        return 0;
    }

    /* memset first, so the strncpy leaves the name NUL-terminated. */
    sound_slot *s = &g_sound[id];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->loop    = loop;
    s->volume  = 100;

#ifdef WASM_FREESTANDING
    /* No filesystem in the throughput build; the handle still exists so the
     * engine's own bookkeeping runs. */
    s->used = 1;
    sound_host_open(id, s->name, loop, NULL, 0);
#else
    unsigned len = 0;
    unsigned char *data = read_sound(name, &len);
    if (!data) {
        /* Not an error the engine can act on -- it just gets 0 and, for the
         * sounds it merely preloads, never notices. Worth a line, because a
         * missing asset is the first thing to suspect when a sound is silent. */
        host_log("[snd] SoundCreate(\"%s\"): not in %s\n", name,
                 ASSET_ARCHIVE);
        return 0;
    }

    s->used = 1;
    sound_host_open(id, s->name, loop, data, len);
    free(data);

    if (g_jni_verbose) {
        static int n;
        if (n < 24) {
            n++;
            host_log("[snd] SoundCreate(\"%s\", loop=%d) -> %d (%u bytes)\n",
                     name, loop, id, len);
        }
    }
#endif
    return id;
}

int sound_prepare(int handle)
{
    int id = slot_of(handle);
    if (!id)
        return -1;
    /* The context is created on the first prepare rather than at boot: it is
     * the earliest point every host has reached the sound system, and on the
     * browser an AudioContext created here comes back suspended, which the
     * page's click then resumes. Creating it is not what iOS forbids --
     * starting playback outside a gesture is. */
    static int primed;
    if (!primed) {
        primed = 1;
        sound_host_audio_prime();
    }
    /* The host decides what "prepare" costs: it has the bytes and knows how
     * big they are (see tools/sound.js -- small effects decode now so the
     * first swing is not late, background music waits). */
    sound_host_prepare(id);
    return 0;
}

int sound_play(int handle, int loop)
{
    int id = slot_of(handle);
    if (!id)
        return -1;
    g_sound[id].loop = loop;
    if (g_jni_verbose) {
        static int n;
        if (n < 12) {
            n++;
            host_log("[snd] SoundPlay(%d \"%s\", loop=%d)\n", id,
                     g_sound[id].name, loop);
        }
    }
    /* The page unlocks audio from a click, but on iOS the context can be
     * suspended again later (backgrounding, a phone call), and the C side is
     * the only place that sees the next play. A no-op once it is running.
     *
     * This call also keeps sound_host_audio_unlock in the build: it is called
     * from JavaScript only, and emscripten drops library functions nothing in
     * the module references -- which made the page's unlock silently raise
     * ReferenceError inside its own try/catch. */
    static int unlocked;
    if (!unlocked) {
        unlocked = 1;
        sound_host_audio_unlock();
    }
    sound_host_play(id, loop);
    return 0;
}

int sound_pause(int handle)
{
    int id = slot_of(handle);
    if (!id)
        return -1;
    sound_host_pause(id);
    return 0;
}

int sound_resume(int handle)
{
    int id = slot_of(handle);
    if (!id)
        return -1;
    sound_host_resume(id);
    return 0;
}

int sound_stop(int handle)
{
    int id = slot_of(handle);
    if (!id)
        return -1;
    sound_host_stop(id);
    return 0;
}

int sound_destroy(int handle)
{
    int id = slot_of(handle);
    if (!id)
        return -1;
    /* close, not just stop: the host is holding the compressed bytes, and
     * this is the engine telling us it will never play this sound again. */
    sound_host_close(id);
    memset(&g_sound[id], 0, sizeof(g_sound[id]));
    return 0;
}

int sound_set_volume(int handle, int volume)
{
    int id = slot_of(handle);
    if (!id)
        return -1;
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    g_sound[id].volume = volume;
    if (g_jni_verbose) {
        static int n;
        if (n < 8) {
            n++;
            host_log("[snd] SoundSetVolume(%d \"%s\", %d)\n", id,
                     g_sound[id].name, volume);
        }
    }
    sound_host_volume(id, volume);
    return 0;
}

int sound_get_volume(int handle)
{
    int id = slot_of(handle);
    return id ? g_sound[id].volume : 100;
}
