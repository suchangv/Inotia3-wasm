// Report where a wasm trap actually happened.
//
// A trap inside the run surfaces as an uncaught exception, and emscripten's
// shell prints only the message ("memory access out of bounds") -- no function,
// no line, nothing that names a call site. The Error object still carries the
// stack; nothing prints it. These two listeners do, and they are injected with
// --pre-js so they are installed before any of the run starts.
//
// The frames name wasm functions because the debug builds pass --profiling-funcs
// (and -g for the JS glue). In an -O2 build without those the names are just
// indices, which is why this is wired into the debug target only.
(function () {
  function report(what, e) {
    var err = e && (e.error || e.reason || e);
    console.error('[trap] ' + what + ': ' + (err && err.message ? err.message : err));
    if (err && err.stack) console.error('[trap] ' + String(err.stack).replace(/\n/g, ' | '));
  }
  window.addEventListener('error', function (e) { report('error', e); });
  window.addEventListener('unhandledrejection', function (e) { report('rejection', e); });
})();
