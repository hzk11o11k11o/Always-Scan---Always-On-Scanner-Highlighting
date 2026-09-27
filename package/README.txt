========================================================================
 Starfield Always Scan (SFSE)
 Always-on scanner highlighting for Starfield
========================================================================

Version  : 2.0  (plugin build 5.1.0)
           2.0 = the mod brings its own outline colour channels (described
           below), plus fifteen fixes, all from player reports.
           (1) "Ores and plants are green before *and* after scanning."
               The mod remembers a confirmed "surveyed" verdict so a target
               cannot fall back to cyan (added in 1.7.9 / 1.8.1), but that
               memory was keyed by *species* only and was kept across save
               files - so a plant surveyed once in one save turned that whole
               species green in every other save as well, before you had
               surveyed anything there, which is exactly what "green before I
               scan it" looks like. (Inside one save a species-wide verdict
               is correct - the game itself tracks species, not single
               plants; see fix (4) below.) The mod now records the individual
               reference you actually confirmed, and the species-wide
               shortcut is rebuilt per session from what this save confirmed
               - it never leaks into another save. The file next to the esm
               (SAS_AlwaysScan.flora-learn.txt) stores one "reference +
               species" pair per line; lines in the old (species-only)
               format are ignored - the log says how many - and you can
               simply delete the file to start fresh.
               One-off side effect when you update: things you surveyed with an
               older version are no longer known to the mod (the old memory
               was species-based and is skipped). Scanning them once - or
               simply raising the scanner for a moment - records them again.
               That is the intended reset.
           (2) "Frame rate keeps dropping the longer I play (1.8.1)."
               The "read the state the game itself painted" step, added in
               1.8.1, used the game's own lookup-or-**add** function. When
               the object is not in the game's table yet, that function
               *inserts* an entry and takes a reference count on the object.
               The step runs for every plant / deposit around you, several
               times a minute, so the game's table - and the memory those
               references pin - kept growing for the whole session. That is
               exactly the "it gets slower the longer I play" report, and
               1.7.3, which predates that step, was fine. The mod now walks
               that table **read-only**: nothing is inserted, no reference
               count is taken. The periodic log line now also reports the
               table's entry count (`引擎状态表: 条目=N`) so you can watch
               it - it should stay roughly flat no matter how long you play.
           (3) "A few targets stay cyan after you scan them." Fixed
               2026-09-27. Two things were wrong in the check that decides
               whether a surveyed target turns green. First, the mod reads
               the state the game itself painted (green = surveyed) by
               walking the game's own "reference -> state" table read-only -
               but it was pointed at the wrong address (the constant was
               computed one digit short while adding a rip-relative
               displacement), so that read always came up empty: the
               periodic log line showed a nonsense entry count
               (`条目=18446744073709551615`, `读=0`) and the strongest piece
               of evidence - "the game itself painted this one green" - was
               never actually used. That left the fallback query, and the
               game answers "I don't know" (neither surveyed nor not) for
               about one object in five, so those stayed cyan. Second, a
               "not surveyed yet" note learned for an object used to end the
               check right there, so nothing could ever upgrade it later.
               Both are fixed: the address is corrected (0x5F39CE0, and it
               cross-checks with the manager array that sits 0x10 further
               on) and a "not surveyed" note is now a hint only - the game's
               own query and the survey-data check can still turn the object
               green, while a confirmed "surveyed" verdict remains one-way.
               Nothing to configure; the startup log line now says
               `RVA 0x5F39CE0` and the stats line gained `引擎状态: 未知=N`.
           (4) "Some plants stay cyan after I scan them - opening the scanner
               and closing it again turns them green." Fixed 2026-09-27.
               "Surveyed" is a *species-wide* fact in the game: survey one
               plant and the game paints every plant of that species green.
               The mod's memory, however, was keyed by the individual object,
               so plants the game never painted green during your scanner
               sweep - out of its evaluation range, behind you, or past its
               per-frame limit - were never learned, and stayed cyan until a
               later sweep happened to paint them, which is why raising and
               lowering the scanner "fixed" them. A confirmed verdict is now
               spread to the whole species / resource, exactly like the game
               does it: surveying any one instance turns every instance of it
               green. The species table lives in memory only and is never
               written to disk, so it cannot leak into another save; after a
               game restart it is rebuilt from the per-object records as you
               walk past them. Nothing to configure; the stats line gained
               `按物种扩散(★v5.1.2)` counters.
           (5) "The frame rate still drops." Fixed 2026-09-27. Fix (2) stopped
               the mod from growing the game's own table; what remained was
               the cost of the scan itself. Five times a second the mod
               walks the object list of the cell you are in *and* of the
               recently visited cells around it (that is what keeps objects
               just across a border outlined). Together those lists may hold
               up to 60,000 references, and walking that much took about
               55 ms per pass in a busy scene - a fifth of a second's worth
               of main-thread work, five times a second, which shows up as a
               steady frame-rate drop outdoors and in dense areas. The log's
               scan timings make it visible: `timing2: ... loop avg=55ms`.
               Each surrounding cell's list is now walked **in slices** -
               one slice per pass, rotating - so a pass touches about a
               fifth of it. The cell you stand in is always walked in full,
               so targets in view react immediately, and small lists (2000
               references or fewer) are never sliced. The number of slices
               is capped automatically so that one full rotation always
               finishes well inside the existing "grace period" (how long a
               highlight that left the selection is kept before it is
               removed): 5 slices x 200 ms = 1 s against a 1500 ms grace, so
               slicing can never make a highlight flicker. Nothing to
               configure; RingSliceMaxRounds=5 (0 disables the slicing and
               restores the old behaviour) tunes it, and the periodic log
               line now breaks the scan time down: `timing2: ... (refs/scan:
               cur=... ring=... | walk=...ms loot=...ms flora=...ms) |
               sync ... (unh=... add=... 3D=...ms)`.
           (6) "The frame rate looks fine when I stand still, but stutters in
               busy scenes - combat, or while moving when new items get
               outlined." Fixed 2026-09-27. Fix (5) assumed the scan cost was
               proportional to the number of objects it walks, but the log
               disproved that: the same ~1540 objects cost 10 ms while
               standing still and 63 ms while moving. What actually changes is
               the price of a single operation: the mod reads game memory and
               checks it is readable through ReadProcessMemory and
               VirtualQuery, and both of those have to take the process
               address-space lock. While the game streams the world (walking,
               combat) that lock is busy, so a single call goes from ~10 us to
               a few hundred microseconds - and a scan makes hundreds of them.
               Both paths are now lock-free: memory is read directly with a
               hardware-exception guard, and the readability check probes one
               byte per page. On top of that, the inventory check that decides
               whether a container or corpse is empty (which ran for every one
               of them, five times a second) is now cached per object and
               invalidated by the actual events - taking or putting an item,
               opening or closing the loot UI - so "loot it empty and the
               highlight goes out" still reacts exactly as before. A startup
               self-test verifies the direct reads (including that an
               unreadable address is caught safely) and prints the cost of
               both methods in the log; if it ever fails, the mod falls back
               to the old behaviour automatically. Two new ini keys: FastReadMem=1
               (0 = old behaviour) and LootCacheTtlMs=1500 (0 = check every
               pass). The stats line gained a `timing3:` row that breaks the
               scan down in microseconds and reports how much of the time was
               spent stalled rather than working.
           (7) "Planet resources I have not surveyed show up green right away -
               especially after loading an older save." Fixed 2026-09-27.
               "Surveyed" is a fact of *the save you have loaded*: the game's
               own check reads the survey data of that save, and that data
               goes backwards when you load an earlier save. The mod's memory
               was not tied to a save at all, so what one playing session had
               learned stayed valid after loading a different (older) save and
               those objects were still treated as surveyed there. A second
               layer made it worse - fix (4)'s species-wide table was global,
               so a species confirmed on one planet also turned the same
               species green on every other planet, before you had surveyed
               anything there.
               Two scopes are now built in.
               (a) Save scope: the mod subscribes to the game's load-game
                   event. Every time you load a save the memory's scope is
                   bumped, so only what *this* save confirmed still counts;
                   the species table is cleared with it and is rebuilt only
                   from what this save confirms. In addition, the survey-data
                   check is asked once per load for every resource the memory
                   mentions - when it shows a resource is not in this save's
                   survey data, that species' records are deleted outright
                   (the game's own check reads the current save, so this is a
                   proof, not a guess).
               (b) Planet scope: the species table now records the worldspace
                   the species was confirmed in and only spreads inside that
                   same planet.
               Long-lived memory still works the way it used to across game
               restarts: the first save you load in a session is trusted (the
               "continue where I left off" case); every later load in the same
               session trusts only what that save confirmed.
               Trade-off, stated honestly: after loading a save, surveyed
               *plants* read as not-surveyed cyan until you raise the scanner
               once (the game paints them green at that point and the mod
               learns it again). Ores / gas / liquid are unaffected, because
               the survey-data check answers for them right away. Two new ini
               keys: FloraMemoryScope=1 (0 = old behaviour, 2 = strictest:
               never trust the remembered file), FloraSpeciesPlanetScope=1
               (0 = global spread; note: fix (10) made 0 the default again).
               The stats line gained a
               `记忆作用域(★v5.1.5): 读档= 事件= 跳过= 翻案= 重见证= 链复核: 丢=...` block
               and `按物种扩散 ... 星球外拒=`.
           (8) "After travelling / changing maps, already-surveyed items turn
               back to the pre-survey cyan; raising the scanner once turns
               them green again." Fixed 2026-09-27. Fix (7) treats the game's
               load-game event as the moment a save is loaded and wipes the
               remembered verdicts there. It turns out the game also sends
               that event for some travels / map changes (the log of the
               reported session shows the travel being handled as "load #2"),
               so a trip was mistaken for loading an older save and true
               verdicts were thrown away - one look at the scanner afterwards
               proved it: the game repainted the very same objects green,
               meaning they really were surveyed in this save.
               The load event is now only a *candidate*. The mod waits four
               seconds after the loading screen closes (so the world is
               settled), then asks the survey-data check about every resource
               the memory mentions:
                 - no resource at all can be disproved => this was a travel or
                   a reload of the same save (the survey data did not go
                   backwards) => the memory is kept as it is (nothing is
                   dropped, the scope is not bumped, the species table is not
                   cleared), and resources the check confirmed as surveyed are
                   re-anchored to the current scope;
                 - at least one resource is disproved => this really was an
                   older save => the fix-(7) behaviour applies (scope bump,
                   species table cleared, the disproved records dropped).
                   (Corrected in fix (9): nothing is deleted any more - the
                   records are only taken out of scope.)
               Resources whose records were only ever confirmed on another
               planet / worldspace are skipped during that check - asking
               about them from the current one can only produce a false
               "not surveyed". One new ini key: FloraLoadBoundaryEvidence=1
               (0 = old behaviour, every load event is treated as a real
               load). The stats line gained a
               `读档边界(★v5.1.6): 事件保留= 确认base= 推进=` block.
           (9) The same report as (8), sent again after fix (8): "after
               travelling / changing maps, already-surveyed items turn back
               to cyan; raising the scanner once turns them green again."
               Fixed 2026-09-27. The evidence this time was complete: in the
               reported session a *plain* save load was reviewed, the survey
               check answered "not surveyed" for nine resources and 132
               remembered references were deleted right there - while the
               game itself, ten seconds later, painted those very resources
               green (they *are* surveyed in that save), and eighty seconds
               later the same survey check answered "surveyed" for one of
               them. In other words: right after a load screen, the "not
               surveyed" answer of that check has no contrast at all (a
               not-yet-ready lookup answers the same way), so fix (8) was
               building an irreversible decision on it.
               The load boundary now uses the *game clock* instead
               (`Calendar::gameDaysPassed`, which is stored inside the save
               and restored when it is loaded). Every remembered reference is
               stamped with the game time it was learned at (third field of
               the flora-learn file), and every load candidate records the
               game time of the save that was just loaded as an anchor:
                 - learned at / before the anchor => that scan happened in
                   the past of this save => the memory stays valid;
                 - learned after the anchor => it belongs to a newer timeline
                   => it is simply *out of scope* for this save - and,
                   unlike before, nothing is deleted, so loading a newer save
                   brings those records back automatically (the verdicts are
                   reversible now);
                 - a travel or a reload of the same save barely moves the
                   anchor => nothing at all is invalidated (this is the core
                   of the fix);
                 - loading a genuinely older save invalidates only the
                   "future" records - the ones you had surveyed in that older
                   save stay green (the old behaviour wiped everything and
                   asked you to raise the scanner once to rebuild it).
               Records written by earlier versions have no timestamp; they
               are anchored at the game time of the first load of the session,
               which reproduces the previous "trusted while you continue the
               same save" behaviour. One new ini key:
               FloraSaveFingerprint=1 (0 = fall back to the fix-(8) path; the
               mod also falls back automatically, with a warning in the log, if
               the game clock cannot be read - and that fallback path no longer
               deletes records either, it only takes them out of scope, so no
               unreliably answered check can erase anything any more). The
               stats line gained a
               `存档指纹(★v5.1.7): 锚=…天 处理= 作废=条 表剪= 失败= 存档=` block.
           (10) The same report as (8) / (9), sent a third time after fix (9):
               "after travelling / changing maps, already-surveyed items turn
               back to cyan; raising the scanner once turns them green again."
               Fixed 2026-09-27. Fix (9) had already made the load boundary
               harmless (in that session the stats read `作废=0条` - nothing was
               invalidated any more), so the cyan had a different cause, and
               the same log holds it:
                 - the reported session used the star map at 10:50:24 and
                   finished a fast travel at 10:50:45; from then on the mod's
                   `按物种扩散 ... 星球外拒=` counter climbed to 37 and the whole
                   batch of plants was painted cyan;
                 - ten seconds later the player raised the scanner and *the
                   game itself* painted those very plants green (state 4 / 5) -
                   and those were the species the player had scanned by hand
                   three minutes earlier, on the planet the trip started from.
               In other words: the game's "this species is surveyed" knowledge
               works *across planets*, while fix (7) had taught the mod to hard
               -reject any spread that crosses a worldspace - so targets that
               really are surveyed in this save came out cyan. Two changes:
                 (a) the species spread is global again by default
                     (FloraSpeciesPlanetScope=0 = the game's own behaviour; 1
                     keeps the strict per-planet rule for comparison only).
                     Out-of-save leakage is still prevented, by the *save*
                     scope alone: the species table is session-only (never
                     written to disk) and every load prunes it by the game-time
                     anchor of fix (9);
                 (b) the species table is now *seeded* from the remembered
                     file: planet targets on a surface are runtime-created
                     references whose FormIDs are handed out again after a new
                     session / map change, so reference-level memory simply
                     cannot match them (that session: `记忆: 命中=2` with 271
                     already-surveyed plants sitting cyan). After the file is
                     loaded - and again after every load's pruning - every
                     green record that is *in scope* is added to the species
                     table by species, so those instances are green right away
                     instead of after raising the scanner once. The stats line
                     gained `播种=` and `跨星球放行=` inside the
                     `按物种扩散(★v5.1.8)` block.
                     (11) Following directly from (10): plants now ask *the game itself*
                     "has this species been surveyed in this save?" instead of
                     relying on the mod's own memory. Player request (2026-09-27,
                     translated): "can't you just read the game's own object state?
                     ... let's go with reading the game's own state for plants too
                     and drop our own memory." The game's per-reference evaluation
                     (the very path that writes state 4 / 5 - surveyed green - for
                     plants) reads one byte out of the game's own knowledge
                     database: the species' survey `percent` (0..100; 100 =
                     surveyed), reached through two read-only hash lookups. The mod
                     now replays exactly that read-only chain (component -> species
                     id, ref -> knowledge id, two FNV-1a table lookups, byte at
                     [element+0x20]) at any time, for any reference - including
                     runtime-created surface plants whose FormIDs change every
                     session. percent == 100 = green; anything else falls through to
                     the other checks (never short-circuits to green). Results are
                     cached per species (the game's own granularity), so "scan one
                     instance -> the whole species turns green" now comes from the
                     game's data, not from a spread table. The mod's own memory
                     layers are OFF by default now (FloraUseMemory=0): the
                     reference-level memory, the species spread table, the seeding
                     from the remembered file and the "keep the previous verdict"
                     sticky rule no longer take part in any verdict. This also
                     removes the suspected cause of "all planet resources are stuck
                     green": the fix-(10) global spread plus seeding had filled the
                     species table quite generously. Two new ini keys:
                     FloraEngineProgress=1 (0 = turn the direct read off) and
                     FloraUseMemory=0 (1 = bring the old memory layers back, for
                     comparison only). The stats line gained an
                     `引擎进度直读(★v5.2): 问= 满= 未满= 失败= 缓存命中= 冲突= ready= 记忆层=` block.
                     (12) "Surveyed things are cyan again, and this time opening
                     and closing the scanner does not fix them either." Fixed
                     2026-09-27, one build after (11). The direct survey read of
                     (11) never actually ran: the game builds its lookup key
                     from a 16-bit "type" word it reads from one of its own
                     globals, and the mod read that word at load time - but the
                     word lives in the uninitialised part of the exe's data
                     (the file contains no bytes there; the value only appears
                     once the game initialises it), so it was always 0 at load
                     time and the mod treated "0" as an error and switched the
                     whole check off. The startup log said so
                     (`flora progress: key 类型常量异常（0）-> ... DISABLED`) and
                     the stats line showed `ready=0 问=0` - zero queries. With
                     the check off, plants were left with the checks that only
                     hold *while the scanner is up* - so lowering the scanner
                     turned them cyan again, and because the mod's own memory is
                     off by design (fix (11)) there was nothing left to learn
                     from, which is why raising the scanner no longer helped
                     either. The mod now reads that word **fresh on every
                     query**, exactly like the game does, so the key it builds
                     always matches the key the game wrote; the startup message
                     is now `keyTypeAddr=+0x61E1D94 keyType@load=0x0` (the load
                     value is informational only - it is never used). The stats
                     line block is now
                     `引擎进度直读(★v5.2/订正R10): 问= 满= 未满= 失败= 缓存命中= 冲突= keyType=0x…(现读) keyZero= ready= 记忆层=`,
                     and the first queries of a session log one detail line each
                     (`flora progress probe: base=… key1=… key2=… keyType=…(现读) percent=… stage=… -> …`)
                     so any remaining problem can be pinpointed to the exact
                     step of the chain.
                     (13) "Surveyed things are cyan again, and this time even
                     opening and closing the scanner does not fix them - newly
                     highlighted objects seem to be affected most." Fixed
                     2026-09-27, one build after (12). The direct survey read of
                     (11) was still never succeeding - every query stopped at
                     the very first step, "0x81 component not found". That
                     component (the first thing the game itself reads) simply
                     does not exist on these surface plants, and the game does
                     not stop there either: when the component is missing it
                     falls back to a lookup that finds the species id from the
                     surrounding world data - and *that fallback* is what the
                     game's own green-painting path actually uses. The mod had
                     only reimplemented the component step, so the whole check
                     kept failing silently (log: `flora progress probe: …
                     stage=3 -> 0x81 component not found`; stats:
                     `问=0 失败=…`). The mod now follows the game's own order:
                     try the component first, and if it is missing (or empty)
                     call the game's own fallback lookup for the species id -
                     read-only, and the very same call the game's own outline
                     update was already running through the mod's engine-state
                     check for many versions (so no new side effects are
                     introduced). The first few queries of a session now log
                     `k1来源=组件(0x81)` / `k1来源=兜底(0x910690)` and the stats
                     line gained a `兜底K1=` counter, so the answering path is
                     visible at a glance.
                     (14) Log size is now a setting: `LogMaxMB` in the ini
                     (in MiB, default 1; the developer copy of this ini uses
                     10). The log is still self-limiting - once a new line
                     would cross the cap, the file is emptied and starts over
                     - but the cap no longer requires a new build to change.
                     The startup line reports the effective cap. Also in this
                     build, the internal "direct survey read" diagnostics got
                     finer: a failed lookup now says *which* layer stopped it
                     (empty table = this species has no record in this save,
                     vs. a bad table shape) and prints the ids it had already
                     resolved, every distinct failure layer and the first
                     successful query are always logged, and the "judged
                     green" lines have their own log budget (they used to be
                     squeezed out by the "judged cyan" lines, so fix (13)'s
                     effect could not even be seen in the log). Fix (13)'s
                     read itself is unchanged - this build changes
                     diagnostics only.
                     (15) Diagnostics only - no behaviour change. Reviewing a
                     long session showed the "judged green" log lines (fix
                     (14) gave them a budget of eight) being spent on three
                     species within the first two minutes, so the rest of the
                     session logged nothing - exactly the evidence you would
                     want if you ever have to report "this one stays cyan".
                     Two changes: every species now gets its first "judged
                     green" line logged (plus the original first eight; hard
                     cap 64 per session), the "why is this one cyan" lines get
                     the same per-species treatment, and cached answers print
                     the real key ids instead of 0x0 placeholders (the cache
                     simply did not store them - the same "only part of the
                     data comes back" trap as in (13) and (14)). Fix (13)'s
                     read itself, and every verdict, is unchanged.
                     Also in 2.0 (the headline feature) - the mod brings its own
                     outline colour channels instead of borrowing the game's.
           The game has eleven outline "states"
           (0..10); six of them are painted by the game itself while the
           scanner is up (ordinary references such as people, and planet
           targets before / after surveying). Previous releases borrowed
           the remaining five - that is why colours had to be grouped and
           why changing one of them also changed the same state inside the
           vanilla scanner. The renderer underneath, however, keeps two
           per-id tables that grow on demand ("colour parameters" per
           manager id, and "which manager paints this reference"); the
           eleven states are only the game's own book-keeping on top of
           that. So 2.0 creates its own colour channels - thirteen of them,
           one per category plus one for surveyed plants - without touching
           the game's state table or its colour blocks at all.
           What you get:
             - vanilla colours are never modified: people, planet targets
               and the vanilla scanner look exactly stock;
             - every category is independent: weapons / apparel / notes /
               resources keep the grouped colours (red / red / purple /
               purple) only because that is the requested layout - give
               them separate ColorXxx values and they will differ;
             - the game can no longer wipe the mod's highlights when it
               tears down its own scanner book-keeping (the mod's channels
               are not part of it).
           INI: ChannelMode=1 (default) enables the new channels;
           ChannelMode=0 restores the previous behaviour (the mod writes
           into the game's own outline states, exactly as in 1.8.1).
           If the engine's function signatures ever change, the mod falls
           back to the old path automatically and logs a warning - no
           manual INI edit needed. ColorFloraScanned sets the "already
           surveyed" plant colour (default: the game's green #27C684);
           unscanned plants use ColorFlora (default: the game's cyan
           #72E8FF).
           1.8.1 = "surveyed plants sometimes drop back to cyan", second
           pass - the one that makes it stay fixed. 1.7.9's one-way
           memory had three holes, all found in a long test session:
           it was wiped on every load / cell change (the game's "loading
           screen closed" event), it lived in memory only (so every game
           session had to re-learn it by raising the scanner once), and
           the "keep the previous verdict" rule had a condition that could
           never be true for plants (their record layout makes the chain
           look "answered" when it answers nothing). All three are fixed:
           the memory survives loads and cell changes
           (FloraLearnClearOnLoad=0, new default), it is written to a file
           next to the esm (SAS_AlwaysScan.flora-learn.txt) and read back
           on startup (FloraLearnPersist=1, new default - a new session
           starts already knowing), and the keep-rule now covers plants.
           A "not scanned" verdict is also re-checked every 5 s instead
           of 30 s (FloraUnscannedTtlMs), and whenever the mod's own
           checks cannot answer it now also reads the state the game
           itself painted (green = surveyed) - so the game's own verdict
           is picked up even with the scanner put away. Note: the file is
           keyed by species, not by save file (simply a colour effect if
           you play several saves); set FloraLearnPersist=0 for
           session-only behaviour, delete the file to reset.
           (Superseded in 2.0: the memory is now keyed by the individual
           reference, so a species surveyed in one save no longer shows
           green in another. See the 2.0 notes at the top.)
           1.8.0 = the colour classification, final grouping. The set of
           colours and the number of outline states the mod writes are
           unchanged from 1.7.9 - what changed is which categories share
           a colour:
             RED    = weapons / throwables AND spacesuits / helmets / packs /
                      clothing (the whole equipment set is one red again;
                      the suits were the game's own light cyan-blue in 1.7.9)
             ORANGE = containers and corpses
             PURPLE = notes AND resources (notes were the game's own cyan
                      in 1.7.9)
             GREEN  = ammo and aid (the game's own green, unchanged)
             WHITE  = doors
             BLUE   = misc items (the game's own blue, unchanged)
           Still exactly five outline states are written (2 / 3 / 6 / 9 / 10);
           states 0 / 1 (people), 4 / 5 and 7 / 8 (planet targets) remain
           untouched, so NPCs and planet surveying are unchanged. This time
           no colour had to give way - every requested colour fits.
           1.7.9 = two fixes, one for each report.
           (1) Containers, corpses and doors lost their colours in 1.7.8
           (they had been turned into the game's plain cyan in order to free
           up two outline states for weapons / apparel). Both are back:
           containers and corpses are ORANGE again, doors are WHITE again,
           exactly as in 1.7.7 and earlier. There is a hard limit at work
           here: of the eleven outline states, six are painted by the game
           itself while the scanner is up (plain references such as NPCs =
           two, already-scanned planet targets = two, not-yet-scanned ones =
           two), and 1.7.8 returned all six of them to the game - leaving
           five states the mod can borrow, while the colours being asked
           for needed six. So two had to give way: apparel (was magenta) and
           notes (was yellow) now use the game's own colours (light cyan-
           blue and cyan). Both swaps are one edit away in the INI
           (StateApparel / ColorApparel / StateWeapon, and StateNote /
           ColorNote / StateDoor) - no DLL swap needed.
           (2) Already-surveyed plants could drop back to the "not yet
           scanned" cyan, at a low rate; raising and lowering the scanner
           would turn them green again. The mod re-checks each plant against
           the game every 30 s; very occasionally that fresh check comes back
           "no answer / not scanned" (a game component gets rebuilt, or
           timing), and the previous "scanned" verdict was thrown away with
           it. Survey data never goes backward, so the verdict is now
           remembered per species for the rest of the session ("one-way
           memory"), and a re-check that cannot answer authoritatively keeps
           the previous verdict instead of erasing it. Two new counters show
           both mechanisms working in the periodic log line
           ("学习表: 命中=… 沿用=…"). The flora-related INI keys are
           unchanged.
           1.7.8 = fix: while the scanner was up, every NPC showed the
           red / magenta colours the game itself reserves for its "bounty"
           markers (report + screenshot: three pedestrians, one red, two
           magenta). The reason: the game paints ordinary references -
           people included - with two outline states whose native colours
           are cyan (far) / light cyan (near), and the mod had been
           overwriting exactly those two states with "weapons red" and
           "apparel magenta". Both states are now left completely alone
           (not a single byte written), so people are back to the vanilla
           cyan. Weapons and apparel (still red / magenta) moved to two
           other low-traffic states; containers / corpses and doors used
           to occupy those two states and now show the game's own cyan as
           well. Practical consequence: containers, corpses and doors are
           no longer orange / white - the mod now recolours only five of
           the eleven outline states in total (misc blue, resources
           purple, notes yellow, weapons red, apparel magenta). Every
           state and colour is listed in the INI and can be changed back
           key by key (no DLL swap).
           1.7.7 = fix: with the scanner put away, already-scanned planet
           targets (ores / gas / liquids / plants) still showed the
           "not scanned" cyan. 1.7.6 fixed the other half (with the scanner
           UP, the vanilla green is back); but once you lower the scanner
           the mod re-hangs the highlight itself, and it only ever used one
           state - the vanilla "not scanned" cyan pulse - so nothing told
           "already surveyed" and "never surveyed" apart. The mod now asks
           the game itself, from three independent sources (any one is
           enough): (a) it calls the game's own "has this reference been
           surveyed?" query - that is literally the native function the
           game exposes as IsScanned, and it answers per reference (the
           game resolves plants / deposits / creatures internally), so it is
           correct for plants too; (b) while you hold the scanner up, it
           reads which outline state the game itself paints each target with
           - the game's green means "already surveyed" - and remembers that
           for the session; (c) with the scanner away it calls the very
           function the vanilla scanner uses (the one that checks whether a
           resource has entered your survey data) and walks the game's own
           record chain (flora record -> the item it produces -> that item's
           resource), auto-detecting the record layout if the game moves it.
           Source (c) only applies to flora that produce a level list - the
           ore / gas / liquid deposits; the game itself checks that same
           record type before walking the chain.
           (1.7.7 was rebuilt with plugin build 4.29.0. Build 4.27.0 fixed a
           silent bug in source (c) - the record type it had to recognise was
           rejected by the mod's own sanity check. Build 4.28.0 fixes the
           report that PLANTS showed the "already surveyed" green before
           being scanned: the mod had been applying source (c) to plants as
           well, and a plant's produced item is a plain item (not a level
           list), so "the resource this plant yields is in your survey data"
           was mistaken for "this plant has been surveyed". Plants now go
           through source (a) / (b) only, exactly like the game does.
           Build 4.29.0 (this package) makes putting the scanner away much
           snappier for planet targets. Every time you lower the scanner the
           game tears its highlight managers down, so the mod has to re-hang
           every target it keeps lit; that "re-hang" pass was spread out at
           64 items per frame no matter what, so in a busy scene (268 lit
           targets in the test session) plants and ores visibly lagged about
           a second behind everything else - including the "cyan -> green"
           repaint of something you had just scanned. The first 2.5 s after
           you lower the scanner now run at a bigger per-frame budget (192),
           and targets whose colour has to change are repainted before the
           rest of the queue. Tune or turn off with ResyncBoostMs /
           ResyncBoostBudget in the INI; no DLL swap needed.)
           Already-scanned targets use the game's own
           "scanned" state - GREEN, the same colour and slot the game uses
           while the scanner is up; unscanned ones keep the cyan pulse.
           Scan a plant or a deposit, lower the scanner: it turns green
           within about 0.2 s. FloraScannedByEngineState=0 (main source off),
           FloraScannedByResource=0 (chain off) or StateFloraScanned=7 brings
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
  * Weapons, throwables and the whole equipment set (guns, melee, grenades,
    mines, spacesuits, helmets, packs, clothing)                      RED
  * Ammo and aid (meds, food, drinks) - the game's own green
    (state 4/5 is shared with the vanilla "scanned planet target" green;
    write ColorAmmoAid=00FF66 for bright green instead)                GREEN
  * Notes (notes, data slates, magazines, books) and resources (iron,
    aluminium, helium-3, organics, ...) - one purple               PURPLE
  * Misc items (digipicks, credits, toys) - unchanged from 1.5        BLUE
  * Containers (loot the good stuff)                                 ORANGE
  * Bodies / corpses - people, creatures and wrecked robots / turrets
    (anything you can search) - same state as containers             ORANGE
  * Interactive devices / computers - the game's own green (same note as
    above; write ColorDevice=00E5FF for cyan instead)                  GREEN
  * Doors                                                             WHITE
  * Flora, ores, gas vents, liquid pools (all one record type in the game
    data) - the game's own "scannable target" colour, a cyan pulse; once
    you have surveyed one it switches to the game's own "scanned" green.
    Since 1.7.5 this category is ON by default, its colours are never
    overwritten (see below) and since 1.7.7 scanned targets stay green
    even with the scanner put away                        CYAN / GREEN
  * (Not a mod category, for reference:) while the scanner is up, the
    game itself outlines people and other plain references with the two
    states whose native colours are cyan / light cyan. The mod stopped
    touching those two states in 1.7.8, so NPCs look exactly like they
    do in the vanilla game.

Since 1.8.0 the categories are grouped into colours that are easy to tell
apart (red = equipment, orange = lootable world objects, purple = knowledge
and crafting materials, green = consumables, white = doors, plus the
original misc blue and the game's own cyan / green for planet targets).
Every colour is assigned explicitly by the mod (see ColorXxx in the INI),
by writing into the engine's per-state colour block. Five of the eleven
states are written in total (2 / 3 / 6 / 9 / 10); the other six are
deliberately left untouched because the game itself paints with them -
the two "plain reference / NPC" states (0 / 1, native cyan; left alone
since 1.7.8), the two "scanned planet target" states (4 / 5, native
green; 1.7.6) and the two "not scanned planet target" states (7 / 8,
native cyan; 1.7.5) - see the planet-survey section below.

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
put the scanner away they are put back (well under a second; since build
4.29.0 the pass runs at a bigger per-frame budget for the first 2.5 s and
repaints colour changes first), painted in the game's own cyan, so planets
are readable with or without the scanner.
Since 1.7.7 that "put back" pass asks the game whether each target has
been surveyed already, using three independent sources (any one is
enough): the game's own "has this reference been surveyed?" query (the
native function the game exposes as IsScanned - it answers per reference,
so plants are covered), the outline state the game itself painted while
the scanner was up (remembered for the session), and the game's own check
function plus record chain (flora record -> the item it produces -> that
item's resource) for the deposits that produce a level list - which is
what the game itself requires before it walks that chain. Targets you have
already scanned come back GREEN - the exact state and colour the game uses
while the scanner is up - and unscanned ones stay cyan, so with the
scanner away you can tell at a glance what is left to survey. Scan
something, put the scanner away: it turns green within ~0.2 s.
★ Build 4.28.0 fixed "plants showed the scanned green
before being scanned": the record chain had been applied to plants too,
but a plant's produced item is a plain item (not a level list), so "the
resource this plant yields is in your survey data" was mistaken for "this
plant has been surveyed". Plants now use the first two sources only,
exactly like the game does; ores / gas / liquids are unchanged.
FloraScannedByEngineState=0 / FloraScannedByResource=0 (or
StateFloraScanned=7) in the INI restores the old "all cyan" behaviour. Resources vs. misc items: "resources" (iron, aluminium,
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
  StateWeapon=10        outline color state per category (0..10; the INI
  StateApparel=10       documents them). Defaults (since 1.8.0):
  StateAmmoAid=5          weapons / throwables / suits / helmets / packs /
  StateNote=3             clothing -> 10 (red)
  StateResource=3         notes / resources -> 3 (purple)
  StateLoot=2             ammo & aid -> 5 (the game's own green)
  StateContainer=9        misc -> 2 (blue, = 1.5)
  StateDevice=4           containers & bodies -> 9 (orange)
  StateDoor=6             devices -> 4 (the game's own green)
  StateFlora=7            doors -> 6 (white)
                        flora 7 (= the vanilla scannable-target state,
                        colour not overwritten) and MSTT 2.
                        Five of the eleven states are written in total
                        (2 / 3 / 6 / 9 / 10); the other six are never
                        touched: 0 / 1 (the game paints plain references
                        and people with them - native cyan / light cyan;
                        left alone since 1.7.8), 7 / 8 (vanilla "not
                        scanned planet target" states; since 1.7.5) and
                        4 / 5 (vanilla "scanned planet target" states,
                        native green; since 1.7.6). Ammo & aid and devices
                        use 4 / 5, so they show the game's own green by
                        default (1.7.6). Since 1.7.7 already-scanned
                        planet targets use state 5 as well (see
                        StateFloraScanned below) - same state, same green,
                        so nothing changes visually.
  FloraScannedByEngineState=1
                        main switch: ask the game itself whether a planet
                        target has been surveyed already (default on,
                        build 4.28.0). It calls the game's own query (the
                        native IsScanned) which answers per reference, so it
                        is correct for plants, ores, gas vents and liquid
                        pools alike. 0 = only the record-chain check below
                        is used (plants then lean green - the 4.27.0
                        behaviour).
  FloraScannedByResource=1
                        second switch: also walk the game's own record
                        chain (flora record -> produced level list -> its
                        item -> that item's resource) and ask the game
                        whether that resource has entered your survey data.
                        ★ Since 4.28.0 it only applies to flora that produce
                        a level list - the ore / gas / liquid deposits -
                        exactly like the game itself (a plant's produced
                        item is a plain item, and treating it as if it were
                        a deposit was the "plants are green before being
                        scanned" bug). 0 = chain off.
                        Both switches off = old behaviour: the whole flora
                        category uses StateFlora only.
  StateFloraScanned=5   state used for already-scanned planet targets
                        (default 5 = the game's own "scanned" green,
                        #27C684). Set it to 7 (= StateFlora) to make
                        scanned and unscanned look the same again.
  FloraMemoryScope=1    how far the mod's "this one is surveyed" memory is
                        allowed to reach (see fixes (7) / (8) / (9) at the
                        top of this file). "Surveyed" is a fact of the save
                        you loaded, so every remembered reference is stamped
                        with the game time it was learned at, and every
                        load-game event only records the game time of the save
                        that was just loaded as an anchor: records learned at
                        or before it stay valid, records learned later belong
                        to a newer timeline and are out of scope for this save
                        (they are *not* deleted - loading a newer save brings
                        them back). A travel moves that anchor by seconds, so
                        nothing is invalidated; loading an older save
                        invalidates only the "future" records.
                        1 = default. 0 = old behaviour (memory never
                        expires - the "green before I scanned it" reports
                        come back). 2 = strictest (never trust the
                        remembered file, so plants may read cyan after a
                        load until you raise the scanner once).
  FloraSaveFingerprint=1
                        decide the load boundary by the game clock (fix (9),
                        default). 0 = fall back to the fix-(8) path (survey
                        check right after the loading screen closes - its
                        "not surveyed" answer has no contrast at that moment).
                        The mod also falls back automatically - with a warning
                        in the log - if the game clock cannot be read, so a
                        broken Calendar can never make the mod delete
                        anything.
  FloraLoadBoundaryEvidence=1
                        only used by the fallback path (fix (8); with
                        FloraSaveFingerprint=1 this key does nothing unless
                        the game clock cannot be read): decide a load-game
                        event by evidence instead of trusting it blindly: the
                        mod waits four seconds after the loading screen
                        closes, then asks the survey-data check about the
                        resources its memory mentions; only records it can
                        actively disprove are deleted, so travels / map
                        changes (which also send that event but do not make
                        your survey data go backwards) no longer wipe true
                        verdicts. Resources only ever confirmed on another
                        planet are skipped during that check. 1 = default.
                        0 = old behaviour (every load event counts as a real
                        load).
  FloraSpeciesPlanetScope=1
                       FloraSpeciesPlanetScope=0
                        how far a confirmed "surveyed" verdict spreads to the
                        rest of its species / resource. 0 = default (fix (10)):
                        spread everywhere, which is what the game itself does -
                        raise the scanner after surveying a species and the game
                        paints that species green on any planet you meet it on.
                        The mod still keeps this inside the save: the species
                        table is session-only and pruned by the game-time anchor
                        of fix (9), and only in-scope records seed it. 1 = strict
                        per-planet spread (fix (7)'s rule); it paints targets
                        that are surveyed in this save cyan whenever you
                        witnessed the species on another planet - comparison /
                        fallback only. Note: with fix (11) the memory layers
                        are off by default (FloraUseMemory=0), so this key only
                        matters when you set FloraUseMemory=1.
  FloraEngineProgress=1
                      fix (11) + (12) + (13), default: plants read the game's
                      own survey progress - the very byte the game's state-4/5
                      path reads (PlayerKnowledge species percent; 100 =
                      surveyed) - through a fully read-only replay of the
                      engine's own lookups, at any time and for any reference
                      (including runtime-created surface plants). Fix (12): the
                      lookup key's "type" word is read **fresh on every query**
                      (the game does the same; the value only exists once the
                      game has initialised it, so the earlier load-time read of
                      it was always 0 and had switched the whole check off).
                      Fix (13): the species id is obtained the way the game
                      itself obtains it - the "0x81 component" first, and when
                      that component is missing (the normal case for surface
                      plants) the game's own fallback lookup is called. percent
                      == 100 = green; a lower value never short-circuits.
                      0 = turn the direct read off (falls back to the older
                      checks).
  FloraUseMemory=0
                        fix (11), default: the mod's own memory layers are OFF
                        (reference-level memory, species spread table, seeding
                        from the remembered file, "keep the previous verdict").
                        "Surveyed" is answered by the game's own data only
                        (FloraEngineProgress + the read-only outline-state probe
                        + GetOutlineState + the survey-data chain for LVLI
                        deposits). 1 = bring the old layers back (comparison
                        only; their false-green / false-cyan issues come back
                        with them).
  ColorWeapon=FF2E2E    exact RGB per category (written by default):
  ColorApparel=FF2E2E     equipment (weapons / suits / clothing) red,
  ColorNote=B36BFF        notes & resources purple, misc blue - plus
  ColorResource=B36BFF    containers & corpses orange and doors white.
  ColorContainer=FF9500   Five states are written in total (states
  ColorDoor=FFFFFF        2 / 3 / 6 / 9 / 10). Ammo & aid / devices /
                          flora / MSTT are NOT written - they show the
                          game's own colours (green / green / cyan-green /
                          blue respectively). Since 1.7.6 the two
                          "scanned planet target" states (4 / 5) are
                          also left untouched, and since 1.7.8 the two
                          "plain reference / NPC" states (0 / 1) as well.
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
  ChannelMode=1         own outline colour channels (default since 2.0).
                        See the 2.0 notes at the top of this file: vanilla
                        colours (people, planet targets, the scanner itself)
                        are never modified, and every category is
                        independent.  ChannelMode=0 = the 1.8.1 behaviour
                        (borrow the game's own outline states).  If the
                        engine's function signatures ever change, the mod
                        falls back to 1.8.1 behaviour automatically and
                        logs a warning line starting with "channel:".
  ColorFloraScanned     colour of an already-surveyed plant / mineral
                        deposit in ChannelMode=1 (default: the game's green
                        #27C684).  Unscanned ones use ColorFlora (default:
                        the game's cyan #72E8FF).
  AlphaWeapon=0         pulse (contour) opacity per category (0-255).
  AlphaApparel=0        0 = keep the engine's own value (default), 255 = a
  AlphaDoor=0           fixed, non-breathing contour.  NOTE: this does not
                        make the fill transparent - use NoFill for that
                        (the alpha byte is ignored by the renderer).
  ResourceByKeyword=1   recognise "resources" from the item record's own
                        ResourceType keywords (default on).  0 = every MISC
                        item counts as misc (the 1.5 behaviour).
  FastReadMem=1         read game memory directly, guarded by a
                        hardware-exception handler, instead of calling
                        ReadProcessMemory; and check whether memory is
                        readable by probing one byte per page instead of
                        calling VirtualQuery.  Both of those APIs take the
                        process address-space lock, which the game holds while
                        it streams the world - that is what made moving /
                        combat scenes stutter (see fix (6) at the top of this
                        file).  A startup self-test verifies the direct reads
                        and prints both methods' cost in the log.
                        0 = the old behaviour (useful for comparison only).
  LootCacheTtlMs=1500   how long an "is this container / corpse empty" verdict
                        is reused (milliseconds).  The cache is dropped
                        immediately when an item is taken or put, or when the
                        loot UI opens / closes, so looting a container empty
                        still puts the highlight out exactly as quickly as
                        before.  It only cuts the number of memory reads per
                        pass (30+ down to a handful).  0 = check every
                        container / corpse on every pass.
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
                        the scanner away too (FloraScannedByEngineState /
                        FloraScannedByResource / StateFloraScanned below).
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
  RingSliceMaxRounds=5  since build 5.1.0 (the "frame rate still drops"
                        fix): the cells around the one you stand in (kept
                        outlined across borders) are walked in rotating
                        slices - one slice per pass - so a single pass never
                        walks the whole neighbourhood (together those lists
                        may hold up to 60,000 references, and walking all of
                        them took ~55 ms per pass in a busy scene). The cell
                        you stand in is always walked in full, and lists of
                        2000 references or fewer are never sliced. The slice
                        count is capped automatically so that one full
                        rotation finishes inside UnhighlightGraceMs -
                        slicing can therefore never make a highlight flicker.
                        0 = disabled (walk everything every pass).
  NotifyOnToggle=1      show a HUD message on toggle
  LogStats=1            write a stats line to the log every 5 seconds
  LogMaxMB=1            log size cap in MiB (1..1024). The log never grows
                        past the cap: once a new line would cross it, the
                        file is emptied and starts over (fix (14)). Raise
                        it (e.g. 10) if you are collecting a long debug log.

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
    FloraScannedByEngineState=1, FloraScannedByResource=1 and
    StateFloraScanned=5 (an INI from 1.7.6 or older does not contain them -
    the DLL then uses its built-in defaults, so the feature works even
    without the new keys). In the log look for
    "flora scanned: 主判据就绪 ... (0x1306E80)" / "... ready" at startup and
    the stats line "planet targets ... : 未扫描=... 已扫描=... | 引擎状态:
    问=... 已扫描=... ..."; "已扫描" (scanned) should grow once you have
    surveyed something near you, and "引擎状态" counts the raw answers the
    game itself gave (1 = not surveyed / 2 = surveyed).
    If "链失败" (chain failures) grows instead, you are most likely running
    the first 1.7.7 build (plugin 4.26.0): its record chain had a silent bug
    that is fixed in build 4.27.0, so grab the current file.
    If the current build still shows it, the game's record layout has
    changed - please report that log line plus the "flora probe" lines.
* On a planet, PLANTS (not ores) are green before you scan them:
    that was build 4.27.0 and earlier - the record-chain check was applied
    to plants as well, whose produced item is a plain item instead of a
    level list, so "the resource this plant yields is already surveyed" was
    read as "the plant is surveyed". Fixed in build 4.28.0:
    plants answer to the game's own per-reference query only, ores / gas /
    liquids are unchanged. If you still see it, check the log for the probe
    lines "flora scan: ... 引擎状态=N ..." and the stats segment
    "| 引擎状态: 问=... 已扫描=...": if the game itself answers 2 (=already
    surveyed) for a plant you never scanned, please report those lines.
    Workaround without swapping the DLL: set FloraScannedByEngineState=0
    (only the record chain is used - plants go back to always cyan).
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
* The log size cap is the ini key `LogMaxMB` (in MiB, default 1): once the
  log would grow past the cap, the file is emptied and starts over, so it
  can never fill up your disk. Raise it (e.g. LogMaxMB=10) when collecting
  a log for a bug report.
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
