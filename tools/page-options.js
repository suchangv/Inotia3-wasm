// Read the run's options from the page URL, so a headless run can vary them
// without a rebuild.
//
//   spike.html?frames=3000&gltrace=200&dump=1&fixed=1
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
  if (q.has('fixed'))   M.spikeFixed = num('fixed');
  // The engine's frame period, in ms. Off unless the page asks: the engine's
  // own 66 ms is the game's speed, and lowering it accelerates the game. This
  // exists to measure that, not to ship it -- see src/patches.c.
  if (q.has('framems')) M.spikeFrameMs = num('framems');

  // Do not let stdout go into the shell's <textarea id="output">.
  //
  // The generated HTML defines Module.print as "console.log(...) and then
  // append the line to the output textarea, then set scrollTop = scrollHeight".
  // The engine logs one line per frame, so that is a forced DOM reflow per
  // frame on a string that grows for the whole run -- the cost per line grows
  // with the run. On the idle main menu, where the engine does 0.26 ms of work
  // per frame, the requestAnimationFrame callback was taking 47.6 ms and the
  // page ran at 17 fps; detaching the textarea mid-run took the same page to
  // 3.4 ms and 60 fps. The loader went from 89 s to 23 s.
  //
  // emscripten binds its stdout sink once, at module init
  // (`if(Module["print"])out=Module["print"]`), and --pre-js runs before that,
  // so replacing the two here is what routes stdout to the console and
  // nowhere else.
  M.print = function (line) { console.log(line); };
  M.printErr = function (line) { console.error(line); };
})();
