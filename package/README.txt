========================================================================
 Starfield Always Scan (SFSE)
 Always-on scanner highlighting for Starfield
========================================================================

Version  : 1.7.7  (plugin build 4.27.0)
           1.7.7 = fix: with the scanner put away, already-scanned planet
           targets (ores / gas / liquids / plants) still showed the
           "not scanned" cyan. 1.7.6 fixed the other half (with the scanner
           UP, the vanilla green is back); but once you lower the scanner
           the mod re-hangs the highlight itself, and it only ever used one
           state - the vanilla "not scanned" cyan pulse - so nothing told
           "already surveyed" and "never surveyed" apart. The mod now asks
           the game itself, from two independent sources (either one is
           enough): (a) while you hold the scanner up, it reads which
           outline state the game itself paints each target with - the
           game's green means "already surveyed", and that knowledge is
           remembered for the session; (b) with the scanner away it calls
           the very function the vanilla scanner uses (the one that checks
           whether a resource has entered your survey data) and walks the
           game's own record chain (flora record -> the item it produces ->
           that item's resource), auto-detecting the record layout if the
           game moves it. (1.7.7 was rebuilt with plugin build 4.27.0: source
           (b) above had a silent bug - the record type it had to recognise
           was rejected by the mod's own sanity check, so in practice only
           (a) was doing the work. Both sources are active now, and the log
           names the record offsets it adopts.)
           Already-scanned targets use the game's own
           "scanned" state - GREEN, the same colour and slot the game uses
           while the scanner is up; unscanned ones keep the cyan pulse.
           Scan a deposit, lower the scanner: it turns green within about
           0.2 s. FloraScannedByResource=0 (or StateFloraScanned=7) brings
           the old behaviour back without swapping the DLL.
           1.7.6 = fix: scanned planet targets did not turn green.
           The game paints already-scanned planet targets (ores / gas /
           liquids / plants) with two outline states whose native colour is
           GREEN - and the mod had been colouring those exact two states as
           "devices cyan" and "ammo bright green", so with the scanner up
           you saw cyan (far targets) / bright green (near ones) instead of
           the vanilla green. Those two states are now left completely
           alone (not a single byte written), so the game's own "scanned"
           green is back - near and far. Practical consequences: ammo & aid
           and interactive devices still use those two states and therefore
           now show the game's own green. Write ColorAmmoAid=00FF66 /
           ColorDevice=00E5FF in the INI if you want the old colours back
           (that will hide the vanilla green again). Also new:
           ManagerOccupancyProbe=1 logs how many entries each of the 11
           outline states holds, 1.5 s after you raise the scanner - handy
           evidence if any color ever looks wrong.
           1.7.5 = planet targets light up again, and the real reason why
           "resources" looked exactly like "misc".
           (1) Ores, gas vents, liquid pools and plants had no colour at
           all on planets. 1.7.4 had switched that category off completely
           to hand it back to the vanilla scanner - but the vanilla scanner
           only colors those targets while you are actually holding the
           scanner up, and this mod exists exactly to highlight things with
           the scanner put away. They are back on now, painted with the
           game's own "scannable target" colour (a cyan pulse), and while
           you hold the scanner up the mod steps aside for that category
           only (YieldTargetsWhileScanning=1): it writes nothing, so the
           vanilla "scanned / not scanned / being scanned" colours are
           exactly the game's. Every other category is still refreshed as
           usual, so highlighting beyond the scanner's centre circle keeps
           working while surveying.
           (2) "Resources" and "misc" had the same colour because of a
           read bug: the mod read the game's keyword array in the wrong
           byte order, so no item ever matched and every resource silently
           fell back to the misc colour. Fixed (plus a self-calibrating
           fallback and a log line that says which read layout was adopted).
           Resources are purple again, misc items stay blue.
           1.7.4 = two fixes, both about telling colors apart.
           (1) Planet surveying: the two outline states the vanilla scanner
           uses for scannable planet targets (state 7 / 8) are no longer
           overwritten at all; the "resource" color moved to state 3.
           (2) "Resources" and "misc" items no longer look the same: the
           resource check is on by default (it has its own safety checks)
           and only a clearly failed test turns it off.
           1.7.3 = fix: the 1.7.2 opacity setting turned out to have no
           effect at all. Pixel measurements on a screenshot showed the
           covered area was pixel-identical at 40% and at 100% - the
           engine does not consume that alpha byte, so "semi-transparent"
           never happened on screen. The outline is now drawn as a
           contour: the fill layer is switched off (its alpha is written
           as 0), which is exactly what the engine itself does for its
           scannable targets. You see the item's own material in full,
           with the category colour on the contour. NoFill=0 in the INI
           restores the old filled look. AlphaXxx now only changes the
           pulse (contour) transparency.
           1.7.2 = (superseded, see 1.7.3) the 1.7.1 colors could cover a
           nearby object so completely (doors, weapons, suits) that you
           could no longer see what the item itself looked like; doors,
           weapons and apparel rendered at ~40% opacity (AlphaXxx).
           1.7.1 = two things. (1) The category colors are now picked for
           maximum contrast - the engine's own palette is all blues in
           the range the pick-up items used, which is why guns, suits
           and slates still looked alike. (2) The color override itself
           was writing into the wrong engine table (a float constant
           area), so it never changed anything on screen; the correct
           table is now used and every category is written to it before
           any highlight manager exists. New defaults: weapons RED,
           spacesuits/clothing MAGENTA, ammo & aid BRIGHT GREEN, notes
           YELLOW, resources PURPLE, misc BLUE (unchanged), containers &
           bodies ORANGE, devices CYAN, doors WHITE, flora BRIGHT GREEN.
           See "CATEGORY COLORS" below.
           1.7 = fix: with 1.6 the new category colors could end up not
           showing at all (if you saw the same outline color on guns,
           spacesuits and data slates, this is it). The custom colors
           are now written into the engine's color tables *before* any
           highlight manager is created - a manager only reads those
           tables at creation time and is never refreshed afterwards.
           No INI changes.
           1.6 = lootable items are now color-coded by inventory
           category. See "CATEGORY COLORS" below.
           Also: the configuration file SAS_AlwaysScan.ini now lives
           next to the mod's esm (same folder as the log). If you have an
           older installation with the INI in SFSE\Plugins\, that one is
           still read as a fallback - but move it next to the esm to keep
           both in one place (the build script does this automatically).
           1.5 = the log file is written next to the mod instead of in
           your Documents folder (see TROUBLESHOOTING below). No
           gameplay or INI changes (if you are upgrading, your
           SAS_AlwaysScan.ini is untouched).
           1.4 = crash fixes only: no more crash while quitting the game,
           and no more rare crash during cell transitions / loading.
