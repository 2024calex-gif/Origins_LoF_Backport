// ---------------------------------------------------------------------------------------------------------
// CtorProbe.inl (v0.27.1): state constructor probe, section Q) of the header comment in BackEjectLoF.cpp.
// Included by BackEjectLoF.cpp inside its anonymous namespace, after StateLog.inl (uses statelog::ReadNodeGuarded
// and the statelog::g_sjEnters / g_dwEnters counters).
// Read-only apart from two function-start hooks (9 stolen bytes each, verified before patching):
//   kind 0 "SJ": 0x141ABA990, constructor of state InSafeJumpOrLoweringToHang (called often, mostly discarded)
//   kind 1 "DW": 0x141A274D0, constructor of state DetachingFromWall
// Each callback only snapshots registers, xmm0..7, 48 stack qwords and the state-node candidates into a ring.
// The frame side keeps a history, counts entries per (kind, ret), and when a player enter of the matching state
// happens it logs the constructor entries just before it (the ones that led to the enter).
// ---------------------------------------------------------------------------------------------------------
namespace ctorprobe {

constexpr uintptr_t kSJAddr = 0x141ABA990;         // InSafeJumpOrLoweringToHang ctor
constexpr uintptr_t kDWAddr = 0x141A274D0;         // DetachingFromWall ctor
constexpr size_t kCtorBytes = 9;
constexpr uint8 kCtorExpect[kCtorBytes] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };   // push rbx; sub rsp,20h; mov rbx,rcx
constexpr size_t kRingSize = 256;                  // hook -> frame ring (single producer, the game thread)
constexpr size_t kHistSize = 256;                  // frame-side history of drained entries (oldest overwritten)
constexpr size_t kMaxRows = 64;                    // distinct (kind, ret) keys tracked; new keys are ignored when full
constexpr uint32 kFirstPerKey = 2;                 // the first entries of each new key are logged in full
constexpr size_t kNumRegs = 15;                    // rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15
constexpr size_t kStackQwords = 48;                // qwords copied from rsp
constexpr size_t kNodeSlots = kNumRegs + kStackQwords;   // slot j < 15: register j, else stack[j - 15]
constexpr size_t kMaxLogged = 200;                 // total full entry logs per session (trigger headers still print)
constexpr size_t kPerTrigger = 6;                  // full entries logged per trigger, newest first
constexpr int kTriggerFrames = 3;                  // a trigger is re-scanned for this many frames
constexpr double kBeforeSec = 0.5;                 // scan window around the enter: [enter - 0.5 s, enter + 0.05 s]
constexpr double kAfterSec = 0.05;
constexpr size_t kTopPairs = 16;                   // pairs on the "ctorprobe counts:" line
constexpr uint64 kTextLo = 0x140001000;            // range of a state's enter function (ACU.exe code)
constexpr uint64 kTextHi = 0x142E696BF;
constexpr uint64 kEnterOffset = 0xE0;              // node+0xE0 holds the enter function pointer
constexpr const char* kRegNames[kNumRegs] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8", "r9",
                                              "r10", "r11", "r12", "r13", "r14", "r15" };

// One entry as the hook writes it.
struct Entry
{
    uint64 qpc;                       // QueryPerformanceCounter at the call
    uint32 kind;                      // 0 = SJ, 1 = DW
    uint32 stackOk;                   // 1 when the 48 stack qwords were read
    uint64 regs[kNumRegs];            // general registers (rax via GetRAX)
    uint64 rsp;                       // GetRSP()
    float xmm[8][4];                  // low 4 floats of xmm0..xmm7
    uint64 stack[kStackQwords];       // qwords at rsp
    uint64 nodeEnter[kNodeSlots];     // qword at (candidate + 0xE0) when it is a state enter function, else 0
    uint8 nodeIsPlayer[kNodeSlots];   // statelog::ReadNodeGuarded player test, only when nodeEnter != 0
};

// Frame-side count of one (kind, ret) key. ret = stack[0].
struct CountRow
{
    uint32 kind;
    uint64 ret;
    uint64 count;
};

