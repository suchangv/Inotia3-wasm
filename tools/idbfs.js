// Persist the engine's data directory across page loads.
//
// On emscripten the filesystem is MEMFS unless something replaces it, so /data
// -- the 12 asset files the engine installs on first run, plus every save --
// lives in RAM and a reload throws all of it away. That is the browser build's
// "saves do not work" bug: the engine's save code runs, fwrite succeeds, the
// file is there, and then the page reloads into an empty filesystem and the
// save is gone. It also costs ~12s of reinstalling on every load.
//
// IDBFS is emscripten's IndexedDB-backed filesystem and is the standard answer:
// mount it at /data, pull in whatever a previous session left there *before*
// main() runs, and push writes back out afterwards.
//
// Two details are load-bearing:
//
//   * The initial load is a run dependency, not a race. If main() starts while
//     FS.syncfs(true) is still in flight the engine sees an empty directory and
//     reinstalls everything, which is exactly what we are trying to avoid.
//   * The mount is guarded against preRun running twice. Deferring main() means
//     emscripten calls run() again once the dependency is released, which runs
//     preRun a second time; without the flag that would mount twice and add a
//     dependency nobody ever releases.
//
// ?nostore=1 skips the mount, which is how the difference gets measured.

(function () {
  var M = (typeof Module !== 'undefined') ? Module : (Module = {});
  var mounted = false;

  M['preRun'] = M['preRun'] || [];
  M['preRun'].push(function () {
    if (mounted) return;
    mounted = true;

    if (/[?&]nostore=1/.test(location.search)) {
      console.log('spike: ?nostore=1 -- /data stays in memory, saves will not persist');
      return;
    }
    if (typeof FS === 'undefined' || typeof IDBFS === 'undefined') {
      console.warn('spike: IDBFS unavailable; /data stays in memory, saves will not persist');
      return;
    }

    var dir = '/data';
    try { FS.mkdir(dir); } catch (e) { /* already exists, which is the normal case */ }
    try {
      FS.mount(IDBFS, {}, dir);
    } catch (e) {
      console.warn('spike: IDBFS mount failed; saves will not persist', e);
      return;
    }

    function flush(why) {
      FS.syncfs(false, function (err) {
        if (err) console.error('spike: IDBFS flush (' + why + ') failed', err);
      });
    }

    // A browser can unload or freeze the page between frames, so the tick-based
    // flush in platform_emscripten.c cannot be the only one. These two events
    // fire before the page goes away.
    addEventListener('pagehide', function () { flush('pagehide'); });
    addEventListener('beforeunload', function () { flush('beforeunload'); });
    document.addEventListener('visibilitychange', function () {
      if (document.visibilityState === 'hidden') flush('hidden');
    });

    addRunDependency('idbfs-load');
    FS.syncfs(true, function (err) {
      if (err) console.error('spike: IDBFS load failed; starting with an empty /data', err);
      else {
        // List what came back, one level down as well: "is the save there?" is
        // the only question this log exists to answer, and a bare /data listing
        // shows just the package directory either way.
        var lines = [];
        try {
          FS.readdir('/data').forEach(function (n) {
            if (n === '.' || n === '..') return;
            var sub = '/data/' + n, names = null;
            try {
              if (FS.isDir(FS.stat(sub).mode)) names = FS.readdir(sub);
            } catch (e) { /* not a directory */ }
            lines.push(names && names.length ? n + '/ {' + names.join(', ') + '}' : n);
          });
        } catch (e) { /* nothing stored yet */ }
        console.log('spike: /data restored from IndexedDB: ' +
                    (lines.length ? lines.join(', ') : '(empty, first run)'));
      }
      removeRunDependency('idbfs-load');
    });
  });
})();
