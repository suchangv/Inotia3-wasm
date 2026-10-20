Inotia 3 -- Children of Carnia
==============================

A PortMaster port of the Android game, running its original ARM libgame.so
under the same interpreter the browser and Android builds use. No reimplementation
of the game: the ELF from your own APK is the game.

What you have to supply
-----------------------
Two files from your legally-obtained copy of the APK, placed in this directory
(/roms/ports/inotia3/):

    libgame.so     from lib/armeabi/libgame.so inside the APK
    assets.apk     the APK itself (renamed to assets.apk)

Nothing else is needed. Saves and the engine's installed data go into
./data/ and survive updates.

If the game does not start, inotia3.log has the reason -- including
"GLIBC_2.xx not found", which means this build is newer than the firmware's C
library and needs rebuilding against an older sysroot (see
docs/PORTMASTER.md in the source tree).

Controls
--------
Left stick     virtual mouse pointer over the picture; it hides after 3 seconds
               of no input
L3 (stick)     mouse button: press and release is a click, holding drags
Start          pause menu (from gameplay)
Escape (keyboard) / closing the window quits

The button-to-HUD mapping (d-pad movement, A/B/X/Y skills, L1/R1/L2 items) is
the next step -- this build is the pointer, to get into the game and be sure the
picture, sound-less boot and input plumbing are right on the device.

Logs
----
inotia3.log holds everything the port prints, including a crash report that
names the guest PC and registers. That file is the first thing to look at if the
game does not start.

Known gaps
----------
* No sound yet (the engine runs; the audio backend is the next step).
* Buttons other than L3 do nothing yet.
* Verified on a PC with Mesa; the handheld is what this package is for.
