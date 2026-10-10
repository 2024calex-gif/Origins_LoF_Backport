// ---------------------------------------------------------------------------------------------------------
// HayCamera.inl (v0.23.0): early hay camera during the eject, section M) of the header comment in BackEjectLoF.cpp.
// Included by BackEjectLoF.cpp inside its anonymous namespace, right after LofCamera.inl.
// Uses camdiag:: guarded readers, origins:: arm state and g_lastEject / g_hasPlayer / g_playerPos.
//
// No write to ACU.exe memory. Two rule objects get our own vtable: the parent group 0x103CA083FC and the hay camera
// node 0x139346D58A. Both rules are BooleanRuleConditions; our copy of their vtable (0x14306F850) has slot +0x58
// (GetResult, original 0x1420B5210) replaced by HayRuleHook. The hook always calls the original first, and only
// returns true for these two rules while the force window is open.
// ---------------------------------------------------------------------------------------------------------
namespace haycam {

using ull = unsigned long long;

constexpr uint64 kBoolVt = 0x14306F850ULL;             // BooleanRuleCondition vtable (13 slots, 0x00..0x60)
constexpr uint64 kBoolRuleOrigFn = 0x1420B5210ULL;     // GetResult the slot +0x58 must hold before we swap
constexpr uint64 kHayGroupHandle = 0x103CA083FCULL;    // SharedBlock handle of the group node above the hay camera
constexpr uint64 kHayCamHandle = 0x139346D58AULL;      // SharedBlock handle of the hay camera node
constexpr double kLeadSec = 0.20;                      // seconds of fall speed used for the force height
constexpr float kMinLead = 1.5f;                       // metres: smallest force height above the hay
constexpr double kMaxAfterLand = 0.8;                  // seconds: force window after the force starts
constexpr double kCapSec = 6.0;                        // seconds: force window after the arm
constexpr uint64 kMinPtr = 0x10000ULL;
constexpr int kSearchDepth = 6;                        // BFS levels below the root
constexpr int kSearchNodes = 800;                      // nodes expanded per search
constexpr uint32 kSearchChildren = 64;                 // children read per list
constexpr uint64 kSearchEvery = 30;                    // frames between searches (a rule is unknown)
constexpr uint64 kValidateEvery = 60;                  // frames between rule re-checks (a rule is known)

// Our vtable copy: [0] is the index -1 qword (0), [1 + k] is the original slot k (k = 0..12), slot 11 is the hook.
alignas(16) uint64 g_fakeVt[16] = {};
const uint64 kFakeVtAddr = static_cast<uint64>(reinterpret_cast<uintptr_t>(&g_fakeVt[1]));

std::atomic<uint64> g_groupRule{ 0 };                  // rule pointer of the group node, 0 = unknown
std::atomic<uint64> g_groupNode{ 0 };                  // group node object, 0 = unknown
std::atomic<uint64> g_hayRule{ 0 };                    // rule pointer of the hay camera node, 0 = unknown
std::atomic<uint64> g_hayNode{ 0 };                    // hay camera node object, 0 = unknown
std::atomic<bool> g_on{ false };                       // menu checkbox, default OFF (v0.24)
std::atomic<bool> g_force{ false };                    // hook: the two rules return true
std::atomic<bool> g_groupNatural{ false };             // hook: the group rule was true by itself while forcing
std::atomic<bool> g_hayNatural{ false };               // hook: the hay rule was true by itself while forcing
std::atomic<uint64> g_forced{ 0 };                     // forced true results (both rules)
bool g_installed = false;

uint64 g_frameN = 0;                                   // frames seen by Frame()
uint64 g_lastSearch = 0;
uint64 g_lastValidate = 0;

// Force state (frame side only).
bool g_active = false;
bool g_forcing = false;
double g_t0 = 0.0;
double g_landT = -1.0;                                 // time the force started
float g_hayZ = 0.0f;
float g_prevZ = 0.0f;
double g_prevT = 0.0;
float g_vz = 0.0f;                                     // vertical speed of the player, m/s (negative when falling)
uint32 g_serialSeen = 0;
bool g_armedPrev = false;

// Hook: runs on the GAME thread. No allocation, no logging, no locks, no SEH, no C++ objects.
// The original result is always computed first, so the game's own caching is unchanged.
bool __fastcall HayRuleHook(uint64 rule)
{
    const bool orig = reinterpret_cast<bool(__fastcall*)(uint64)>(kBoolRuleOrigFn)(rule);
    if (!g_force.load(std::memory_order_relaxed) || !g_on.load(std::memory_order_relaxed)) return orig;
    if (rule == g_groupRule.load(std::memory_order_relaxed)) {
        if (orig) g_groupNatural.store(true, std::memory_order_relaxed);
        else g_forced.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (rule == g_hayRule.load(std::memory_order_relaxed)) {
        if (orig) g_hayNatural.store(true, std::memory_order_relaxed);
        else g_forced.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return orig;
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
        LogLine("haycam: BooleanRuleCondition vtable not as expected (slot +0x58 = 0x%llX): OFF",
                static_cast<ull>(orig[11]));
        return;
    }
    g_fakeVt[0] = 0;
    for (int k = 0; k < 13; ++k) g_fakeVt[1 + k] = orig[k];
    g_fakeVt[1 + 11] = static_cast<uint64>(reinterpret_cast<uintptr_t>(&HayRuleHook));
    g_installed = true;
    LogLine("haycam: fake vtable ready at 0x%llX", static_cast<ull>(kFakeVtAddr));
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
        const LONG64 prev = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(obj),
                                                         static_cast<LONG64>(newVt), static_cast<LONG64>(expectOld));
        const uint64 p = static_cast<uint64>(prev);
        return p == expectOld || p == newVt;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// What one search found: the first group node / hay camera node with a non-zero rule.
struct Hits
{
    uint64 groupNode = 0;
    uint64 groupRule = 0;
    uint64 hayNode = 0;
    uint64 hayRule = 0;
};

void Consider(uint64 obj, uint64 handle, Hits& h)
{
    if (handle == kHayGroupHandle && h.groupRule == 0) {
        const uint64 r = camdiag::Rd64(obj + 0x18);
        if (r != 0) {
            h.groupNode = obj;
            h.groupRule = r;
        }
    } else if (handle == kHayCamHandle && h.hayRule == 0) {
        const uint64 r = camdiag::Rd64(obj + 0x18);
        if (r != 0) {
            h.hayNode = obj;
            h.hayRule = r;
        }
    }
}

// Bounded breadth-first search from the root through both child lists (+0x40, then +0x34). Stops when both
// handles are found. Frame side only; uses guarded reads.
void SearchRules(uint64 root, uint64 rootHandle, Hits& h)
{
    if (root < kMinPtr) return;
    Consider(root, rootHandle, h);
    if (h.groupRule != 0 && h.hayRule != 0) return;

    struct QItem { uint64 obj; int depth; };
    std::vector<QItem> queue;
    queue.push_back(QItem{ root, 0 });
    size_t head = 0;
    int visited = 0;
    while (head < queue.size() && visited < kSearchNodes) {
        const QItem cur = queue[head++];
        ++visited;
        if (cur.depth >= kSearchDepth) continue;       // its children would be below the depth limit

        const uint64 offs[2] = { 0x40, 0x34 };
        for (int L = 0; L < 2; ++L) {
            uint64 arr = 0;
            uint32 n = 0;
            if (!camdiag::ReadArrHdrG(cur.obj + offs[L], arr, n)) continue;
            if (n > kSearchChildren) n = kSearchChildren;
            for (uint32 i = 0; i < n; ++i) {
                uint64 sb = 0;
                if (!camdiag::ReadU64G(arr + 8ULL * i, sb) || sb == 0) continue;
                uint64 co = 0, ch = 0;
                if (!camdiag::ReadSharedBlockGuarded(sb, co, ch)) continue;
                if (co < kMinPtr) continue;
                Consider(co, ch, h);
                if (h.groupRule != 0 && h.hayRule != 0) return;
                queue.push_back(QItem{ co, cur.depth + 1 });
            }
        }
    }
}

// Stores a found rule and swaps its vtable (when the fake vtable is ready).
void StoreRule(const char* name, std::atomic<uint64>& ruleA, std::atomic<uint64>& nodeA, uint64 node, uint64 rule)
{
    nodeA.store(node);
    ruleA.store(rule);
    LogLine("haycam: %s node 0x%llX rule 0x%llX found", name, static_cast<ull>(node), static_cast<ull>(rule));
    if (!g_installed) return;
    if (SwapVtGuarded(rule, kBoolVt, kFakeVtAddr)) {
        LogLine("haycam: %s rule 0x%llX vtable swapped", name, static_cast<ull>(rule));
    } else {
        LogLine("haycam: %s rule 0x%llX vtable swap FAILED (vtable 0x%llX)", name, static_cast<ull>(rule),
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
        LogLine("haycam: %s rule changed, searching again", name);
        ruleA.store(0);
        nodeA.store(0);
        g_lastSearch = 0;
        return;
    }
    if (g_installed && camdiag::Rd64(rule) != kFakeVtAddr) {
        if (SwapVtGuarded(rule, kBoolVt, kFakeVtAddr)) {
            LogLine("haycam: %s vtable re-swapped", name);
        } else {
            LogLine("haycam: %s vtable re-swap FAILED (vtable 0x%llX)", name, static_cast<ull>(camdiag::Rd64(rule)));
        }
    }
}

// Force state machine (frame side), every frame.
void ForceStep()
{
    const bool armed = origins::g_armedAtomic.load();
    const uint32 serial = origins::GetArmSnapshot().serial;
    const double now = origins::NowSec();

    // START: a new arm (the window opened, or a new serial while already armed).
    if (armed && (!g_armedPrev || serial != g_serialSeen)) {
        g_serialSeen = serial;
        g_active = true;
        g_forcing = false;
        g_force.store(false);
        g_t0 = now;
        g_landT = -1.0;
        g_hayZ = g_lastEject.hayPos[2];
        g_prevZ = g_playerPos[2];
        g_prevT = now;
        g_groupNatural.store(false);
        g_hayNatural.store(false);
        LogLine("haycam: eject start serial=%u hayZ=%.2f groupRule=0x%llX hayRule=0x%llX",
                static_cast<unsigned>(serial), static_cast<double>(g_hayZ), static_cast<ull>(g_groupRule.load()),
                static_cast<ull>(g_hayRule.load()));
    }

    if (g_active) {
        // FORCE ON: the player is within max(1.5 m, 0.2 s of fall speed) above the hay, after the first 0.3 s.
        if (!g_forcing && g_hasPlayer) {
            const float z = g_playerPos[2];
            const double dt = now - g_prevT;
            if (dt > 0.001) g_vz = static_cast<float>((static_cast<double>(z) - static_cast<double>(g_prevZ)) / dt);
            g_prevZ = z;
            g_prevT = now;
            const float lead = std::max(kMinLead, -g_vz * static_cast<float>(kLeadSec));
            if (now - g_t0 > 0.3 && z - g_hayZ <= lead) {
                if (g_groupRule.load() != 0 && g_hayRule.load() != 0) {
                    g_forcing = true;
                    g_landT = now;
                    g_force.store(true);
                    LogLine("haycam: force on t=%.2f z=%.2f vz=%.1f lead=%.2f", now - g_t0, static_cast<double>(z),
                            static_cast<double>(g_vz), static_cast<double>(lead));
                } else {
                    LogLine("haycam: rules unknown, not forcing");
                    g_active = false;
                }
            }
        }

        // END: the first condition that applies.
        const char* why = nullptr;
        if (g_forcing && g_groupNatural.load() && g_hayNatural.load()) why = "game took over";
        else if (g_forcing && now - g_landT > kMaxAfterLand) why = "0.8 s cap";
        else if (!armed) why = "disarmed";
        else if (now - g_t0 > kCapSec) why = "6 s cap";
        if (why != nullptr) {
            g_force.store(false);
            g_active = false;
            g_forcing = false;
            LogLine("haycam: end (%s) t=%.2f forced=%llu groupNatural=%d hayNatural=%d", why, now - g_t0,
                    static_cast<ull>(g_forced.load()), g_groupNatural.load() ? 1 : 0, g_hayNatural.load() ? 1 : 0);
        }
    }
    g_armedPrev = armed;
}

// Frame callback (section M). Called from GameFrame right after lofcam::Frame(), every frame.
void Frame()
{
    ++g_frameN;
    const uint64 frame = g_frameN;

    // Re-validate the known rules every 60 frames.
    if (frame - g_lastValidate >= kValidateEvery) {
        g_lastValidate = frame;
        ValidateOne("group", g_groupRule, g_groupNode);
        ValidateOne("hay", g_hayRule, g_hayNode);
    }

    // Search at most once every 30 frames while either rule is unknown: from the root of the selector tree.
    const bool groupUnknown = g_groupRule.load() == 0;
    const bool hayUnknown = g_hayRule.load() == 0;
    if (groupUnknown || hayUnknown) {
        if (frame - g_lastSearch >= kSearchEvery) {
            g_lastSearch = frame;
            uint64 sbRoot = 0, sbCine = 0, sbFixed = 0;
            if (camdiag::ReadRootsGuarded(sbRoot, sbCine, sbFixed) && sbRoot != 0) {
                uint64 rootObj = 0, rootHandle = 0;
                if (camdiag::ReadSharedBlockGuarded(sbRoot, rootObj, rootHandle) && rootObj >= kMinPtr) {
                    Hits h;
                    SearchRules(rootObj, rootHandle, h);
                    if (groupUnknown && h.groupRule != 0) StoreRule("group", g_groupRule, g_groupNode, h.groupNode, h.groupRule);
                    if (hayUnknown && h.hayRule != 0) StoreRule("hay", g_hayRule, g_hayNode, h.hayNode, h.hayRule);
                }
            }
        }
    }

    ForceStep();
}

// Menu: checkbox and status line.
void Menu()
{
    bool on = g_on.load();
    if (ImGui::Checkbox("Early hay camera at landing (v0.23)", &on)) g_on.store(on);

    const uint64 gr = g_groupRule.load();
    const uint64 hr = g_hayRule.load();
    const char* groupState = "unknown";
    if (gr != 0) groupState = (camdiag::Rd64(gr) == kFakeVtAddr) ? "swapped" : "not swapped";
    const char* hayState = "unknown";
    if (hr != 0) hayState = (camdiag::Rd64(hr) == kFakeVtAddr) ? "swapped" : "not swapped";
    ImGui::Text("haycam: %s, group rule 0x%llX (%s), hay rule 0x%llX (%s), forced %llu, active %d",
                g_installed ? "fake vtable ready" : "OFF", static_cast<ull>(gr), groupState, static_cast<ull>(hr),
                hayState, static_cast<ull>(g_forced.load()), g_active ? 1 : 0);
}

} // namespace haycam
