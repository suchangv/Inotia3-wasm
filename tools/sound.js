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
        /* Decoded PCM is the one thing here that is genuinely large: the
         * music tracks are minutes long and decode to tens of megabytes each,
         * while the compressed files they came from are a few hundred KB. The
         * engine can ask for any of them at any time (one per map, one per
         * event), so they are kept decoded only while they are recent: past
         * this budget the least recently used ones are dropped back to their
         * compressed bytes. Without a cap a long session grows by the size of
         * every track the player ever walked into. */
        pcm: 0,
        pcm_max: 96 * 1024 * 1024,
        lru: [],            /* slot ids, most recently touched last */

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
            /* ?pcmmax=<MB> exists to exercise the eviction path: the shipped
             * budget is 96 MB, which a normal session never reaches. */
            if (typeof Module !== 'undefined' && Module.spikePcmMax)
                PAudio.pcm_max = Module.spikePcmMax * 1048576;
            return PAudio.ctx;
        },

        /* Mark a slot as recently used, and drop decoded buffers (oldest
         * first) while the total is over budget. A slot that is playing keeps
         * its audio: the source node holds its own reference to the buffer,
         * and dropping ours only means a later play decodes it again. */
        note: function (id) {
            var at = PAudio.lru.indexOf(id);
            if (at >= 0) PAudio.lru.splice(at, 1);
            PAudio.lru.push(id);
        },

        evict: function (keep) {
            if (PAudio.pcm <= PAudio.pcm_max)
                return;
            for (var i = 0; i < PAudio.lru.length && PAudio.pcm > PAudio.pcm_max; ) {
                var id = PAudio.lru[i];
                var slot = PAudio.slots[id];
                if (!slot || !slot.buffer || id === keep) { i++; continue; }
                PAudio.pcm -= slot.buffer.length * 4;
                slot.buffer = null;
                PAudio.lru.splice(i, 1);
            }
        },

        decode: function (slot) {
            if (slot.buffer || slot.decoding || !slot.bytes) return;
            var ctx = PAudio.ensure();
            if (!ctx) return;
            slot.decoding = true;
            /* decodeAudioData *detaches* the ArrayBuffer it is handed. Passing
             * slot.bytes.buffer directly would therefore destroy the only copy
             * this slot can decode from -- and since the PCM budget below drops
             * decoded buffers, a sound that gets evicted could never be decoded
             * again: the second call fails on a detached buffer and the sound is
             * silent for the rest of the session. A copy is a few hundred KB,
             * once per play that follows an eviction. */
            ctx.decodeAudioData(slot.bytes.slice().buffer,
                function (buf) {
                    slot.buffer = buf;
                    slot.decoding = false;
                    PAudio.pcm += buf.length * 4;
                    PAudio.note(slot.id);
                    PAudio.evict(slot.id);
                    if (PAudio.decoded < 8 || PAudio.decoded % 25 === 24) {
                        console.log('[snd] decoded ' + slot.name + ' ' +
                                    buf.duration.toFixed(2) + 's ' +
                                    buf.sampleRate + 'Hz  pcm=' +
                                    Math.round(PAudio.pcm / 1048576) + 'MB');
                    }
                    PAudio.decoded++;
                    if (slot.want) { slot.want = false; PAudio.start(slot); }
                },
                function (err) {
                    slot.decoding = false;
                    if ((PAudio.failures = (PAudio.failures || 0) + 1) <= 6)
                        console.warn('[snd] decode failed: ' + slot.name + ': ' +
                                     (err && err.message ? err.message : err));
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
            PAudio.note(slot.id);
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
            id: id,
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
        /* A looping sound that is already playing is already doing what was
         * asked. The engine re-issues BGM/ambience plays as its state machine
         * re-evaluates, and restarting one each time would rebuild the audio
         * graph several times a second -- audible as a stutter, and thousands
         * of live nodes in a long session. One-shot effects still restart on
         * every play, which is what a repeated swing should do. */
        if (loop && slot.src) return;
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
        if (slot.buffer) PAudio.pcm -= slot.buffer.length * 4;
        var at = PAudio.lru.indexOf(id);
        if (at >= 0) PAudio.lru.splice(at, 1);
        PAudio.slots[id] = null;
    }
});
