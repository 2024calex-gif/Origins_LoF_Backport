// ---------------------------------------------------------------------------------------------------------
// StateLog.inl (v0.26.0): player state log, section P) of the header comment in BackEjectLoF.cpp.
// Included by BackEjectLoF.cpp inside its anonymous namespace, after GraphVars.inl.
// Read-only apart from one mid-function hook at 0x1427554E3 (the state Enter() call in ACU.exe, 7 stolen bytes,
// the same bytes ACUFixes uses its logger at 0x1427554F2 for). The hook only reads the state node and writes a ring.
// ---------------------------------------------------------------------------------------------------------
namespace statelog {

constexpr uintptr_t kHookAddr = 0x1427554E3;       // mov rax, [rbx+0xE0] (7 bytes)
constexpr size_t kHookBytes = 7;
constexpr uint8 kHookExpect[kHookBytes] = { 0x48, 0x8B, 0x83, 0xE0, 0x00, 0x00, 0x00 };
constexpr size_t kRingSize = 4096;                 // hook -> frame ring (single producer, the game thread)
constexpr size_t kHistSize = 1500;                 // frame-side history
constexpr uint64 kSJEnterFn = 0x141ADC860;         // enter function of InSafeJump (v0.27.1)
constexpr uint64 kDWEnterFn = 0x141A4B010;         // enter function of DetachingFromWall (v0.27.1)
constexpr double kDumpBefore = 12.0;               // dump window: [trigger - 12 s, trigger + 8 s]
constexpr double kDumpAfter = 8.0;

// One entry as the hook writes it.
struct Entry
{
    uint64 qpc;        // QueryPerformanceCounter at the call
    uint64 enterFn;    // qword at node+0xE0 (may be 0)
    uint64 node;       // RBX: the state about to be entered
    uint32 depth;      // parentStack size
};

// One entry as the frame side keeps it.
struct HistEntry
{
    double t;          // QPC seconds (NowSec clock)
    uint64 enterFn;
    uint64 node;
    uint32 depth;
    float z;           // player z at drain time, -1 when there is no player
    bool armed;        // origins::g_armedAtomic at drain time
};

std::atomic<bool> g_on{ true };                    // menu checkbox, default ON
std::atomic<uint64> g_writeIdx{ 0 };               // entries written by the hook (only the game thread stores it)
Entry g_ring[kRingSize];                           // written by the hook only, read by Frame after an acquire load
bool g_installed = false;                          // the hook is active
uint64 g_readIdx = 0;                              // frame side: next entry to copy
uint64 g_lost = 0;                                 // frame side: entries overwritten before they were copied
uint64 g_drained = 0;                              // frame side: entries copied so far
HistEntry g_hist[kHistSize];                       // frame side history, oldest overwritten first
size_t g_histHead = 0;                             // next slot to write
size_t g_histCount = 0;                            // valid slots (<= kHistSize)
// v0.27.1 (for the ctor probe): player enters of InSafeJump / DetachingFromWall. Written by the hook, read by ctorprobe::Frame.
// The qpc store is relaxed and happens before the release fetch_add of the counter, so a reader that acquires the counter sees it.
std::atomic<uint64> g_lastSJEnterQpc{ 0 };
std::atomic<uint64> g_sjEnters{ 0 };
std::atomic<uint64> g_lastDWEnterQpc{ 0 };
std::atomic<uint64> g_dwEnters{ 0 };

// Plain SEH function: no C++ objects, so __try is allowed.
bool ReadBytesGuarded(uintptr_t addr, uint8* out, size_t n)
{
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct NodeInfo
{
    uint64 enterFn;
    uint32 depth;
    bool isPlayer;
};

// Plain SEH function: no C++ objects, so __try is allowed. Same player test as ACUFixes (LoggingTheHumanStates.cpp):
// the topmost parent in parentStack is the player's HumanStatesHolder; the holder itself also counts.
bool ReadNodeGuarded(uint64 node, NodeInfo* out)
{
    __try {
        FunctorBase* st = reinterpret_cast<FunctorBase*>(node);
        const uint64* enterSlot = reinterpret_cast<const uint64*>(node + 0xE0);
        FunctorBase* holder = (FunctorBase*)HumanStatesHolder::GetForPlayer();
        out->enterFn = *enterSlot;
        const size_t depth = st->parentStack.size;
        out->depth = static_cast<uint32>(depth);
        bool isPlayer = holder != nullptr && st == holder;
        if (!isPlayer && holder != nullptr && depth > 0)
            isPlayer = st->parentStack[static_cast<int>(depth) - 1] == holder;
        out->isPlayer = isPlayer;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Mid-function hook callback (GAME thread). No allocation, no logging, no locks, no exceptions.
void OnEnterState(AllRegisters* params)
{
    if (!g_on.load(std::memory_order_relaxed)) return;
    const uint64 node = params->rbx_;
    NodeInfo info{};
    if (!ReadNodeGuarded(node, &info) || !info.isPlayer) return;
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    const uint64 w = g_writeIdx.load(std::memory_order_relaxed);   // this thread is the only writer
    Entry& e = g_ring[w % kRingSize];
    e.qpc = static_cast<uint64>(qpc.QuadPart);
    e.enterFn = info.enterFn;
    e.node = node;
    e.depth = info.depth;
    if (info.enterFn == kSJEnterFn) {                  // v0.27.1: remember the last player InSafeJump enter
        g_lastSJEnterQpc.store(e.qpc, std::memory_order_relaxed);
        g_sjEnters.fetch_add(1, std::memory_order_release);
    } else if (info.enterFn == kDWEnterFn) {           // v0.27.1: same for DetachingFromWall
        g_lastDWEnterQpc.store(e.qpc, std::memory_order_relaxed);
        g_dwEnters.fetch_add(1, std::memory_order_release);
    }
    g_writeIdx.store(w + 1, std::memory_order_release);
}

class StateEnterHook : public AutoAssemblerCodeHolder_Base
{
public:
    StateEnterHook()
    {
        PresetScript_CCodeInTheMiddle(kHookAddr, kHookBytes, OnEnterState, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
    }
};
std::unique_ptr<AutoAssembleWrapper<StateEnterHook>> g_hook;

// Verifies the 7 bytes, then creates and activates the hook. On a mismatch nothing is created or written.
void Install()
{
    uint8 got[kHookBytes] = {};
    const bool readOk = ReadBytesGuarded(kHookAddr, got, kHookBytes);
    if (!readOk || memcmp(got, kHookExpect, kHookBytes) != 0) {
        char hex[64] = "unreadable";
        if (readOk) {
            size_t pos = 0;
            for (size_t i = 0; i < kHookBytes; ++i)
                pos += static_cast<size_t>(snprintf(hex + pos, sizeof(hex) - pos, "%s%02X", i ? " " : "", got[i]));
        }
        LogLine("statelog: bytes at 0x1427554E3 differ (%s): NOT installed", hex);
        return;
    }
    g_hook = std::make_unique<AutoAssembleWrapper<StateEnterHook>>();
    g_hook->Activate();
    g_installed = true;
    LogLine("statelog: hook installed at 0x1427554E3");
}

double QpcFreq()
{
    static const double f = [] { LARGE_INTEGER x{}; QueryPerformanceFrequency(&x); return static_cast<double>(x.QuadPart); }();
    return f;
}

// Frame callback (section P). Called from GameFrame right after graphvars::Frame(), every frame.
void Frame()
{
    const uint64 w = g_writeIdx.load(std::memory_order_acquire);
    if (w - g_readIdx > kRingSize) {                     // the producer lapped us: skip ahead and count the loss
        g_lost += (w - g_readIdx) - kRingSize;
        g_readIdx = w - kRingSize;
    }
    if (w == g_readIdx) return;

    const double freq = QpcFreq();
    const float z = g_hasPlayer ? g_playerPos[2] : -1.0f;
    const bool armed = origins::g_armedAtomic.load();
    for (uint64 i = g_readIdx; i < w; ++i) {
        const Entry e = g_ring[i % kRingSize];
        HistEntry& h = g_hist[g_histHead];
        h.t = static_cast<double>(e.qpc) / freq;
        h.enterFn = e.enterFn;
        h.node = e.node;
        h.depth = e.depth;
        h.z = z;
        h.armed = armed;
        g_histHead = (g_histHead + 1) % kHistSize;
        if (g_histCount < kHistSize) ++g_histCount;
        ++g_drained;
    }
    g_readIdx = w;
}

// Writes states_<stamp>_<k>.csv: the history entries with t in [triggerT - 12, triggerT + 8]. Called by graphvars::WriteDump.
void Dump(const std::string& stamp, int k, double triggerT, const char* reason)
{
    const std::string name = "states_" + stamp + "_" + std::to_string(k) + ".csv";
    const fs::path path = AbsolutePathInThisDLLDirectory("BackEjectLoF\\traj\\" + name);
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        LogLine("statelog: could not write %s", path.string().c_str());
        return;
    }

    WriteTrajLine(out, "# BackEjectLoF 0.27.1 player states (read-only)");
    WriteTrajLine(out, "# reason=%s", reason);
    WriteTrajLine(out, "# trigger_t=%.4f", triggerT);
    WriteTrajLine(out, "# lost=%llu", static_cast<unsigned long long>(g_lost));
    out << "t,enter,node,depth,z,armed\n";

    int rows = 0;
    const size_t start = (g_histHead + kHistSize - g_histCount) % kHistSize;   // oldest entry
    char line[160];
    for (size_t s = 0; s < g_histCount; ++s) {
        const HistEntry& h = g_hist[(start + s) % kHistSize];
        if (h.t < triggerT - kDumpBefore || h.t > triggerT + kDumpAfter) continue;
        snprintf(line, sizeof(line), "%.4f,0x%llX,0x%llX,%u,%.3f,%d", h.t - triggerT,
                 static_cast<unsigned long long>(h.enterFn), static_cast<unsigned long long>(h.node),
                 static_cast<unsigned>(h.depth), static_cast<double>(h.z), h.armed ? 1 : 0);
        out << line << "\n";
        ++rows;
    }
    out.close();
    LogLine("statelog: wrote %s rows=%d", name.c_str(), rows);
}

// Menu: the checkbox and the recorder status.
void Menu()
{
    bool on = g_on.load();
    if (ImGui::Checkbox("Log player states (v0.26)", &on)) g_on.store(on);
    ImGui::Text("statelog: hook %s, entries %llu, lost %llu", g_installed ? "installed" : "not installed",
                static_cast<unsigned long long>(g_drained), static_cast<unsigned long long>(g_lost));
}

} // namespace statelog
