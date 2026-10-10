// ---------------------------------------------------------------------------------------------------------
// CameraDiag.inl (v0.19.0): the camera selector diagnostic, section K) of the header comment in BackEjectLoF.cpp.
// Included by BackEjectLoF.cpp inside its anonymous namespace, after DrawTrajMenu and before GameFrame.
// Read-only, polling only (no hooks, no writes to game memory). Frame side and menu only.
// Every game read goes through an SEH-guarded helper; no C++ objects live inside a __try function.
// ---------------------------------------------------------------------------------------------------------
namespace camdiag {

using ull = unsigned long long;
inline ull U(uint64 v) { return static_cast<ull>(v); }

constexpr uint64 kMinPtr = 0x10000ULL;          // addresses below this are never read
constexpr uint64 kImageLo = 0x140001000ULL;     // ACU.exe .text start (functions); also the TypeInfo lower bound
constexpr uint64 kFnHi = 0x142E696C0ULL;        // .text end
constexpr uint64 kVtableLo = 0x142E6A000ULL;    // vtables live in .rdata
constexpr uint64 kVtableHi = 0x1439C03F0ULL;
constexpr uint64 kTiHi = 0x145281000ULL;        // TypeInfos: .rdata/.data (end of .data)
constexpr uint32 kFaultStop = 1000;             // more SEH faults than this: feature OFF for the session
constexpr int kChangeCap = 3000;                 // change lines per game session
constexpr size_t kDumpCap = 300;                // nodes dumped per session
constexpr size_t kPendingMax = 4096;            // queued child nodes
constexpr int kChildPerFrame = 8;
constexpr int kDumpLineCap = 250;               // log lines per node dump
constexpr int kTypeChainMax = 8;
constexpr int kMembersMax = 64;

struct PendingChild
{
    uint64 obj;
    uint64 handle;
};

std::atomic<bool> g_on{ false };                // menu checkbox, default OFF (v0.24)
bool g_stopped = false;                         // too many faults: off for the session
std::atomic<uint32> g_faults{ 0 };              // SEH faults in guarded reads (counted in the __except blocks only)
bool g_started = false;
double g_t0 = 0.0;                              // NowSec() at the first Frame call
int g_changeLines = 0;
uint32 g_armSerialSeen = 0;
bool g_armedSeen = false;
bool g_havePrev = false;
uint64 pCam = 0, pHandle = 0, pNode = 0, pNodeCD = 0, pCD = 0, pN9 = 0;   // previous tuple
std::set<uint64> g_dumped;
std::vector<PendingChild> g_pending;            // children waiting for a dump (FIFO)
uint64 g_frames = 0;                            // v0.22: Frame() calls
bool g_treeDone = false;                        // v0.22: camera tree dumped (re-armed by the menu button)
bool g_treeNow = false;                         // v0.22: the menu button asked for a dump
std::set<uint64> g_dumpVisited;                 // per DumpNode call
int g_dumpLines = 0;                            // per DumpNode call
bool g_dumpCapLogged = false;

// ---------------------------------------------------------------------------------------------------------
// Guarded readers. Plain SEH functions: no C++ objects, so __try is allowed. Addresses below kMinPtr are not read.
// A fault (SEH exception) is counted in g_faults.
// ---------------------------------------------------------------------------------------------------------
bool ReadU64G(uint64 addr, uint64& v)
{
    v = 0;
    if (addr < kMinPtr) return false;
    __try {
        v = *reinterpret_cast<const uint64*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ++g_faults; return false; }
}

bool ReadU32G(uint64 addr, uint32& v)
{
    v = 0;
    if (addr < kMinPtr) return false;
    __try {
        v = *reinterpret_cast<const uint32*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ++g_faults; return false; }
}

bool ReadU16G(uint64 addr, uint16& v)
{
    v = 0;
    if (addr < kMinPtr) return false;
    __try {
        v = *reinterpret_cast<const uint16*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ++g_faults; return false; }
}

bool ReadBytesG(uint64 addr, void* dst, size_t n)
{
    if (addr < kMinPtr || n == 0) return false;
    __try {
        memcpy(dst, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ++g_faults; return false; }
}

uint64 Rd64(uint64 addr)
{
    uint64 v = 0;
    ReadU64G(addr, v);
    return v;
}

// SmallArray header at base: qword array pointer at base, uint16 count at base + 0x0A.
bool ReadArrHdrG(uint64 base, uint64& arr, uint32& n)
{
    arr = 0;
    n = 0;
    uint16 cnt = 0;
    if (!ReadU64G(base, arr)) return false;
    if (!ReadU16G(base + 0x0A, cnt)) return false;
    n = cnt;
    return true;
}

// SharedBlock: +0x00 manObj, +0x0C int32 strongRefCountAndFlags, +0x10 handle.
// obj = manObj when bit 31 of the flags is set, else 0 (the masking formula: manObj & (((int64)flags << 32) >> 63)).
bool ReadSharedBlockGuarded(uint64 sb, uint64& obj, uint64& handle)
{
    obj = 0;
    handle = 0;
    if (sb < kMinPtr) return false;
    __try {
        const uint64 manObj = *reinterpret_cast<const uint64*>(sb);
        const uint32 flags = *reinterpret_cast<const uint32*>(sb + 0x0C);
        handle = *reinterpret_cast<const uint64*>(sb + 0x10);
        const int64_t mask = static_cast<int64_t>(static_cast<uint64>(flags) << 32) >> 63;
        obj = manObj & static_cast<uint64>(mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { obj = 0; handle = 0; ++g_faults; return false; }
}

// The first camera component: CameraManager singleton, then SmallArray at +0x40 (qword arr, uint16 size at +0x4A),
// element 0. cam is 0 when there is no camera (no singleton or empty array).
bool CameraComponentGuarded(uint64& cam)
{
    cam = 0;
    __try {
        const CameraManager* mgr = CameraManager::GetSingleton();
        const uint64 m = reinterpret_cast<uint64>(mgr);
        if (m < kMinPtr) return true;
        const uint64 arr = *reinterpret_cast<const uint64*>(m + 0x40);
        const uint16 n = *reinterpret_cast<const uint16*>(m + 0x4A);
        if (n > 0 && arr >= kMinPtr) cam = *reinterpret_cast<const uint64*>(arr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { cam = 0; ++g_faults; return false; }
}

// v0.22: the three SharedBlock roots of the CameraManager singleton: +0x10 ACU_Camera_Root, +0x18 cinematic
// transition profiles, +0x20 fixed cameras. Plain SEH, no C++ objects.
bool ReadRootsGuarded(uint64& sbRoot, uint64& sbCine, uint64& sbFixed)
{
    sbRoot = 0;
    sbCine = 0;
    sbFixed = 0;
    __try {
        const CameraManager* mgr = CameraManager::GetSingleton();
        const uint64 m = reinterpret_cast<uint64>(mgr);
        if (m < kMinPtr) return false;
        sbRoot = *reinterpret_cast<const uint64*>(m + 0x10);
        sbCine = *reinterpret_cast<const uint64*>(m + 0x18);
        sbFixed = *reinterpret_cast<const uint64*>(m + 0x20);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { sbRoot = 0; sbCine = 0; sbFixed = 0; ++g_faults; return false; }
}

// ---------------------------------------------------------------------------------------------------------
// Type names. A game object has a vtable; slot 5 (+0x28) is GetTI() (this in rcx, returns TypeInfo*).
// The call is made only when the vtable and the function pointer are inside ACU.exe, and the returned TypeInfo
// is inside ACU.exe too. TypeInfo: +0x18 char* name, +0x38 base TypeInfo*.
// ---------------------------------------------------------------------------------------------------------

// Object -> TypeInfo -> class name (up to n-1 chars, non-printable replaced by '?'). On failure out is "?" and false.
bool TypeNameGuarded(uint64 obj, char* out, size_t n, uint64* tiOut)
{
    if (tiOut) *tiOut = 0;
    if (n < 2) return false;
    out[0] = '?';
    out[1] = 0;
    if (obj < kMinPtr) return false;
    __try {
        const uint64 v = *reinterpret_cast<const uint64*>(obj);
        if (v < kVtableLo || v >= kVtableHi) return false;
        // Every slot 0..5 must point into .text, so a non-Object (data whose first qword happens to point into .rdata)
        // is very unlikely to pass and get a random function called on it.
        for (int k = 0; k < 5; ++k) {
            const uint64 sk = *reinterpret_cast<const uint64*>(v + 8ULL * k);
            if (sk < kImageLo || sk >= kFnHi) return false;
        }
        const uint64 fn = *reinterpret_cast<const uint64*>(v + 0x28);
        if (fn < kImageLo || fn >= kFnHi) return false;
        using GetTIFn = void* (__fastcall*)(void*);
        void* tiPtr = reinterpret_cast<GetTIFn>(fn)(reinterpret_cast<void*>(obj));
        const uint64 ti = reinterpret_cast<uint64>(tiPtr);
        if (ti < kImageLo || ti >= kTiHi) return false;
        const uint64 np = *reinterpret_cast<const uint64*>(ti + 0x18);
        if (np < kMinPtr) return false;
        size_t i = 0;
        for (; i + 1 < n; ++i) {
            const char c = *reinterpret_cast<const char*>(np + i);
            if (c == 0) break;
            out[i] = (c >= 0x20 && c <= 0x7E) ? c : '?';
        }
        out[i] = 0;
        if (tiOut) *tiOut = ti;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '?';
        out[1] = 0;
        ++g_faults;
        return false;
    }
}

// One TypeInfo level: class name and base TypeInfo (baseOut = 0 when none or not in ACU.exe).
bool TypeNameOfTIGuarded(uint64 ti, char* out, size_t n, uint64& baseOut)
{
    baseOut = 0;
    if (n < 2) return false;
    out[0] = '?';
    out[1] = 0;
    if (ti < kImageLo || ti >= kTiHi) return false;
    __try {
        const uint64 np = *reinterpret_cast<const uint64*>(ti + 0x18);
        const uint64 base = *reinterpret_cast<const uint64*>(ti + 0x38);
        if (np < kMinPtr) return false;
        size_t i = 0;
        for (; i + 1 < n; ++i) {
            const char c = *reinterpret_cast<const char*>(np + i);
            if (c == 0) break;
            out[i] = (c >= 0x20 && c <= 0x7E) ? c : '?';
        }
        out[i] = 0;
        if (base >= kImageLo && base < kTiHi) baseOut = base;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '?';
        out[1] = 0;
        ++g_faults;
        return false;
    }
}

// "Name : Base : Base2" from a TypeInfo, up to 8 levels.
void ChainText(uint64 ti, char* out, size_t n)
{
    if (n == 0) return;
    out[0] = 0;
    size_t used = 0;
    uint64 cur = ti;
    for (int lvl = 0; lvl < kTypeChainMax && cur != 0; ++lvl) {
        char nm[64];
        uint64 base = 0;
        if (!TypeNameOfTIGuarded(cur, nm, sizeof(nm), base)) {
            snprintf(nm, sizeof(nm), "?");
        }
        const int w = snprintf(out + used, n - used, "%s%s", lvl ? " : " : "", nm);
        if (w < 0 || static_cast<size_t>(w) >= n - used) break;
        used += static_cast<size_t>(w);
        cur = base;
    }
}

// Full chain of an object's type; "-" for null, "?" when unknown.
void ObjChainText(uint64 obj, char* out, size_t n)
{
    if (obj < kMinPtr) { snprintf(out, n, "-"); return; }
    uint64 ti = 0;
    char nm[64];
    if (!TypeNameGuarded(obj, nm, sizeof(nm), &ti)) { snprintf(out, n, "?"); return; }
    ChainText(ti, out, n);
}

// Class name only; "-" for null, "?" when unknown.
void ObjNameText(uint64 obj, char* out, size_t n)
{
    if (obj < kMinPtr) { snprintf(out, n, "-"); return; }
    TypeNameGuarded(obj, out, n, nullptr);
}

// ---------------------------------------------------------------------------------------------------------
// Logging helpers for dumps (line cap per node dump)
// ---------------------------------------------------------------------------------------------------------
void DLog(const char* fmt, ...)
{
    if (g_dumpLines >= kDumpLineCap) {
        if (!g_dumpCapLogged) {
            g_dumpCapLogged = true;
            LogLine("    ... line cap");
        }
        return;
    }
    ++g_dumpLines;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LogLine("%s", buf);
}

// Logs count qwords starting at addr on one line (guarded).
void LogQwordsAt(uint64 addr, int count, int ind, const char* label)
{
    uint64 q[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (count < 1 || count > 8) return;
    if (!ReadBytesG(addr, q, static_cast<size_t>(count) * 8)) {
        DLog("%*s%s unreadable", ind, "", label);
        return;
    }
    char line[256];
    int used = snprintf(line, sizeof(line), "%*s%s", ind, "", label);
    for (int i = 0; i < count && used > 0 && static_cast<size_t>(used) < sizeof(line); ++i) {
        used += snprintf(line + used, sizeof(line) - used, " %016llX", U(q[i]));
    }
    DLog("%s", line);
}

// Dump state of the current DumpNode call (visited set, line cap) lives in the globals above.
void DumpObject(uint64 obj, int depth, int maxDepth);

// One member of a TypeInfo class: prints the header line and, for pointers, recurses (depth+1, up to maxDepth).
void DumpMember(uint64 obj, uint32 nameHash, uint32 typeHash, uint32 prim, uint32 offStruct, uint32 offBits,
                uint32 bitW, uint32 container, int depth, int maxDepth)
{
    const int ind = 4 + 2 * depth;
    const uint64 addr = obj + offStruct;
    char vt[256];
    vt[0] = 0;
    uint64 kids[16] = { 0 };
    int nk = 0;

    // v0.20: a SmallArrayObjectPtr container is handled as such whatever the primitive type says.
    switch (container == 0x1D ? 0x1Du : prim) {
    case 0x00: {    // Bool
        uint32 v = 0;
        if (bitW > 0) {
            uint32 w = 0;
            if (ReadU32G(addr, w)) v = (w >> offBits) & ((1u << bitW) - 1u);
        } else {
            uint8_t b = 0;
            if (ReadBytesG(addr, &b, 1)) v = b;
        }
        snprintf(vt, sizeof(vt), "bool=%u", v);
        break;
    }
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x19: {   // SInt8..UInt32, Enum
        const bool sgn = (prim == 0x02 || prim == 0x04 || prim == 0x06);
        const size_t size = (prim == 0x02 || prim == 0x03) ? 1 : ((prim == 0x04 || prim == 0x05) ? 2 : 4);
        uint32 raw = 0;
        bool ok = false;
        if (bitW > 0) {
            ok = ReadU32G(addr, raw);
            if (ok) raw = (raw >> offBits) & ((1u << bitW) - 1u);
        } else {
            ok = ReadBytesG(addr, &raw, size);
        }
        if (!ok) {
            snprintf(vt, sizeof(vt), "?");
        } else if (sgn) {
            int32_t sv = static_cast<int32_t>(raw);
            if (size == 1) sv = static_cast<int8_t>(raw);
            else if (size == 2) sv = static_cast<int16_t>(raw);
            snprintf(vt, sizeof(vt), "%d (0x%X)", static_cast<int>(sv), raw);
        } else {
            snprintf(vt, sizeof(vt), "%u (0x%X)", raw, raw);
        }
        break;
    }
    case 0x08: case 0x09: case 0x11: case 0x1E: {   // SInt64, UInt64, ObjectID, SloppyHandle
        uint64 v = 0;
        if (ReadU64G(addr, v)) snprintf(vt, sizeof(vt), "0x%llX", U(v));
        else snprintf(vt, sizeof(vt), "?");
        break;
    }
    case 0x0A: {    // Float
        float f = 0.0f;
        if (ReadBytesG(addr, &f, sizeof(f))) snprintf(vt, sizeof(vt), "%.4f", static_cast<double>(f));
        else snprintf(vt, sizeof(vt), "?");
        break;
    }
    case 0x0B: case 0x0C: case 0x0D: case 0x0E: {  // Vector2, Vector3, Vector4, Quaternion
        const size_t cnt = (prim == 0x0B) ? 2 : (prim == 0x0C) ? 3 : 4;
        float f[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (ReadBytesG(addr, f, cnt * sizeof(float))) {
            if (cnt == 2) snprintf(vt, sizeof(vt), "(%.4f, %.4f)", static_cast<double>(f[0]), static_cast<double>(f[1]));
            else if (cnt == 3) snprintf(vt, sizeof(vt), "(%.4f, %.4f, %.4f)", static_cast<double>(f[0]),
                                        static_cast<double>(f[1]), static_cast<double>(f[2]));
            else snprintf(vt, sizeof(vt), "(%.4f, %.4f, %.4f, %.4f)", static_cast<double>(f[0]), static_cast<double>(f[1]),
                          static_cast<double>(f[2]), static_cast<double>(f[3]));
        } else {
            snprintf(vt, sizeof(vt), "?");
        }
        break;
    }
    case 0x14: case 0x15: {     // RawPtrSubclassObject, RawPtr
        uint64 p = 0;
        ReadU64G(addr, p);
        snprintf(vt, sizeof(vt), "ptr=0x%llX", U(p));
        kids[nk++] = p;
        break;
    }
    case 0x12: case 0x1C: {     // WeakPtr, StrongPtr: SharedBlock*
        uint64 sb = 0;
        ReadU64G(addr, sb);
        uint64 o = 0, h = 0;
        if (sb) ReadSharedBlockGuarded(sb, o, h);
        snprintf(vt, sizeof(vt), "sp=0x%llX handle=0x%llX obj=0x%llX", U(sb), U(h), U(o));
        kids[nk++] = o;
        break;
    }
    case 0x13: {                // InstanceSubclassObject: inline object (has a vtable)
        snprintf(vt, sizeof(vt), "inline@+0x%X", offStruct);
        kids[nk++] = addr;
        break;
    }
    case 0x16: {                // Instance: inline object with no vtable. Printed raw only, never recursed (see report).
        uint64 q[4] = { 0, 0, 0, 0 };
        if (ReadBytesG(addr, q, sizeof(q))) {
            snprintf(vt, sizeof(vt), "inline@+0x%X (no vtable) raw %016llX %016llX %016llX %016llX", offStruct,
                     U(q[0]), U(q[1]), U(q[2]), U(q[3]));
        } else {
            snprintf(vt, sizeof(vt), "inline@+0x%X (no vtable) unreadable", offStruct);
        }
        break;
    }
    case 0x1D: {                // SmallArrayObjectPtr
        uint64 arr = 0;
        uint32 n = 0;
        ReadArrHdrG(addr, arr, n);
        snprintf(vt, sizeof(vt), "array n=%u", n);
        const uint32 m = n < 16 ? n : 16u;
        for (uint32 i = 0; i < m && arr >= kMinPtr; ++i) {
            uint64 e = 0;
            ReadU64G(arr + 8ULL * i, e);
            kids[nk++] = e;
        }
        break;
    }
    default: {                  // BigArray, InlineArray, String, LString, Matrix4 and anything else: raw 16 bytes
        uint64 q[2] = { 0, 0 };
        if (ReadBytesG(addr, q, sizeof(q))) snprintf(vt, sizeof(vt), "raw %016llX %016llX", U(q[0]), U(q[1]));
        else snprintf(vt, sizeof(vt), "?");
        break;
    }
    }

    DLog("%*s  +0x%X h=%08X th=%08X t=%u c=%u %s", ind, "", offStruct, nameHash, typeHash, prim, container, vt);
    if (depth < maxDepth) {
        for (int k = 0; k < nk; ++k) {
            if (kids[k] >= kMinPtr) DumpObject(kids[k], depth + 1, maxDepth);
        }
    }
}

// Dumps an object: its type chain and the members of every TypeInfo level (up to 8 levels, 64 members each).
// Visited objects are skipped (per DumpNode call).
void DumpObject(uint64 obj, int depth, int maxDepth)
{
    if (obj < kMinPtr) return;
    if (g_dumpVisited.count(obj)) return;
    g_dumpVisited.insert(obj);
    const int ind = 4 + 2 * depth;

    uint64 ti = 0;
    char nm[64];
    if (!TypeNameGuarded(obj, nm, sizeof(nm), &ti)) {
        DLog("%*sobj 0x%llX type=?", ind, "", U(obj));
        LogQwordsAt(obj, 8, ind + 2, "first 0x40 bytes:");
        return;
    }
    char chain[256];
    ChainText(ti, chain, sizeof(chain));
    DLog("%*sobj 0x%llX type=%s", ind, "", U(obj), chain);

    uint64 cur = ti;
    for (int lvl = 0; lvl < kTypeChainMax && cur >= kImageLo && cur < kTiHi; ++lvl) {
        uint64 members = 0;
        uint16 numMembers = 0;
        uint64 next = 0;
        ReadU64G(cur + 0x00, members);
        ReadU16G(cur + 0xA0, numMembers);
        ReadU64G(cur + 0x38, next);
        const uint32 nMem = numMembers < kMembersMax ? numMembers : static_cast<uint32>(kMembersMax);
        for (uint32 i = 0; i < nMem; ++i) {
            uint8_t mb[0x38];
            if (!ReadBytesG(members + 0x38ULL * i, mb, sizeof(mb))) break;
            uint32 nameHash = 0, typeHash = 0, packed = 0;
            memcpy(&nameHash, mb + 0x04, 4);
            memcpy(&typeHash, mb + 0x08, 4);
            memcpy(&packed, mb + 0x10, 4);
            const uint32 prim = packed & 0x1Fu;
            const uint32 offBits = (packed >> 5) & 0x3Fu;
            const uint32 bitW = (packed >> 11) & 0x1Fu;
            const uint32 offStruct = packed >> 18;
            const uint32 container = mb[0x0D] & 0x1Fu;
            DumpMember(obj, nameHash, typeHash, prim, offStruct, offBits, bitW, container, depth, maxDepth);
        }
        cur = next;
    }
}

// Dumps one node (a CameraSelectorNode or BlenderNode) as described in section K. handle is the SharedBlock handle
// when the node came from a SharedBlock, 0 when unknown. Pending children are queued, not dumped here.
void DumpNode(uint64 node, const char* why, uint64 handle)
{
    g_dumpVisited.clear();
    g_dumpLines = 0;
    g_dumpCapLogged = false;
    if (node < kMinPtr) return;

    char chain[256];
    ObjChainText(node, chain, sizeof(chain));
    if (handle) DLog("camnode 0x%llX (%s) type=%s handle=0x%llX", U(node), why, chain, U(handle));
    else DLog("camnode 0x%llX (%s) type=%s", U(node), why, chain);

    uint64 q[12];
    memset(q, 0, sizeof(q));
    if (!ReadBytesG(node, q, sizeof(q))) {
        DLog("  raw unreadable");
        return;
    }
    DLog("  raw +00: %016llX %016llX %016llX %016llX %016llX %016llX",
         U(q[0]), U(q[1]), U(q[2]), U(q[3]), U(q[4]), U(q[5]));
    DLog("  raw +30: %016llX %016llX %016llX %016llX %016llX %016llX",
         U(q[6]), U(q[7]), U(q[8]), U(q[9]), U(q[10]), U(q[11]));

    const uint64 rule = q[3];                   // +0x18 BooleanRuleCondition*
    const uint64 cd = q[4];                     // +0x20 CameraData*
    DLog("  rule 0x%llX", U(rule));
    if (rule >= kMinPtr) DumpObject(rule, 0, 4);
    char nm[96];
    ObjNameText(cd, nm, sizeof(nm));
    DLog("  cameraData 0x%llX type=%s", U(cd), nm);

    // transitions: SmallArray<TransitionInfoHolder*> at +0x28 (count at +0x32)
    uint64 tArr = 0;
    uint32 tN = 0;
    ReadArrHdrG(node + 0x28, tArr, tN);
    DLog("  transitions n=%u", tN);
    for (uint32 i = 0; i < tN && i < 16; ++i) {
        uint64 e = 0;
        ReadU64G(tArr + 8ULL * i, e);
        ObjNameText(e, nm, sizeof(nm));
        DLog("    [%u] 0x%llX type=%s", i, U(e), nm);
    }

    // children: SmallArray<SharedBlock*> at +0x34 (count at +0x3E)
    uint64 cArr = 0;
    uint32 cN = 0;
    ReadArrHdrG(node + 0x34, cArr, cN);
    DLog("  children n=%u", cN);
    for (uint32 i = 0; i < cN && i < 32; ++i) {
        uint64 sb = 0;
        ReadU64G(cArr + 8ULL * i, sb);
        uint64 cObj = 0, cHandle = 0;
        if (sb) ReadSharedBlockGuarded(sb, cObj, cHandle);
        const uint64 cRule = cObj ? Rd64(cObj + 0x18) : 0;
        const uint64 cCd = cObj ? Rd64(cObj + 0x20) : 0;
        char cn[96], rn[96];
        ObjNameText(cObj, cn, sizeof(cn));
        ObjNameText(cRule, rn, sizeof(rn));
        DLog("    [%u] handle=0x%llX obj=0x%llX type=%s rule=0x%llX(%s) cd=0x%llX", i, U(cHandle), U(cObj), cn,
             U(cRule), rn, U(cCd));
        if (cObj >= kMinPtr && g_pending.size() < kPendingMax) g_pending.push_back(PendingChild{ cObj, cHandle });
    }
}

// ---------------------------------------------------------------------------------------------------------
// v0.22: camera tree dump (section K). Read-only, guarded reads only.
// ---------------------------------------------------------------------------------------------------------
constexpr int kTreeMaxDepth = 10;
constexpr int kTreeNodeCap = 600;               // nodes printed per DumpTree call
constexpr uint32 kTreeListCap = 64;             // children read per list
constexpr uint64 kTreeFrames = 300;             // Frame() calls before the automatic dump
constexpr uint32 kRuleChildMax = 8;

// Appends formatted text to out (size n) at used; never writes past n-1 and always keeps out terminated.
void AppendF(char* out, size_t n, size_t& used, const char* fmt, ...)
{
    if (used >= n) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(out + used, n - used, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    const size_t room = n - used;
    used += (static_cast<size_t>(w) < room) ? static_cast<size_t>(w) : (room - 1);
}

// Rule condition text: "-" for 0, else "Type(en=%u neg=%u e=%d)". A BooleanRuleCondition also lists its children,
// " op=%d [child(neg=%u), ...]"; a child that is itself a BooleanRuleCondition adds "{op=%d: type, ...}" (one level).
void DescribeRule(uint64 rule, char* out, size_t n)
{
    if (n == 0) return;
    out[0] = 0;
    if (rule == 0) { snprintf(out, n, "-"); return; }
    char tn[64];
    ObjNameText(rule, tn, sizeof(tn));
    uint8_t en = 0, neg = 0;
    uint32 e = 0;
    ReadBytesG(rule + 0x0C, &en, 1);
    ReadBytesG(rule + 0x0D, &neg, 1);
    ReadU32G(rule + 0x08, e);
    size_t used = 0;
    AppendF(out, n, used, "%s(en=%u neg=%u e=%d)", tn, static_cast<unsigned>(en), static_cast<unsigned>(neg),
            static_cast<int>(e));
    if (strcmp(tn, "BooleanRuleCondition") != 0) return;

    uint32 op = 0;
    ReadU32G(rule + 0x20, op);
    AppendF(out, n, used, " op=%d [", static_cast<int>(op));
    uint64 arr = 0;
    uint32 cnt = 0;
    ReadArrHdrG(rule + 0x24, arr, cnt);
    const uint32 m = cnt < kRuleChildMax ? cnt : kRuleChildMax;
    for (uint32 i = 0; i < m; ++i) {
        uint64 child = 0;
        ReadU64G(arr + 8ULL * i, child);
        char cn[64];
        ObjNameText(child, cn, sizeof(cn));
        uint8_t cneg = 0;
        ReadBytesG(child + 0x0D, &cneg, 1);
        AppendF(out, n, used, "%s%s(neg=%u)", i ? ", " : "", cn, static_cast<unsigned>(cneg));
        if (child >= kMinPtr && strcmp(cn, "BooleanRuleCondition") == 0) {
            uint32 nop = 0;
            ReadU32G(child + 0x20, nop);
            AppendF(out, n, used, "{op=%d:", static_cast<int>(nop));
            uint64 narr = 0;
            uint32 ncnt = 0;
            ReadArrHdrG(child + 0x24, narr, ncnt);
            const uint32 nm = ncnt < kRuleChildMax ? ncnt : kRuleChildMax;
            for (uint32 j = 0; j < nm; ++j) {
                uint64 gc = 0;
                ReadU64G(narr + 8ULL * j, gc);
                char gn[64];
                ObjNameText(gc, gn, sizeof(gn));
                AppendF(out, n, used, "%s%s", j ? ", " : " ", gn);
            }
            AppendF(out, n, used, "}");
        }
    }
    AppendF(out, n, used, "]");
}

// One DumpTree call: visited set, node count, cap flag.
struct TreeState
{
    const char* label;
    std::set<uint64> seen;
    int nodes;
    bool capped;
};

// Prints the node behind sb, then its children: the +0x40 list first, then the +0x34 list (selector order).
// lk is "r" (root), "c" (+0x34 list) or "p" (+0x40 list); idx is the index in that list.
void TreeWalk(TreeState& st, uint64 sb, const char* lk, int idx, int depth)
{
    if (depth > kTreeMaxDepth || sb == 0) return;
    uint64 obj = 0, handle = 0;
    if (!ReadSharedBlockGuarded(sb, obj, handle)) return;
    if (obj < kMinPtr || st.seen.count(obj)) return;
    if (st.nodes >= kTreeNodeCap) { st.capped = true; return; }
    st.seen.insert(obj);
    ++st.nodes;

    uint32 id = 0;
    ReadU32G(obj + 0x08, id);
    char tn[64];
    ObjNameText(obj, tn, sizeof(tn));
    uint8_t fl[4] = { 0, 0, 0, 0 };
    ReadBytesG(obj + 0x4C, fl, sizeof(fl));
    const uint64 cd = Rd64(obj + 0x20);
    char rule[400];
    DescribeRule(Rd64(obj + 0x18), rule, sizeof(rule));
    LogLine("tree %s %*s[%s%d] handle=0x%llX obj=0x%llX id=0x%X type=%s flags=%u%u%u cd=0x%llX rule=%s",
            st.label, 2 * depth, "", lk, idx, U(handle), U(obj), static_cast<unsigned>(id), tn,
            static_cast<unsigned>(fl[0]), static_cast<unsigned>(fl[1]), static_cast<unsigned>(fl[3]), U(cd), rule);

    if (depth >= kTreeMaxDepth) return;
    const char* lists[2] = { "p", "c" };
    const uint64 offs[2] = { 0x40, 0x34 };
    for (int L = 0; L < 2; ++L) {
        uint64 arr = 0;
        uint32 n = 0;
        ReadArrHdrG(obj + offs[L], arr, n);
        if (n > kTreeListCap) n = kTreeListCap;
        for (uint32 i = 0; i < n; ++i) {
            uint64 csb = 0;
            ReadU64G(arr + 8ULL * i, csb);
            TreeWalk(st, csb, lists[L], static_cast<int>(i), depth + 1);
        }
    }
}

// Dumps the whole selector tree below one root SharedBlock as "tree <label> ..." lines.
void DumpTree(const char* label, uint64 sbRoot)
{
    LogLine("tree %s begin root sb=0x%llX", label, U(sbRoot));
    TreeState st;
    st.label = label;
    st.nodes = 0;
    st.capped = false;
    TreeWalk(st, sbRoot, "r", 0, 0);
    LogLine("tree %s end nodes=%d%s", label, st.nodes, st.capped ? " (node cap)" : "");
}

// Stops the feature after too many faults (logged once).
void CheckStop()
{
    if (!g_stopped && g_faults.load() > kFaultStop) {
        g_stopped = true;
        g_havePrev = false;
        LogLine("camdiag: OFF after %u faults", static_cast<unsigned>(g_faults.load()));
    }
}

// Frame callback (section K). Called from GameFrame right after TrajFrame, every frame.
void Frame(uint64 frame)
{
    ++g_frames;
    if (!g_started) {
        g_started = true;
        g_t0 = origins::NowSec();
    }
    const origins::ArmSnapshot a = origins::GetArmSnapshot();
    const bool active = g_on.load() && !g_stopped;
    const double t = origins::NowSec() - g_t0;

    // Arm and disarm lines. The state is tracked even when the feature is off, so logs do not repeat.
    if (a.serial != g_armSerialSeen) {
        g_armSerialSeen = a.serial;
        if (active) LogLine("cam arm serial=%u t=%.3f armed=%d", static_cast<unsigned>(a.serial), t, a.armed ? 1 : 0);
    }
    if (g_armedSeen && !a.armed && active) LogLine("cam disarm t=%.3f", t);
    g_armedSeen = a.armed;

    if (!active) {
        g_havePrev = false;     // the next active frame logs the state again
        return;
    }

    // Reads: camera component, its selector block (+0x9B8), CameraData (+0x8F8), node at +0x9C0 -> +0x18.
    uint64 cam = 0;
    CameraComponentGuarded(cam);
    uint64 handle = 0, node = 0, cd = 0, p9 = 0, n9 = 0, nodeCD = 0;
    if (cam) {
        const uint64 sb = Rd64(cam + 0x9B8);
        if (sb) ReadSharedBlockGuarded(sb, node, handle);
        cd = Rd64(cam + 0x8F8);
        p9 = Rd64(cam + 0x9C0);
        if (p9) n9 = Rd64(p9 + 0x18);
    }
    if (node) nodeCD = Rd64(node + 0x20);
    CheckStop();
    if (g_stopped) return;

    // Change log: one line whenever any element of the tuple differs from the previous frame.
    const bool changed = !g_havePrev || cam != pCam || handle != pHandle || node != pNode || nodeCD != pNodeCD
                         || cd != pCD || n9 != pN9;
    if (changed) {
        if (g_changeLines < kChangeCap) {
            ++g_changeLines;
            char nn[96], n9n[96], rn[96];
            ObjNameText(node, nn, sizeof(nn));
            ObjNameText(n9, n9n, sizeof(n9n));
            const uint64 rule = node ? Rd64(node + 0x18) : 0;
            ObjNameText(rule, rn, sizeof(rn));
            const double z = g_hasPlayer ? static_cast<double>(g_playerPos[2]) : -1.0;
            LogLine("cam t=%.3f frame=%llu arm=%u armed=%d handle=0x%llX node=0x%llX(%s) nodeCD=0x%llX cd=0x%llX n9C0=0x%llX(%s) rule=0x%llX(%s) z=%.2f",
                    t, U(frame), static_cast<unsigned>(a.serial), a.armed ? 1 : 0, U(handle), U(node), nn, U(nodeCD),
                    U(cd), U(n9), n9n, U(rule), rn, z);
            if (g_changeLines == kChangeCap) LogLine("camdiag: change-line cap reached");
        }
        pCam = cam;
        pHandle = handle;
        pNode = node;
        pNodeCD = nodeCD;
        pCD = cd;
        pN9 = n9;
        g_havePrev = true;
    }

    // First-sight dumps of the active node and the n9C0 node.
    if (node >= kMinPtr && g_dumped.size() < kDumpCap && !g_dumped.count(node)) {
        g_dumped.insert(node);
        DumpNode(node, "active", handle);
    }
    if (n9 >= kMinPtr && g_dumped.size() < kDumpCap && !g_dumped.count(n9)) {
        g_dumped.insert(n9);
        DumpNode(n9, "n9C0", 0);
    }

    // Queued children: at most kChildPerFrame per frame.
    int done = 0;
    while (!g_pending.empty() && done < kChildPerFrame) {
        if (g_dumped.size() >= kDumpCap) {
            g_pending.clear();
            break;
        }
        const PendingChild pc = g_pending.front();
        g_pending.erase(g_pending.begin());
        ++done;
        if (g_dumped.count(pc.obj)) continue;
        g_dumped.insert(pc.obj);
        DumpNode(pc.obj, "child", pc.handle);
    }

    // v0.22: camera tree dump, once per session (after kTreeFrames frames, or at once after the menu button).
    if (!g_treeDone && (g_treeNow || g_frames >= kTreeFrames)) {
        uint64 sbRoot = 0, sbCine = 0, sbFixed = 0;
        if (ReadRootsGuarded(sbRoot, sbCine, sbFixed)) {
            uint64 rootObj = 0, rootHandle = 0;
            if (sbRoot && ReadSharedBlockGuarded(sbRoot, rootObj, rootHandle) && rootObj >= kMinPtr) {
                g_treeDone = true;
                g_treeNow = false;
                DumpTree("main", sbRoot);
                if (sbCine) DumpTree("cine", sbCine);
            }
        }
    }
    CheckStop();
}

// Menu: checkbox, counters, re-dump button.
void Menu()
{
    bool on = g_on.load();
    if (ImGui::Checkbox("Log camera selector changes (v0.19)", &on)) g_on.store(on);
    ImGui::Text("camdiag: changes logged %u, nodes dumped %zu, faults %u", static_cast<unsigned>(g_changeLines),
                g_dumped.size(), static_cast<unsigned>(g_faults.load()));
    if (g_stopped) ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "camdiag OFF after too many faults (see log)");
    if (ImGui::Button("Re-dump current node")) {
        g_dumped.erase(pNode);
        g_dumped.erase(pN9);
    }
    if (ImGui::Button("Dump camera tree")) {    // v0.22: re-arms the tree dump for the next active frame
        g_treeDone = false;
        g_treeNow = true;
    }
}

} // namespace camdiag
