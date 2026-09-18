========================================================================
 Starfield Always Scan (SFSE)
 Always-on scanner highlighting for Starfield
========================================================================

Version  : 4.6.0
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
(What a body still carries but you cannot take - the invisible NPC-only
gear it wears, or clothing that is still equipped - does not count as
loot.)

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
  TreatNullInvAsEmpty=1 a reference whose inventory list was never created
                        counts as empty (default on). Set it to 0 only if
                        you ever find a body / container you never looted
                        staying dark - that reverts to "always outline".
  CorpseUnconscious=1   also outline "unconscious" units (default on).
                        In the vanilla data most of those are wrecked
                        robots and turrets you can loot; a few are
                        knocked-out living characters (also lootable).
                        Set to 0 if you ever see a walking NPC outlined.
  CorpseLifeState=1     decide "is it dead?" through the engine's own
                        life-state check (this is what makes enemies you
                        kill light up the moment they die). Set to 0 only
                        for debugging.
  CorpseBleedout=1      also outline units that are downed / bleeding out
                        (default on; they are lootable)
  SkipNonPlayableLoot=1 bodies carry invisible NPC-only gear that you
                        cannot take and the loot panel does not show.
                        Ignoring those entries is what lets a fully
                        looted body go dark (default on). Set to 0 only
                        for debugging.
  SkipEquippedLoot=1    same for gear the dead actor is still wearing -
                        it cannot be taken either, so it does not count
                        as loot (default on). This is what makes enemies
                        killed in combat go dark once looted. Set to 0
                        only for debugging.
  ActorProbeMax=24      how many per-actor diagnostic lines ("actor
                        probe:") the log prints per session. Set to 0 to
                        turn them off; they are only useful when reporting
                        a "this one should/should not be highlighted" bug.
  ActorChangeProbeMax=32  diagnostic: one line whenever an actor's verdict
                        changes (for example the moment you kill it)
  LootProbeMax=16       diagnostic: for bodies that still count as "has
                        loot", list what is left inside them. This is the
                        line to send when reporting a "still glowing after
                        looting" bug. 0 = off.
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
    - the log's "actor probe:" lines list every actor around you (dead or
      alive) with its raw flags and the verdict - send me the line for
      that body;
    - if the inventory offset calibration has not succeeded yet, containers
      and bodies are always outlined (the log says so). That is a safe
      fallback, not a crash - it retries until it succeeds.
* A looted (empty) body / container is still highlighted:
    the log's "corpse (window)" line has the counters
    (empty / notEmpty / unknown / null / shapeBad), and the
    "loot probe:" line lists exactly what is still inside. Worn gear
    and NPC-only gear are expected to appear there - they cannot be
    taken and are ignored on purpose (the log's counters
    "判空跳过: np=... eq=..." show the rule working). If a loot probe
    still lists playable items, send me that line plus the calibration
    line.
* An enemy you killed stays highlighted after you loot it:
    same as above - send me the "loot probe:" and the
    "actor probe (changed):" lines. Killing is detected through the
    engine's own life state, so the log also shows the moment the
    verdict changed.
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
  everything from them. What cannot be taken (invisible NPC-only gear,
  and clothing a killed actor is still wearing) does not count as loot.
* With "Simple Immersive Looting" (Nexus 12677) installed, using its
  "Strip" option unequips a body's gear - the body starts glowing again
  because the gear is takeable now; loot it and the outline goes away.
* The log is capped at 1 MiB: once it grows past that it is emptied and
  starts over, so it can never fill up your disk.
* Since 4.3 the inventory calibration keeps retrying until it succeeds
  (it samples nearby containers or actors), so "looted means dark" also
  starts working when your first area had nothing to sample from.
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
