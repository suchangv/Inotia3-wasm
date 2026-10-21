// The web UI's backend: import a user's APK once, keep it in IndexedDB, boot the
// engine from it, and move saves in and out as zip files.
//
// Nothing here is legal or technical boilerplate that could live in the page:
// all of it needs the emscripten filesystem, which is only reachable from
// inside the module (pre-js), so the page itself stays a few buttons and a
// status line.
//
// The flow:
//
//   1. page loads inotia3.js with `Module.spikeUI = true`, so main() is gated on
//      a run dependency and the engine does not start by itself;
//   2. any previously imported APK is fetched from IndexedDB and written into
//      MEMFS as /assets.apk (the archive asset_zip.c reads) and /libgame.so
//      (the image the loader maps) -- this is what makes "import once" true;
//   3. the page watches Module.spike.state; "开始游戏" releases the gate.
//
// The APK is stored as-is, because it is also the asset archive: the engine
// reads resources out of it at byte offsets (src/asset_zip.c), so re-zipping the
// assets would break every offset. libgame.so is stored separately as well only
// so a start does not have to unzip 22 MB again.

(function () {
  var M = (typeof Module !== 'undefined') ? Module : (Module = {});
  if (!M['spikeUI']) return;                 // the dev spike.html build autostarts

  var DB_NAME = 'spike-files', DB_STORE = 'files';
  var state = 'booting';                     // booting | no-apk | ready | running
  var listeners = [];
  /* Sizes of what is currently loaded. The page cannot see MEMFS, and without
   * these it could not tell "imported in this session" from "loaded from
   * IndexedDB on this visit" -- which is why the 清除 button only appeared right
   * after an import and vanished on reload. */
  var loaded = { apk: 0, so: 0 };

  function emit() {
    for (var i = 0; i < listeners.length; i++) {
      try { listeners[i](state); } catch (e) { console.error(e); }
    }
  }
  function setState(s) { if (s !== state) { state = s; emit(); } }

  /* ---- IndexedDB: the imported APK and the image extracted from it ------- */

  function idbOpen() {
    return new Promise(function (res, rej) {
      var r = indexedDB.open(DB_NAME, 1);
      r.onupgradeneeded = function () { r.result.createObjectStore(DB_STORE); };
      r.onsuccess = function () { res(r.result); };
      r.onerror = function () { rej(r.error); };
    });
  }
  async function idbGet(key) {
    var db = await idbOpen();
    return new Promise(function (res, rej) {
      var q = db.transaction(DB_STORE, 'readonly').objectStore(DB_STORE).get(key);
      q.onsuccess = function () { res(q.result || null); };
      q.onerror = function () { rej(q.error); };
    });
  }
  async function idbDel(key) {
    var db = await idbOpen();
    return new Promise(function (res, rej) {
      var t = db.transaction(DB_STORE, 'readwrite');
      t.objectStore(DB_STORE).delete(key);
      t.oncomplete = function () { res(); };
      t.onerror = function () { rej(t.error); };
    });
  }
  async function idbPut(key, val) {
    var db = await idbOpen();
    return new Promise(function (res, rej) {
      var t = db.transaction(DB_STORE, 'readwrite');
      t.objectStore(DB_STORE).put(val, key);
      t.oncomplete = function () { res(); };
      t.onerror = function () { rej(t.error); };
    });
  }

  /* ---- zip: read the APK, write and read save archives -------------------
   *
   * Both directions use the platform's own streams (CompressionStream with
   * 'deflate-raw', i.e. a bare deflate stream with no zlib header) instead of
   * bundling a zip library: the browser already has inflate and deflate, and a
   * 200-line dependency in a page whose whole job is four buttons is not worth
   * it. Method 0 (stored) is accepted too, in both directions. */

  var CRC_TABLE = null;
  function crc32(u8) {
    if (!CRC_TABLE) {
      CRC_TABLE = new Uint32Array(256);
      for (var n = 0; n < 256; n++) {
        var c = n;
        for (var k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
        CRC_TABLE[n] = c >>> 0;
      }
    }
    var crc = 0xFFFFFFFF;
    for (var i = 0; i < u8.length; i++) crc = CRC_TABLE[(crc ^ u8[i]) & 0xFF] ^ (crc >>> 8);
    return (crc ^ 0xFFFFFFFF) >>> 0;
  }

  async function deflateRaw(u8) {
    var cs = new CompressionStream('deflate-raw');
    var w = cs.writable.getWriter();
    w.write(u8); w.close();
    return new Uint8Array(await new Response(cs.readable).arrayBuffer());
  }
  async function inflateRaw(u8) {
    var ds = new DecompressionStream('deflate-raw');
    var w = ds.writable.getWriter();
    w.write(u8); w.close();
    return new Uint8Array(await new Response(ds.readable).arrayBuffer());
  }

  async function zipRead(u8) {
    var dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    var eocd = -1;
    for (var i = u8.length - 22; i >= 0 && i >= u8.length - 65558; i--) {
      if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) throw new Error('不是有效的 zip 文件');
    var count = dv.getUint16(eocd + 10, true);
    var p = dv.getUint32(eocd + 16, true);
    var out = [];
    for (var n = 0; n < count; n++) {
      if (dv.getUint32(p, true) !== 0x02014b50) break;
      var method = dv.getUint16(p + 10, true);
      var csize = dv.getUint32(p + 20, true);
      var nlen = dv.getUint16(p + 28, true);
      var elen = dv.getUint16(p + 30, true);
      var clen = dv.getUint16(p + 32, true);
      var lho = dv.getUint32(p + 42, true);
      var name = new TextDecoder().decode(u8.subarray(p + 46, p + 46 + nlen));
      var lnlen = dv.getUint16(lho + 26, true), lelen = dv.getUint16(lho + 28, true);
      var start = lho + 30 + lnlen + lelen;
      var data = u8.subarray(start, start + csize);
      if (name.endsWith('/')) { p += 46 + nlen + elen + clen; continue; }
      if (method === 8) data = await inflateRaw(data);
      else if (method === 0) data = data.slice();
      else throw new Error('不支持的压缩方式 ' + method + '（' + name + '）');
      out.push({ name: name, data: data });
      p += 46 + nlen + elen + clen;
    }
    return out;
  }

  async function zipWrite(entries) {
    var parts = [], central = [], offset = 0, enc = new TextEncoder();
    for (var i = 0; i < entries.length; i++) {
      var e = entries[i];
      var raw = e.data, method = 0, data = raw;
      if (raw.length > 128) {
        try { data = await deflateRaw(raw); method = 8; } catch (_) { data = raw; method = 0; }
      }
      var name = enc.encode(e.name);
      var crc = crc32(raw);
      var lh = new DataView(new ArrayBuffer(30));
      lh.setUint32(0, 0x04034b50, true); lh.setUint16(4, 20, true);
      lh.setUint16(6, 0x0800, true); lh.setUint16(8, method, true);
      lh.setUint16(10, 0, true); lh.setUint16(12, 0x21, true);
      lh.setUint32(14, crc, true); lh.setUint32(18, data.length, true);
      lh.setUint32(22, raw.length, true);
      lh.setUint16(26, name.length, true); lh.setUint16(28, 0, true);
      parts.push(new Uint8Array(lh.buffer), name, data);
      central.push({ name: name, method: method, crc: crc,
                     csize: data.length, usize: raw.length, offset: offset });
      offset += 30 + name.length + data.length;
    }
    var cdStart = offset, cdSize = 0;
    for (var j = 0; j < central.length; j++) {
      var c = central[j];
      var cd = new DataView(new ArrayBuffer(46));
      cd.setUint32(0, 0x02014b50, true); cd.setUint16(4, 20, true);
      cd.setUint16(6, 20, true); cd.setUint16(8, 0x0800, true);
      cd.setUint16(10, c.method, true); cd.setUint16(12, 0, true);
      cd.setUint16(14, 0x21, true); cd.setUint32(16, c.crc, true);
      cd.setUint32(20, c.csize, true); cd.setUint32(24, c.usize, true);
      cd.setUint16(28, c.name.length, true);
      cd.setUint32(42, c.offset, true);
      parts.push(new Uint8Array(cd.buffer), c.name);
      cdSize += 46 + c.name.length;
    }
    var eocd = new DataView(new ArrayBuffer(22));
    eocd.setUint32(0, 0x06054b50, true);
    eocd.setUint16(8, central.length, true); eocd.setUint16(10, central.length, true);
    eocd.setUint32(12, cdSize, true); eocd.setUint32(16, cdStart, true);
    parts.push(new Uint8Array(eocd.buffer));
    var total = 0, k;
    for (k = 0; k < parts.length; k++) total += parts[k].length;
    var out = new Uint8Array(total), at = 0;
    for (k = 0; k < parts.length; k++) { out.set(parts[k], at); at += parts[k].length; }
    return out;
  }

  /* ---- the emscripten filesystem ---------------------------------------- */

  function mkdirp(path) {
    var parts = path.split('/').filter(Boolean), cur = '';
    for (var i = 0; i < parts.length; i++) {
      cur += '/' + parts[i];
      try { FS.mkdir(cur); } catch (e) { /* exists, or it is a file: the write will say so */ }
    }
  }
  function writeGuestFile(path, u8) {
    var slash = path.lastIndexOf('/');
    if (slash > 0) mkdirp(path.slice(0, slash));
    try { FS.unlink(path); } catch (e) { /* did not exist */ }
    FS.writeFile(path, u8);
  }
  // The engine opens "libgame.so" and probes for "assets.apk" relative to the
  // working directory, which is "/" here; host_apk_path() also probes
  // "/assets.apk". One copy each is enough -- and both have to be in MEMFS, not
  // in the IDBFS mount, because that is where the loader looks.
  var GAME_FILES = { apk: '/assets.apk', so: '/libgame.so' };

  function gameFilesPresent() {
    try {
      return FS.analyzePath(GAME_FILES.apk).exists && FS.analyzePath(GAME_FILES.so).exists;
    } catch (e) { return false; }
  }

  /* IndexedDB gives back whatever structured clone put in, and that is not the
   * same thing on every engine: Safari has been known to hand back a Blob where
   * an ArrayBuffer was written. `new Uint8Array(blob)` is an empty array rather
   * than an error, so the page would happily "start" with a 0-byte APK and sit
   * on a black screen -- so both shapes are accepted, and a size that cannot be
   * a real APK is treated as "not imported". */
  async function asBytes(v) {
    if (!v) return null;
    if (v instanceof ArrayBuffer) return v;
    if (ArrayBuffer.isView(v))
      return v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength);
    if (typeof Blob !== 'undefined' && v instanceof Blob) {
      try { return await v.arrayBuffer(); } catch (e) { return null; }
    }
    return null;
  }

  async function loadGameFromCache() {
    var apk = await asBytes(await idbGet('apk'));
    if (!apk || apk.byteLength < 1024 * 1024) {   // a real APK is tens of MB
      if (apk) console.warn('spike: stored APK is only ' + apk.byteLength + ' B; ignoring it');
      return false;
    }
    var so = await asBytes(await idbGet('libgame.so'));
    if (!so || so.byteLength < 256 * 1024) {
      // Stored before libgame.so was cached separately, or the copy did not
      // survive: derive it again from the APK.
      so = await extractLibgame(apk);
      if (so) await idbPut('libgame.so', so);
    }
    if (!so) return false;
    FS.writeFile(GAME_FILES.apk, new Uint8Array(apk));
    FS.writeFile(GAME_FILES.so, new Uint8Array(so));
    loaded.apk = apk.byteLength;
    loaded.so = so.byteLength;
    var wrote = 0;
    try { wrote = FS.stat(GAME_FILES.apk).size; } catch (e) { wrote = 0; }
    if (wrote !== apk.byteLength) {
      console.error('spike: /assets.apk is ' + wrote + ' B, expected ' + apk.byteLength);
      return false;
    }
    console.log('spike: game files from IndexedDB -- apk ' + apk.byteLength +
                ' B, libgame.so ' + so.byteLength + ' B');
    return true;
  }

  async function extractLibgame(apkBuf) {
    var entries = await zipRead(new Uint8Array(apkBuf));
    var hits = entries.filter(function (e) { return /^lib\/[^/]+\/libgame\.so$/.test(e.name); });
    if (!hits.length) return null;
    return hits[0].data.buffer.slice(hits[0].data.byteOffset,
                                    hits[0].data.byteOffset + hits[0].data.byteLength);
  }

  /* ---- the public API ---------------------------------------------------- */

  M['spike'] = {
    state: function () { return state; },

    /** Called by the page for its first paint. */
    onState: function (cb) { listeners.push(cb); cb(state); },

    /** What is loaded right now, in bytes (0 = nothing). The page needs this to
     *  draw the 清除 button for an APK that came from IndexedDB rather than from
     *  an import in this session. */
    info: function () {
      return { apk: loaded.apk, so: loaded.so, present: gameFilesPresent() };
    },

    /** Import the user's own APK: keep it, extract the image, drop both into
     *  MEMFS so "开始游戏" can run. Returns a small summary for the UI. */
    importApk: async function (arrayBuffer) {
      var u8 = new Uint8Array(arrayBuffer);
      if (!(u8[0] === 0x50 && u8[1] === 0x4B))
        throw new Error('这不是一个 zip/apk 文件（缺少 PK 头）');
      var so = await extractLibgame(arrayBuffer);
      if (!so)
        throw new Error('APK 里没有 lib/armeabi/libgame.so，可能不是 Inotia 3 的包');
      // Blob in storage, bytes in MEMFS: a Blob is what every engine's
      // structured clone stores most predictably, and Safari's IndexedDB has
      // been the one that turned ArrayBuffers into Blobs on read.
      try { await idbPut('apk', new Blob([u8], { type: 'application/vnd.android.package-archive' })); }
      catch (e) { await idbPut('apk', arrayBuffer); }
      await idbPut('libgame.so', new Blob([new Uint8Array(so)]));
      FS.writeFile(GAME_FILES.apk, u8);
      FS.writeFile(GAME_FILES.so, new Uint8Array(so));
      loaded.apk = u8.length;
      loaded.so = so.byteLength;
      // The engine's install step will rewrite /data; keep whatever is there.
      setState('ready');
      return { apk: u8.length, so: so.byteLength };
    },

    /** Forget the imported APK entirely: IndexedDB copy *and* the two files in
     *  MEMFS. The importer is the only way to replace a wrong APK, and a wrong
     *  APK that cannot be removed is a dead end -- so this exists as a button.
     *  Saves are untouched: they live in /data and have nothing to do with the
     *  image that runs them. */
    clearApk: async function () {
      await idbDel('apk');
      await idbDel('libgame.so');
      try { FS.unlink(GAME_FILES.apk); } catch (e) { /* not there */ }
      try { FS.unlink(GAME_FILES.so); } catch (e) { /* not there */ }
      loaded.apk = 0;
      loaded.so = 0;
      setState('no-apk');
      return true;
    },

    /** Everything under /data, as a zip the user can keep. */
    exportSaves: async function () {
      var entries = [];
      (function walk(dir, prefix) {
        var names;
        try { names = FS.readdir(dir); } catch (e) { return; }
        for (var i = 0; i < names.length; i++) {
          var n = names[i];
          if (n === '.' || n === '..') continue;
          var path = dir + '/' + n, rel = prefix ? prefix + '/' + n : n;
          var st = FS.stat(path);
          if (FS.isDir(st.mode)) walk(path, rel);
          else if (FS.isFile(st.mode)) entries.push({ name: rel, data: FS.readFile(path) });
        }
      })('/data', '');
      if (!entries.length) throw new Error('/data 里还没有任何档案');
      var zipped = await zipWrite(entries);
      return { bytes: zipped, files: entries.length,
               total: entries.reduce(function (a, e) { return a + e.data.length; }, 0) };
    },

    /** A zip made by exportSaves (or by hand, paths relative to /data). Files
     *  are written over whatever is there; nothing is deleted first. */
    importSaves: async function (arrayBuffer) {
      var entries = await zipRead(new Uint8Array(arrayBuffer));
      var n = 0;
      for (var i = 0; i < entries.length; i++) {
        writeGuestFile('/data/' + entries[i].name, entries[i].data);
        n++;
      }
      FS.syncfs(false, function (err) {
        if (err) console.error('spike: syncfs after import failed', err);
      });
      return { files: n };
    },

    /** Force pending writes out to IndexedDB (the page calls this before it
     *  tells the user the export finished, and on "开始游戏" for good measure). */
    flush: function () {
      if (typeof FS !== 'undefined') FS.syncfs(false, function () {});
    },

    /** Create the audio context (safe outside a gesture; it comes up suspended). */
    audioPrime: function () {
      try { return sound_host_audio_prime(); } catch (e) { return 0; }
    },

    /** Resume + unlock audio. iOS only grants this inside a user gesture, so
     *  the page calls it from the start click and from every pointerdown. */
    audioUnlock: function () {
      try { return sound_host_audio_unlock(); } catch (e) { return 0; }
    },

    /** Release the boot gate. Everything above only prepared files. */
    start: function () {
      if (!gameFilesPresent())
        throw new Error('还没有导入 APK');
      setState('running');
      if (M['spikeFrames']) { /* headless runs stop on their own */ }
      removeRunDependency('ui-start');
    }
  };

  /* ---- boot: hold main() until the page says start ----------------------- */

  M['preRun'] = M['preRun'] || [];
  M['preRun'].push(function () {
    addRunDependency('ui-start');
    // The context is created here rather than at the first sound: it belongs to
    // this document, it starts suspended on iOS, and the page's click resumes
    // it. Creating it later, from inside the engine's boot, is the case iOS
    // refuses to resume.
    try { M['spike'].audioPrime(); } catch (e) { /* no WebAudio: silent, not fatal */ }
    loadGameFromCache().then(function (ok) {
      setState(ok ? 'ready' : 'no-apk');
    }).catch(function (e) {
      console.error('spike: reading cached game files failed', e);
      setState(gameFilesPresent() ? 'ready' : 'no-apk');
    });
  });
})();
