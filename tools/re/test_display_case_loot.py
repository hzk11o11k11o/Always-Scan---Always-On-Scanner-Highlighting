"""Compile production display-case event, observation and empty-verdict logic against an isolated engine."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from test_highlight_scheduler import definition, without_comments

ROOT = Path(__file__).resolve().parents[2]

PREAMBLE = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
namespace RE {
enum class FormType : std::uint8_t { kCONT=0x1E, kREFR=0x40, kACHR=0x41, kMISC=0x20 };
template<class T> class NiPointer {
    T* ptr = nullptr;
public:
    NiPointer() = default;
    explicit NiPointer(T* value) : ptr(value) { if (ptr) ptr->IncRefCount(); }
    NiPointer(const NiPointer& rhs) : NiPointer(rhs.ptr) {}
    NiPointer(NiPointer&& rhs) noexcept : ptr(std::exchange(rhs.ptr, nullptr)) {}
    ~NiPointer() { reset(); }
    NiPointer& operator=(const NiPointer& rhs) { if (this != &rhs) reset(rhs.ptr); return *this; }
    NiPointer& operator=(NiPointer&& rhs) noexcept {
        if (this != &rhs) { reset(); ptr = std::exchange(rhs.ptr, nullptr); }
        return *this;
    }
    void reset(T* value=nullptr) {
        if (value == ptr) return;
        if (ptr) ptr->DecRefCount();
        ptr = value;
        if (ptr) ptr->IncRefCount();
    }
    T* get() const { return ptr; }
    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};
struct TESForm {
    std::array<std::byte, 0x28> head{};
    std::uint32_t id = 0;
    std::uint16_t pad = 0;
    FormType type = FormType::kCONT;
    bool display = true;
    mutable int owners = 0;
    void IncRefCount() const { ++owners; }
    void DecRefCount() const { check(owners > 0, "unbalanced reference release"); --owners; }
    std::uint32_t GetFormID() const { return id; }
    FormType GetFormType() const { return type; }
    static TESForm* LookupByID(std::uint32_t id);
};
static_assert(offsetof(TESForm, id) == 0x28);
static_assert(offsetof(TESForm, type) == 0x2E);
struct TESObjectCELL {};
struct NiPoint3 {
    float x=0, y=0, z=0;
    float GetSquaredDistance(const NiPoint3& p) const {
        return (x-p.x)*(x-p.x) + (y-p.y)*(y-p.y) + (z-p.z)*(z-p.z);
    }
};
struct TESObjectREFR : TESForm {
    TESObjectCELL* parentCell = nullptr;
    struct Data { NiPoint3 location; NiPointer<TESForm> objectReference; } data;
    void* inventory = nullptr;
    bool live=true, deleted=false, disabled=false, player=false;
    bool IsDeleted() const { return deleted; }
    bool IsDisabled() const { return disabled; }
    bool IsPlayerRef() const { return player; }
    NiPoint3 GetPosition() const { return data.location; }
};
struct PlayerCharacter : TESObjectREFR { static PlayerCharacter* GetSingleton(); };
enum class BSEventNotifyControl { kContinue };
template<class T> struct BSTEventSource {};
template<class T> struct BSTEventSink {
    virtual ~BSTEventSink() = default;
    virtual BSEventNotifyControl ProcessEvent(const T&, BSTEventSource<T>*) = 0;
};
}
namespace REX { template<class... T> void INFO(T&&...) {} }
std::unordered_map<std::uint32_t, RE::TESForm*> forms;
RE::PlayerCharacter* testPlayer = nullptr;
std::size_t lookupCalls = 0;
RE::TESForm* RE::TESForm::LookupByID(std::uint32_t id) {
    ++lookupCalls;
    auto it = forms.find(id);
    return it == forms.end() ? nullptr : it->second;
}
RE::PlayerCharacter* RE::PlayerCharacter::GetSingleton() { return testPlayer; }
std::uint64_t clockMs = 1000;
std::uint64_t NowMs() { return clockMs; }
struct Region { std::uintptr_t start; std::size_t size; };
std::vector<Region> memory;
void remember(const void* p, std::size_t size) { memory.push_back({reinterpret_cast<std::uintptr_t>(p), size}); }
bool SafeReadMem(const void* source, void* target, std::size_t size) {
    const auto p = reinterpret_cast<std::uintptr_t>(source);
    for (auto r : memory) {
        if (p >= r.start && p-r.start <= r.size && size <= r.size-(p-r.start)) {
            std::memcpy(target, source, size); return true;
        }
    }
    return false;
}
bool IsPlausiblePointer(std::uint64_t p) { return p >= 0x10000 && p < 0x0000800000000000ULL; }
bool LooksLikeLiveRef(std::uint64_t p) {
    for (auto [id, form] : forms) {
        (void)id;
        if (reinterpret_cast<std::uint64_t>(form) == p &&
            (form->type == RE::FormType::kREFR || form->type == RE::FormType::kACHR)) {
            return static_cast<RE::TESObjectREFR*>(form)->live;
        }
    }
    return false;
}
struct Config {
    bool skipEmptyLoot=true, skipDisplayCaseEmpty=true, displayCaseUiEmpty=true;
    bool displayCaseFrameWatch=true, containerLootEvents=true, displayCaseQuickOpenEmpty=false;
    bool skipNonPlayableLoot=true, skipEquippedLoot=true, skipBuiltinLoot=true, treatNullInvAsEmpty=true;
    float radiusMeters=50, unitsPerMeter=1;
    int displayCaseEmptyNoContentMs=3000, lootEventLogMax=0;
} g_cfg;
std::size_t g_invOff = offsetof(RE::TESObjectREFR, inventory);
bool IsDisplayCaseBase(const RE::TESForm* base) { return base && base->display; }
void RecordDisplayCaseRuntime(const RE::TESObjectREFR*) {}
void DcTrace(const RE::TESObjectREFR*, int, bool) {}
void DcUiProbe(const RE::TESObjectREFR*) {}
bool IsNonPlayableForm(std::uint64_t) { return false; }
bool IsCreatureBuiltinForm(std::uint64_t) { return false; }
'''

TESTS = r'''
struct Stack { std::uint64_t extra=0; std::uint32_t count=0, pad=0; };
struct InvItem {
    RE::TESForm* object=nullptr;
    std::uint64_t pad=0;
    std::uint32_t size=0, cap=0;
    Stack* stacks=nullptr;
    std::uint32_t flags=kInvItemFlagTemporary, pad2=0;
};
struct Inventory {
    std::array<std::byte, kOffInvData> head{};
    std::uint32_t size=0, cap=0;
    InvItem* items=nullptr;
};
static_assert(sizeof(Stack) == kOffStackSize);
static_assert(sizeof(InvItem) == kOffInvItemSize);
static_assert(offsetof(InvItem, size) == kOffInvItemStacks);
static_assert(offsetof(InvItem, flags) == kOffInvItemFlags);
static_assert(offsetof(Inventory, size) == kOffInvData);
struct World {
    static constexpr std::size_t count=40, itemCount=10;
    RE::TESObjectCELL cell;
    RE::PlayerCharacter pc;
    std::array<RE::TESObjectREFR, count> refs;
    std::array<RE::TESForm, count> bases;
    std::array<RE::TESForm, itemCount> objects;
    std::array<Inventory, count> inventories;
    std::array<std::array<InvItem, itemCount>, count> items;
    std::array<std::array<Stack, itemCount>, count> stacks;
    World() {
        g_state = {}; g_cfg = {}; g_lootEvtQueueCount = 0;
        forms.clear(); memory.clear(); clockMs=1000; lookupCalls=0;
        testPlayer=&pc; pc.parentCell=&cell;
        g_state.quickOpenActive=true; g_state.lootEvtActive=true;
        for (std::size_t j=0; j<itemCount; ++j) {
            objects[j].id=0x8000+static_cast<std::uint32_t>(j);
            objects[j].type=RE::FormType::kMISC;
        }
        for (std::size_t i=0; i<count; ++i) {
            refs[i].id=0x1000+static_cast<std::uint32_t>(i);
            refs[i].type=RE::FormType::kREFR;
            refs[i].parentCell=&cell;
            refs[i].data.objectReference.reset(&bases[i]);
            refs[i].inventory=&inventories[i];
            inventories[i].cap=itemCount; inventories[i].items=items[i].data();
            forms.emplace(refs[i].id, &refs[i]);
            for (std::size_t j=0; j<itemCount; ++j) {
                items[i][j].object=&objects[j]; items[i][j].size=1; items[i][j].cap=1;
                items[i][j].stacks=&stacks[i][j];
            }
        }
        remember(refs.data(), sizeof(refs)); remember(objects.data(), sizeof(objects));
        remember(inventories.data(), sizeof(inventories)); remember(items.data(), sizeof(items));
        remember(stacks.data(), sizeof(stacks));
    }
    ~World() { g_state={}; testPlayer=nullptr; }
    void contents(std::size_t i, std::uint32_t quantity, std::uint32_t entries=1) {
        inventories[i].size=quantity ? entries : 0;
        for (std::size_t j=0; j<entries; ++j) stacks[i][j].count=quantity;
    }
    void open(std::size_t i) {
        SasQuickContainerOpenedEvent event{&refs[i]};
        remember(&event, sizeof(event));
        g_quickOpenSink.ProcessEvent(event, nullptr);
        memory.pop_back();
        ProcessLootEvents(clockMs);
    }
    void transfer(std::size_t i, bool take, std::uint32_t quantity=1, std::size_t object=0) {
        SasContainerChangedEvent event{};
        (take ? event.source : event.target)=refs[i].id;
        event.baseObject=objects[object].id; event.itemCount=quantity;
        remember(&event, sizeof(event));
        g_invEvtSink.ProcessEvent(event, nullptr);
        memory.pop_back();
        ProcessLootEvents(clockMs);
    }
    void tick(std::uint64_t ms) { clockMs=ms; DcTickWatch(); }
    void watch(std::size_t i, std::uint64_t event=0) { DcWatchAdd(&refs[i], 0.0f, event); }
    bool watched(std::size_t i) const {
        for (std::size_t j=0; j<g_state.dcWatchCount; ++j)
            if (g_state.dcWatch[j].ref.get() == &refs[i]) return true;
        return false;
    }
    bool empty(std::size_t i) { return DisplayCaseReadsEmpty(&refs[i]) != kDcEmptyNone; }
    auto& verdict(std::size_t i) { return DisplayCaseVerdictFor(&refs[i]); }
    void snapshot(std::size_t i) {
        bool temporary=false; DcSnapOut snap{};
        check(RefLootState(&refs[i], &temporary, &snap) == 1, "content snapshot must be nonempty");
        NoteDisplayCaseOccupied(&refs[i], temporary, &snap);
    }
};
void full_watch_accepts_interaction() {
    World w;
    for (std::size_t i=0; i<kDcWatchMax; ++i) w.watch(i);
    check(!w.watched(12), "new target should start outside full observer table");
    for (std::size_t i=12; i<16; ++i) {
        w.open(i);
        check(w.watched(i), "interacted weapon/equipment display not promoted");
        check(w.verdict(i).noContentOpenMs == clockMs, "full table lost quick-open evidence");
        ++clockMs;
    }
    check(g_state.quickOpenMatched == 4 && g_state.quickOpenUnmatched == 0, "full table rejected events");
    check(g_state.dcWatchCount == kDcWatchMax, "per-frame read budget exceeded");
    w.tick(5000);
    for (std::size_t i=12; i<16; ++i) check(!w.empty(i), "unobserved time counted as empty observation");
    w.tick(8000);
    for (std::size_t i=12; i<16; ++i) check(w.empty(i), "promoted empty display stayed highlighted");
}
void unwatched_take_uses_scan_snapshot() {
    World w;
    for (std::size_t i=0; i<kDcWatchMax; ++i) w.watch(i);
    for (std::size_t i=12; i<16; ++i) {
        w.contents(i, 1); w.snapshot(i); w.contents(i, 0); w.transfer(i, true);
        check(w.empty(i), "unwatched last-item event was ignored");
        check(DcStickyFidHas(w.refs[i].id), "last-item removal was not sticky");
    }
    check(g_state.lootEvtEmptyDecided == 4, "take ledger did not process equipment displays");
}
void rapid_switch_preserves_pending_empty() {
    World w;
    w.open(0); w.tick(1000);
    clockMs=1100; w.open(1); w.tick(1100);
    w.bases[2].display=false; clockMs=1200; w.open(2);
    check(w.verdict(0).quickOpenMs == 0 && w.verdict(1).quickOpenMs == 0, "ordinary container did not end old active sessions");
    clockMs=1300; w.open(0);
    check(w.verdict(0).noContentOpenMs == 1000 && w.verdict(0).emptySinceMs == 1000, "reopen reset pending evidence");
    w.tick(3999); check(!w.empty(0) && !w.empty(1), "empty verdict preceded full observation window");
    w.tick(4100); check(w.empty(0) && w.empty(1), "target switching discarded pending empty verdict");
}
void duplicate_open_keeps_window_and_content() {
    World w;
    w.open(0); w.tick(1000); clockMs=2200; w.open(0);
    check(w.verdict(0).quickOpenMs == 1000 && w.verdict(0).noContentOpenMs == 1000, "duplicate open reset timers");
    w.tick(4000); check(w.empty(0), "duplicate event delayed empty confirmation");
    w.contents(1, 2); clockMs=5000; w.open(1); w.tick(5000);
    clockMs=5100; w.open(1);
    check(w.verdict(1).sawContentInSession && w.verdict(1).snap.cnt[0] == 2, "duplicate open erased content evidence");
}
void delayed_projection_cancels_empty() {
    World w;
    w.open(0); w.tick(1000); w.tick(3129);
    w.contents(0, 2); w.tick(3130);
    check(w.verdict(0).noContentOpenMs == 0, "late content failed to cancel pending empty");
    w.contents(0, 0); clockMs=3200; w.open(1); w.tick(3200);
    clockMs=3300; w.open(0); w.tick(3300); w.tick(9000);
    check(!w.empty(0), "closing/reopening a known occupied case falsely emptied it");
}
void partial_take_close_then_final_take() {
    World w;
    w.contents(0, 2); w.open(0); w.tick(1000);
    clockMs=1100; w.transfer(0, true);
    w.contents(0, 0); w.tick(1100); clockMs=1200; w.open(1);
    clockMs=1300; w.open(0); w.tick(6000);
    check(!w.empty(0) && w.verdict(0).snap.cnt[0] == 1, "partial take followed by close falsely emptied case");
    w.transfer(0, true); check(w.empty(0), "final removal failed after target switching");
}
void missing_or_incomplete_snapshot_is_conservative() {
    World w;
    w.open(0); w.tick(1000); w.transfer(0, true); w.tick(5000);
    check(!w.empty(0) && w.verdict(0).noContentOpenMs == 0, "take with no baseline falsely emptied display");
    w.contents(1, 1, 10); w.snapshot(1); w.contents(1, 0);
    for (std::size_t j=0; j<10; ++j) w.transfer(1, true, 1, j);
    check(!w.empty(1), "overflowed snapshot was treated as exact");
    w.contents(2, 1); w.snapshot(2); w.contents(2, 0); w.transfer(2, true, 2);
    check(!w.empty(2) && w.verdict(2).snap.uncertain, "over-removal was treated as trustworthy");
}
void put_cancels_and_relits_unwatched_case() {
    World w;
    w.open(0); w.tick(1000); clockMs=2000; w.transfer(0, false); w.tick(6000);
    check(!w.empty(0) && w.verdict(0).noContentOpenMs == 0, "put did not cancel pending empty");
    MarkDisplayCaseEmptied(&w.refs[13], "test");
    for (std::size_t i=0; i<kDcWatchMax; ++i) w.watch(i);
    check(!w.watched(13), "put target should be unwatched");
    w.transfer(13, false);
    check(!w.empty(13) && !DcStickyFidHas(w.refs[13].id), "unwatched put did not clear sticky empty");
}
void unknown_interrupts_confirmation() {
    World w;
    w.open(0); w.tick(1000); w.inventories[0].cap=0; w.inventories[0].size=1;
    w.tick(3999); check(!w.empty(0) && w.verdict(0).emptySinceMs == 0, "unknown read used as empty");
    w.inventories[0].size=0; w.inventories[0].cap=10;
    w.tick(4000); w.tick(6999); check(!w.empty(0), "unknown gap counted as continuous empty");
    w.tick(7000); check(w.empty(0), "confirmed empty failed after readable inventory returned");
}
void unopened_case_never_assumed_empty() {
    World w;
    w.watch(0); w.tick(1000); w.tick(100000);
    check(!w.empty(0), "closed uninspected case was assumed empty");
}
void frame_watch_off_still_handles_events() {
    World w;
    g_cfg.displayCaseFrameWatch=false;
    w.open(0); check(g_state.dcWatchCount == 0, "frame-watch switch ignored");
    w.verdict(0).emptySinceMs=1000;
    clockMs=4000; check(DisplayCaseReadsEmpty(&w.refs[0]) == kDcEmptyNoContent, "scan fallback needs observer membership");
    w.contents(1, 1); w.snapshot(1); w.contents(1, 0); w.transfer(1, true);
    check(w.empty(1), "frame-watch off disabled exact event ledger");
}
void loading_and_feature_switches_block_reads() {
    World w;
    g_state.loadingSeen=true; w.open(0);
    check(lookupCalls == 0 && g_state.displayCaseVerdict.empty(), "event resolved references while loading");
    g_state.loadingSeen=false; g_state.loadingNow=true; w.open(0);
    check(lookupCalls == 0, "previous-frame loading guard ignored");
    g_state.loadingNow=false;
    for (bool* flag : {&g_cfg.skipEmptyLoot, &g_cfg.skipDisplayCaseEmpty, &g_cfg.displayCaseUiEmpty}) {
        *flag=false; w.open(0); *flag=true;
        check(g_state.displayCaseVerdict.empty(), "disabled empty feature accepted evidence");
    }
    w.watch(0); g_cfg.displayCaseUiEmpty=false; w.tick(5000);
    check(g_state.displayCaseVerdict.empty(), "frame watch ignored empty feature switch");
    g_cfg.containerLootEvents=false; w.open(0);
    check(g_lootEvtQueueCount == 0, "disabled event feature did not drain queue");
}
void invalid_refs_and_ordinary_containers_ignored() {
    World w;
    w.refs[0].deleted=true; w.refs[1].disabled=true; w.refs[2].parentCell=nullptr;
    w.refs[3].type=RE::FormType::kACHR; w.bases[4].display=false;
    w.bases[5].type=RE::FormType::kMISC; w.refs[6].data.location.x=21;
    w.refs[7].data.location.x=std::numeric_limits<float>::quiet_NaN();
    for (std::size_t i=0; i<8; ++i) w.open(i);
    check(g_state.displayCaseVerdict.empty(), "invalid or non-display event entered verdict map");
    LootEvtRec rec{}; rec.kind=static_cast<std::uint8_t>(LootEvtKind::kQuickOpen);
    rec.refFid=w.refs[8].id; rec.refPtr=reinterpret_cast<std::uint64_t>(&w.refs[9]);
    PushLootEvt(rec); ProcessLootEvents(clockMs);
    check(g_state.displayCaseVerdict.empty(), "mismatched event address and form identity accepted");
    forms.erase(w.refs[8].id); rec.refPtr=0; PushLootEvt(rec); ProcessLootEvents(clockMs);
    check(g_state.displayCaseVerdict.empty(), "unloaded reference resolved");
    SasQuickContainerOpenedEvent invalid{reinterpret_cast<void*>(0x12345)};
    remember(&invalid, sizeof(invalid)); g_quickOpenSink.ProcessEvent(invalid, nullptr); memory.pop_back();
    check(g_lootEvtQueueCount == 0, "invalid callback pointer entered event queue");
}
void observer_eviction_releases_and_prioritizes() {
    World w;
    for (std::size_t i=0; i<kDcWatchMax; ++i) w.watch(i);
    check(w.refs[0].owners == 1, "observer did not retain reference");
    w.open(12); check(w.refs[0].owners == 0 && w.refs[12].owners == 1, "observer replacement leaked references");
    for (std::size_t i=0; i<kDcWatchMax; ++i) w.watch(i);
    check(w.watched(12), "passive discovery evicted active interaction");
    w.refs[12].data.location.x=21; w.tick(1000);
    check(!w.watched(12) && w.refs[12].owners == 0, "out-of-range observer was not released");
    w.watch(13); w.refs[13].live=false;
    for (std::size_t i=0; i<kDcWatchMissMax; ++i) w.tick(1010+i);
    check(!w.watched(13) && w.refs[13].owners == 0, "invalid observer retained forever");
    ClearWatchFromSceneReset();
    check(g_state.dcWatchCount == 0, "scene reset did not clear observer count");
    for (auto& ref : w.refs) check(ref.owners == 0, "scene reset retained stale-world reference");
}
void pointer_reuse_cannot_inherit_verdict() {
    World w;
    MarkDisplayCaseEmptied(&w.refs[0], "test");
    w.refs[0].id=0x4000;
    check(!w.empty(0), "reused address inherited another FormID's empty verdict");
    check(w.verdict(0).lastKnown == 0 && w.verdict(0).refFid == 0x4000, "reused address did not reset state");
}
void sticky_follows_form_and_content_clears_it() {
    World w;
    MarkDisplayCaseEmptied(&w.refs[0], "test");
    w.refs[1].id=w.refs[0].id;
    g_state.displayCaseVerdict.clear();
    check(DisplayCaseReadsEmpty(&w.refs[1]) == kDcEmptyStickyFid, "scene rebuild lost sticky FormID");
    w.contents(1, 1); w.snapshot(1);
    check(!w.empty(1) && !DcStickyFidHas(w.refs[1].id), "visible contents failed to clear sticky verdict");
}
void no_content_switch_off_and_zero_stack_counts() {
    World w;
    g_cfg.displayCaseEmptyNoContentMs=0; w.open(0); w.tick(1000); w.tick(100000);
    check(!w.empty(0), "no-content confirmation switch ignored");
    w.contents(1, 1); w.snapshot(1); w.stacks[1][0].count=0;
    bool temporary=false; DcSnapOut snap{};
    check(RefLootState(&w.refs[1], &temporary, &snap) == 0, "zero-count residual inventory was considered occupied");
    w.contents(2, 3); w.open(2); w.tick(100000); clockMs=100100; w.transfer(2, true, 0);
    check(!w.empty(2) && w.verdict(2).snap.cnt[0] == 3, "zero-count event changed content ledger");
}
void reentry_requires_fresh_observation() {
    World w;
    w.open(0); w.tick(1000); w.refs[0].data.location.x=100; w.tick(1100);
    check(w.verdict(0).emptySinceMs == 0 && w.verdict(0).noContentOpenMs == 1000, "range eviction did not interrupt only observation clock");
    w.refs[0].data.location.x=0; clockMs=6000; w.open(0); w.tick(6000);
    check(!w.empty(0), "unobserved time counted on reentry");
    w.tick(8999); check(!w.empty(0), "reentry did not require full empty interval");
    w.tick(9000); check(w.empty(0), "reentry failed to confirm empty after fresh interval");
}
void distant_put_clears_both_verdicts() {
    World w;
    MarkDisplayCaseEmptied(&w.refs[0], "test"); w.refs[0].data.location.x=100;
    w.transfer(0, false);
    check(!w.empty(0) && !DcStickyFidHas(w.refs[0].id), "distant put only cleared one sticky state");
    check(w.verdict(0).lastKnown == 1 && !w.verdict(0).snap.valid, "distant put retained stale ledger");
    w.refs[0].data.location.x=0; clockMs=5000; w.open(0); w.tick(10000);
    check(!w.empty(0), "known distant put regressed to no-content empty");
}
void expired_open_session_renews() {
    World w;
    g_cfg.displayCaseQuickOpenEmpty=true; w.contents(0, 1); w.open(0); w.tick(1000);
    w.contents(0, 0); clockMs=40000; w.open(0);
    check(w.verdict(0).quickOpenMs == 40000 && w.verdict(0).emptySinceMs == 0, "expired same-ref session did not renew");
    w.tick(40000); w.tick(40399); check(!w.empty(0), "renewed optional fallback counted old empty interval");
    w.tick(40400); check(w.empty(0), "renewed optional fallback never became eligible");
}
void removal_without_baseline_does_not_become_no_content() {
    World w;
    w.open(0); w.transfer(0, true); w.tick(1000);
    clockMs=1100; w.open(1); clockMs=1200; w.open(0); w.tick(10000);
    check(!w.empty(0) && w.verdict(0).noContentOpenMs == 0, "reopening erased prior removal evidence");
}
int main() {
    full_watch_accepts_interaction();
    unwatched_take_uses_scan_snapshot();
    rapid_switch_preserves_pending_empty();
    duplicate_open_keeps_window_and_content();
    delayed_projection_cancels_empty();
    partial_take_close_then_final_take();
    missing_or_incomplete_snapshot_is_conservative();
    put_cancels_and_relits_unwatched_case();
    unknown_interrupts_confirmation();
    unopened_case_never_assumed_empty();
    frame_watch_off_still_handles_events();
    loading_and_feature_switches_block_reads();
    invalid_refs_and_ordinary_containers_ignored();
    observer_eviction_releases_and_prioritizes();
    pointer_reuse_cannot_inherit_verdict();
    sticky_follows_form_and_content_clears_it();
    no_content_switch_off_and_zero_stack_counts();
    reentry_requires_fresh_observation();
    distant_put_clears_both_verdicts();
    expired_open_session_renews();
    removal_without_baseline_does_not_become_no_content();
    std::cout << "PASS: 21 production display-case loot regression scenarios\n";
}
'''


def make_cpp(source: str) -> str:
    clean = without_comments(source)
    constants = (
        "kOffFormID kOffInvData kOffInvItemSize kOffInvItemStacks kOffInvItemObject "
        "kOffInvItemFlags kInvItemFlagSlotMask kInvItemFlagTemporary kOffStackSize "
        "kOffStackCount kInvMaxItems kInvMaxStacks kInvWalkItemsMax kDcSnapMax "
        "kDcSnapStackBudget kLootEvtQueueMax kLootEvtPerRefLogMax kDcEmptyConfirmMs "
        "kQuickOpenSessionMs kDcWatchMax kDcWatchMissMax kDcWatchRadiusMeters "
        "kDisplayCaseVerdictMax kDcStickyFidMax kLootBuiltinLogMax"
    ).split()
    constant_code = "\n".join(re.search(rf"constexpr[^;\n]*\b{name}\b[^;]*;", clean)[0] for name in constants)
    names = [
        "bool DcStickyFidHas(", "bool DcStickyFidErase(", "void DcStickyFidAdd(",
        "State::DisplayCaseVerdict& DisplayCaseVerdictFor(", "void NoteDisplayCaseOccupied(",
        "const char* DcEmptyReasonText(", "int DisplayCaseReadsEmpty(", "void MarkDisplayCaseEmptied(",
        "RE::NiPointer<const RE::TESObjectREFR> ResolveDisplayCaseEvent(", "bool LootEvtMayLog(",
        "void ProcessLootEvents(", "void DcWatchAdd(const RE::TESObjectREFR* a_ref, float a_distSq, std::uint64_t a_eventMs)\n",
        "void DcTickWatch(", "void AddToSnap(DcSnapOut& a_snap, std::uint32_t a_baseFid, std::uint32_t a_count)\n",
        "int RefLootState(const RE::TESObjectREFR* a_ref, bool* a_outSawTemporary, DcSnapOut* a_outSnap)\n",
    ]
    implementations = "\n".join(definition(clean, name) for name in names)
    callbacks = definition(clean, "class SasInvEvtSink final") + definition(clean, "class SasQuickOpenSink final")
    special = {"displayCaseVerdict", "dcWatch", "dcStickyFids"}
    fields = sorted((set(re.findall(r"g_state\.(\w+)", implementations + callbacks)) | {"dcUiProbeLastMs"}) - special)
    state = "struct State {\n"
    for name in ("DcSnap", "DisplayCaseVerdict", "DcWatch", "DcStickyFidRec"):
        state += definition(clean, f"struct {name}\n") + "\n"
    state += "std::unordered_map<const RE::TESObjectREFR*, DisplayCaseVerdict> displayCaseVerdict;\n"
    state += "std::array<DcWatch, kDcWatchMax> dcWatch{};\n"
    state += "std::array<DcStickyFidRec, kDcStickyFidMax> dcStickyFids{};\n"
    state += "\n".join(f"std::uint64_t {field}=0;" for field in fields)
    state += "\n} g_state;\n"
    queue_start = clean.index("std::mutex                               g_lootEvtLock;")
    queue_end = clean.index("void PushLootEvt(", queue_start)
    events = "\n".join(definition(clean, name) for name in (
        "struct SasContainerChangedEvent\n", "struct SasQuickContainerOpenedEvent\n",
        "enum class LootEvtKind :", "struct LootEvtRec\n"))
    events += clean[queue_start:queue_end] + definition(clean, "void PushLootEvt(")
    reset_start = clean.index("g_state.dcWatch = {};")
    reset_end = clean.index("g_state.dcUiProbeLastMs = 0;", reset_start) + len("g_state.dcUiProbeLastMs = 0;")
    reset = "void ClearWatchFromSceneReset() {" + clean[reset_start:reset_end] + "}\n"
    declarations = """
