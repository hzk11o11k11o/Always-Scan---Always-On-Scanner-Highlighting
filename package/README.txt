========================================================================
 Starfield Always Scan (SFSE)
 Always-on scanner highlighting for Starfield
========================================================================

Version  : 4.2.1
Author   : (fill in your Nexus username before uploading)
Game     : Starfield 1.16.244.0 (matching version required)
Requires : SFSE (Starfield Script Extender) 0.2.21+
           "(1.16.244.0) SFSE Address Library"

------------------------------------------------------------------------
 WHAT IT DOES
------------------------------------------------------------------------
Tired of holding the handheld scanner to see what is worth picking up?
This mod keeps the scanner highlight on at all times, even when you have
a weapon or a tool in your hands, and it highlights everything around
you inside a configurable radius instead of only the small circle in the
center of the screen.

The visuals are the game's own outline system - the exact same colors
and thickness the vanilla scanner uses - so it looks like the vanilla
feature, just always on.

Highlights are color-coded by category (all values configurable):
  * Lootable items (misc / books / armor / weapons / ammo / aid ...)  blue
  * Containers (loot the good stuff)                                 orange
  * Bodies / corpses - people, creatures and wrecked robots / turrets
    (anything you can search)                                        orange
  * Interactive devices / computers                                  green
  * Doors                                                            red
  * Flora (harvestable plants)                                       green

Looted containers and looted bodies stop glowing as soon as they are
empty, so the outline always means "there is still something to take".

Press the toggle hotkey (default: F8) to switch the whole thing on or
off at any moment. A short HUD message confirms the new state.

------------------------------------------------------------------------
 REQUIREMENTS
------------------------------------------------------------------------
* Starfield version 1.16.244.0 (the DLL resolves engine functions by
  Address Library ID; other versions need a rebuild).
* SFSE (Starfield Script Extender) 0.2.21 or newer.
* The latest "(1.16.244.0) SFSE Address Library" (a load-order plugin).

------------------------------------------------------------------------
 INSTALLATION (mod manager - recommended)
------------------------------------------------------------------------
Install this archive with Mod Organizer 2 or Vortex. The archive root is
the Data folder layout, so its contents map to  ...\Starfield\Data\ :

  StarfieldAlwaysScan.esm
  Scripts\SAS_Bridge.pex
  Scripts\Source\SAS\SAS_Bridge.psc     (source code, optional at runtime)
  SFSE\Plugins\SAS_AlwaysScan.dll
  SFSE\Plugins\SAS_AlwaysScan.ini

Then enable "StarfieldAlwaysScan.esm" in your load order.

------------------------------------------------------------------------
 MANUAL INSTALLATION
------------------------------------------------------------------------
Extract the archive contents into  ...\Starfield\Data\  and merge the
folders when asked.

------------------------------------------------------------------------
 CONFIGURATION
------------------------------------------------------------------------
All options live in  Data\SFSE\Plugins\SAS_AlwaysScan.ini
(the file is heavily commented). The game reads it once at startup, so
restart the game after editing.

Most useful options:
  StartEnabled=1        start highlighting as soon as you load a save
  HotkeyVK=119          toggle hotkey, 119 = F8 (see the file for a key table)
  RadiusMeters=50       highlight radius around the player
  MaxTargets=256        how many objects are outlined at the same time
  StateLoot=2           outline color state per category
  EnableOther=0         also outline movable statics (crates, tables).
                        Off by default: they cannot be picked up and the
                        vanilla scanner does not outline them either.
  EnableCorpse=1        outline dead bodies (people and creatures, default on)
  SkipEmptyLoot=1       stop outlining containers / bodies once they are
                        empty (default on; see "corpses and empty
                        containers" in the INI for the details)
  CorpseUnconscious=1   also outline "unconscious" units (default on).
                        In the vanilla data most of those are wrecked
                        robots and turrets you can loot; a few are
                        knocked-out living characters (also lootable).
                        Set to 0 if you ever see a walking NPC outlined.
  NotifyOnToggle=1      show a HUD message on toggle
  LogStats=1            write a stats line to the log every 5 seconds

------------------------------------------------------------------------
 TROUBLESHOOTING
------------------------------------------------------------------------
Log file:
  Documents\My Games\Starfield\SFSE\Logs\SAS_AlwaysScan.log

At startup the log prints the active configuration and whether the
native outline functions were found ("native outline ready"). While
playing it prints a stats line every 5 seconds (object counts, timings,
categories).

* Nothing is highlighted:
    - check that the plugin "StarfieldAlwaysScan.esm" is enabled;
    - check the log for "forms bound" / "native outline ready";
    - make sure the Address Library matches your game version.
* Some things are highlighted that you do not want:
    set the matching EnableXxx=0 in the INI. The log's "candTypes" line
    prints the form type of everything that was considered.
* Colors are hard to tell apart:
    the log's "stateByCategory" line shows which state each category
    uses; change the StateXxx values and restart.
* A specific body is not highlighted:
    - it may be empty (nothing left to take) - that is intended;
    - a container / body stays lit when the inventory cannot be read
      (the log then says "搜空判空**关闭**" in the calibration line);
      that is a safe fallback, not a crash. Send me that log line.
* A walking NPC is outlined (should not happen):
    set CorpseUnconscious=0 in the INI - the log line starting with
    "corpse probe:" will tell you which reference it was.

------------------------------------------------------------------------
 NOTES
------------------------------------------------------------------------
* The mod only decides *which* references the engine outlines. It never
  touches activation, pick up, dialogue or any other interaction.
* Living NPCs and creatures are never outlined. Dead bodies - humans and
  creatures alike - are, and they stop glowing once you have taken
  everything from them.
* Using the vanilla handheld scanner is fine: when you put it down, the
  mod automatically re-applies its highlights (ResyncOnScannerClose=1).
* Objects are highlighted inside the current cell / loading space, the
  same as the vanilla scanner's reach, limited by RadiusMeters.

------------------------------------------------------------------------
 CREDITS / LICENCE
------------------------------------------------------------------------
Built with SFSE and CommonLibSF. Thanks to the SFSE and CommonLibSF
teams, and to everyone who tested and reported issues.

The source code is included (Scripts\Source\SAS\SAS_Bridge.psc); the C++
source lives in the project repository. Do not re-upload this archive.
========================================================================
