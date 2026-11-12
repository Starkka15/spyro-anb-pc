The Legend of Spyro: A New Beginning -- PC port
================================================

A native Windows version of the original Xbox game, made by static
recompilation: the Xbox program is translated into PC code ahead of time, so
the game runs directly instead of in an emulator.

This port contains none of the game. It needs your own copy of The Legend of
Spyro: A New Beginning for the original Xbox (title VU-074, version 1). Setup
copies the game's files from it: a folder with the game's files, the disc in a
drive that can read it, or an Xbox disc image (.iso, full or trimmed).

This is an early release: only the first level has been played through, so
expect bugs further in. Reports are welcome at
https://github.com/Starkka15/spyro-anb-pc/issues


Playing
-------

Start "The Legend of Spyro - A New Beginning" from the Start menu. It opens the
launcher, where the settings are; Play starts the game. "Play (skip launcher)"
starts the game straight away with the settings you saved.

  Alt+Enter     fullscreen on/off (remembered)
  F9            release the mouse (press again to capture it)
  Close window  quit to the desktop (a save in progress is finished first)

An Xbox-style controller (or any controller Windows sees as one) works, and so
do the PlayStation 5 DualSense and PlayStation 4 DualShock 4, over USB or
Bluetooth, with no extra software. Controllers can be plugged in or out at any
time. If the controller you are using is unplugged, the game pauses; plug it
back in, or press a key, to carry on.

The on-screen button prompts show Xbox, PlayStation or Nintendo Switch buttons.
"Button prompts" in the launcher sets which; Auto picks PlayStation for a Sony
controller, Switch for a Nintendo one, and Xbox for everything else. Some
controllers present themselves as Xbox ones (many third-party pads, adapters,
Steam Input), so Auto can't tell what they are: choose the style yourself.


Voices and text from the European PS2 disc
------------------------------------------

The Xbox game's cutscenes only have English voices, and some lines of its
translations were left in English. The European PS2 release (SLES-54359) has
the cutscenes dubbed in French, Spanish, German, Italian and Dutch and finishes
more of the text. If you have it, the launcher can copy both from it:

  1. Click "Add from PS2 disc..." in the launcher.
  2. Choose the PS2 disc in a drive, a folder with its files, or an .iso of it.
  3. Choose the language under Language.

This needs about 700 MB. The PS2 disc isn't needed afterwards, and the game's
own files are not changed: the voices and text are kept in the "lang" folder
beside the game. The US PS2 disc has English cutscenes only and won't work.

Lines that talk about the Xbox console (the autosave warning, the Dashboard)
are reworded for PC in every language, with or without the PS2 disc.


Keyboard and mouse
------------------

  Move            W A S D
  Camera          mouse
  Jump / glide    Space          (Enter in menus)
  Melee attack    left mouse     (Backspace = back in menus)
  Breath (hold)   right mouse
  Ranged breath   E, middle mouse
  Charge          Shift
  Strafe          Ctrl
  Fury            F
  Breath element  1 2 3 4, or the arrow keys
  Pause           Esc
  Back button     Tab

On-screen button prompts follow whatever you used last: keys and the mouse
when you play with them, the controller's buttons when you pick up a pad.

Every key can be changed in mkb.ini ("Key bindings..." in the launcher). The
file explains itself; delete it to get the defaults back.


Settings
--------

The launcher writes spyro.ini beside the game. You can edit it by hand too:

  Fullscreen, window size     Alt+Enter and resizing the window are remembered
  Widescreen                  the game's own 16:9 mode (1) or 4:3 (0)
  Sharpness (RenderScale)     the game draws at 640x480 times this; Auto
                              matches your monitor
  VSync                       no tearing, slightly more input lag
  Skip intro movies           straight to the title screen
  Language                    English, French, Spanish, German, Italian or
                              Dutch; Auto follows Windows
  Button prompts              Auto, Xbox, PlayStation or Nintendo Switch
  Console (spyro.ini only)    US or PAL: what console the game thinks it
                              runs on. PAL makes the game switch languages
                              itself, the way a European Xbox does
  Log                         write spyro.log, for bug reports


Saves
-----

Saves are kept in %LOCALAPPDATA%\xboxrecomp (the launcher's "Saves folder"
button opens it). Uninstalling leaves them alone.


If something goes wrong
-----------------------

If the game stops with an error, it writes crash.txt beside the game and says
so. For anything else, set Log = 1 in spyro.ini, reproduce the problem, and
include spyro.log with your report.

The game needs Windows 10 or 11 (64-bit) and a DirectX 11 graphics card. On
"N" editions of Windows, install the Media Feature Pack for the game's music
and movies.


Credits
-------

The Legend of Spyro: A New Beginning was developed by Krome Studios and
published by Sierra Entertainment; Spyro is a trademark of its owner. This port
is not affiliated with or endorsed by any of them.

Built with xboxrecomp (MIT). It includes code from xemu under the LGPL 2.1;
see LICENSES.txt.