// Frame-side pending trigger (one per kind): scanned for kTriggerFrames frames after the player enter.
struct Pending
{
    bool active;
    uint64 enterQpc;                  // qpc of the player enter (statelog::g_last*EnterQpc)
    int framesLeft;
    size_t shown;                     // full entries logged for this trigger
};

std::atomic<bool> g_on{ true };                    // menu checkbox, default ON
std::atomic<uint64> g_writeIdx{ 0 };               // entries written by the hooks (only the game thread stores it)
Entry g_ring[kRingSize];                           // written by the hooks only, read by Frame after an acquire load
bool g_installedSJ = false;
bool g_installedDW = false;
uint64 g_readIdx = 0;                              // frame side: next entry to copy
uint64 g_lost = 0;                                 // frame side: entries overwritten before they were copied
uint64 g_captured = 0;                             // frame side: entries copied so far
uint64 g_logged = 0;                               // frame side: full entries written to the log (capped)
uint64 g_skipped = 0;                              // frame side: full entries that were due but over the cap
Entry g_hist[kHistSize];                           // frame side history of drained entries
bool g_histLogged[kHistSize];                      // per history slot: already written in full (cleared when overwritten)
size_t g_histHead = 0;                             // next slot to write
size_t g_histCount = 0;                            // valid slots (<= kHistSize)
CountRow g_rows[kMaxRows];                         // per (kind, ret) counts
size_t g_rowCount = 0;
Pending g_pend[2];                                 // [0] SJ, [1] DW
uint64 g_seenEnters[2] = { 0, 0 };                 // last seen statelog::g_sjEnters / g_dwEnters
uint64 g_trig[2] = { 0, 0 };                       // triggers seen per kind

