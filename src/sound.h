/*
 * sound.h -- the Java-side sound player the engine expects.
 *
 * libgame.so's own C2S media layer is compiled out of this build: every
 * CS_mda* entry point in the .so is a two-instruction stub returning 0, so
 * SOUNDPLAYER_Play's CS_mdaClipCreate gives up immediately. What the engine
 * actually uses is the Java surface -- CS_mdaSoundCreate() calls
 * jSoundCreate(), which is WrapperJinterface.SoundCreate -- and on Android
 * that Java side is MediaPlayer/SoundPool reading the same .ogg files.
 *
 * There is no Android here, so the port is that player. See sound.c.
 */
#ifndef SOUND_H
#define SOUND_H

/* SoundCreate(String path, boolean loop): a handle >= 1, or 0 when the sound
 * cannot be opened. Everything else takes that handle and returns 0 on
 * success, negative on failure -- the same convention CS_mdaSound* checks. */
int sound_create(const char *name, int loop);
int sound_prepare(int handle);
int sound_play(int handle, int loop);
int sound_pause(int handle);
int sound_resume(int handle);
int sound_stop(int handle);
int sound_destroy(int handle);
int sound_set_volume(int handle, int volume);   /* volume is 0..100 */
int sound_get_volume(int handle);

#endif /* SOUND_H */
