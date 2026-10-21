// Ogg Vorbis decoded by a separate wasm module, for browsers that have no Vorbis
// decoder of their own -- that is Safari, and therefore every iOS device:
// decodeAudioData rejects on all 180 sounds this game ships.
//
// Why this is not in the main module, and why that took a second attempt: the
// first fix compiled stb_vorbis into inotia3.wasm and primed an AudioContext
// during boot, and that build black-screened on launch. A decoder that can stop
// the game from starting is not a fix. So the decoder is its own module, built
// by tools/build_vorbis.sh, fetched only after decodeAudioData has already
// failed: if the fetch, the compile or the decode fails, the game keeps running
// and is silent, which is the state it was in before.
//
// Chrome and Firefox never get here -- their decodeAudioData handles Vorbis.
//
// The module hands back planar float PCM because that is what AudioBuffer wants;
// see src/vorbis_wasm.c for the ABI (spike_scratch / spike_out / spike_decode).

(function () {
  var modPromise = null;

  function load() {
    if (!modPromise) {
      modPromise = (async function () {
        var res = await fetch('vorbis.wasm', { cache: 'force-cache' });
        if (!res.ok) throw new Error('vorbis.wasm: HTTP ' + res.status);
        var bytes = await res.arrayBuffer();
        var mod = await WebAssembly.compile(bytes);

        // Standalone-wasm output imports a few WASI stubs that never run here
        // (no fd writes, no exit). Supplying harmless ones keeps the module
        // self-contained instead of pulling in a WASI shim.
        var imports = {};
        WebAssembly.Module.imports(mod).forEach(function (i) {
          imports[i.module] = imports[i.module] || {};
          imports[i.module][i.name] = i.kind === 'function' ? function () { return 0; } : null;
        });

        var inst = await WebAssembly.instantiate(mod, imports);
        var M = inst.exports;
        if (M._initialize) M._initialize();       // runs static ctors once
        console.log('spike: Vorbis decoder ready (' + bytes.byteLength + ' B)');
        return M;
      })();
      // A failed load must not poison the next attempt (a transient 404 while a
      // deploy is in flight should not mean "silent forever").
      modPromise.catch(function () { modPromise = null; });
    }
    return modPromise;
  }

  async function decode(u8) {
    var M = await load();
    var scratch = M.spike_scratch();
    if (!scratch) throw new Error('Vorbis decoder is out of memory');
    // The module owns the input buffer; JS only writes into it. Views are taken
    // fresh after each call because growth can move (and detach) the buffer.
    new Uint8Array(M.memory.buffer, scratch, u8.length).set(u8);

    var outPtr = M.spike_out();
    if (!M.spike_decode(u8.length)) throw new Error('not a decodable Ogg Vorbis stream');

    var dv = new DataView(M.memory.buffer);
    var channels = dv.getInt32(outPtr, true);
    var rate = dv.getInt32(outPtr + 4, true);
    var frames = dv.getInt32(outPtr + 8, true);
    var pcmPtr = dv.getInt32(outPtr + 12, true);
    if (channels <= 0 || frames <= 0 || !rate) throw new Error('decoder returned an empty stream');

    var flat = new Float32Array(M.memory.buffer, pcmPtr, channels * frames);
    var planar = [];
    for (var c = 0; c < channels; c++) {
      // Copy out: the PCM lives in the decoder's heap only until the next decode.
      planar.push(flat.subarray(c * frames, (c + 1) * frames).slice());
    }
    return { channels: channels, rate: rate, frames: frames, pcm: planar };
  }

  function install() {
    var M = window.Module;
    if (!M) { setTimeout(install, 50); return; }
    // tools/sound.js looks this up only after decodeAudioData has rejected.
    M.spikeVorbisDecode = function (bytes) { return decode(bytes); };
  }

  install();
})();