Author   : hzk11o11k11o (Nexus Mods)
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

Highlights are color-coded by category (all values configurable).
Pick-up items follow the game's own inventory categories:
  * Weapons and throwables (guns, melee, grenades, mines)            RED
  * Spacesuits / helmets / packs / clothing                          MAGENTA
  * Ammo and aid (meds, food, drinks) - the game's own green
    (state 4/5 is shared with the vanilla "scanned planet target" green;
    write ColorAmmoAid=00FF66 for bright green instead)                GREEN
  * Notes (notes, data slates, magazines, books)                     YELLOW
  * Resources (iron, aluminium, helium-3, organics, ...)             PURPLE
  * Misc items (digipicks, credits, toys) - unchanged from 1.5       BLUE
  * Containers (loot the good stuff)                                 ORANGE
  * Bodies / corpses - people, creatures and wrecked robots / turrets
    (anything you can search)                                        ORANGE
  * Interactive devices / computers - the game's own green (same note as
    above; write ColorDevice=00E5FF for cyan instead)                  GREEN
  * Doors                                                            WHITE
  * Flora, ores, gas vents, liquid pools (all one record type in the game
    data) - the game's own "scannable target" colour, a cyan pulse; once
    you have surveyed one it switches to the game's own "scanned" green.
    Since 1.7.5 this category is ON by default, its colours are never
    overwritten (see below) and since 1.7.7 scanned targets stay green
    even with the scanner put away                        CYAN / GREEN

