// ---------------------------------------------------------------------------------------------------------
// GraphVars.inl (v0.26.0): graph variable recorder, section O) of the header comment in BackEjectLoF.cpp.
// Included by BackEjectLoF.cpp inside its anonymous namespace, right after HayCamera.inl.
// Read-only: the player's AtomAnimComponent -> pD0 (GraphEvaluation) -> rtcp (variable hashes and offsets) and
// stru400.graphVarsBuffer (the values). No write to game memory, no hooks.
// ---------------------------------------------------------------------------------------------------------
namespace statelog { void Dump(const std::string& stamp, int k, double triggerT, const char* reason); }   // StateLog.inl (v0.26)

namespace graphvars {

constexpr int kMaxVars = 1024;                        // variables read per frame (the list is the same every frame)
constexpr int kRing = 600;                            // samples kept (the oldest are overwritten)
constexpr double kAfterSec = 2.0;                     // seconds after a HayStack trigger before the dump
constexpr double kArmAfterSec = 6.0;                  // seconds after an eject arm before the dump
constexpr int kMaxDumps = 12;                         // CSV files per session
constexpr uint32 kHayStackHash = 0xd8fdbe1aU;         // the HayStack graph variable
constexpr int kMaxFaults = 50;                        // faulted reads before the recorder stops for the session

struct GSample
{
    double t;                  // NowSec() of the frame
    float pos[3];              // player position (valid when hasPlayer)
    bool hasPlayer;
    int n;                     // variables read in this sample
    uint32 values[kMaxVars];   // values, in the order of the hash list
};

// Plain SEH function: no C++ objects, so __try is allowed.
// Returns the number of variables written (0 when anything on the way is null, -1 on an SEH fault).
int ReadGraphVarsGuarded(uint32* hashesOut, uint32* valuesOut, int maxVars)
{
    __try {
        AtomAnimComponent* anim = ACU::GetPlayerCpnt_AtomAnimComponent();
        if (!anim) return 0;
        GraphEvaluation* ge = anim->pD0;
        if (!ge) return 0;
        AtomGraph_RTCP* rtcp = ge->rtcp;
        if (!rtcp) return 0;
        const uint32* hashArr = rtcp->graphVarHashes.arr;
        const uint32* offArr = rtcp->graphVarsOffsets.arr;
        const uint8* buf = ge->stru400.graphVarsBuffer.arr;
        if (!hashArr || !offArr || !buf) return 0;
        const uint64 bufSize = ge->stru400.graphVarsBuffer.size;
        int n = static_cast<int>(rtcp->graphVarHashes.size);
        n = std::min(n, static_cast<int>(rtcp->graphVarsOffsets.size));
        n = std::min(n, maxVars);
        for (int i = 0; i < n; ++i) {
            hashesOut[i] = hashArr[i];
            const uint64 off = offArr[i];
            if (off + 4 <= bufSize) memcpy(&valuesOut[i], buf + off, 4);
            else valuesOut[i] = 0xFFFFFFFFu;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

std::atomic<bool> g_on{ true };             // menu checkbox, default ON
bool g_stopped = false;                     // faulted reads stopped the recorder for the session
std::vector<GSample> g_ring;                // kRing samples, allocated once in Init()
size_t g_head = 0;                          // next slot to write
size_t g_count = 0;                         // valid samples (<= kRing)
uint32 g_hashes[kMaxVars] = {};             // variable hashes of the current list
uint32 g_hashScratch[kMaxVars] = {};        // hashes read this frame
int g_nHashes = 0;
int g_hayIdx = -1;                          // index of kHayStackHash in g_hashes, -1 when absent
bool g_hayPrev = false;                     // HayStack was non-zero in the previous good read
bool g_armInit = false;
uint32 g_armSeen = 0;                       // last arm serial seen by Frame()
bool g_pending = false;                     // a dump is waiting for its time
double g_pendingAt = 0.0;                   // NowSec() when the pending dump is written
double g_trigT = 0.0;                       // NowSec() of the trigger (CSV time zero)
const char* g_reason = "";                  // trigger reason of the pending dump
int g_dumps = 0;                            // dumps written this session
int g_faults = 0;                           // faulted reads this session
char g_last[256] = "";                      // menu status line

void Init()
{
    g_ring.resize(kRing);
}

void FindHayIndex()
{
    g_hayIdx = -1;
    for (int i = 0; i < g_nHashes; ++i) {
        if (g_hashes[i] == kHayStackHash) { g_hayIdx = i; break; }
    }
}

void Trigger(double now, double at, const char* reason)
{
    g_pending = true;
    g_pendingAt = at;
    g_trigT = now;
    g_reason = reason;
}

// Writes the ring (oldest to newest) to graph_<stamp>_<k>.csv. Called from Frame only, when a dump is due.
void WriteDump()
{
    // session stamp (first dump of this game session), same idea as FinishTraj
    static const std::string stamp = [] { char b[32]; std::time_t now = std::time(nullptr); std::tm tmv{};
        localtime_s(&tmv, &now); std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &tmv); return std::string(b); }();
    const int k = g_dumps + 1;
    const std::string name = "graph_" + stamp + "_" + std::to_string(k) + ".csv";
    const fs::path path = AbsolutePathInThisDLLDirectory("BackEjectLoF\\traj\\" + name);
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        LogLine("graphvars: could not write %s", path.string().c_str());
        snprintf(g_last, sizeof(g_last), "could not write %s", name.c_str());
        return;
    }

    const size_t ringSize = static_cast<size_t>(kRing);
    const size_t count = g_count;
    const size_t start = (g_head + ringSize - count) % ringSize;   // oldest sample
    const int nVars = g_nHashes;

    // first = value in the oldest sample; changed = differs between any two samples of the ring
    std::vector<uint32> firsts(static_cast<size_t>(nVars), 0u);
    std::vector<int> changed;
    for (int i = 0; i < nVars; ++i) {
        const uint32 first = count ? g_ring[start].values[i] : 0u;
        firsts[static_cast<size_t>(i)] = first;
        bool diff = false;
        for (size_t s = 1; s < count && !diff; ++s) {
            if (g_ring[(start + s) % ringSize].values[i] != first) diff = true;
        }
        if (diff) changed.push_back(i);
    }

    WriteTrajLine(out, "# BackEjectLoF 0.27.1 graph variables (read-only)");
    WriteTrajLine(out, "# reason=%s", g_reason);
    WriteTrajLine(out, "# trigger_t=%.4f", g_trigT);
    WriteTrajLine(out, "# vars=%d", nVars);
    WriteTrajLine(out, "# samples=%zu", count);
    for (int i = 0; i < nVars; ++i)
        WriteTrajLine(out, "# var %d hash=0x%08X first=0x%08X", i, static_cast<unsigned>(g_hashes[i]),
                      static_cast<unsigned>(firsts[static_cast<size_t>(i)]));

    std::string header = "t,x,y,z";
    char cb[64];
    for (int i : changed) {
        snprintf(cb, sizeof(cb), ",h%08X", static_cast<unsigned>(g_hashes[i]));
        header += cb;
    }
    out << header << "\n";

    for (size_t s = 0; s < count; ++s) {
        const GSample& g = g_ring[(start + s) % ringSize];
        snprintf(cb, sizeof(cb), "%.4f", g.t - g_trigT);
        std::string row = cb;
        if (g.hasPlayer) {
            snprintf(cb, sizeof(cb), ",%.3f,%.3f,%.3f", static_cast<double>(g.pos[0]), static_cast<double>(g.pos[1]),
                     static_cast<double>(g.pos[2]));
            row += cb;
        } else {
            row += ",,,";
        }
        for (int i : changed) {
            const uint32 v = g.values[i];
            const uint32 expo = (v >> 23) & 0xFFu;
            if (expo != 0 && expo != 0xFFu) {
                float fv = 0.0f;
                memcpy(&fv, &v, 4);
                snprintf(cb, sizeof(cb), ",%.6g", static_cast<double>(fv));
            } else if (v == 0) {
                snprintf(cb, sizeof(cb), ",0");
            } else {
                snprintf(cb, sizeof(cb), ",i%u", static_cast<unsigned>(v));
            }
            row += cb;
        }
        out << row << "\n";
    }
    out.close();
    statelog::Dump(stamp, k, g_trigT, g_reason);   // v0.26 (P): player states, same stamp, k, trigger and reason

    LogLine("graphvars: wrote %s (%s), samples=%zu vars=%d changed=%zu", name.c_str(), g_reason, count, nVars, changed.size());
    snprintf(g_last, sizeof(g_last), "last: %s, %zu samples, %zu changed", name.c_str(), count, changed.size());
}

// Frame callback (section O). Called from GameFrame right after haycam::Frame(), every frame.
void Frame()
{
    if (!g_on.load() || g_stopped || g_ring.size() != static_cast<size_t>(kRing)) return;
    const double t = origins::NowSec();

    // Arm trigger: a new game-thread arm (serial change). The first call only records the serial.
    const uint32 serial = origins::GetArmSnapshot().serial;
    if (!g_armInit) {
        g_armInit = true;
        g_armSeen = serial;
    } else if (serial != g_armSeen) {
        g_armSeen = serial;
        if (serial != 0 && !g_pending && g_dumps < kMaxDumps) Trigger(t, t + kArmAfterSec, "arm");
    }

    // Read into the next ring slot.
    GSample& slot = g_ring[g_head];
    const int n = ReadGraphVarsGuarded(g_hashScratch, slot.values, kMaxVars);
    bool listChanged = false;
    if (n > 0) {
        listChanged = (n != g_nHashes) || (memcmp(g_hashScratch, g_hashes, sizeof(uint32) * static_cast<size_t>(n)) != 0);
        if (listChanged) {
            if (g_nHashes > 0) {
                g_count = 0;                                  // the ring holds values of another list: drop them
                LogLine("graphvars: variable list changed (n=%d)", n);
            }
            memcpy(g_hashes, g_hashScratch, sizeof(uint32) * static_cast<size_t>(n));
            g_nHashes = n;
            FindHayIndex();
        }
        slot.t = t;
        slot.n = n;
        slot.hasPlayer = g_hasPlayer;
        if (g_hasPlayer) memcpy(slot.pos, g_playerPos, sizeof(slot.pos));
        else memset(slot.pos, 0, sizeof(slot.pos));
        g_head = (g_head + 1) % static_cast<size_t>(kRing);
        if (g_count < static_cast<size_t>(kRing)) ++g_count;

        // HayStack trigger: rising edge of the variable (non-zero = true).
        const bool hayNow = g_hayIdx >= 0 && g_hayIdx < n && slot.values[g_hayIdx] != 0;
        if (listChanged) g_hayPrev = hayNow;                  // a new list is not an edge
        if (hayNow && !g_hayPrev && !g_pending && g_dumps < kMaxDumps) Trigger(t, t + kAfterSec, "HayStack");
        g_hayPrev = hayNow;
    } else if (n < 0 || g_hasPlayer) {
        // A fault, or no graph while a player exists. No player and no graph is not a fault.
        ++g_faults;
        if (g_faults >= kMaxFaults) {
            g_stopped = true;
            g_pending = false;
            LogLine("graphvars: recorder OFF after %d faulted reads (this session)", g_faults);
            return;
        }
    }

    // Due dump (written after this frame's sample, so it is in the file).
    if (g_pending && t >= g_pendingAt) {
        WriteDump();
        g_pending = false;
        ++g_dumps;
    }
}

// Menu: the checkbox and the recorder status.
void Menu()
{
    bool on = g_on.load();
    if (ImGui::Checkbox("Record animation graph variables (v0.25)", &on)) g_on.store(on);
    if (g_stopped) ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "graphvars OFF after faults (this session, see log)");
    ImGui::Text("graphvars: vars %d, ring %d/%d, dumps %d, pending %d, faults %d", g_nHashes,
                static_cast<int>(g_count), kRing, g_dumps, g_pending ? 1 : 0, g_faults);
    if (g_last[0]) ImGui::TextUnformatted(g_last);
}

} // namespace graphvars
