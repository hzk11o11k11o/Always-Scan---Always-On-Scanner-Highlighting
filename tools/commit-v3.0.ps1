$ErrorActionPreference = 'Stop'
Set-Location 'd:\workspace\starfield mod\always scan'

$msg = @'
v3.0 guide path -> world-anchored rolling light band

Root causes of user feedback (only one bead visible / heavy stutter / not continuous):
1) In Starfield SetPosition / MoveToNearestNavmeshLocation / Enable / SetScale are all
   GameScript::DelayFunctor deferred functions, so reading GetPositionZ right after
   SetPosition returns the OLD value. v2.8 did "snap to ground then read back and lift",
   which put every bead 2.25m above the player's head (the floating ball in the screenshot).
2) v2.8 moved all 8 beads with 3 engine calls each (incl. 8 navmesh queries) in one frame
   every 1.5m of movement -> periodic hitch + the whole chain jumped 1.5m at a time.

Changes (scripts/SAS_Bridge.psc only):
- 20 beads, 0.55m spacing, scale 4.0 => 11m near-continuous light band
- world-anchored + ring recycle: the head bead is moved to the tail once per spacing walked
- budget: at most 3 bead moves per tick (relay / recycle / trim share it); math only otherwise
- chain length = min(20*0.55, targetDist - margin): auto-compresses when the target is close
- beads past the target are parked by SetPosition underground instead of Disable/Enable
- ground snap = one SetPosition then MoveToNearestNavmeshLocation, no read-back
- removed the 1.5m movement throttle; added CfgGuidePoolVer for pool rebuild migration
- diagnostics line guide/paint now reports lay/relay/head/sp/adv/vis/moved

Docs: docs/04 section 14 (root cause + design), docs/99 v3.0 entry + new test criteria.
'@

git add -A
git commit -m $msg
git --no-pager log --oneline -1