The six pick-up categories sit at hues that are at least ~44 degrees
apart (red / magenta / green / yellow / purple, plus the original misc
blue), so they can be told apart at a single glance. Every category gets
its colour assigned explicitly by the mod (see ColorXxx in the INI),
instead of borrowing the engine's default per-state colours - except the
planet targets and the two categories that share the game's "scanned
target" states, which are deliberately left untouched
(1.7.5 / 1.7.6 / 1.7.7 - see the planet-survey section below).

Since 1.7.3 the outline is drawn as a contour instead of a fill: the
mod writes alpha=0 into the outline's base colour, which is the "no
fill" state the engine itself uses for its scannable targets. You see
the item's own material and shape in full, with the category colour
sitting on the contour / pulse. Set NoFill=0 in the INI to bring the
old filled look back (the 1.7.1 / 1.7.2 behaviour).

Since 1.7.5 / 1.7.6 the mod stays out of the vanilla scanner's way on
planet surfaces while you are surveying. The game marks ores, gas vents,
liquid pools and plants with the same record type (FLOR) and colors them
by "scanned / not scanned" through outline states the vanilla scanner
owns: the "not scanned" look lives in two states (a cyan pulse) and the
"scanned" look in two more whose native colour is GREEN. The mod
overwrites none of those four states' colours (1.7.5 handed the first
pair back, 1.7.6 the second - before that, a scanned deposit showed up as
cyan / bright green and never turned green), and while you hold the
scanner up it leaves that whole category alone (no new outlines, no
refresh, and it does not remove the ones already there, because those
entries live in the manager the game is using right now). The moment you
put the scanner away they are put back (about 0.8 s), painted in the
game's own cyan, so planets are readable with or without the scanner.
Since 1.7.7 that "put back" pass asks the game whether each target has
been surveyed already: the mod calls the game's own check function (the
one the vanilla scanner uses) and walks the game's own record chain
(flora record -> the item it produces -> that item's resource). Targets
you have already scanned come back GREEN - the exact state and colour the
game uses while the scanner is up - and unscanned ones stay cyan, so with
the scanner away you can tell at a glance what is left to survey. Scan
something, put the scanner away: it turns green within ~0.2 s.
FloraScannedByResource=0 (or StateFloraScanned=7) in the INI restores the
old "all cyan" behaviour. Resources vs. misc items: "resources" (iron, aluminium,
helium-3, organics, ...) are purple, misc items (digipicks, credits,
toys) are blue. If the resource check ever fails, the log says so and
everything falls back to the misc color.

"Resources" are recognised from the item record itself (the game marks
them with its own ResourceType keywords), so both vanilla and mod-added
resources get the resource color. If the check does not apply for any
reason the mod simply falls back to the misc color (the log says so).

Looted containers and looted bodies stop glowing as soon as they are
empty, so the outline always means "there is still something to take".
(What a body still carries but you cannot take - the invisible NPC-only
gear it wears, or clothing that is still equipped - does not count as
loot.)

Display cases are covered as well: weapon cases, weapon racks, helmet /
backpack / datapad racks and the outpost display cases glow even while
they are closed (the game does not keep their contents in the normal
container inventory), and they go dark once you have taken everything
out of them.

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
  SAS_AlwaysScan.ini                    (configuration - next to the esm)
  Scripts\SAS_Bridge.pex
  Scripts\Source\SAS\SAS_Bridge.psc     (source code, optional at runtime)
  SFSE\Plugins\SAS_AlwaysScan.dll

Then enable "StarfieldAlwaysScan.esm" in your load order.

------------------------------------------------------------------------
 MANUAL INSTALLATION
------------------------------------------------------------------------
Extract the archive contents into  ...\Starfield\Data\  and merge the
folders when asked.

------------------------------------------------------------------------
 CONFIGURATION
------------------------------------------------------------------------
All options live in  SAS_AlwaysScan.ini  **next to StarfieldAlwaysScan.esm**
(with Mod Organizer 2: inside the mod's own folder; manual install:
...\Starfield\Data\SAS_AlwaysScan.ini). The file is heavily commented and
read once at game start, so restart the game after editing. (1.5 and older
kept it in Data\SFSE\Plugins\ - that file is still read if the new one does
not exist, and the log's "config file:" line tells you which one was used.)

Most useful options:
  StartEnabled=1        start highlighting as soon as you load a save
  HotkeyVK=119          toggle hotkey, 119 = F8 (see the file for a key table)
  RadiusMeters=50       highlight radius around the player
  MaxTargets=256        how many objects are outlined at the same time
  StateWeapon=0         outline color state per category (0..10; the INI
  StateApparel=1        documents them). Defaults:
  StateAmmoAid=5          weapons 0   suits/helmet/pack 1   ammo&aid 5
  StateNote=6             notes 6     resources 3           misc 2 (= 1.5)
  StateResource=3         containers & bodies 9             devices 4
  StateLoot=2             doors 10    flora 7 (= the vanilla scannable-
                        target state, colour not overwritten) and MSTT 2
                        Since 1.7.4 "resources" uses state 3; 7 / 8 (the
                        vanilla "not scanned planet target" states) and
                        4 / 5 (the vanilla "scanned planet target" states,
                        native green) are never touched. Ammo & aid and
                        devices still use 4 / 5, so they show the game's
                        own green by default (1.7.6). Since 1.7.7 already-
                        scanned planet targets use state 5 as well (see
                        StateFloraScanned below) - same state, same green,
                        so nothing changes visually.
  FloraScannedByResource=1
                        ask the game whether a planet target has been
                        surveyed already and paint scanned ones green
                        (default on, 1.7.7). 0 = old behaviour: the whole
                        flora category uses StateFlora only.
  StateFloraScanned=5   state used for already-scanned planet targets
                        (default 5 = the game's own "scanned" green,
                        #27C684). Set it to 7 (= StateFlora) to make
                        scanned and unscanned look the same again.
  ColorWeapon=FF2E2E    exact RGB per category (written by default):
  ColorApparel=FF3BD4     weapons red / suits magenta / notes yellow /
  ColorNote=FFD700        resources purple / misc blue / containers &
  ColorResource=B36BFF    bodies orange / doors white
  ColorContainer=FF9500   ammo & aid and devices are NOT written by
  ColorDoor=FFFFFF        default (they show the game's own green), and
                          so are flora (the game's own cyan) and MSTT.
  ColorAmmoAid / ColorDevice
                        write these (00FF66 / 00E5FF) only if you want the
                        old bright green / cyan back - it will hide the
                        vanilla "scanned planet target" green again.
                        Note that any ColorXxx writes the engine's global
                        per-state color block, so the same state of the
                        vanilla scanner changes too (see the INI).
  NoFill=1              draw the outline as a contour only (default since
                        1.7.3): the fill layer's alpha is written as 0, so
                        the item's own material stays visible.  NoFill=0 =
                        the old filled look (1.7.1 / 1.7.2).
  AlphaWeapon=0         pulse (contour) opacity per category (0-255).
  AlphaApparel=0        0 = keep the engine's own value (default), 255 = a
  AlphaDoor=0           fixed, non-breathing contour.  NOTE: this does not
                        make the fill transparent - use NoFill for that
                        (the alpha byte is ignored by the renderer).
  ResourceByKeyword=1   recognise "resources" from the item record's own
                        ResourceType keywords (default on).  0 = every MISC
                        item counts as misc (the 1.5 behaviour).
  EnableWeapon=1 ...    per-category on/off switches (weapon / apparel /
  EnableResource=1      ammoaid / note / resource / loot(misc) / container /
                        device / door / flora / corpse).
  EnableFlora=1         outline flora / mineral deposits (the game stores
                        ores, gas vents, liquid pools and plants in the same
                        record type). Default ON since 1.7.5 - painted in
                        the game's own cyan, and the mod steps aside while
                        you hold the scanner up, so the vanilla
                        scanned / not-scanned colours stay intact (the
                        "scanned" green is preserved since 1.7.6 - that was
                        the state 4 / 5 colour, see the version notes).
                        Since 1.7.7 surveyed targets come back GREEN with
                        the scanner away too (FloraScannedByResource /
                        StateFloraScanned below).
                        Set it to 0 to leave this category completely to the
                        vanilla scanner (you will only see those targets
                        while the scanner is up), or StateFlora=5 plus
                        ColorFlora=00FF66 for the old always-on green.
  YieldTargetsWhileScanning=1
                        while you hold the scanner up, leave the
                        flora / mineral category alone (recommended, default
                        on). The vanilla scanner paints scannable planet
                        targets itself; if the mod kept refreshing them it
                        would paint over the game's own
                        "scanned / not scanned / being scanned" colours.
                        0 = old behaviour (refresh that category even while
                        the scanner is up). The log's "skip (window)" line
                        shows "yield=N" growing while you survey.
  ManagerOccupancyProbe=1
                        diagnostic (default on): 1.5 s after you raise the
                        scanner, log one line with the number of entries in
                        each of the 11 outline states, e.g. "manager
                        occupancy[举着扫描仪]: 0=5 1=2 ... 10=1". Handy
                        evidence when a colour looks wrong (you can see
                        which states the game itself is writing). Max 6
                        lines per session, read-only.
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
  SkipDisplayCaseEmpty=1  weapon cases / weapon racks and other display
                        cases glow even while closed (default on): the
                        game does not keep their contents in the normal
                        container inventory, so the "looted = empty"
                        check does not apply to them. Set to 0 only if
                        you want the old behaviour back (cases stay dark
                        until opened).
  DisplayCaseUiEmpty=1  a display case you have emptied goes dark and
                        stays dark (default on). The mod tracks what you
                        take out of it, item by item.
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
  ExteriorContinuous=1  walking across an exterior cell border is treated
                        as a continuous transition: highlights are kept
                        (nothing goes dark) and the area you came from is
                        scanned as well, so objects on both sides of the
                        border stay outlined. 0 = old behaviour (everything
                        is dropped and re-scanned on every border cross).
  SettleOnCellCrossMs=300  how long to pause after such a border cross
                        (0 = no pause at all)
  StreamJumpTolerance=256  while you walk, the engine keeps streaming
                        objects in and out. Small changes like that no
                        longer interrupt scanning (0 = old behaviour).
  Verify3DPerScan=32    per pass, re-check this many outlined objects whose
                        3D was rebuilt by streaming (LOD <-> full model) and
                        re-apply the outline if it was lost - this is what
                        cures the rare "it was glowing, then suddenly went
                        dark" case out in the open. 0 = off.
  NotifyOnToggle=1      show a HUD message on toggle
  LogStats=1            write a stats line to the log every 5 seconds

------------------------------------------------------------------------
 TROUBLESHOOTING
------------------------------------------------------------------------
Log file (since 1.5 it lives right next to the mod):
  SAS_AlwaysScan.log - same folder as StarfieldAlwaysScan.esm:
    * Mod Organizer 2 : inside the mod's folder, i.e.
      ...\mods\Starfield Always Scan (SFSE)\SAS_AlwaysScan.log
    * manual install  : ...\Starfield\Data\SAS_AlwaysScan.log
  (In 1.4 and older it was Documents\My Games\Starfield\SFSE\Logs\.)

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
    since 1.7.1 every category has its own colour, so this should not
    happen - but you can change any of them: the "colorOverride" config
    line lists the RGB the mod applies, and "outline colors[...]" prints
    the per-state values (base colour + pulse). The "renderer params"
    lines show what the renderer actually received (base colour = what
    you see on screen). Set ColorXxx=RRGGBB in the INI and restart.
* On a planet, "scanned" and "not scanned" look the same:
    since 1.7.5 the mod writes nothing at all for those objects (ores,
    gas vents, liquid pools, plants - all stored as FLOR records) while
    you hold the scanner up, so the game's own colours are on screen.
    Check that your INI has YieldTargetsWhileScanning=1 and StateFlora=7
    (older INI files are not overwritten by the installer - see the note
    in the INI itself); the log's stats line should show "yield=" growing
    while you survey.
* On a planet, scanned targets do not turn green (they stay cyan, or turn
  bright green / stay cyan far away):
    the "scanned planet target" green is the native colour of outline
    states 4 and 5, and 1.7.6 stopped writing those two states. If you
    upgrade with your own INI, make sure it does not contain
    ColorAmmoAid=... or ColorDevice=... (those write the same two states
    and hide the green again - remove the lines or update the values).
    In the log, the startup line "outline colors[install]: state= 4 ..."
    should show "基色=#27C684" (green), and there should be no
    "state=4 覆盖为" / "state=5 覆盖为" line.
* On a planet with the scanner AWAY, everything is cyan - scanned targets
    do not show green: that is the 1.7.7 behaviour switch. Check the INI for
    FloraScannedByResource=1 and StateFloraScanned=5 (an INI from 1.7.6 or
    older does not contain them - the DLL then uses its built-in defaults,
    so the feature works even without the new keys). In the log look for
    "flora scanned: ... ready" at startup and the stats line
    "planet targets ... : 未扫描=... 已扫描=... | 判据: 查询=... 命中=...";
    "已扫描" (scanned) should grow once you have surveyed something near you.
    If "链失败" (chain failures) grows instead, you are most likely running
    the first 1.7.7 build (plugin 4.26.0): its record chain had a silent bug
    that is fixed in build 4.27.0 (this package), so grab the current file.
    If the current build still shows it, the game's record layout has
    changed - please report that log line plus the "flora probe" lines.
* On a planet, ores / gas vents / liquid pools / plants have no colour
    at all: check EnableFlora=1 in your INI (1.7.4 shipped it as 0).
    Those targets are only painted by the vanilla scanner while it is up,
    so with the category off you will not see them with the scanner away.
* The colour covers the object so you cannot see its material:
    that cannot happen since 1.7.3 - the outline is a contour, not a
    fill.  If you set NoFill=0 yourself, set it back to 1.  Note that
    AlphaXxx cannot fix a fill: it only affects the pulse (the engine
    ignores the alpha byte for the fill, which is why 1.7.2 had no
    visible effect).  Log lines to check: "config: noFill=1 ..." at
    startup, and the "no fill" wording on each color override line.
* Resources are not purple (they show up blue like misc):
    since 1.7.5 the keyword array is read with the correct layout (1.7.4
    and earlier read it byte-swapped, so no item ever matched). The log
    should show "resource keyword: 关键词数组标定 = base+0x208 ..." once
    (the read layout that was adopted) and
    "resource keyword: 首个资源命中 base=0x... -> ..." the first time a
    resource is recognized. If neither line appears, grab the log lines
    starting with "resource keyword:" / "misc kw probe" and send them
    over. (You can also force the check off with ResourceByKeyword=0.)
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
* A display case is highlighted when it should be dark (or the other
    way around):
    display cases (weapon cases, weapon racks, outpost displays) are
    tracked separately: they glow even while closed, and they go dark
    once you have taken everything out of them. Taking only part of the
    contents - or opening one and taking nothing - keeps it glowing, by
    design. If it still looks wrong, send me the log lines starting
    with "display case trace:" and "loot events".
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
* Display cases (weapon cases, weapon racks, outpost display cases) glow
  even while they are closed - the game keeps their contents out of the
  normal container inventory until the case is opened, so they are
  handled separately. Once you take everything out of one, it goes dark
  like any other container; taking part of its contents leaves it lit.
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
  same as the vanilla scanner's reach, limited by RadiusMeters. Going
  through a loading door (building, ship, fast travel) re-scans the new
  area within about a second.
* Outdoors, walking across the invisible border between two worldspace
  cells no longer drops the highlights: what was glowing keeps glowing,
  and the objects on the other side of the border are outlined too while
  you are near it. If a model is unloaded and rebuilt by the engine's
  streaming (very common outdoors), the outline is re-applied
  automatically (Verify3DPerScan).

------------------------------------------------------------------------
 CREDITS / LICENCE
------------------------------------------------------------------------
Built with SFSE and CommonLibSF. Thanks to the SFSE and CommonLibSF
teams, and to everyone who tested and reported issues.

The source code is included (Scripts\Source\SAS\SAS_Bridge.psc); the C++
source lives in the project repository. Do not re-upload this archive.
========================================================================
