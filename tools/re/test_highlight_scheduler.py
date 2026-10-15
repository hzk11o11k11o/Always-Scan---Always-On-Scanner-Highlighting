"""Compile and exercise production discovery/scheduling code with a stub game engine."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/')


def without_comments(source: str) -> str:
    return TOKEN.sub(lambda m: " " * len(m[0]) if m[0].startswith(("//", "/*")) else m[0], source)


def definition(source: str, marker: str) -> str:
    start = source.index(marker)
    masked = TOKEN.sub(lambda m: " " * len(m[0]), source)
    opening = masked.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    if source[end:end + 1] == ";":
        end += 1
    return source[start:end]


PREAMBLE = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace RE {
template<class T> struct NiPointer {
    T* value = nullptr;
    NiPointer() = default;
    NiPointer(T* p) : value(p) {}
    T* get() const { return value; }
    T* operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
struct NiAVObject {};
struct TESObjectCELL {};
struct NiPoint3 {
    float x = 0, y = 0, z = 0;
    float GetSquaredDistance(const NiPoint3& p) const {
        return (x-p.x)*(x-p.x) + (y-p.y)*(y-p.y) + (z-p.z)*(z-p.z);
    }
};
struct TESForm { int cat = 0; int GetFormType() const { return cat; } };
struct TESObjectREFR {
    std::uint32_t id = 0;
    TESObjectCELL* parentCell = nullptr;
    struct Data { NiPoint3 location, angle; NiPointer<TESForm> objectReference; } data;
    NiAVObject* root = nullptr;
    int loot = 1;
    bool deleted = false, disabled = false, actor = false, dead = false;
    bool scanned = false, display = false, emptied = false, failMount = false;
    bool IsDeleted() const { return deleted; }
    bool IsDisabled() const { return disabled; }
    bool IsPlayerRef() const { return false; }
    std::uint32_t GetFormID() const { return id; }
};
struct PlayerCharacter : TESObjectREFR {
    NiPoint3 GetPosition() const { return data.location; }
};
}
namespace REX { template<class... T> void INFO(T&&...) {} }
enum class Category : std::uint8_t { kLoot=0, kContainer=6, kFlora=9, kCorpse=11 };
constexpr std::size_t kCategoryCount = 13, kChannelFloraScanned = 13, kLootMemoMax = 512;
constexpr std::uint32_t kRingRefsCap = 60000;
constexpr std::uint64_t kStallThresholdUs = 3000;
constexpr int kDcEmptyNone = 0;
bool g_scannableOutlineStateReady = true, g_isResourceScannedReady = true, g_floraProgressReady = true;
std::size_t g_invOff = 0xA0;
struct Config {
    int maxOutlineOpsPerScan = 64, verify3DPerScan = 32, reassertMs = 0, unhighlightGraceMs = 1500;
    std::uint32_t resyncBoostBudget = 192, ringSliceRoundsEff = 5;
    int scanIntervalMs = 200, maxTargets = 256, lootCacheTtlMs = 0;
    float radiusMeters = 50, unitsPerMeter = 1;
    bool onlyInFront = false;
    float frontFovDeg = 110;
    bool yieldTargetsWhileScanning = true, skipEmptyLoot = true, skipDisplayCaseEmpty = true;
    bool displayCaseUiEmpty = true, resourceByKeyword = false;
    bool floraEngineProgress = true, floraScannedByEngineState = true, floraScannedByResource = true;
    bool channelMode = true;
    std::array<bool, kCategoryCount> categoryEnabled;
    std::array<int, kCategoryCount> stateByCategory;
    Config() {
        categoryEnabled.fill(true);
        for (int i = 0; i < static_cast<int>(kCategoryCount); ++i) stateByCategory[i] = i;
    }
} g_cfg;
std::uint64_t clockUs = 0;
std::uint64_t NowUs() { return ++clockUs; }
struct PhaseTimer { explicit PhaseTimer(std::uint64_t*, std::uint64_t* = nullptr) {} };
struct DcSnapOut {};
struct DcVerdict { std::uint64_t emptySinceMs = 0; };
struct ScanCell {
    RE::TESObjectCELL* cell = nullptr;
    RE::TESObjectREFR* const* list = nullptr;
    std::uint32_t begin = 0, count = 0;
    std::vector<RE::TESObjectREFR*> known;
};
'''

STUBS = r'''
std::unordered_map<RE::TESObjectREFR*, int> mounts, removes;
bool OutlineRef(RE::TESObjectREFR* ref, std::uint32_t, std::uint32_t, RE::NiAVObject* root) {
    ++mounts[ref];
    return root && !ref->failMount;
}
void UnoutlineRef(RE::TESObjectREFR* ref, std::uint32_t) { ++removes[ref]; }
std::uint32_t LedgerKeyOf(const Candidate& c) { return g_cfg.channelMode ? c.channel : c.state; }
std::uint32_t LedgerKeyOf(const OutlineEntry& e) { return g_cfg.channelMode ? e.channel : e.state; }
RE::NiPointer<RE::NiAVObject> RefGet3D(RE::TESObjectREFR* ref) { return {ref->root}; }
bool IsPlausiblePointer(std::uint64_t p) { return p != 0; }
void CalibrateInventory(RE::TESObjectREFR* const*, std::uint32_t) {}
void ResourceKeywordSelfTest() {}
int ClassifyRef(RE::TESObjectREFR* ref, const RE::TESForm* base, float, bool& corpse) {
    corpse = ref->dead;
    if (ref->actor && !corpse) return -1;
    return base ? (corpse ? 11 : base->cat) : -1;
}
void ProbeFloraEngineState(RE::TESObjectREFR*, const RE::TESForm*) {}
bool IsDisplayCaseBase(const RE::TESForm*) { return false; }
int RefLootState(RE::TESObjectREFR* ref, bool*, DcSnapOut*) { return ref->loot; }
void ContProbe(RE::TESObjectREFR*, const RE::TESForm*, float, int, bool) {}
void DcWatchAdd(RE::TESObjectREFR*, float) {}
void DcUiProbe(RE::TESObjectREFR*) {}
DcVerdict& DisplayCaseVerdictFor(RE::TESObjectREFR*) { static DcVerdict v; return v; }
int DisplayCaseReadsEmpty(RE::TESObjectREFR* ref, std::uint64_t) { return ref->emptied ? 1 : 0; }
const char* DcEmptyReasonText(int) { return "empty"; }
void MarkDisplayCaseEmptied(RE::TESObjectREFR*, const char*, int) {}
void NoteDisplayCaseOccupied(RE::TESObjectREFR*, bool, DcSnapOut*) {}
void LootProbe(RE::TESObjectREFR*, float) {}
bool FloraTargetScanned(RE::TESObjectREFR* ref, const RE::TESForm*) { return ref->scanned; }
std::uint32_t FloraScannedState() { return 5; }
'''

TESTS = r'''
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct World {
    RE::TESObjectCELL current, ring;
    RE::PlayerCharacter player;
    std::array<RE::TESForm, kCategoryCount> bases;
    std::vector<RE::TESObjectREFR> refs;
    std::vector<RE::NiAVObject> roots, rebuilt;
    std::vector<RE::TESObjectREFR*> currentList, ringList;
    explicit World(std::size_t n, bool inRing = false) : refs(n), roots(n), rebuilt(n) {
        g_cfg = Config{};
        g_state = State{};
        g_state.nativeReady = true;
        g_state.invCalibDone = true;
        g_state.resKeywordTest = 1;
        mounts.clear(); removes.clear();
        g_scannableOutlineStateReady = g_isResourceScannedReady = g_floraProgressReady = true;
        for (int i = 0; i < static_cast<int>(kCategoryCount); ++i) bases[i].cat = i;
        player.parentCell = &current;
        for (std::size_t i = 0; i < n; ++i) {
            auto& ref = refs[i];
            ref.id = static_cast<std::uint32_t>(i + 1);
            ref.parentCell = inRing ? &ring : &current;
            ref.data.location = {static_cast<float>(i) / 100 + 1, 1, 0};
            ref.data.objectReference = &bases[0];
            ref.root = &roots[i];
            (inRing ? ringList : currentList).push_back(&ref);
        }
    }
    void scan(std::uint64_t now, std::uint32_t begin = 0, std::uint32_t count = UINT32_MAX,
              bool withRing = true) {
        std::vector<ScanCell> cells{{&current, currentList.data(), 0,
            static_cast<std::uint32_t>(currentList.size()), {}}};
        if (withRing && !ringList.empty()) cells.push_back({&ring, ringList.data(), begin,
            std::min(count, static_cast<std::uint32_t>(ringList.size()) - begin), {}});
        Scan(now, &player, cells);
        const auto limit = now < g_state.resyncBoostUntilMs ?
            std::max(g_cfg.maxOutlineOpsPerScan, static_cast<int>(g_cfg.resyncBoostBudget)) :
            g_cfg.maxOutlineOpsPerScan;
        check(g_cfg.maxOutlineOpsPerScan == 0 || g_state.opsThisScan <= static_cast<unsigned>(limit),
            "per-scan budget exceeded");
    }
    void kind(std::size_t i, int cat) { refs[i].data.objectReference = &bases[cat]; }
};
void no_model_does_not_block() {
    World w(72);
    g_cfg.maxTargets = g_cfg.maxOutlineOpsPerScan = 8;
    for (int i = 0; i < 64; ++i) w.refs[i].root = nullptr;
    w.scan(1000);
    check(g_state.outlined.size() == 8 && g_state.selCount == 8, "unloaded refs consumed selection");
    check(g_state.no3DSkips == 64 && g_state.opsThisScan == 8, "unloaded refs consumed mount budget");
    check(mounts.size() == 8, "engine mount attempted without model");
}
void pending_survives_slice() {
    World w(40, true);
    g_cfg.maxOutlineOpsPerScan = 2;
    w.scan(1000, 0, 8);
    check(g_state.outlined.size() == 2, "initial mount budget");
    w.scan(1200, 8, 8);
    check(g_state.outlined.contains(&w.refs[2]) && g_state.outlined.contains(&w.refs[3]),
        "pending refs lost when slice changed");
    check(g_state.nearbyReused >= 8, "previous slice not re-evaluated");
}
void newly_loaded_model_retries_next_scan() {
    World w(40, true);
    w.refs[0].root = nullptr;
    w.scan(1000, 0, 8);
    check(!g_state.outlined.contains(&w.refs[0]), "unloaded model entered ledger");
    w.refs[0].root = &w.roots[0];
    w.scan(1200, 8, 8);
    check(g_state.outlined.contains(&w.refs[0]), "loaded model waited for next slice");
}
void nearby_is_revalidated() {
    World w(40, true);
    w.kind(0, 6); w.kind(1, 11);
    w.refs[1].actor = w.refs[1].dead = true;
    w.scan(1000, 0, 8);
    w.refs[0].loot = w.refs[1].loot = 0;
    w.refs[2].deleted = true; w.refs[3].disabled = true;
    w.refs[4].data.location.x = 100;
    w.kind(5, 1); g_cfg.categoryEnabled[1] = false;
    w.refs[6].actor = true;
    w.refs[7].data.location.y = -10;
    g_cfg.onlyInFront = true;
    w.scan(1200, 8, 8);
    for (int i = 0; i < 8; ++i) check(g_state.outlined.at(&w.refs[i]).dropAt != 0,
        "cached candidate bypassed loot/live/deleted/disabled/radius/category/FOV filter");
    check(g_state.emptySkips >= 2, "cached containers/corpses not rechecked");
}
void ttl_and_missing_cell_evict() {
    World w(40, true);
    w.scan(1000, 0, 8);
    w.scan(2400, 8, 8);
    check(!g_state.nearbyRefs.contains(&w.refs[0]), "cache TTL extended by reuse");
    w.scan(2600, 0, 0, false);
    check(g_state.nearbyRefs.empty(), "unvalidated cell survived cache pruning");
}
void duplicate_refs_are_not_candidates_twice() {
    World w(8, true);
    w.scan(1000, 0, 8);
    w.scan(1200, 0, 8);
    check(g_state.candCount == 8 && g_state.selCount == 8, "raw and reused refs not deduplicated");
}
void max_targets_applies_across_slices() {
    World w(40, true);
    g_cfg.maxTargets = 8;
    for (int round = 0; round < 20; ++round) w.scan(1000 + round * 200, (round % 5) * 8, 8);
    check(g_state.outlined.size() == 8, "each slice retained a separate MaxTargets set");
}
void model_changes_all_checked_and_fair() {
    World w(160);
    for (int i = 0; i < 3; ++i) w.scan(1000 + i * 200);
    check(g_state.outlined.size() == 160, "setup 160 mounted refs");
    for (int i = 0; i < 160; ++i) w.refs[i].root = &w.rebuilt[i];
    for (int round = 0; round < 5; ++round) {
        std::reverse(w.currentList.begin(), w.currentList.end());
        w.scan(2000 + round * 200);
        check(g_state.opsThisScan == 32, "idle repair budget not reused up to Verify3DPerScan");
    }
    for (int i = 0; i < 160; ++i) check(g_state.outlined.at(&w.refs[i]).last3D == &w.rebuilt[i],
        "model rebuild skipped due to list position");
}
void repairs_not_starved_by_removals() {
    World w(96);
    for (int i = 0; i < 2; ++i) w.scan(1000 + i * 200);
    for (int i = 0; i < 32; ++i) w.refs[i].root = &w.rebuilt[i];
    for (int i = 32; i < 96; ++i) {
        w.refs[i].data.location.x = 100;
        g_state.outlined.at(&w.refs[i]).dropAt = 1;
    }
    w.scan(2000);
    check(g_state.outline3DReasserts >= 16, "removals starved model repairs");
    check(g_state.opsThisScan == 64, "repair/removal budget accounting");
}
void frequently_rebuilt_ref_cannot_starve_old_ref() {
    World w(2);
    w.scan(1000);
    g_cfg.maxOutlineOpsPerScan = 1;
    w.refs[0].root = &w.rebuilt[0]; w.refs[1].root = &w.rebuilt[1];
    w.scan(1200);
    w.refs[0].root = &w.roots[0];
    w.scan(1400);
    check(g_state.outlined.at(&w.refs[1]).last3D == &w.rebuilt[1], "repair ordering unfair");
}
void single_op_budget_serves_all_queues() {
    World w(3);
    w.refs[2].root = nullptr;
    w.scan(1000);
    w.refs[1].data.location.x = 100;
    g_state.outlined.at(&w.refs[1]).dropAt = 1;
    w.refs[2].root = &w.roots[2];
    g_cfg.maxOutlineOpsPerScan = 1;
    for (int i = 0; i < 6; ++i) {
        w.refs[0].root = (i % 2) ? &w.roots[0] : &w.rebuilt[0];
        w.scan(1200 + i * 200);
    }
    check(!g_state.outlined.contains(&w.refs[1]), "single-op repairs starved expired removal");
    check(g_state.outlined.contains(&w.refs[2]), "single-op repairs/removals starved new mount");
    check(g_state.outline3DReasserts > 0, "single-op ordinary queues starved repairs");
}
void failed_color_change_attempted_once() {
    World w(1);
    w.scan(1000);
    w.kind(0, 9); w.refs[0].scanned = true; w.refs[0].failMount = true;
    mounts.clear(); removes.clear();
    w.scan(1200);
    check(mounts[&w.refs[0]] == 1 && removes[&w.refs[0]] == 1,
        "failed color change attempted twice in one scan");
    check(g_state.opsThisScan == 1 && !g_state.outlined.contains(&w.refs[0]), "failed mount ledger");
}
void scanner_yield_and_color_preserved() {
    World w(40, true);
    w.kind(0, 9);
    w.scan(1000, 0, 8);
    mounts.clear(); removes.clear();
    g_state.monocleOpen = true;
    w.scan(1200, 8, 8);
    check(!mounts.contains(&w.refs[0]) && !removes.contains(&w.refs[0]) &&
        g_state.outlined.at(&w.refs[0]).dropAt == 0, "native scanner yield regressed");
    g_state.monocleOpen = false; w.refs[0].scanned = true;
    w.scan(1400, 16, 8);
    check(g_state.outlined.at(&w.refs[0]).channel == 13, "flora did not switch to scanned channel");
}
void progress_only_path() {
    World w(1);
    w.kind(0, 9); w.refs[0].scanned = true;
    g_scannableOutlineStateReady = g_isResourceScannedReady = false;
    w.scan(1000);
    check(g_state.outlined.at(&w.refs[0]).channel == 13, "direct progress path incorrectly gated");
}
void boost_unlimited_and_legacy() {
    World w(200);
    g_state.resyncBoostUntilMs = 5000;
    w.scan(1000);
    check(g_state.outlined.size() == 192, "resync boost not preserved");
    g_cfg.maxOutlineOpsPerScan = 0;
    w.scan(1200);
    check(g_state.outlined.size() == 200, "unlimited budget not preserved");
    g_cfg.channelMode = false;
    w.kind(0, 9); w.refs[0].scanned = true;
    w.scan(1400);
    check(g_state.outlined.at(&w.refs[0]).state == 5, "legacy state change not preserved");
}
int main() {
    no_model_does_not_block();
    pending_survives_slice();
    newly_loaded_model_retries_next_scan();
    nearby_is_revalidated();
    ttl_and_missing_cell_evict();
    duplicate_refs_are_not_candidates_twice();
    max_targets_applies_across_slices();
    model_changes_all_checked_and_fair();
    repairs_not_starved_by_removals();
    frequently_rebuilt_ref_cannot_starve_old_ref();
    single_op_budget_serves_all_queues();
    failed_color_change_attempted_once();
    scanner_yield_and_color_preserved();
    progress_only_path();
    boost_unlimited_and_legacy();
    std::cout << "PASS: 15 production discovery/scheduler regression scenarios\n";
}
'''


def make_cpp(source: str) -> str:
    clean = without_comments(source)
    candidate = definition(clean, "struct Candidate\n")
    entry = definition(clean, "struct OutlineEntry\n")
    nearby = definition(clean, "struct NearbyRef\n")
    sync = definition(clean, "void SyncNativeOutline(")
    rescan = definition(clean, "void Rescan(")
    tail = rescan[rescan.index("const RE::NiPoint3 origin"):]
    scan = """void Scan(std::uint64_t a_nowMs, RE::PlayerCharacter* a_player,
        std::vector<ScanCell> scanCells) {
        const auto cellCount = scanCells.size();
        const bool yieldTargets = g_cfg.yieldTargetsWhileScanning && g_state.monocleOpen;
        constexpr float kYawForwardSign = 1;
        """ + tail
    special = {"outlined", "nearbyRefs", "lootMemo", "candTypes", "rejectTypes", "categoryCounts"}
    fields = sorted(set(re.findall(r"g_state\.(\w+)", scan + sync)) - special)
    state = "struct State {\n" + nearby + "\n"
    state += "struct LootMemoRec { std::uint64_t atMs, serial; int loot; };\n"
    state += "std::unordered_map<RE::TESObjectREFR*, OutlineEntry> outlined;\n"
    state += "std::unordered_map<RE::TESObjectREFR*, NearbyRef> nearbyRefs;\n"
    state += "std::unordered_map<RE::TESObjectREFR*, LootMemoRec> lootMemo;\n"
    state += "std::array<std::uint32_t, 256> candTypes{}, rejectTypes{};\n"
    state += "std::array<std::uint32_t, kCategoryCount> categoryCounts{};\n"
    state += "\n".join(f"std::uint64_t {name} = 0;" for name in fields)
    state += "\n} g_state;\n"
    return PREAMBLE + entry + candidate + state + STUBS + sync + scan + TESTS


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vcvars", type=Path, help="vcvars64.bat, if cl is not on PATH")
    args = parser.parse_args()
    env = os.environ.copy()
    if args.vcvars:
        command = f'call "{args.vcvars.resolve()}" >nul && set'
        result = subprocess.run(command, shell=True, capture_output=True, text=True, errors="replace")
        if result.returncode:
            print(result.stdout, file=sys.stderr)
            print(result.stderr, file=sys.stderr)
            return result.returncode
        env.update(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line and not line.startswith("="))
    compiler = shutil.which("cl", path=env.get("Path", env.get("PATH")))
    if not compiler:
        parser.error("cl.exe not found; pass --vcvars or use a VS developer shell")
    output = ROOT / "out" / "tests" / "highlight-scheduler"
    output.mkdir(parents=True, exist_ok=True)
    cpp = output / "regression.cpp"
    exe = output / "regression.exe"
    cpp.write_text(make_cpp((ROOT / "plugin" / "src" / "AlwaysScan.cpp").read_text(encoding="utf-8")), encoding="utf-8")
    subprocess.run([compiler, "/nologo", "/std:c++20", "/EHsc", "/utf-8", "/W4",
                    str(cpp), f"/Fo{output / 'regression.obj'}", f"/Fe{exe}"],
                   cwd=output, env=env, check=True)
    return subprocess.run([str(exe)], cwd=output, env=env, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
