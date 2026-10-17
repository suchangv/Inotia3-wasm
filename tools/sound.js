/*
 * sound.js -- the browser's audio backend for src/sound.c (linked in with
 * --js-library; the C side declares these as extern functions).
 *
 * libgame.so's own C2S media layer is compiled out of this build -- every
 * CS_mda* entry point is a stub returning 0 -- so all sound goes through the
 * Java surface WrapperJinterface.Sound*, which on Android is MediaPlayer
 * (looping ambience and BGM) and SoundPool (effects). This file is that
 * player, on WebAudio.
 *
 * Why no Vorbis decoder here: the game ships only Ogg Vorbis (stored as
 * "NAME.ogg.jpg" so aapt keeps it uncompressed), and the browser decodes it
 * with decodeAudioData -- a real decoder, in native code, off the main thread.
 * The bytes are copied out of the wasm heap once and kept compressed: the
 * engine preloads 160 sounds (~10 MB) at boot and most never play, so decoding
 * everything up front would cost far more memory than the files do.
 *
 * Decoding is therefore lazy -- at SoundPrepare for small effects (so the
 * first swing is not late) and at SoundPlay for everything else. A play that
 * arrives while its decode is still in flight is remembered and starts when
 * the decode lands; that is the one behavioural difference from Android,
 * where SoundPool plays an already-decoded sample.
 *
 * The state lives in a `$PAudio` library symbol rather than a top-level `var`:
 * emscripten emits library functions into the module's own closure, where the
 * file's top-level declarations are not visible to them (the symptom is
 * "PAudio is not defined" at the first SoundCreate, from inside the wasm).
 * `$`-prefixed names are the documented way to share a helper between library
 * functions, and each function that uses it lists it in __deps.
 */
mergeInto(LibraryManager.library, {
    $PAudio: {
        ctx: null,
        slots: [],          /* handle (from sound.c) -> slot */
        decoded: 0,
        plays: 0,

        ensure: function () {
            if (!PAudio.ctx) {
                var AC = window.AudioContext || window.webkitAudioContext;
                if (!AC) return null;
                PAudio.ctx = new AC();
            }
            /* Chrome starts a context suspended until a user gesture. The
             * game's own clicks are that gesture; this just takes the first
             * opportunity to run again. */
            if (PAudio.ctx.state === 'suspended') PAudio.ctx.resume();
            return PAudio.ctx;
        },

        decode: function (slot) {
            if (slot.buffer || slot.decoding || !slot.bytes) return;
            var ctx = PAudio.ensure();
            if (!ctx) return;
            slot.decoding = true;
            var bytes = slot.bytes;
            ctx.decodeAudioData(bytes.buffer,
                function (buf) {
                    slot.buffer = buf;
                    slot.bytes = null;
                    slot.decoding = false;
                    if (PAudio.decoded < 8) {
                        PAudio.decoded++;
                        console.log('[snd] decoded ' + slot.name + ' ' +
                                    buf.duration.toFixed(2) + 's ' +
                                    buf.sampleRate + 'Hz');
                    }
                    if (slot.want) { slot.want = false; PAudio.start(slot); }
                },
                function (err) {
                    slot.decoding = false;
                    console.warn('[snd] decode failed: ' + slot.name, err);
                });
        },

        stopSource: function (slot) {
            if (slot.src) {
                try { slot.src.stop(); } catch (e) { /* already stopped */ }
                try { slot.src.disconnect(); } catch (e) { /* ignore */ }
                slot.src = null;
            }
            if (slot.gain) {
                try { slot.gain.disconnect(); } catch (e) { /* ignore */ }
                slot.gain = null;
            }
        },

        start: function (slot) {
            var ctx = PAudio.ensure();
            if (!ctx || !slot.buffer) {
                slot.want = true;
                PAudio.decode(slot);
                return;
            }
            PAudio.stopSource(slot);

            var src = ctx.createBufferSource();
            src.buffer = slot.buffer;
            src.loop = !!slot.loop;
            var gain = ctx.createGain();
            gain.gain.value = slot.volume / 100;
            src.connect(gain);
            gain.connect(ctx.destination);

            var offset = slot.offset || 0;
            if (offset >= slot.buffer.duration) offset = 0;
            try {
                src.start(0, offset);
            } catch (e) {
                try { src.start(); } catch (e2) { return; }
                offset = 0;
            }
            slot.offset = 0;
            slot.src = src;
            slot.gain = gain;
            slot.startedAt = ctx.currentTime - offset;
            if (PAudio.plays < 6) {
                PAudio.plays++;
                console.log('[snd] play ' + slot.name + ' gain=' +
                            (slot.volume / 100).toFixed(2) +
                            ' loop=' + src.loop);
            }
        }
    },

    sound_host_open__deps: ['$PAudio'],
    sound_host_open: function (id, namePtr, loop, dataPtr, len) {
        PAudio.slots[id] = {
            name: UTF8ToString(namePtr),
            loop: !!loop,
            volume: 100,
            bytes: len ? HEAPU8.slice(dataPtr, dataPtr + len) : null,
            buffer: null,
            decoding: false,
            want: false,
            src: null,
            gain: null,
            offset: 0,
            startedAt: 0
        };
        return 1;
    },

    sound_host_prepare__deps: ['$PAudio'],
    sound_host_prepare: function (id) {
        var slot = PAudio.slots[id];
        /* 256 KB: every effect is far under it and every BGM far over. */
        if (slot && slot.bytes && slot.bytes.length <= 256 * 1024)
            PAudio.decode(slot);
    },

    sound_host_play__deps: ['$PAudio'],
    sound_host_play: function (id, loop) {
        var slot = PAudio.slots[id];
        if (!slot) return;
        slot.loop = !!loop;
        if (slot.buffer) {
            PAudio.start(slot);
        } else {
            slot.want = true;
            PAudio.decode(slot);
        }
    },

    sound_host_stop__deps: ['$PAudio'],
    sound_host_stop: function (id) {
        var slot = PAudio.slots[id];
        if (!slot) return;
        slot.want = false;
        slot.offset = 0;
        PAudio.stopSource(slot);
    },

    sound_host_pause__deps: ['$PAudio'],
    sound_host_pause: function (id) {
        var slot = PAudio.slots[id];
        if (!slot) return;
        if (slot.src && PAudio.ctx)
            slot.offset = Math.max(0, PAudio.ctx.currentTime - slot.startedAt);
        slot.want = false;
        PAudio.stopSource(slot);
    },

    sound_host_resume__deps: ['$PAudio'],
    sound_host_resume: function (id) {
        var slot = PAudio.slots[id];
        if (!slot) return;
        if (slot.buffer) PAudio.start(slot);
    },

    sound_host_volume__deps: ['$PAudio'],
    sound_host_volume: function (id, volume) {
        var slot = PAudio.slots[id];
        if (!slot) return;
        slot.volume = volume;
        if (slot.gain) slot.gain.gain.value = volume / 100;
    },

    sound_host_close__deps: ['$PAudio'],
    sound_host_close: function (id) {
        var slot = PAudio.slots[id];
        if (!slot) return;
        PAudio.stopSource(slot);
        PAudio.slots[id] = null;
    }
});
