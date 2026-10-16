// Read the run's options from the page URL, so a headless run can vary them
// without a rebuild.
//
//   spike.html?frames=3000&gltrace=200&dump=1
//
// The C side reads these out of Module at startup (see platform_emscripten.c),
// so they have to be set before the module runs -- which is what --pre-js
// guarantees. Without this every experiment is an edit and a 40-second
// relink, and the flags are exactly the ones a black screen makes you want to
// change repeatedly.
(function () {
  var q = new URLSearchParams(location.search);
  function num(name) {
    var v = q.get(name);
    return v === null ? 0 : (parseInt(v, 10) || 0);
  }
  // Module may not exist yet; --pre-js runs before the glue defines it, so
  // create it and let the glue extend it.
  var M = (typeof Module !== 'undefined') ? Module : (Module = {});
  if (q.has('frames'))  M.spikeFrames = num('frames');
  if (q.has('gltrace')) M.spikeGlTrace = num('gltrace');
  if (q.has('dump'))    M.spikeDumpScreen = num('dump');
})();