void DcWatchAdd(const RE::TESObjectREFR*, float, std::uint64_t = 0);
void AddToSnap(DcSnapOut&, std::uint32_t, std::uint32_t);
int RefLootState(const RE::TESObjectREFR*, bool*, DcSnapOut*);
"""
    return (PREAMBLE + constant_code + state + definition(clean, "struct DcSnapOut\n") + events + callbacks
            + "SasInvEvtSink g_invEvtSink; SasQuickOpenSink g_quickOpenSink;\n"
            + definition(clean, "enum : int\n\t\t{\n\t\t\tkDcEmptyNone") + declarations
            + implementations + reset + TESTS)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vcvars", type=Path, help="vcvars64.bat, if cl is not on PATH")
    args = parser.parse_args()
    env = os.environ.copy()
    if args.vcvars:
        result = subprocess.run(f'call "{args.vcvars.resolve()}" >nul && set', shell=True,
                                capture_output=True, text=True, errors="replace")
        if result.returncode:
            print(result.stdout, file=sys.stderr)
            print(result.stderr, file=sys.stderr)
            return result.returncode
        env.update(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line and not line.startswith("="))
    compiler = shutil.which("cl", path=env.get("Path", env.get("PATH")))
    if not compiler:
        parser.error("cl.exe not found; pass --vcvars or use a VS developer shell")
    output = ROOT / "out" / "tests" / "display-case-loot"
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