// Plain SEH function: no C++ objects, so __try is allowed. Reads n qwords at addr into out (out is zeroed by the caller).
bool ReadQwordsGuarded(uint64 addr, uint64* out, size_t n)
{
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n * sizeof(uint64));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Plain SEH function: no C++ objects, so __try is allowed.
bool ReadBytesGuarded(uintptr_t addr, uint8* out, size_t n)
{
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Mid-function hook callback core (GAME thread). No allocation, no logging, no locks, no exceptions.
void Capture(uint32 kind, AllRegisters* p)
{
    if (!g_on.load(std::memory_order_relaxed)) return;
    const uint64 w = g_writeIdx.load(std::memory_order_relaxed);   // this thread is the only writer
    Entry& e = g_ring[w % kRingSize];

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    e.qpc = static_cast<uint64>(qpc.QuadPart);
    e.kind = kind;

    e.regs[0] = p->GetRAX();
    e.regs[1] = p->rbx_;
    e.regs[2] = p->rcx_;
    e.regs[3] = p->rdx_;
    e.regs[4] = p->rsi_;
    e.regs[5] = p->rdi_;
    e.regs[6] = p->rbp_;
    e.regs[7] = p->r8_;
    e.regs[8] = p->r9_;
    e.regs[9] = p->r10_;
    e.regs[10] = p->r11_;
    e.regs[11] = p->r12_;
    e.regs[12] = p->r13_;
    e.regs[13] = p->r14_;
    e.regs[14] = p->r15_;
    e.rsp = p->GetRSP();

    const xmmreg* xs[8] = { &p->XMM0, &p->XMM1, &p->XMM2, &p->XMM3, &p->XMM4, &p->XMM5, &p->XMM6, &p->XMM7 };
    for (size_t i = 0; i < 8; ++i)
        for (size_t k = 0; k < 4; ++k) e.xmm[i][k] = xs[i]->fa[k];

    memset(e.stack, 0, sizeof(e.stack));
    e.stackOk = ReadQwordsGuarded(e.rsp, e.stack, kStackQwords) ? 1 : 0;

    // Candidate values: the 15 registers, then the 48 stack qwords.
    uint64 vals[kNodeSlots];
    for (size_t j = 0; j < kNumRegs; ++j) vals[j] = e.regs[j];
    for (size_t j = 0; j < kStackQwords; ++j) vals[kNumRegs + j] = e.stack[j];

    for (size_t j = 0; j < kNodeSlots; ++j) {
        e.nodeEnter[j] = 0;
        e.nodeIsPlayer[j] = 0;
        const uint64 v = vals[j];
        if (v < 0x10000 || v > 0x7FFFFFFFFFFF) continue;
        uint64 enter = 0;
        if (!ReadQwordsGuarded(v + kEnterOffset, &enter, 1)) continue;
        if (enter < kTextLo || enter > kTextHi) continue;
        e.nodeEnter[j] = enter;
        statelog::NodeInfo info{};
        e.nodeIsPlayer[j] = (statelog::ReadNodeGuarded(v, &info) && info.isPlayer) ? 1 : 0;
    }

    g_writeIdx.store(w + 1, std::memory_order_release);
}

void OnSJ(AllRegisters* params) { Capture(0, params); }
void OnDW(AllRegisters* params) { Capture(1, params); }

class SJCtorHook : public AutoAssemblerCodeHolder_Base
{
public:
    SJCtorHook() { PresetScript_CCodeInTheMiddle(kSJAddr, kCtorBytes, OnSJ, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true); }
};
class DWCtorHook : public AutoAssemblerCodeHolder_Base
{
public:
    DWCtorHook() { PresetScript_CCodeInTheMiddle(kDWAddr, kCtorBytes, OnDW, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true); }
};
std::unique_ptr<AutoAssembleWrapper<SJCtorHook>> g_hookSJ;
std::unique_ptr<AutoAssembleWrapper<DWCtorHook>> g_hookDW;

// Verifies the 9 bytes at addr. On a mismatch (or unreadable memory) logs it and returns false.
bool VerifyBytes(const char* tag, uintptr_t addr)
{
    uint8 got[kCtorBytes] = {};
    const bool readOk = ReadBytesGuarded(addr, got, kCtorBytes);
    if (readOk && memcmp(got, kCtorExpect, kCtorBytes) == 0) return true;
    char hex[64] = "unreadable";
    if (readOk) {
        size_t pos = 0;
        for (size_t i = 0; i < kCtorBytes; ++i)
            pos += static_cast<size_t>(snprintf(hex + pos, sizeof(hex) - pos, "%s%02X", i ? " " : "", got[i]));
    }
    LogLine("ctorprobe: %s bytes at 0x%llX differ (%s): NOT installed", tag, static_cast<unsigned long long>(addr), hex);
    return false;
}

// Verifies each site's 9 bytes, then creates and activates its hook. A mismatch skips only that hook.
void Install()
{
    if (VerifyBytes("SJ", kSJAddr)) {
        g_hookSJ = std::make_unique<AutoAssembleWrapper<SJCtorHook>>();
        g_hookSJ->Activate();
        g_installedSJ = true;
        LogLine("ctorprobe: SJ hook installed at 0x%llX", static_cast<unsigned long long>(kSJAddr));
    }
    if (VerifyBytes("DW", kDWAddr)) {
        g_hookDW = std::make_unique<AutoAssembleWrapper<DWCtorHook>>();
        g_hookDW->Activate();
        g_installedDW = true;
        LogLine("ctorprobe: DW hook installed at 0x%llX", static_cast<unsigned long long>(kDWAddr));
    }
}

// Appends to buf at pos, never past cap.
void Append(char* buf, size_t cap, size_t& pos, const char* fmt, ...)
{
    if (pos >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n > 0) pos = (static_cast<size_t>(n) < cap - pos) ? pos + static_cast<size_t>(n) : cap - 1;
}

// Writes one captured entry as log lines (rare events, so direct logging is fine).
void LogEntry(const Entry& e, double freq, float z, bool armed)
{
    LogLine("ctorprobe %s t=%.4f z=%.3f armed=%d ret=0x%llX rsp=0x%llX",
            e.kind == 0 ? "SJ" : "DW", static_cast<double>(e.qpc) / freq, static_cast<double>(z), armed ? 1 : 0,
            static_cast<unsigned long long>(e.stack[0]), static_cast<unsigned long long>(e.rsp));

    // Registers, 8 per line.
    char buf[1024];
    size_t pos = 0;
    buf[0] = '\0';
    for (size_t j = 0; j < kNumRegs; ++j) {
        Append(buf, sizeof(buf), pos, "%s%s=0x%llX", j ? " " : "", kRegNames[j], static_cast<unsigned long long>(e.regs[j]));
        if (j == 7) { LogLine("  regs %s", buf); pos = 0; buf[0] = '\0'; }
    }
    LogLine("  regs %s", buf);

    // xmm0..7 low floats, 4 registers per line.
    for (size_t g = 0; g < 2; ++g) {
        pos = 0;
        buf[0] = '\0';
        for (size_t i = g * 4; i < g * 4 + 4; ++i)
            Append(buf, sizeof(buf), pos, "%sxmm%u=(%g %g %g %g)", (i != g * 4) ? " " : "", static_cast<unsigned>(i),
                   static_cast<double>(e.xmm[i][0]), static_cast<double>(e.xmm[i][1]),
                   static_cast<double>(e.xmm[i][2]), static_cast<double>(e.xmm[i][3]));
        LogLine("  %s", buf);
    }

    // State-node candidates among the registers.
    for (size_t j = 0; j < kNumRegs; ++j) {
        if (e.nodeEnter[j] == 0) continue;
        LogLine("  node reg=%s ptr=0x%llX enter=0x%llX player=%d", kRegNames[j],
                static_cast<unsigned long long>(e.regs[j]), static_cast<unsigned long long>(e.nodeEnter[j]),
                static_cast<int>(e.nodeIsPlayer[j]));
    }

    if (!e.stackOk) LogLine("  stk unreadable (values below are zeros)");
    for (size_t base = 0; base < kStackQwords; base += 8) {
        pos = 0;
        buf[0] = '\0';
        for (size_t i = base; i < base + 8; ++i) {
            const size_t slot = kNumRegs + i;
            Append(buf, sizeof(buf), pos, "%s[%02X]=0x%llX", (i != base) ? " " : "", static_cast<unsigned>(i),
                   static_cast<unsigned long long>(e.stack[i]));
            if (e.nodeEnter[slot] != 0)
                Append(buf, sizeof(buf), pos, "{enter=0x%llX,p=%d}", static_cast<unsigned long long>(e.nodeEnter[slot]),
                       static_cast<int>(e.nodeIsPlayer[slot]));
        }
        LogLine("  stk %s", buf);
    }
}

// Logs an entry in full if the session cap allows it. Returns true when it was written.
bool TryLogFull(const Entry& e, double freq, float z, bool armed)
{
    if (g_logged >= kMaxLogged) { ++g_skipped; return false; }
    ++g_logged;
    LogEntry(e, freq, z, armed);
    return true;
}

// The row of a (kind, ret) key, added when there is room. nullptr when the table is full and the key is new.
CountRow* FindOrAddRow(uint32 kind, uint64 ret)
{
    for (size_t r = 0; r < g_rowCount; ++r)
        if (g_rows[r].kind == kind && g_rows[r].ret == ret) return &g_rows[r];
    if (g_rowCount >= kMaxRows) return nullptr;
    CountRow& row = g_rows[g_rowCount++];
    row.kind = kind;
    row.ret = ret;
    row.count = 0;
    return &row;
}

// "ctorprobe counts:" followed by the kTopPairs largest (kind, ret) counts of the session.
void LogCounts()
{
    char buf[1024];
    size_t pos = 0;
    buf[0] = '\0';
    Append(buf, sizeof(buf), pos, "ctorprobe counts:");
    bool used[kMaxRows] = {};
    for (size_t n = 0; n < kTopPairs; ++n) {
        size_t best = kMaxRows;
        for (size_t r = 0; r < g_rowCount; ++r)
            if (!used[r] && (best == kMaxRows || g_rows[r].count > g_rows[best].count)) best = r;
        if (best == kMaxRows) break;
        used[best] = true;
        Append(buf, sizeof(buf), pos, " %s:0x%llX=%llu", g_rows[best].kind == 0 ? "SJ" : "DW",
               static_cast<unsigned long long>(g_rows[best].ret), static_cast<unsigned long long>(g_rows[best].count));
    }
    LogLine("%s", buf);
}

// Scans the history for one pending trigger (k = 0 SJ, 1 DW). Newest first, at most kPerTrigger full logs in total.
// The window is [enter - 0.5 s, enter + 0.05 s]. Already-written slots are skipped. The summary is logged when it expires.
void ScanPending(uint32 k, double freq, float z, bool armed)
{
    Pending& p = g_pend[k];
    const double enterT = static_cast<double>(p.enterQpc) / freq;
    for (size_t s = 0; s < g_histCount && p.shown < kPerTrigger; ++s) {
        const size_t slot = (g_histHead + kHistSize - 1 - s) % kHistSize;   // newest first
        if (g_histLogged[slot]) continue;
        const Entry& e = g_hist[slot];
        if (e.kind != k) continue;
        const double t = static_cast<double>(e.qpc) / freq;
        if (t < enterT - kBeforeSec || t > enterT + kAfterSec) continue;
        if (TryLogFull(e, freq, z, armed)) {
            g_histLogged[slot] = true;
            ++p.shown;
        }
    }
    if (--p.framesLeft <= 0) {
        p.active = false;
        LogCounts();
    }
}

double QpcFreq() { return statelog::QpcFreq(); }

// Frame callback (section Q). Called from GameFrame right after statelog::Frame(), every frame.
void Frame()
{
    const double freq = QpcFreq();
    const float z = g_hasPlayer ? g_playerPos[2] : -1.0f;
    const bool armed = origins::g_armedAtomic.load();

    // 1) Drain the ring into the history and the counts. The first kFirstPerKey entries of a new key are logged in full.
    const uint64 w = g_writeIdx.load(std::memory_order_acquire);
    if (w - g_readIdx > kRingSize) {                     // the producer lapped us: skip ahead and count the loss
        g_lost += (w - g_readIdx) - kRingSize;
        g_readIdx = w - kRingSize;
    }
    for (uint64 i = g_readIdx; i < w; ++i) {
        ++g_captured;
        const Entry e = g_ring[i % kRingSize];
        const size_t slot = g_histHead;
        g_hist[slot] = e;
        g_histLogged[slot] = false;
        g_histHead = (g_histHead + 1) % kHistSize;
        if (g_histCount < kHistSize) ++g_histCount;
        CountRow* row = FindOrAddRow(e.kind, e.stack[0]);
        if (row && row->count < kFirstPerKey && TryLogFull(e, freq, z, armed)) g_histLogged[slot] = true;
        if (row) ++row->count;
    }
    g_readIdx = w;

    // 2) Triggers: a new player enter of InSafeJump (k = 0) or DetachingFromWall (k = 1) starts a 3-frame scan.
    for (uint32 k = 0; k < 2; ++k) {
        const uint64 n = (k == 0) ? statelog::g_sjEnters.load(std::memory_order_acquire)
                                  : statelog::g_dwEnters.load(std::memory_order_acquire);
        if (n != g_seenEnters[k]) {
            g_seenEnters[k] = n;
            const uint64 enterQpc = (k == 0) ? statelog::g_lastSJEnterQpc.load(std::memory_order_relaxed)
                                             : statelog::g_lastDWEnterQpc.load(std::memory_order_relaxed);
            ++g_trig[k];
            g_pend[k] = Pending{ true, enterQpc, kTriggerFrames, 0 };
            LogLine("ctorprobe trigger %s t=%.4f", (k == 0) ? "SJenter" : "DWenter", static_cast<double>(enterQpc) / freq);
        }
        if (g_pend[k].active) ScanPending(k, freq, z, armed);
    }
}

// Menu: the checkbox and the probe status.
void Menu()
{
    bool on = g_on.load();
    if (ImGui::Checkbox("Log state constructors (v0.27)", &on)) g_on.store(on);
    ImGui::Text("ctorprobe: SJ %s, DW %s, captured %llu, skipped %llu, lost %llu",
                g_installedSJ ? "installed" : "not installed", g_installedDW ? "installed" : "not installed",
                static_cast<unsigned long long>(g_captured), static_cast<unsigned long long>(g_skipped),
                static_cast<unsigned long long>(g_lost));
    ImGui::Text("ctorprobe triggers: SJenter %llu, DWenter %llu, full logs %llu/%llu",
                static_cast<unsigned long long>(g_trig[0]), static_cast<unsigned long long>(g_trig[1]),
                static_cast<unsigned long long>(g_logged), static_cast<unsigned long long>(kMaxLogged));
}

} // namespace ctorprobe
