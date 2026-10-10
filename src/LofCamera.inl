// ---------------------------------------------------------------------------------------------------------
// LofCamera.inl (v0.22.0): LoF camera during the eject, section L) of the header comment in BackEjectLoF.cpp.
// Included by BackEjectLoF.cpp inside its anonymous namespace, right after CameraDiag.inl.
// Uses camdiag:: guarded readers and origins::g_armedAtomic.
//
// No write to ACU.exe memory. The rule objects of two camera nodes (the LoF node and its parent group) get our own
// vtable: a copy of the BooleanRuleCondition vtable (0x14306F850) with slot +0x58 (GetResult) replaced by our hook.
// Only those swapped objects go through the hook; every other BooleanRuleCondition keeps the game's vtable.
// The hook is called on the game thread, many times per frame: it returns true for the LoF rule while the eject is
// forced (armed to the end), and for the parent rule only in the landing phase.
// ---------------------------------------------------------------------------------------------------------
namespace lofcam {

using ull = unsigned long long;

constexpr uint64 kBoolVt = 0x14306F850ULL;             // BooleanRuleCondition vtable (13 slots, 0x00..0x60)
constexpr uint64 kBoolRuleOrigFn = 0x1420B5210ULL;     // GetResult the slot +0x58 must hold before we swap
constexpr uint64 kLofNodeHandle = 0x1316A17B3EULL;     // SharedBlock handle of the LoF camera node
constexpr uint64 kParentHandle = 0x102BC104D7ULL;      // SharedBlock handle of the LoF node's parent group
constexpr uint64 kHayCamHandle = 0x139346D58AULL;      // SharedBlock handle of the hay camera node
constexpr uint64 kRootHandle = 0x863EA9EBAULL;         // SharedBlock handle of the root selector node (reference)
constexpr uint64 kMinPtr = 0x10000ULL;
constexpr int kSearchDepth = 3;                        // BFS levels below the start node
constexpr int kSearchNodes = 200;                      // nodes dequeued per search
constexpr int kSearchChildren = 64;                    // children read per node
constexpr uint64 kSearchEvery = 30;                    // frames between searches (a rule is unknown)
constexpr uint64 kValidateEvery = 60;                  // frames between rule re-checks (a rule is known)

// Our vtable copy: [0] is the index -1 qword (0), [1 + k] is the original slot k (k = 0..12), slot 11 is the hook.
alignas(16) uint64 g_fakeVt[16] = {};
const uint64 kFakeVtAddr = static_cast<uint64>(reinterpret_cast<uintptr_t>(&g_fakeVt[1]));

std::atomic<uint64> g_lofRule{ 0 };                    // rule pointer of the LoF node, 0 = unknown
std::atomic<uint64> g_lofNode{ 0 };                    // LoF node object, 0 = unknown
std::atomic<uint64> g_parentRule{ 0 };                 // rule pointer of the parent group, 0 = unknown
std::atomic<uint64> g_parentNode{ 0 };                 // parent group object, 0 = unknown
std::atomic<bool> g_on{ false };                       // menu checkbox, default OFF (v0.22)
std::atomic<bool> g_bridgeOn{ false };                 // menu checkbox, default OFF (v0.22)
std::atomic<bool> g_forceLof{ false };                 // hook: the LoF rule returns true
std::atomic<bool> g_forceParent{ false };              // hook: the parent rule returns true (landing phase)
std::atomic<uint64> g_forcedLof{ 0 };                  // forced true results for the LoF rule
std::atomic<uint64> g_forcedParent{ 0 };               // forced true results for the parent rule
uint64 g_orig = 0;                                     // original GetResult (set by Install)
bool g_installed = false;

uint64 g_frameN = 0;                                   // frames seen by Frame()
uint64 g_lastSearch = 0;
uint64 g_lastValidate = 0;

// Force state (frame side only).
bool g_active = false;
double g_t0 = 0.0;
double g_landT = -1.0;
float g_hayZ = 0.0f;
uint32 g_serialSeen = 0;
bool g_armedPrev = false;

// Hook: runs on the GAME thread. No allocation, no logging, no locks, no SEH, no C++ objects.
bool __fastcall BoolRuleResultHook(uint64 rule)
{
    if (g_on.load(std::memory_order_relaxed)) {
        if (g_forceLof.load(std::memory_order_relaxed) && rule == g_lofRule.load(std::memory_order_relaxed)) {
            g_forcedLof.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (g_bridgeOn.load(std::memory_order_relaxed) && g_forceParent.load(std::memory_order_relaxed)
            && rule == g_parentRule.load(std::memory_order_relaxed)) {
            g_forcedParent.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return reinterpret_cast<bool(__fastcall*)(uint64)>(kBoolRuleOrigFn)(rule);
}

// Builds the fake vtable from the game's BooleanRuleCondition vtable. Writes nothing to ACU.exe.
void Install()
{
    uint64 orig[13] = {};
    bool ok = true;
    for (int k = 0; k < 13; ++k) {
        if (!camdiag::ReadU64G(kBoolVt + 8ULL * static_cast<uint64>(k), orig[k]) || orig[k] == 0) ok = false;
    }
    if (!ok || orig[11] != kBoolRuleOrigFn) {
        LogLine("lofcam: BooleanRuleCondition vtable not as expected (slot +0x58 = 0x%llX): OFF",
                static_cast<ull>(orig[11]));
        return;
    }
    g_fakeVt[0] = 0;
    for (int k = 0; k < 13; ++k) g_fakeVt[1 + k] = orig[k];
    g_fakeVt[1 + 11] = static_cast<uint64>(reinterpret_cast<uintptr_t>(&BoolRuleResultHook));
    g_orig = kBoolRuleOrigFn;
    g_installed = true;
    LogLine("lofcam: fake BooleanRuleCondition vtable ready at 0x%llX (orig GetResult 0x%llX)",
            static_cast<ull>(kFakeVtAddr), static_cast<ull>(g_orig));
}

// Replaces the vtable pointer (first qword) of obj with newVt, if it holds expectOld (or newVt already).
// Plain SEH function: no C++ objects.
bool SwapVtGuarded(uint64 obj, uint64 expectOld, uint64 newVt)
{
    if (obj < kMinPtr) return false;
    __try {
        const uint64 cur = *reinterpret_cast<const uint64*>(obj);
        if (cur == newVt) return true;
        if (cur != expectOld) return false;
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(obj), static_cast<LONG64>(newVt));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// What one search found: the first LoF node / parent node with a non-zero rule.
struct Hits
{
    uint64 lofNode = 0;
    uint64 lofRule = 0;
    uint64 parNode = 0;
    uint64 parRule = 0;
};

void Consider(uint64 obj, uint64 handle, Hits& h)
{
    if (handle == kLofNodeHandle && h.lofRule == 0) {
        const uint64 r = camdiag::Rd64(obj + 0x18);
        if (r != 0) {
            h.lofNode = obj;
            h.lofRule = r;
        }
    } else if (handle == kParentHandle && h.parRule == 0) {
        const uint64 r = camdiag::Rd64(obj + 0x18);
        if (r != 0) {
            h.parNode = obj;
            h.parRule = r;
        }
    }
}

// Bounded breadth-first search from start through the +0x34 children. Stops when both handles are found.
// Frame side only; uses guarded reads.
void SearchRules(uint64 start, uint64 startHandle, Hits& h)
{
    if (start < kMinPtr) return;
    Consider(start, startHandle, h);
    if (h.lofRule != 0 && h.parRule != 0) return;

    struct QItem { uint64 obj; int depth; };
    std::vector<QItem> queue;
    queue.push_back(QItem{ start, 0 });
    size_t head = 0;
    int visited = 0;
    while (head < queue.size() && visited < kSearchNodes) {
        const QItem cur = queue[head++];
        ++visited;
        if (cur.depth >= kSearchDepth) continue;       // its children would be below the depth limit

        uint64 arr = 0;
        uint32 n = 0;
        if (!camdiag::ReadArrHdrG(cur.obj + 0x34, arr, n)) continue;
        if (n > static_cast<uint32>(kSearchChildren)) n = kSearchChildren;
        for (uint32 i = 0; i < n; ++i) {
            uint64 sb = 0;
            if (!camdiag::ReadU64G(arr + 8ULL * i, sb) || sb == 0) continue;
            uint64 co = 0, ch = 0;
            if (!camdiag::ReadSharedBlockGuarded(sb, co, ch)) continue;
            if (co < kMinPtr) continue;
            Consider(co, ch, h);
            if (h.lofRule != 0 && h.parRule != 0) return;
            queue.push_back(QItem{ co, cur.depth + 1 });
        }
    }
}

// Stores a found rule and swaps its vtable (when the fake vtable is ready).
void StoreRule(const char* name, std::atomic<uint64>& ruleA, std::atomic<uint64>& nodeA, uint64 node, uint64 rule,
               uint64 searchHandle)
{
    nodeA.store(node);
    ruleA.store(rule);
    LogLine("lofcam: %s camera node 0x%llX rule 0x%llX found (search start handle 0x%llX)", name,
            static_cast<ull>(node), static_cast<ull>(rule), static_cast<ull>(searchHandle));
    if (!g_installed) return;
    if (SwapVtGuarded(rule, kBoolVt, kFakeVtAddr)) {
        LogLine("lofcam: %s rule 0x%llX vtable swapped", name, static_cast<ull>(rule));
    } else {
        LogLine("lofcam: %s rule 0x%llX vtable swap FAILED (vtable 0x%llX)", name, static_cast<ull>(rule),
                static_cast<ull>(camdiag::Rd64(rule)));
    }
}

// Re-checks one known rule: forgotten when its node no longer holds it, re-swapped when its vtable was put back.
void ValidateOne(const char* name, std::atomic<uint64>& ruleA, std::atomic<uint64>& nodeA)
{
    const uint64 rule = ruleA.load();
    if (rule == 0) return;
    const uint64 node = nodeA.load();
    if (camdiag::Rd64(node + 0x18) != rule) {
        LogLine("lofcam: %s rule changed, searching again", name);
        ruleA.store(0);
        nodeA.store(0);
        g_lastSearch = 0;
        return;
    }
    if (g_installed && camdiag::Rd64(rule) != kFakeVtAddr) {
        if (SwapVtGuarded(rule, kBoolVt, kFakeVtAddr)) {
            LogLine("lofcam: %s vtable re-swapped", name);
        } else {
            LogLine("lofcam: %s vtable re-swap FAILED (vtable 0x%llX)", name, static_cast<ull>(camdiag::Rd64(rule)));
        }
    }
}

// Force state machine (frame side). curHandle is the handle of the current camera node.
void ForceStep(uint64 curHandle)
{
    const bool armed = origins::g_armedAtomic.load();
    const uint32 serial = origins::GetArmSnapshot().serial;
    const double now = origins::NowSec();

    // START: a new arm (the window opened, or a new serial while already armed).
    if (armed && (!g_armedPrev || serial != g_serialSeen)) {
        g_serialSeen = serial;
        g_active = true;
        g_t0 = now;
        g_landT = -1.0;
        g_hayZ = g_lastEject.hayPos[2];
        g_forceLof.store(true);
        g_forceParent.store(false);
        LogLine("lofcam: eject start serial=%u hayZ=%.2f lofRule=0x%llX parentRule=0x%llX installed=%d",
                static_cast<unsigned>(serial), static_cast<double>(g_hayZ), static_cast<ull>(g_lofRule.load()),
                static_cast<ull>(g_parentRule.load()), g_installed ? 1 : 0);
    }

    if (g_active) {
        // LANDED: the player is down to the hay level (after the first 0.3 s).
        if (g_landT < 0.0 && now - g_t0 > 0.3 && g_hasPlayer && g_playerPos[2] <= g_hayZ + 1.5f) {
            g_landT = now;
            g_forceParent.store(true);
            LogLine("lofcam: landing phase t=%.2f z=%.2f", now - g_t0, static_cast<double>(g_playerPos[2]));
        }

        // END: the first condition that applies.
        const char* why = nullptr;
        if (curHandle == kHayCamHandle) why = "hay camera";
        else if (g_landT >= 0.0 && now - g_landT > 0.6) why = "0.6 s after landing";
        else if (!armed) why = "disarmed";
        else if (now - g_t0 > 6.0) why = "6 s cap";
        if (why != nullptr) {
            g_active = false;
            g_forceLof.store(false);
            g_forceParent.store(false);
            LogLine("lofcam: end (%s) t=%.2f forcedLof=%llu forcedParent=%llu", why, now - g_t0,
                    static_cast<ull>(g_forcedLof.load()), static_cast<ull>(g_forcedParent.load()));
        }
    }
    g_armedPrev = armed;
}

// Frame callback (section L). Called from GameFrame right after camdiag::Frame(frame), every frame.
void Frame()
{
    ++g_frameN;
    const uint64 frame = g_frameN;

    // Current camera node (guarded reads).
    uint64 cam = 0, curNode = 0, curHandle = 0;
    camdiag::CameraComponentGuarded(cam);
    if (cam) {
        const uint64 sb = camdiag::Rd64(cam + 0x9B8);
        if (sb) camdiag::ReadSharedBlockGuarded(sb, curNode, curHandle);
    }

    ForceStep(curHandle);

    // Re-validate the known rules every 60 frames.
    if (frame - g_lastValidate >= kValidateEvery) {
        g_lastValidate = frame;
        ValidateOne("LoF", g_lofRule, g_lofNode);
        ValidateOne("parent", g_parentRule, g_parentNode);
    }

    // Search at most once every 30 frames while either rule is unknown, from the current camera node.
    const bool lofUnknown = g_lofRule.load() == 0;
    const bool parUnknown = g_parentRule.load() == 0;
    if (!lofUnknown && !parUnknown) return;
    if (frame - g_lastSearch < kSearchEvery) return;
    g_lastSearch = frame;
    if (curNode < kMinPtr) return;

    Hits h;
    SearchRules(curNode, curHandle, h);
    if (lofUnknown && h.lofRule != 0) StoreRule("LoF", g_lofRule, g_lofNode, h.lofNode, h.lofRule, curHandle);
    if (parUnknown && h.parRule != 0) StoreRule("parent", g_parentRule, g_parentNode, h.parNode, h.parRule, curHandle);
}

// Menu: checkboxes and status line.
void Menu()
{
    bool on = g_on.load();
    if (ImGui::Checkbox("LoF camera during the eject (v0.21)", &on)) g_on.store(on);
    bool bridge = g_bridgeOn.load();
    if (ImGui::Checkbox("Bridge to the hay camera at landing (v0.21)", &bridge)) g_bridgeOn.store(bridge);

    const uint64 lr = g_lofRule.load();
    const uint64 pr = g_parentRule.load();
    const char* lofState = "unknown";
    if (lr != 0) lofState = (camdiag::Rd64(lr) == kFakeVtAddr) ? "swapped" : "not swapped";
    const char* parState = "unknown";
    if (pr != 0) parState = (camdiag::Rd64(pr) == kFakeVtAddr) ? "swapped" : "not swapped";
    ImGui::Text("lofcam: %s, LoF rule 0x%llX (%s), parent rule 0x%llX (%s), forced LoF %llu, parent %llu, active %d",
                g_installed ? "fake vtable ready" : "OFF", static_cast<ull>(lr), lofState, static_cast<ull>(pr),
                parState, static_cast<ull>(g_forcedLof.load()), static_cast<ull>(g_forcedParent.load()),
                g_active ? 1 : 0);
}

} // namespace lofcam
