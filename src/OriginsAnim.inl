// ---------------------------------------------------------------------------------------------------------
// OriginsAnim.inl (v0.26.0): the "Origins backflip" animation, section H) of the header comment in BackEjectLoF.cpp.
// Included by BackEjectLoF.cpp inside its anonymous namespace, after LogLine, g_eAppended and g_hasPlayer, and before
// the experiment E code, which calls ArmFromGameThread() right after it appends our eject.
// Needs the includes ACU/Animation.h, ACU/ManagedPtrs/ManagedPtrs.h, ACU/Memory/ACUAllocs.h, SimpleJSON and ToFromFile.
//
// Data pointer swaps only: no code patches, no new hooks. Track data is built once per Animation object (game codecs,
// as in WD2Vaults_v2) and is kept in memory, never freed. The clip rests with its original tracks.
//
// Arming happens on the GAME THREAD, synchronously, when our eject is appended: ArmFromGameThread() loads the current
// ArmPlan (an immutable list of slot addresses with their original and new pointers) and swaps every slot that still
// holds its original. Only atomics are touched there, no logging and no containers. The frame callback disarms (window,
// checkbox, button) by walking the current plan, writing the original only where the new pointer is still in place.
// Plans are published by the frame callback whenever the set of built entries changes. A replaced plan is leaked on
// purpose (small, and a game thread may still be reading it), so it is never deleted.
//
// Flight variants (v0.14): mapping.json has one entry per (handle, variant_flight). Entries of the same handle and
// different variants target the same Animation object and the same slots, and only ONE variant may be armed at a time.
// An entry without variant_flight is variant 72 (the v0.11-v0.13 data). Load() groups the entries by flight, sorted
// ascending (at most kMaxVariants = 8 flights; a ninth is dropped, logged). Each variant has its own immutable ArmPlan
// in g_plans[index]; the index is the position in the sorted list, and g_variantFlight[] holds the flights, filled at
// Load before the game thread runs and never changed after.
// Selection (game thread, ArmFromGameThread): the drop (player z minus hay z) gives the flight seconds of the game's own
// leap-of-faith timing (DropToSeconds, piecewise linear), times 60 = target frames. The variant is the smallest flight
// >= target, or the largest. A variant with an empty plan falls back to the non-empty one nearest the target. A different
// armed variant is restored before the chosen one is armed. The window is the slider, or the chosen flight plus 3 s.
// Building (Poll): at most kBuildsPerPoll entries per poll (about 11 s for 132 entries). Nothing is built, looked up or
// dropped while a variant is armed, because the "orig" pointers would be ours. Each build publishes its variant's plan.
// Root scaling (v0.17): the game rescales the eject clip's root (TrackID 0, position) so its end point lands on the real
// target. A mapping entry with root_scale holds a normalized root template (10 m across, 50 m down). Its root track keeps a
// copy of the template values and a scratch buffer (both leaked, like the trackdata). ArmFromGameThread(drop, horiz) rescales
// the template of the chosen variant to the real drop and horizontal distance before writing the plan, re-encodes the root
// track and decodes its last key back. Unusable drop or distance, or a failed check, leave the template in place.
// v0.18: root_scale may carry "HG" (horizontal gain, 0.5..2.0, default 1.0). The arm-time horizontal span is horiz * HG;
// the sanity range check still uses the raw horiz.
//
// Game memory is read and written only inside SEH-guarded functions that have no C++ objects of their own.
// The guards call *Impl functions (noinline) that may use C++ objects: the WD2Vaults_v2 pattern.
// ---------------------------------------------------------------------------------------------------------
namespace origins {

// The game's codec table and heap (ACU 1.5.1), the same addresses as WD2Vaults_v2.
constexpr uintptr_t kCodecTable = 0x1451399C0u;
constexpr uintptr_t kCurrentHeap = 0x14525A510u;
constexpr uint8 kRotCodec = 1;         // "full precision quaternion, u16 times": the codec the game uses for imports
constexpr uint8 kTransCodecU8 = 27;    // float3 translation, u8 key times (v0.15: accepted in sources only)
constexpr uint8 kTransCodecU16 = 28;   // float3 translation, u16 key times (v0.15: the codec every baked translation uses)
constexpr uint32 kInfoRot = 0;         // AnimationInformationType::LocalQuaternion
constexpr uint32 kInfoTrans = 3;       // AnimationInformationType::LocalTranslation
constexpr double kPlayerSettleSec = 5.0;   // the player must have existed this long before any object lookup
constexpr double kPollSec = 1.0;           // build and verify at most this often
constexpr float kArmDefaultSec = 5.0f;     // v0.12: 5 s (was 4 s)
constexpr float kArmMinSec = 1.0f;
constexpr float kArmMaxSec = 10.0f;
constexpr float kKeyTol = 1e-3f;           // self-check tolerance on every decoded component
constexpr double kFps = 60.0;              // baked keys are 60 Hz frames
constexpr double kLengthTolSec = 0.01;
constexpr int kMaxFrame = 65535;           // trackdata times are uint16
constexpr int kMaxVariants = 8;            // v0.14: distinct variant_flight values we keep
constexpr int kBuildsPerPoll = 12;         // v0.14: entries built per poll
constexpr float kArmTailSec = 3.0f;        // v0.14: window = max(slider, variant flight seconds + this)

// The game codec layout, copied from WD2Vaults_v2 (same virtual order, size 0x30).
class NativeCodec
{
public:
    virtual void Unk_000();
    virtual void Unk_008();
    virtual void Unk_010();
    virtual uint32 Unk_018_GetInformationType();
    virtual void Unk_020();
    virtual uint32 Unk_028_GetKeyframeSize_Uncompressed();
    virtual uint32 Unk_030_GetKeyframeSize_Compressed();
    virtual void Unk_038();
    virtual uint32 Unk_040_GetNumKeyframes(uint8* trackdata);
    virtual void Unk_048();
    virtual void Unk_050_CompressAndSetKeyframeValues(uint8* trackdata, void* keyframes, unsigned int numKeyframes);
    virtual void Unk_058();
    virtual void Unk_060();
    virtual uint16 Unk_068_GetNthKeyframeTime_mb(uint8* trackdata, uint32 keyframe);
    virtual void* Unk_070_GetStartOfKeyframeValues(uint8* trackdata);
    virtual void Unk_078_UncompressKeyframeValue(uint8* trackdata, unsigned int keyframeIdx, void* p_out);
    virtual void Unk_080_Deserialize_mb();
    virtual uint8* Unk_088_AllocateTrackdata(SmallArray<uint16>& keyframeTimes);

    uint32 dword_8_alignMb;          //0x0008
    char pad_000C[4];                //0x000C
    void* callback_10;               //0x0010
    char pad_0018[8];                //0x0018
    uint8 informationType_mb;        //0x0020
    uint8 thisStructIdxInLUT;        //0x0021
    char pad_0022[14];               //0x0022
};
static_assert(sizeof(NativeCodec) == 0x30);

// The codec table holds 72 entries.
NativeCodec* CodecAt(uint8 id)
{
    if (id >= 72) return nullptr;
    auto& table = *reinterpret_cast<std::array<NativeCodec*, 72>*>(kCodecTable);
    return table[id];
}

// v0.15: codec id for the baked translation tracks. 28 (u16 key times) when the game's table has it with the same value
// format and size as 27; otherwise 27, so the build still runs (flights up to 255 frames only). Decided once, logged once.
std::atomic<int> g_transBuildCodec{ -1 };
uint8 TransBuildCodec()
{
    const int known = g_transBuildCodec.load();
    if (known >= 0) return static_cast<uint8>(known);
    NativeCodec* c27 = CodecAt(kTransCodecU8);
    NativeCodec* c28 = CodecAt(kTransCodecU16);
    const bool ok = c27 && c28
        && c28->Unk_018_GetInformationType() == kInfoTrans
        && c28->Unk_028_GetKeyframeSize_Uncompressed() == c27->Unk_028_GetKeyframeSize_Uncompressed();
    const uint8 chosen = ok ? kTransCodecU16 : kTransCodecU8;
    g_transBuildCodec.store(static_cast<int>(chosen));
    LogLine("origins: translation tracks are built with codec %u (codec 28 %s)", static_cast<unsigned>(chosen),
            ok ? "has the same value format and size as 27" : "not usable, codec 27 used");
    return chosen;
}

// v0.15: sources may store translations with codec 27 or 28 (same values, different key-time width); rotations only 1.
bool SourceCodecOk(bool rot, long long codec)
{
    return rot ? codec == static_cast<long long>(kRotCodec)
               : (codec == static_cast<long long>(kTransCodecU8) || codec == static_cast<long long>(kTransCodecU16));
}

// Values as the codecs read them. OQuat is normalised like WD2Vaults (identity for a zero quaternion).
struct alignas(16) OQuat
{
    float x, y, z, w;
    static OQuat From(const float* v)
    {
        float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
        if (n < 1e-8f) return OQuat{ 0.0f, 0.0f, 0.0f, 1.0f };
        return OQuat{ v[0] / n, v[1] / n, v[2] / n, v[3] / n };
    }
};
static_assert(sizeof(OQuat) == 16);
struct OVec3
{
    float x, y, z;
    static OVec3 From(const float* v) { return OVec3{ v[0], v[1], v[2] }; }
};
static_assert(sizeof(OVec3) == 12);

// ---- Parsed data (kept for the whole session) ----------------------------------------------------------------
struct Key { int frame; float v[4]; };           // v[0..3] for rotation, v[0..2] for translation
struct SrcTrack
{
    uint32 trackId = 0;
    uint32 infotype = 0;                          // kInfoRot or kInfoTrans
    std::vector<Key> keys;
};
// One replaced slot of one clip: the original pointer (read at build time) and our new trackdata.
struct Swap { uint16 slot; void* orig; void* ptr; };

// v0.12 retime (mapping.json takeoff/land/end and new_land/new_end): old frame t -> new frame.
// A = takeoff, L = land, E = end (old timeline); L2 = new land, E2 = new end. F = L - A, F2 = L2 - A.
struct Warp
{
    double A = 0.0, L = 0.0, E = 0.0, L2 = 0.0, E2 = 0.0;
    double F = 0.0, F2 = 0.0;
    double Map(double t) const
    {
        if (t <= A) return t;
        if (t <= L) return A + (t - A) * F2 / F;
        return t + (E2 - E);
    }
    int Frame(int t) const                                   // rounded, clamped to [0, E2]
    {
        const double f = std::floor(Map(static_cast<double>(t)) + 0.5);
        return static_cast<int>(std::clamp(f, 0.0, E2));
    }
    bool Identity() const { return F2 == F && E2 == E; }
};

// v0.17: the root position track (TrackID 0, translation) of a built entry, rescaled at arm time. POD, so a plan can hold
// copies. tmpl and scratch point to leaked storage of n OVec3 (scratch is 16-byte aligned); they never change size.
struct RootPatch
{
    uint8* td;                 // our trackdata for the root position track (never freed)
    NativeCodec* codec;        // the translation codec it was encoded with
    uint32 n;                  // key count
    const OVec3* tmpl;         // template values as built
    OVec3* scratch;            // rescale target, same count
    float yA, zA, HB, DB;      // root_scale anchors and spans (mapping.json)
    float hg;                  // v0.18: horizontal gain (root_scale "HG"; 1.0 when absent or invalid)
};

struct Entry
{
    // parsed once at load
    std::string name;
    std::string source;
    uint64 handle = 0;
    int flight = 72;                               // v0.14: variant_flight (frames); 72 when the field is absent
    int vi = 0;                                    // v0.14: index of the variant in the sorted list (set in Load)
    float fileLength = 0.0f;
    std::vector<SrcTrack> tracks;
    bool parsed = false;                           // the source file was read and has at least one usable track
    bool dead = false;                             // never built again (bad source, failed self-check, not an Animation)
    std::string note;                              // why it is dead
    // built for one Animation object
    bool built = false;
    Animation* anim = nullptr;
    std::unique_ptr<ACU::StrongRef<ManagedObject>> ref;   // keeps the Animation loaded while the entry is built
    std::vector<Swap> swaps;
    int rotBuilt = 0;
    int transBuilt = 0;
    std::vector<uint32> notFound;                  // track IDs (with the wanted information type) that the clip lacks
    // v0.12 retime (only when mapping.json has the new_* fields; validated in Load)
    bool retime = false;
    Warp warp{};
    float origLength = 0.0f;                       // mapping orig_length: must match the game clip (within 0.01 s)
    float newLength = 0.0f;                        // mapping new_length: written to the clip's Length while armed
    float lengthOrig = 0.0f;                       // the game clip's Length read at build time
    // v0.17 root_scale (load: all six numbers present and valid, else ignored)
    bool rootScale = false;
    float rsYA = 0.0f, rsZA = 0.0f, rsHB = 1.0f, rsDB = 1.0f;
    float rsHG = 1.0f;                             // v0.18: root_scale "HG" (1.0 when absent or invalid)
    bool rootBuilt = false;                        // the root position track of this build is one of its swaps
    RootPatch root{};                              // valid when rootBuilt
    std::string state = "waiting for the player";
};

// ---- Arm plan (immutable once published) ---------------------------------------------------------------------
// addr = &anim->rawTracks.arr[slot] of a built clip. orig = the original track pointer, ptr = our new trackdata.
struct PlanSlot { void** addr; void* orig; void* ptr; };
struct PlanFloat { float* addr; float orig; float neu; };   // &anim->Length of a retimed clip (v0.12)
struct ArmPlan { std::vector<PlanSlot> slots; std::vector<PlanFloat> floats; std::vector<RootPatch> roots; };  // v0.17 roots

std::vector<Entry> g_entries;
bool g_anyParsed = false;                          // at least one entry was loaded: the feature is active
std::atomic<bool> g_on{ true };                    // menu checkbox, default ON (cleared on a fault)
std::atomic<float> g_armSeconds{ kArmDefaultSec }; // menu slider
std::atomic<bool> g_restoreReq{ false };           // menu button, handled on the frame callback
std::atomic<bool> g_faulted{ false };              // a fault turned the feature off for the session
std::atomic<uint32> g_faults{ 0 };
// v0.14: the loaded flights, sorted ascending. Filled by Load() before any game-thread use, never changed afterwards.
int g_variantFlight[kMaxVariants] = {};
int g_variantCount = 0;
// One plan per variant index; immutable once published. Replaced plans are leaked on purpose.
std::atomic<const ArmPlan*> g_plans[kMaxVariants]{};
// Game-thread arm state (written by ArmFromGameThread, read by the frame callback and the menu).
std::atomic<bool> g_armedAtomic{ false };          // the window is open
std::atomic<int> g_armedVariant{ -1 };             // index of the armed variant, -1 when none
std::atomic<double> g_armTimeAtomic{ 0.0 };        // QPC seconds of the last game-thread arm
std::atomic<uint32> g_gtArmSerial{ 0 };            // number of game-thread arms
std::atomic<uint32> g_gtWritten{ 0 };              // slots written by the last game-thread arm
std::atomic<uint32> g_gtSkipped{ 0 };              // slots skipped by the last game-thread arm
std::atomic<float> g_gtDrop{ -1.0f };              // drop of the last arm (-1: not available)
std::atomic<float> g_gtTarget{ -1.0f };            // target flight frames of the last arm (-1: none)
std::atomic<int> g_gtChosenFrames{ 0 };            // flight frames of the variant armed last
std::atomic<int> g_gtRestoredVariant{ -1 };        // variant put back by the last arm when it switched (-1: none)
std::atomic<uint32> g_gtRestoredWritten{ 0 };
std::atomic<uint32> g_gtRestoredSkipped{ 0 };
std::atomic<uint32> g_gtRootCount{ 0 };            // v0.17: root templates in the variant armed last
std::atomic<uint32> g_gtRootOk{ 0 };               // rescaled and checked
std::atomic<uint32> g_gtRootFailed{ 0 };           // a check failed (the template was kept)
std::atomic<bool> g_gtRootSane{ false };           // drop and horizontal distance were usable: the roots were rescaled
std::atomic<float> g_gtRootDrop{ -1.0f };
std::atomic<float> g_gtRootHoriz{ -1.0f };
std::atomic<float> g_gtRootGain{ 1.0f };          // v0.18: HG used for the roots of the variant armed last
std::atomic<bool> g_gtFaultPending{ false };       // a game-thread fault, logged by the frame callback
uint32 g_loggedArmSerial = 0;                      // frame side: last arm serial logged
uint64 g_seen = 0;                                 // last seen g_eAppended
bool g_seenInit = false;
double g_playerSince = -1.0;                       // QPC seconds when the player was first seen (-1: none)
double g_lastPoll = -1e9;
uint64 g_disarmCount = 0;

double NowSec()
{
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// ---- JSON helpers (SimpleJSON returns 0 from ToFloat() for integers, and the reverse) ------------------------
double JsonNum(const json::JSON& j)
{
    bool ok = false;
    double f = j.ToFloat(ok);
    if (ok) return f;
    long long i = j.ToInt(ok);
    if (ok) return static_cast<double>(i);
    return 0.0;
}
// v0.17: like JsonNum, but false when the value is neither a float nor an integer (JsonNum would return 0).
bool JsonNumStrict(const json::JSON& j, double& out)
{
    bool ok = false;
    const double f = j.ToFloat(ok);
    if (ok) { out = f; return true; }
    const long long i = j.ToInt(ok);
    if (ok) { out = static_cast<double>(i); return true; }
    return false;
}
long long JsonInt(const json::JSON& j)
{
    bool ok = false;
    long long i = j.ToInt(ok);
    if (ok) return i;
    double f = j.ToFloat(ok);
    if (ok) return std::llround(f);
    return 0;
}
// A decimal string (as the baked files store TrackID) or an integer.
bool JsonU32(const json::JSON& j, uint32& out)
{
    if (j.JSONType() == json::JSON::Class::String)
    {
        try
        {
            unsigned long long v = std::stoull(j.ToString());
            if (v > 0xFFFFFFFFull) return false;
            out = static_cast<uint32>(v);
            return true;
        }
        catch (...) { return false; }
    }
    if (j.JSONType() == json::JSON::Class::Integral)
    {
        long long v = JsonInt(j);
        if (v < 0 || v > 0xFFFFFFFFll) return false;
        out = static_cast<uint32>(v);
        return true;
    }
    return false;
}
// A handle written as "0xF3170F307" (or an integer).
bool JsonHandle(const json::JSON& j, uint64& out)
{
    out = 0;
    if (j.JSONType() == json::JSON::Class::String)
    {
        std::string s = j.ToString();
        char* end = nullptr;
        unsigned long long v = std::strtoull(s.c_str(), &end, 0);
        if (!end || *end != '\0' || v == 0) return false;
        out = static_cast<uint64>(v);
        return true;
    }
    if (j.JSONType() == json::JSON::Class::Integral)
    {
        long long v = JsonInt(j);
        if (v <= 0) return false;
        out = static_cast<uint64>(v);
        return true;
    }
    return false;
}

// v0.14 binary baked source (bake_eject.py write_bin): little-endian, magic "BEJ1", float32 Length, uint32 trackCount;
// per track uint32 trackId, uint8 infotype, uint8 codec, uint16 0, uint32 keyCount, then records of uint16 frame,
// uint16 0 and float32 x4 (rotation) or x3 (translation). Any inconsistency rejects the whole source (e.note says why).
inline uint16 Le16(const uint8* p) { return static_cast<uint16>(p[0] | (p[1] << 8)); }
inline uint32 Le32(const uint8* p)
{
    return static_cast<uint32>(p[0]) | (static_cast<uint32>(p[1]) << 8) | (static_cast<uint32>(p[2]) << 16) | (static_cast<uint32>(p[3]) << 24);
}
inline float LeF32(const uint8* p)
{
    const uint32 u = Le32(p);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}
bool ParseBinary(const fs::path& path, Entry& e, int& skippedTracks)
{
    auto reject = [&e](const char* why) { e.note = why; return false; };
    std::ifstream in(path, std::ios::binary);
    if (!in) return reject("source unreadable");
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    in.seekg(0, std::ios::beg);
    if (end < 12) return reject("binary source too short");
    const size_t size = static_cast<size_t>(end);
    std::vector<uint8> buf(size);
    if (!in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size))) return reject("binary source unreadable");
    if (std::memcmp(buf.data(), "BEJ1", 4) != 0) return reject("binary source: bad magic");
    const float length = LeF32(buf.data() + 4);
    const uint32 trackCount = Le32(buf.data() + 8);
    if (!std::isfinite(length) || !(length > 0.0f)) return reject("binary source: bad Length");

    size_t at = 12;
    for (uint32 ti = 0; ti < trackCount; ++ti)
    {
        if (size - at < 12) return reject("binary source: track header truncated");
        const uint8* h = buf.data() + at;
        SrcTrack t;
        t.trackId = Le32(h);
        t.infotype = h[4];
        const uint8 codec = h[5];
        const uint32 nkeys = Le32(h + 8);
        if (Le16(h + 6) != 0) return reject("binary source: track reserved field not 0");
        at += 12;
        const bool rot = t.infotype == kInfoRot;
        if (!rot && t.infotype != kInfoTrans) return reject("binary source: infotype is neither 0 nor 3");
        if (!SourceCodecOk(rot, codec)) return reject("binary source: codec does not match the infotype");
        const size_t comps = rot ? 4 : 3;
        const size_t rec = 4 + 4 * comps;
        if (nkeys > (size - at) / rec) return reject("binary source: keys run past the end of the file");
        int prevFrame = -1;
        t.keys.reserve(nkeys);
        for (uint32 k = 0; k < nkeys; ++k)
        {
            const uint8* p = buf.data() + at;
            if (Le16(p + 2) != 0) return reject("binary source: key reserved field not 0");
            const int frame = static_cast<int>(Le16(p));
            if (frame < prevFrame) return reject("binary source: frames decrease");
            Key key{};
            key.frame = frame;
            for (size_t i = 0; i < comps; ++i)
            {
                key.v[i] = LeF32(p + 4 + 4 * i);
                if (!std::isfinite(key.v[i])) return reject("binary source: non-finite value");
            }
            t.keys.push_back(key);
            prevFrame = frame;
            at += rec;
        }
        if (nkeys == 0) { ++skippedTracks; continue; }
        e.tracks.push_back(std::move(t));
    }
    if (at != size) return reject("binary source: bytes after the last track");
    if (e.tracks.empty()) return reject("no usable track");
    e.fileLength = length;
    return true;
}

// Reads one baked source file into e.tracks. Tracks that cannot be used are skipped and counted. A ".bin" source is the
// binary format (ParseBinary); anything else is the JSON format.
bool ParseSource(const fs::path& path, Entry& e, int& skippedTracks, int& badKeys)
{
    if (path.extension() == ".bin")
    {
        (void)badKeys;
        return ParseBinary(path, e, skippedTracks);
    }
    json::JSON root;
    try { root = json::FromFile(path); }
    catch (...) { e.note = "source unreadable"; return false; }
    e.fileLength = static_cast<float>(JsonNum(root["Length"]));
    if (root["Tracks"].JSONType() != json::JSON::Class::Array) { e.note = "no Tracks array"; return false; }
    for (json::JSON& jt : root["Tracks"].ArrayRange())
    {
        SrcTrack t;
        if (!JsonU32(jt["TrackID"], t.trackId)) { ++skippedTracks; continue; }
        const long long info = JsonInt(jt["infotype"]);
        if (info != static_cast<long long>(kInfoRot) && info != static_cast<long long>(kInfoTrans)) { ++skippedTracks; continue; }
        t.infotype = static_cast<uint32>(info);
        const bool rot = t.infotype == kInfoRot;
        if (jt.hasKey("codec"))
        {
            const long long codec = JsonInt(jt["codec"]);
            if (!SourceCodecOk(rot, codec))
            {
                LogLine("origins: [%s] track %u: file codec %lld, expected %s: track skipped", e.name.c_str(), t.trackId, codec, rot ? "1" : "27 or 28");
                ++skippedTracks;
                continue;
            }
        }
        if (jt["Keyframes"].JSONType() != json::JSON::Class::Array) { ++skippedTracks; continue; }
        const int need = rot ? 4 : 3;
        for (json::JSON& kf : jt["Keyframes"].ArrayRange())
        {
            json::JSON& val = kf[rot ? "LocalRotation" : "LocalPosition"];
            if (val.JSONType() != json::JSON::Class::Array || static_cast<int>(val.size()) < need) { ++badKeys; continue; }
            Key k{};
            k.frame = static_cast<int>(JsonInt(kf["Frame"]));
            for (int i = 0; i < need; ++i) k.v[i] = static_cast<float>(JsonNum(val[i]));
            t.keys.push_back(k);
        }
        if (t.keys.empty()) { ++skippedTracks; continue; }
        e.tracks.push_back(std::move(t));
    }
    if (e.tracks.empty()) { e.note = "no usable track"; return false; }
    return true;
}

// Reads mapping.json and every source file. A missing folder or file makes the feature inactive; never a crash.
void Load()
{
    g_entries.clear();
    g_anyParsed = false;
    g_variantCount = 0;
    try
    {
        fs::path dir = AbsolutePathInThisDLLDirectory("BackEjectLoF\\origins");
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) { LogLine("origins: folder missing (%s): feature inactive", dir.string().c_str()); return; }
        fs::path mapPath = dir / "mapping.json";
        if (!fs::exists(mapPath, ec)) { LogLine("origins: mapping.json missing: feature inactive"); return; }
        json::JSON map;
        try { map = json::FromFile(mapPath); }
        catch (...) { LogLine("origins: mapping.json unreadable: feature inactive"); return; }
        if (map["clips"].JSONType() != json::JSON::Class::Array) { LogLine("origins: mapping.json has no clips array: feature inactive"); return; }

        int entries = 0, loaded = 0, tracks = 0, rotKeys = 0, transKeys = 0, skipped = 0, badKeys = 0;
        for (json::JSON& c : map["clips"].ArrayRange())
        {
            ++entries;
            Entry e;
            e.name = c["name"].JSONType() == json::JSON::Class::String ? c["name"].ToString() : std::string();
            e.source = c["source"].JSONType() == json::JSON::Class::String ? c["source"].ToString() : std::string();
            if (!JsonHandle(c["handle"], e.handle) || e.source.empty())
            {
                LogLine("origins: entry %d skipped (bad handle or source)", entries);
                continue;
            }
            if (e.name.empty()) e.name = e.source;
            // v0.14: flight variant in frames. An entry without variant_flight is variant 72 (the older data).
            if (c.hasKey("variant_flight"))
            {
                const long long vf = JsonInt(c["variant_flight"]);
                if (vf <= 0 || vf > static_cast<long long>(kMaxFrame))
                {
                    LogLine("origins: [%s] variant_flight %lld invalid: entry skipped", e.name.c_str(), vf);
                    continue;
                }
                e.flight = static_cast<int>(vf);
            }
            // v0.12: optional retime fields. All seven are needed together. An invalid warp skips the entry (logged).
            if (c.hasKey("new_land") || c.hasKey("new_end") || c.hasKey("new_length"))
            {
                static const char* const kRetimeKeys[] = { "takeoff", "land", "end", "new_land", "new_end", "new_length", "orig_length" };
                const char* missingKey = nullptr;
                for (const char* rk : kRetimeKeys) { if (!c.hasKey(rk)) { missingKey = rk; break; } }
                if (missingKey) { LogLine("origins: [%s] retime field \"%s\" missing: entry skipped", e.name.c_str(), missingKey); continue; }
                e.retime = true;
                e.warp.A = JsonNum(c["takeoff"]);
                e.warp.L = JsonNum(c["land"]);
                e.warp.E = JsonNum(c["end"]);
                e.warp.L2 = JsonNum(c["new_land"]);
                e.warp.E2 = JsonNum(c["new_end"]);
                e.warp.F = e.warp.L - e.warp.A;
                e.warp.F2 = e.warp.L2 - e.warp.A;
                e.newLength = static_cast<float>(JsonNum(c["new_length"]));
                e.origLength = static_cast<float>(JsonNum(c["orig_length"]));
                if (!(e.warp.F > 0.0) || e.warp.F2 < e.warp.F || e.warp.E2 < e.warp.E
                    || e.warp.E2 > static_cast<double>(kMaxFrame) || !(e.newLength > 0.0f))
                {
                    LogLine("origins: [%s] retime invalid (takeoff %.0f, land %.0f, end %.0f, new land %.0f, new end %.0f, new length %.3f): entry skipped",
                            e.name.c_str(), e.warp.A, e.warp.L, e.warp.E, e.warp.L2, e.warp.E2, static_cast<double>(e.newLength));
                    continue;
                }
            }
            // v0.17: optional root_scale (bake_eject.py). All six numbers are needed; a missing or invalid one ignores the
            // field (logged). Only yA, zA, HB and DB are used at arm time; A and L2 are checked for presence.
            if (c.hasKey("root_scale"))
            {
                static const char* const kRootKeys[] = { "A", "L2", "yA", "zA", "HB", "DB" };
                json::JSON& rs = c["root_scale"];
                double v[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
                bool good = rs.JSONType() == json::JSON::Class::Object;
                for (size_t i = 0; good && i < 6; ++i)
                    good = rs.hasKey(kRootKeys[i]) && JsonNumStrict(rs[kRootKeys[i]], v[i]);
                good = good && std::isfinite(v[2]) && std::isfinite(v[3]) && v[4] > 0.0 && v[5] > 0.0;
                if (good)
                {
                    e.rootScale = true;
                    e.rsYA = static_cast<float>(v[2]);
                    e.rsZA = static_cast<float>(v[3]);
                    e.rsHB = static_cast<float>(v[4]);
                    e.rsDB = static_cast<float>(v[5]);
                    // v0.18: optional "HG". Absent: 1.0. Present but not a number in 0.5..2.0: 1.0 and a log line.
                    if (rs.hasKey("HG"))
                    {
                        double hg = 0.0;
                        if (JsonNumStrict(rs["HG"], hg) && std::isfinite(hg) && hg >= 0.5 && hg <= 2.0)
                            e.rsHG = static_cast<float>(hg);
                        else
                            LogLine("origins: [%s] root_scale HG invalid (accepted 0.5..2.0): gain 1.0 used", e.name.c_str());
                    }
                }
                else
                {
                    LogLine("origins: [%s] root_scale missing or invalid: ignored (root kept as the template)", e.name.c_str());
                }
            }
            fs::path sp = dir / e.source;
            if (!fs::exists(sp, ec))
            {
                LogLine("origins: [%s] source missing: %s", e.name.c_str(), sp.string().c_str());
                continue;
            }
            int skippedHere = 0, badHere = 0;
            if (!ParseSource(sp, e, skippedHere, badHere))
            {
                LogLine("origins: [%s] source rejected: %s", e.name.c_str(), e.note.c_str());
                skipped += skippedHere;
                badKeys += badHere;
                continue;
            }
            e.parsed = true;
            for (const SrcTrack& t : e.tracks)
            {
                ++tracks;
                if (t.infotype == kInfoRot) rotKeys += static_cast<int>(t.keys.size());
                else transKeys += static_cast<int>(t.keys.size());
            }
            skipped += skippedHere;
            badKeys += badHere;
            ++loaded;
            LogLine("origins: [%s] loaded: handle 0x%llX, variant f%d, %zu track(s), file Length %.3f s, source %s",
                    e.name.c_str(), static_cast<unsigned long long>(e.handle), e.flight, e.tracks.size(),
                    static_cast<double>(e.fileLength), e.source.c_str());
            if (e.retime)
                LogLine("origins: [%s] retime: takeoff %.0f, land %.0f -> %.0f, end %.0f -> %.0f, length %.3f -> %.3f s",
                        e.name.c_str(), e.warp.A, e.warp.L, e.warp.L2, e.warp.E, e.warp.E2,
                        static_cast<double>(e.origLength), static_cast<double>(e.newLength));
            g_entries.push_back(std::move(e));
        }
        // v0.14: group by flight. At most kMaxVariants flights are kept; entries of a larger flight are dropped (logged).
        std::set<int> flightSet;
        for (const Entry& e : g_entries) flightSet.insert(e.flight);
        std::vector<int> flights(flightSet.begin(), flightSet.end());
        if (flights.size() > static_cast<size_t>(kMaxVariants))
        {
            LogLine("origins: %zu flight variants, only %d supported: flights above f%d dropped", flights.size(), kMaxVariants,
                    flights[kMaxVariants - 1]);
            const int top = flights[kMaxVariants - 1];
            flights.resize(static_cast<size_t>(kMaxVariants));
            g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(), [top](const Entry& e) { return e.flight > top; }),
                            g_entries.end());
        }
        g_variantCount = static_cast<int>(flights.size());
        for (int i = 0; i < g_variantCount; ++i) g_variantFlight[i] = flights[static_cast<size_t>(i)];
        for (Entry& e : g_entries)
            e.vi = static_cast<int>(std::lower_bound(flights.begin(), flights.end(), e.flight) - flights.begin());
        for (int vi = 0; vi < g_variantCount; ++vi)
        {
            int count = 0;
            for (const Entry& e : g_entries) if (e.vi == vi) ++count;
            LogLine("origins: variant f%d: %d entr%s loaded", g_variantFlight[vi], count, count == 1 ? "y" : "ies");
        }

        g_anyParsed = !g_entries.empty();
        LogLine("origins: mapping.json: %d entries, %d loaded, %d track(s) (%d rotation keys, %d translation keys), %d track(s) and %d key(s) skipped",
                entries, loaded, tracks, rotKeys, transKeys, skipped, badKeys);
        if (!g_anyParsed) LogLine("origins: no clip loaded: feature inactive");
    }
    catch (...)
    {
        g_entries.clear();
        g_anyParsed = false;
        g_variantCount = 0;
        LogLine("origins: load failed (exception): feature inactive");
    }
}

// ---- Track building (game memory: only inside guards) ----------------------------------------------------------

// Slot of the clip whose TrackID and information type match; -1 when there is none. Existing slots only.
int FindSlot(Animation& anim, uint32 trackId, uint32 wantInfo)
{
    if (!anim.AnimTrackData_) return -1;
    auto& mapping = anim.AnimTrackData_->AnimTrackDataMapping_;
    for (uint16 i = 0; i < mapping.size && i < anim.rawTracks.size; ++i)
    {
        if (mapping[i].TrackID != trackId) continue;
        uint8* td = reinterpret_cast<uint8*>(anim.rawTracks.arr[i]);
        if (!td || td[0] >= 72) continue;
        NativeCodec* codec = CodecAt(td[0]);
        if (codec && codec->Unk_018_GetInformationType() == wantInfo) return i;
    }
    return -1;
}

// Keys in the clip's frames (60 Hz): frames clamped to [0, maxFrame] and strictly increasing. A key that lands on the
// previous frame replaces its value; an out-of-order key is dropped. The first key is held back to frame 0 and the last
// one forward to maxFrame, so the codec sees the whole clip (the same boundary keys WD2Vaults adds to prebaked data).
template <typename V>
void KeysToTrack(const std::vector<Key>& keys, int maxFrame, std::vector<std::pair<int, V>>& out, int& fixups)
{
    out.clear();
    for (const Key& k : keys)
    {
        const int f = std::clamp(k.frame, 0, maxFrame);
        if (f != k.frame) ++fixups;
        const V v = V::From(k.v);
        if (!out.empty() && f < out.back().first) { ++fixups; continue; }
        if (!out.empty() && f == out.back().first) { out.back().second = v; ++fixups; continue; }
        out.push_back({ f, v });
    }
    if (out.empty()) return;
    if (out.front().first > 0)
    {
        std::pair<int, V> head{ 0, out.front().second };
        out.insert(out.begin(), head);
    }
    if (out.back().first < maxFrame) out.push_back({ maxFrame, out.back().second });
}

// Encodes the keys with the codec, using the heap the game uses for trackdata. The result is never freed.
template <typename V>
uint8* EncodeKeys(NativeCodec& codec, const std::vector<std::pair<int, V>>& keys)
{
    SmallArray<uint16> times;
    SmallArray<V> values;
    void* heap = *reinterpret_cast<void**>(kCurrentHeap);
    ACU::Memory::SmallArrayReserve(times, static_cast<uint16>(keys.size()), heap);
    ACU::Memory::SmallArrayReserve(values, static_cast<uint16>(keys.size()), heap);
    for (const auto& kv : keys)
    {
        ACU::Memory::SmallArrayAppend(times, static_cast<uint16>(kv.first));
        ACU::Memory::SmallArrayAppend(values, kv.second);
    }
    uint8* td = codec.Unk_088_AllocateTrackdata(times);
    codec.Unk_050_CompressAndSetKeyframeValues(td, values.arr, static_cast<unsigned int>(keys.size()));
    return td;
}

// True when every component of got matches want (negated when negate is set) within kKeyTol.
bool ComponentsMatch(const float* got, const float* want, int comps, bool negate, float tol = kKeyTol)
{
    for (int i = 0; i < comps; ++i)
    {
        const float w = negate ? -want[i] : want[i];
        if (std::fabs(got[i] - w) > tol) return false;
    }
    return true;
}

// Decodes the first and last key of a new track through the game's codec and compares them with the input.
// Rotations are sign-insensitive: q and -q are the same rotation, and the codec may store w >= 0.
template <typename V>
bool CheckDecode(NativeCodec& codec, uint8* td, const std::vector<std::pair<int, V>>& keys, const char* name, const char* what)
{
    const uint32 n = codec.Unk_040_GetNumKeyframes(td);
    if (n == 0 || n != keys.size())
    {
        LogLine("origins: [%s] self-check %s: %u key(s) decoded, %zu encoded", name, what, n, keys.size());
        return false;
    }
    constexpr int comps = static_cast<int>(sizeof(V) / sizeof(float));
    constexpr bool isQuat = std::is_same_v<V, OQuat>;
    const uint32 idxs[2] = { 0u, n - 1u };
    for (uint32 idx : idxs)
    {
        alignas(16) float got[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        codec.Unk_078_UncompressKeyframeValue(td, idx, got);
        const int frame = static_cast<int>(codec.Unk_068_GetNthKeyframeTime_mb(td, idx));
        if (frame != keys[idx].first)
        {
            LogLine("origins: [%s] self-check %s key %u: frame %d, expected %d", name, what, idx, frame, keys[idx].first);
            return false;
        }
        const float* want = reinterpret_cast<const float*>(&keys[idx].second);
        bool ok = ComponentsMatch(got, want, comps, false);
        if (!ok && isQuat) ok = ComponentsMatch(got, want, comps, true);
        if (!ok)
        {
            LogLine("origins: [%s] self-check %s key %u: decoded (%.5f %.5f %.5f %.5f), expected (%.5f %.5f %.5f %.5f)",
                    name, what, idx,
                    static_cast<double>(got[0]), static_cast<double>(got[1]), static_cast<double>(got[2]), static_cast<double>(got[3]),
                    static_cast<double>(want[0]), static_cast<double>(want[1]),
                    static_cast<double>(comps > 2 ? want[2] : 0.0f), static_cast<double>(comps > 3 ? want[3] : 0.0f));
            return false;
        }
    }
    return true;
}

// Result of one build. Plain data, so it can cross the guard.
struct BuildOut { int rot; int trans; bool fail; int retimed; };

constexpr float kRetimeTol = 2e-3f;   // v0.12: self-check tolerance for retimed values (floats)

// Retimes one trackdata block that no baked track replaces, from the old timeline to the new one (v0.12). On success
// out is the new block, or td itself when the block has no keys (nothing to do). Returns false (logged) when the
// retime cannot be done exactly; the caller then does not use the clip. Game codecs only: runs inside the guard.
bool RetimeTrack(const Warp& w, uint8* td, uint16 slot, const char* name, uint8*& out)
{
    out = td;
    NativeCodec* codec = CodecAt(td[0]);
    if (!codec) { LogLine("origins: [%s] slot %u: codec %u missing: clip not used", name, slot, static_cast<unsigned>(td[0])); return false; }
    const uint32 info = codec->Unk_018_GetInformationType();
    const uint32 size = codec->Unk_028_GetKeyframeSize_Uncompressed();
    if (size > 16) { LogLine("origins: [%s] slot %u: keyframe size %u > 16: clip not used", name, slot, size); return false; }
    const uint32 n = codec->Unk_040_GetNumKeyframes(td);
    if (n == 0) return true;
    if (n > 0xFFFFu) { LogLine("origins: [%s] slot %u: %u keys: clip not used", name, slot, n); return false; }

    // The u16-time codec of the same value format: codecs come in triples (+0 u8 times, +1 u16 times, +2 bitmask).
    const uint8 newId = static_cast<uint8>(td[0] - td[0] % 3 + 1);
    NativeCodec* newCodec = CodecAt(newId);
    if (!newCodec || newCodec->Unk_018_GetInformationType() != info || newCodec->Unk_028_GetKeyframeSize_Uncompressed() != size)
    {
        LogLine("origins: [%s] slot %u: codec %u -> %u has another value format: clip not used", name, slot,
                static_cast<unsigned>(td[0]), static_cast<unsigned>(newId));
        return false;
    }

    // Old keys decoded with the old codec: new frames (rounded, clamped, never smaller than the previous) and raw values.
    std::vector<uint16> newFrames(n);
    std::vector<uint8> raw(static_cast<size_t>(n) * size + 16);
    uint8* vals = reinterpret_cast<uint8*>((reinterpret_cast<uintptr_t>(raw.data()) + 15) & ~static_cast<uintptr_t>(15));
    int prev = 0;
    for (uint32 k = 0; k < n; ++k)
    {
        int f = w.Frame(static_cast<int>(codec->Unk_068_GetNthKeyframeTime_mb(td, k)));
        if (f < prev) f = prev;
        newFrames[k] = static_cast<uint16>(f);
        prev = f;
        alignas(16) uint8 tmp[16] = {};
        codec->Unk_078_UncompressKeyframeValue(td, k, tmp);
        std::memcpy(vals + static_cast<size_t>(k) * size, tmp, size);
    }

    SmallArray<uint16> times;
    void* heap = *reinterpret_cast<void**>(kCurrentHeap);
    ACU::Memory::SmallArrayReserve(times, static_cast<uint16>(n), heap);
    for (uint32 k = 0; k < n; ++k) ACU::Memory::SmallArrayAppend(times, newFrames[k]);
    uint8* td2 = newCodec->Unk_088_AllocateTrackdata(times);
    if (!td2) { LogLine("origins: [%s] slot %u: no trackdata from codec %u: clip not used", name, slot, static_cast<unsigned>(newId)); return false; }
    newCodec->Unk_050_CompressAndSetKeyframeValues(td2, vals, n);

    // Self-check of every key: frame equal; values equal (floats for information types 0, 3, 7; quaternions
    // sign-insensitive), otherwise byte-equal.
    if (newCodec->Unk_040_GetNumKeyframes(td2) != n) { LogLine("origins: [%s] slot %u: retimed block has %u keys, expected %u: clip not used", name, slot, newCodec->Unk_040_GetNumKeyframes(td2), n); return false; }
    const bool asFloats = (info == 0u || info == 3u || info == 7u) && size >= 4 && size % 4 == 0;
    const bool quat = info == 0u;
    const int comps = asFloats ? static_cast<int>(size / 4) : 0;
    for (uint32 k = 0; k < n; ++k)
    {
        const int got_frame = static_cast<int>(newCodec->Unk_068_GetNthKeyframeTime_mb(td2, k));
        if (got_frame != static_cast<int>(newFrames[k]))
        {
            LogLine("origins: [%s] retime self-check: slot %u key %u (codec %u -> %u): frame %d, expected %d: clip not used",
                    name, slot, k, static_cast<unsigned>(td[0]), static_cast<unsigned>(newId), got_frame, static_cast<int>(newFrames[k]));
            return false;
        }
        alignas(16) uint8 got[16] = {};
        newCodec->Unk_078_UncompressKeyframeValue(td2, k, got);
        const uint8* want = vals + static_cast<size_t>(k) * size;
        bool ok = false;
        if (asFloats)
        {
            const float* g = reinterpret_cast<const float*>(got);
            const float* wv = reinterpret_cast<const float*>(want);
            ok = ComponentsMatch(g, wv, comps, false, kRetimeTol)
              || (quat && comps == 4 && ComponentsMatch(g, wv, comps, true, kRetimeTol));
            if (!ok)
                LogLine("origins: [%s] retime self-check: slot %u key %u (codec %u -> %u): decoded (%.5f %.5f %.5f %.5f), expected (%.5f %.5f %.5f %.5f): clip not used",
                        name, slot, k, static_cast<unsigned>(td[0]), static_cast<unsigned>(newId),
                        static_cast<double>(g[0]), static_cast<double>(comps > 1 ? g[1] : 0.0f),
                        static_cast<double>(comps > 2 ? g[2] : 0.0f), static_cast<double>(comps > 3 ? g[3] : 0.0f),
                        static_cast<double>(wv[0]), static_cast<double>(comps > 1 ? wv[1] : 0.0f),
                        static_cast<double>(comps > 2 ? wv[2] : 0.0f), static_cast<double>(comps > 3 ? wv[3] : 0.0f));
        }
        else
        {
            ok = std::memcmp(got, want, size) == 0;
            if (!ok) LogLine("origins: [%s] retime self-check: slot %u key %u (codec %u -> %u): values differ (bytes): clip not used",
                             name, slot, k, static_cast<unsigned>(td[0]), static_cast<unsigned>(newId));
        }
        if (!ok) return false;
    }
    out = td2;
    return true;
}

// Builds every matching track of one clip. Uses e.anim (set by the caller; kept alive by e.ref) and fills e.swaps.
// fail = true means the clip must not be used (a failed self-check, or no track matched). Logs its own summary.
__declspec(noinline) void BuildImpl(Entry& e, BuildOut& out)
{
    out = BuildOut{ 0, 0, false, 0 };
    e.swaps.clear();
    e.notFound.clear();
    e.rootBuilt = false;
    Animation& anim = *e.anim;
    if (!anim.AnimTrackData_) { LogLine("origins: [%s] clip has no AnimTrackData: not used", e.name.c_str()); out.fail = true; return; }
    const float length = anim.Length;
    if (!(length > 0.01f)) { LogLine("origins: [%s] clip length %.3f: not used", e.name.c_str(), static_cast<double>(length)); out.fail = true; return; }
    e.lengthOrig = length;
    if (e.retime && std::fabs(static_cast<double>(e.origLength) - static_cast<double>(length)) > kLengthTolSec)
    {
        LogLine("origins: [%s] game clip length %.3f s, mapping orig_length %.3f s: not used (not the clip it was baked for)",
                e.name.c_str(), static_cast<double>(length), static_cast<double>(e.origLength));
        out.fail = true;
        return;
    }
    // v0.12: a retimed clip's baked keys are on the new timeline, so its last frame is the new end (E2, checked in Load).
    int maxFrame = e.retime ? static_cast<int>(e.warp.E2) : static_cast<int>(std::lround(static_cast<double>(length) * kFps));
    maxFrame = std::clamp(maxFrame, 0, kMaxFrame);
    if (!e.retime && e.fileLength > 0.0f && std::fabs(e.fileLength - length) > kLengthTolSec)
        LogLine("origins: [%s] file Length %.3f s, game clip %.3f s: keys clamped to the game clip",
                e.name.c_str(), static_cast<double>(e.fileLength), static_cast<double>(length));

    bool selfRot = false, selfTrans = false;
    int fixups = 0;
    for (const SrcTrack& t : e.tracks)
    {
        const int slot = FindSlot(anim, t.trackId, t.infotype);
        if (slot < 0) { e.notFound.push_back(t.trackId); continue; }
        bool dup = false;
        for (const Swap& s : e.swaps) if (s.slot == static_cast<uint16>(slot)) { dup = true; break; }
        if (dup) continue;

        uint8* td = nullptr;
        if (t.infotype == kInfoRot)
        {
            NativeCodec* codec = CodecAt(kRotCodec);
            if (!codec || codec->Unk_018_GetInformationType() != kInfoRot) continue;
            std::vector<std::pair<int, OQuat>> keys;
            KeysToTrack(t.keys, maxFrame, keys, fixups);
            if (keys.empty()) continue;
            td = EncodeKeys(*codec, keys);
            if (!td) continue;
            if (!selfRot)
            {
                if (!CheckDecode(*codec, td, keys, e.name.c_str(), "rotation")) { out.fail = true; return; }
                selfRot = true;
            }
            ++out.rot;
        }
        else
        {
            NativeCodec* codec = CodecAt(TransBuildCodec());
            if (!codec || codec->Unk_018_GetInformationType() != kInfoTrans) continue;
            std::vector<std::pair<int, OVec3>> keys;
            KeysToTrack(t.keys, maxFrame, keys, fixups);
            if (keys.empty()) continue;
            td = EncodeKeys(*codec, keys);
            if (!td) continue;
            if (!selfTrans)
            {
                if (!CheckDecode(*codec, td, keys, e.name.c_str(), "translation")) { out.fail = true; return; }
                selfTrans = true;
            }
            if (e.rootScale && t.trackId == 0)
            {
                // v0.17: the root position track keeps its template values and a 16-byte aligned scratch buffer for the
                // arm-time rescale. Both are leaked on purpose, like the trackdata: a plan may still name them.
                auto* tmpl = new std::vector<OVec3>();
                tmpl->reserve(keys.size());
                for (const auto& kv : keys) tmpl->push_back(kv.second);
                auto* raw = new std::vector<uint8>(keys.size() * sizeof(OVec3) + 16);
                const uintptr_t base = (reinterpret_cast<uintptr_t>(raw->data()) + 15) & ~static_cast<uintptr_t>(15);
                e.root = RootPatch{ td, codec, static_cast<uint32>(keys.size()), tmpl->data(), reinterpret_cast<OVec3*>(base),
                                    e.rsYA, e.rsZA, e.rsHB, e.rsDB, e.rsHG };
                e.rootBuilt = true;
            }
            ++out.trans;
        }
        e.swaps.push_back(Swap{ static_cast<uint16>(slot), anim.rawTracks.arr[slot], td });
    }
    if (out.rot + out.trans == 0) { LogLine("origins: [%s] no track matched the clip: not used", e.name.c_str()); out.fail = true; return; }

    // v0.12: every slot that no baked track replaces is retimed (only when the warp is not the identity). Any slot that
    // cannot be retimed exactly makes the whole clip unusable, since a half-retimed clip would be out of step.
    if (e.retime && !e.warp.Identity())
    {
        for (uint16 i = 0; i < anim.rawTracks.size; ++i)
        {
            bool replaced = false;
            for (const Swap& s : e.swaps) if (s.slot == i) { replaced = true; break; }
            if (replaced) continue;
            uint8* td = reinterpret_cast<uint8*>(anim.rawTracks.arr[i]);
            if (!td) continue;
            uint8* td2 = nullptr;
            if (!RetimeTrack(e.warp, td, i, e.name.c_str(), td2)) { out.fail = true; return; }
            if (td2 == td) continue;                  // no keys in this block
            e.swaps.push_back(Swap{ i, anim.rawTracks.arr[i], td2 });
            ++out.retimed;
        }
    }

    e.rotBuilt = out.rot;
    e.transBuilt = out.trans;
    std::string missing;
    for (size_t i = 0; i < e.notFound.size() && i < 8; ++i)
        missing += (i ? ", " : "") + std::to_string(e.notFound[i]);
    const float newLength = e.retime ? e.newLength : length;
    LogLine("origins: [%s] built %d rotation + %d translation slot(s), %d retimed, %zu track id(s) not found%s%s, %d key fix-up(s), clip length %.3f s -> %.3f s",
            e.name.c_str(), out.rot, out.trans, out.retimed, e.notFound.size(), missing.empty() ? "" : ": ", missing.c_str(),
            fixups, static_cast<double>(length), static_cast<double>(newLength));
}

// Lookup of a handle. On success keep receives a strong reference and out the Animation.
enum LookupRes { kNone = 0, kOk = 1, kNotLoaded = 2, kNotAnim = 3 };
__declspec(noinline) void LookupImpl(uint64 handle, std::unique_ptr<ACU::StrongRef<ManagedObject>>& keep, Animation*& out, int& res)
{
    out = nullptr;
    res = kNotLoaded;
    auto ref = std::make_unique<ACU::StrongRef<ManagedObject>>(handle);
    ManagedObject* obj = ref->GetPtr();
    if (!obj) return;
    if (*reinterpret_cast<uint64*>(obj) != Animation::GetVTBL()) { res = kNotAnim; return; }
    out = static_cast<Animation*>(obj);
    keep = std::move(ref);
    res = kOk;
}

// ---- Guards (no C++ objects in these frames, so __try is legal) ------------------------------------------------
bool SafeLookup(uint64 handle, std::unique_ptr<ACU::StrongRef<ManagedObject>>& keep, Animation*& out, int& res)
{
    __try { LookupImpl(handle, keep, out, res); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeBuild(Entry& e, BuildOut& out)
{
    __try { BuildImpl(e, out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// v0.17 root rescale, game thread, POD only. useRescale: scratch = the template with y and z mapped to horiz and drop
// (y' = yA + (y - yA) * horiz / HB, z' = zA + (z - zA) * drop / DB, x kept); otherwise scratch = the template. The values are
// encoded into the root track, and its last key is decoded and compared with scratch (match). False on an exception.
bool SafeRootSet(const RootPatch& r, float drop, float horiz, bool useRescale, bool& match)
{
    __try
    {
        match = false;
        if (r.n == 0) return true;
        for (uint32 k = 0; k < r.n; ++k)
        {
            const OVec3& t = r.tmpl[k];
            OVec3& s = r.scratch[k];
            s.x = t.x;
            s.y = useRescale ? r.yA + (t.y - r.yA) * (horiz * r.hg) / r.HB : t.y;   // v0.18: horiz * HG
            s.z = useRescale ? r.zA + (t.z - r.zA) * drop / r.DB : t.z;
        }
        r.codec->Unk_050_CompressAndSetKeyframeValues(r.td, r.scratch, r.n);
        alignas(16) float got[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        r.codec->Unk_078_UncompressKeyframeValue(r.td, r.n - 1u, got);
        match = ComponentsMatch(got, reinterpret_cast<const float*>(&r.scratch[r.n - 1u]), 3, false);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Plan walk: POD only, used on the game thread (arm) and on the frame thread (disarm, restore).
// toNew: a slot holding its original gets the new pointer. Otherwise a slot holding the new pointer gets the original.
// The counters are added to (the caller starts them at zero): pointer slots first, then the v0.12 Length values.
struct PlanCounts { uint32 written; uint32 skipped; };
bool PlanSwapGuarded(const PlanSlot* s, size_t n, bool toNew, PlanCounts& c)
{
    __try
    {
        for (size_t i = 0; i < n; ++i)
        {
            void** p = s[i].addr;
            if (toNew)
            {
                if (*p == s[i].orig) { *p = s[i].ptr; ++c.written; }
                else ++c.skipped;
            }
            else
            {
                if (*p == s[i].ptr) { *p = s[i].orig; ++c.written; }
                else ++c.skipped;
            }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Same walk for the clip Length values (v0.12). Arm writes neu only where the value still holds orig; disarm writes orig
// only where it still holds neu.
bool PlanFloatsGuarded(const PlanFloat* f, size_t n, bool toNew, PlanCounts& c)
{
    __try
    {
        for (size_t i = 0; i < n; ++i)
        {
            float* p = f[i].addr;
            if (toNew)
            {
                if (*p == f[i].orig) { *p = f[i].neu; ++c.written; }
                else ++c.skipped;
            }
            else
            {
                if (*p == f[i].neu) { *p = f[i].orig; ++c.written; }
                else ++c.skipped;
            }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- Arm plan publication (frame thread; the game thread reads the published plans) ---------------------------

// Publishes a new plan for one variant from the built entries of that variant. Called after a build, and BEFORE a
// built entry's StrongRef is reset (DropEntry), so no plan names the slots of an object we are about to let go.
// The old plan of that variant is leaked on purpose.
void PublishPlan(int vi)
{
    if (vi < 0 || vi >= g_variantCount) return;
    ArmPlan* p = new ArmPlan();
    for (const Entry& e : g_entries)
    {
        if (!e.built || !e.anim || e.vi != vi) continue;
        for (const Swap& s : e.swaps)
            p->slots.push_back(PlanSlot{ &e.anim->rawTracks.arr[s.slot], s.orig, s.ptr });
        if (e.retime) p->floats.push_back(PlanFloat{ &e.anim->Length, e.lengthOrig, e.newLength });
        if (e.rootBuilt) p->roots.push_back(e.root);
    }
    g_plans[vi].store(p, std::memory_order_release);
}

// True when the variant's plan names at least one slot or Length value.
bool PlanNonEmpty(int vi)
{
    if (vi < 0 || vi >= g_variantCount) return false;
    const ArmPlan* p = g_plans[vi].load(std::memory_order_acquire);
    return p && (!p->slots.empty() || !p->floats.empty());
}

// Slots and Length values named by all plans (log and menu only).
size_t TotalPlanSize()
{
    size_t n = 0;
    for (int vi = 0; vi < g_variantCount; ++vi)
    {
        const ArmPlan* p = g_plans[vi].load(std::memory_order_acquire);
        if (p) n += p->slots.size() + p->floats.size();
    }
    return n;
}

// Walks one variant's plan. toNew is the arm direction (see PlanSwapGuarded). False on a fault.
bool WalkVariant(int vi, bool toNew, PlanCounts& c)
{
    if (vi < 0 || vi >= g_variantCount) return true;
    const ArmPlan* p = g_plans[vi].load(std::memory_order_acquire);
    if (!p) return true;
    if (!p->slots.empty() && !PlanSwapGuarded(p->slots.data(), p->slots.size(), toNew, c)) return false;
    if (!p->floats.empty() && !PlanFloatsGuarded(p->floats.data(), p->floats.size(), toNew, c)) return false;
    return true;
}

// Walks every variant's plan (restore button, checkbox off). False on a fault.
bool WalkAll(bool toNew, PlanCounts& c)
{
    for (int vi = 0; vi < g_variantCount; ++vi)
        if (!WalkVariant(vi, toNew, c)) return false;
    return true;
}

// "f72" for a variant index, "none" otherwise.
std::string FlightName(int vi)
{
    if (vi < 0 || vi >= g_variantCount) return "none";
    return "f" + std::to_string(g_variantFlight[vi]);
}

// Arm window for a variant of the given flight (frames): the slider, or the flight plus a 3 s tail, whichever is longer.
float WindowFor(int flightFrames)
{
    const float slider = g_armSeconds.load();
    if (flightFrames <= 0) return slider;
    return std::max(slider, static_cast<float>(flightFrames) / 60.0f + kArmTailSec);
}

// ---- State changes (frame thread) ------------------------------------------------------------------------------

// Turns the feature off for the session. No further writes happen after this.
void Fault(const char* where)
{
    if (g_faulted.exchange(true)) return;
    g_faults.fetch_add(1);
    g_on.store(false);
    g_armedAtomic.store(false);
    LogLine("origins: FAULT in %s: feature OFF for this session, no further writes", where);
}

// Forgets an entry without writing to its Animation. A later poll rebuilds it. The plan of its variant is republished
// first, so it no longer names this entry's slots before the StrongRef that keeps the object alive is released.
void DropEntry(Entry& e, const char* why)
{
    LogLine("origins: [%s f%d] state dropped (%s): nothing written, rebuilt on a later poll", e.name.c_str(), e.flight, why);
    e.built = false;
    PublishPlan(e.vi);
    e.swaps.clear();
    e.notFound.clear();
    e.rotBuilt = e.transBuilt = 0;
    e.ref.reset();
    e.anim = nullptr;
    e.state = why;
}

// Puts the originals back wherever our pointers are still in place: the armed variant's plan, or every variant's plan
// when allVariants is set (checkbox off).
void DisarmAll(const char* why, bool allVariants)
{
    PlanCounts c{};
    const int av = g_armedVariant.load();
    const bool ok = allVariants ? WalkAll(false, c) : WalkVariant(av, false, c);
    if (!ok) { Fault("disarm"); return; }
    g_armedAtomic.store(false);
    g_armedVariant.store(-1);
    ++g_disarmCount;
    LogLine("origins: disarmed (%s), variant %s: %u slot(s) restored, %u skipped (no longer ours)", why,
            allVariants ? "all" : FlightName(av).c_str(), c.written, c.skipped);
}

void RestoreAll()
{
    if (g_faulted.load()) { LogLine("origins: restore requested, but the feature is faulted: nothing written"); return; }
    PlanCounts c{};
    if (!WalkAll(false, c)) { Fault("restore"); return; }
    g_armedAtomic.store(false);
    g_armedVariant.store(-1);
    LogLine("origins: restore now: %u slot(s) put back to the originals (%u skipped)", c.written, c.skipped);
}

// Flight seconds of the game's own leap-of-faith timing for a drop in metres: piecewise linear, and beyond 100 m
// 0.033 s more per metre.
float DropToSeconds(float d)
{
    static constexpr float kDrop[7] = { 0.0f, 3.0f, 15.0f, 30.0f, 45.0f, 70.0f, 100.0f };
    static constexpr float kSec[7] = { 0.45f, 0.47f, 1.13f, 1.83f, 2.87f, 4.07f, 5.07f };
    if (d <= kDrop[0]) return kSec[0];
    for (int i = 1; i < 7; ++i)
    {
        if (d <= kDrop[i])
            return kSec[i - 1] + (kSec[i] - kSec[i - 1]) * (d - kDrop[i - 1]) / (kDrop[i] - kDrop[i - 1]);
    }
    return kSec[6] + 0.033f * (d - kDrop[6]);
}

// Game thread, called synchronously right after our eject is appended (experiment E). dropMeters is the player z minus
// the hay z of that attempt; a negative value means "not available" (the default variant, index 0, is used).
// horizMeters is the horizontal player-to-hay distance of that attempt (negative: not available); v0.17 uses it with the
// drop to rescale the root templates. Chooses the variant whose flight (frames) is the smallest one >= the target, or the
// largest when none is. A variant with an empty plan falls back to the non-empty one nearest the target. A different
// variant still armed is restored first. Only atomics and POD walks here: no logging, no containers. An eject while armed
// re-arms the same variant and so restarts the window.
void ArmFromGameThread(float dropMeters, float horizMeters)
{
    if (!g_on.load(std::memory_order_relaxed) || g_faulted.load(std::memory_order_relaxed)) return;
    const int n = g_variantCount;
    if (n <= 0) return;

    const bool known = dropMeters >= 0.0f;                 // a NaN drop also counts as not available
    const float target = known ? DropToSeconds(dropMeters) * 60.0f : -1.0f;
    int chosen = 0;
    if (known)
    {
        // the variant whose flight is nearest to the target (review change: nearest, not the next larger one)
        float bestD = -1.0f;
        for (int i = 0; i < n; ++i)
        {
            const float d = std::fabs(static_cast<float>(g_variantFlight[i]) - target);
            if (bestD < 0.0f || d < bestD) { chosen = i; bestD = d; }
        }
    }
    if (!PlanNonEmpty(chosen))
    {
        const float ref = known ? target : static_cast<float>(g_variantFlight[chosen]);
        int best = -1;
        float bestD = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            if (!PlanNonEmpty(i)) continue;
            const float d = std::fabs(static_cast<float>(g_variantFlight[i]) - ref);
            if (best < 0 || d < bestD || (d == bestD && g_variantFlight[i] > g_variantFlight[best])) { best = i; bestD = d; }
        }
        if (best < 0) return;                              // nothing built yet: nothing to arm
        chosen = best;
    }

    g_gtRestoredVariant.store(-1, std::memory_order_relaxed);
    g_gtRestoredWritten.store(0, std::memory_order_relaxed);
    g_gtRestoredSkipped.store(0, std::memory_order_relaxed);
    const int prev = g_armedVariant.load(std::memory_order_relaxed);
    if (prev >= 0 && prev != chosen)
    {
        PlanCounts rc{};
        if (!WalkVariant(prev, false, rc))
        {
            g_faults.fetch_add(1);
            g_faulted.store(true);
            g_on.store(false);
            g_armedAtomic.store(false);
            g_gtFaultPending.store(true);
            return;
        }
        g_gtRestoredVariant.store(prev, std::memory_order_relaxed);
        g_gtRestoredWritten.store(rc.written, std::memory_order_relaxed);
        g_gtRestoredSkipped.store(rc.skipped, std::memory_order_relaxed);
    }

    // v0.17: the root templates of the chosen variant are rescaled before its plan is written. Sane values (drop 1..200 m,
    // horizontal 0..60 m) rescale each root. Otherwise, or when a check fails, the root is set back to its template (a
    // failed rescale counts as failed). An exception in the codec is a fault, as in the walks.
    const bool sane = dropMeters >= 1.0f && dropMeters <= 200.0f && horizMeters >= 0.0f && horizMeters <= 60.0f;
    uint32 rootOk = 0, rootFailed = 0, rootCount = 0;
    bool rootFault = false;
    const ArmPlan* cp = g_plans[chosen].load(std::memory_order_acquire);
    if (cp)
    {
        rootCount = static_cast<uint32>(cp->roots.size());
        for (const RootPatch& r : cp->roots)
        {
            bool match = false;
            bool failedHere = false;
            if (sane)
            {
                if (!SafeRootSet(r, dropMeters, horizMeters, true, match)) { rootFault = true; break; }
                if (match) { ++rootOk; continue; }
                failedHere = true;
            }
            if (!SafeRootSet(r, 0.0f, 0.0f, false, match)) { rootFault = true; break; }
            if (!match) failedHere = true;
            if (failedHere) ++rootFailed;
        }
    }
    if (rootFault)
    {
        g_faults.fetch_add(1);
        g_faulted.store(true);
        g_on.store(false);
        g_armedAtomic.store(false);
        g_gtFaultPending.store(true);
        return;
    }
    g_gtRootCount.store(rootCount, std::memory_order_relaxed);
    g_gtRootOk.store(rootOk, std::memory_order_relaxed);
    g_gtRootFailed.store(rootFailed, std::memory_order_relaxed);
    g_gtRootSane.store(sane, std::memory_order_relaxed);
    g_gtRootDrop.store(dropMeters, std::memory_order_relaxed);
    g_gtRootHoriz.store(horizMeters, std::memory_order_relaxed);
    g_gtRootGain.store(cp && !cp->roots.empty() ? cp->roots[0].hg : 1.0f, std::memory_order_relaxed);   // v0.18

    PlanCounts c{};
    if (!WalkVariant(chosen, true, c))
    {
        g_faults.fetch_add(1);
        g_faulted.store(true);
        g_on.store(false);
        g_armedAtomic.store(false);
        g_gtFaultPending.store(true);
        return;
    }
    g_gtWritten.store(c.written, std::memory_order_relaxed);
    g_gtSkipped.store(c.skipped, std::memory_order_relaxed);
    g_gtChosenFrames.store(g_variantFlight[chosen], std::memory_order_relaxed);
    g_gtTarget.store(target, std::memory_order_relaxed);
    g_gtDrop.store(known ? dropMeters : -1.0f, std::memory_order_relaxed);
    g_armedVariant.store(chosen, std::memory_order_release);
    g_armTimeAtomic.store(NowSec(), std::memory_order_relaxed);
    g_armedAtomic.store(true, std::memory_order_release);
    g_gtArmSerial.fetch_add(1, std::memory_order_relaxed);
}

// v0.16: read-only snapshot of the last game-thread arm, for the trajectory recorder (frame side). Atomics only.
struct ArmSnapshot
{
    uint32 serial;      // g_gtArmSerial: number of game-thread arms
    double armTime;     // QPC seconds of that arm (NowSec clock)
    int flight;         // chosen variant flight (frames)
    float drop;         // drop (m), -1 when not available
    float target;       // target flight (frames), -1 when not available
    bool armed;         // the window is open now
};

ArmSnapshot GetArmSnapshot()
{
    ArmSnapshot s{};
    s.serial = g_gtArmSerial.load();
    s.armTime = g_armTimeAtomic.load();
    s.flight = g_gtChosenFrames.load();
    s.drop = g_gtDrop.load();
    s.target = g_gtTarget.load();
    s.armed = g_armedAtomic.load();
    return s;
}

// Frame side of an eject: logs it. The arming itself happened on the game thread.
void LogEject(uint64 count)
{
    const unsigned long long n = static_cast<unsigned long long>(count);
    if (!g_on.load()) { LogLine("origins: eject #%llu seen, feature off: nothing armed", n); return; }
    const size_t slots = TotalPlanSize();
    if (slots == 0) LogLine("origins: eject #%llu seen: no built clip to arm (all plans empty)", n);
    else LogLine("origins: eject #%llu seen (plans: %zu slot(s) in %d variant(s))", n, slots, g_variantCount);
}

// Once a second (player settled). Verifies every built clip, then builds up to kBuildsPerPoll unbuilt entries.
// Nothing is looked up, dropped or built while a variant is armed: a built clip may hold our pointers right now,
// and the disarm walks only the plans it has. The poll simply waits for the window to end.
void Poll()
{
    if (g_armedAtomic.load()) return;
    int builds = 0;
    for (Entry& e : g_entries)
    {
        if (e.dead || !e.parsed) continue;
        if (e.built)
        {
            std::unique_ptr<ACU::StrongRef<ManagedObject>> tmp;
            Animation* p = nullptr;
            int res = kNone;
            if (!SafeLookup(e.handle, tmp, p, res)) { Fault("verify"); return; }
            if (res != kOk) DropEntry(e, "clip gone");
            else if (p != e.anim) DropEntry(e, "clip object changed");
            continue;
        }
        if (builds >= kBuildsPerPoll) continue;            // the rest waits for the next poll
        Animation* p = nullptr;
        int res = kNone;
        if (!SafeLookup(e.handle, e.ref, p, res)) { Fault("lookup"); return; }
        if (res == kNotLoaded)
        {
            if (e.state != "clip not loaded") { e.state = "clip not loaded"; LogLine("origins: [%s f%d] clip not loaded yet", e.name.c_str(), e.flight); }
            continue;
        }
        if (res == kNotAnim)
        {
            e.dead = true;
            e.note = "handle is not an Animation";
            e.state = e.note;
            PublishPlan(e.vi);
            e.ref.reset();
            LogLine("origins: [%s f%d] %s: not used", e.name.c_str(), e.flight, e.note.c_str());
            continue;
        }
        e.anim = p;
        ++builds;
        BuildOut bo{};
        if (!SafeBuild(e, bo)) { Fault("build"); return; }
        if (bo.fail)
        {
            e.dead = true;
            e.note = "not used (see log)";
            e.state = e.note;
            e.swaps.clear();
            PublishPlan(e.vi);
            e.ref.reset();
            e.anim = nullptr;
            continue;
        }
        e.built = true;
        char st[160];
        std::snprintf(st, sizeof st, "built (%d baked, %d retimed, length %.3f -> %.3f s)", bo.rot + bo.trans, bo.retimed,
                      static_cast<double>(e.lengthOrig), static_cast<double>(e.retime ? e.newLength : e.lengthOrig));
        e.state = st;
        PublishPlan(e.vi);
    }
}

// Frame callback (game thread). Runs every frame, menu open or closed.
void Frame(bool playerPresent)
{
    if (!g_anyParsed) return;
    const double now = NowSec();
    if (!g_seenInit) { g_seen = g_eAppended.load(); g_seenInit = true; }
    if (playerPresent) { if (g_playerSince < 0.0) g_playerSince = now; }
    else g_playerSince = -1.0;

    if (g_gtFaultPending.exchange(false))
        LogLine("origins: FAULT on the game thread while arming: feature OFF for this session, no further writes");
    if (g_restoreReq.exchange(false)) RestoreAll();
    if (!g_on.load() && g_armedAtomic.load()) DisarmAll("checkbox off", true);

    const uint64 ej = g_eAppended.load();
    if (ej != g_seen) { g_seen = ej; LogEject(ej); }

    const uint32 serial = g_gtArmSerial.load();
    if (serial != g_loggedArmSerial)
    {
        g_loggedArmSerial = serial;
        const int rv = g_gtRestoredVariant.load();
        if (rv >= 0)
            LogLine("origins: switched from variant f%d: %u slot(s) put back, %u skipped", rv,
                    static_cast<unsigned>(g_gtRestoredWritten.load()), static_cast<unsigned>(g_gtRestoredSkipped.load()));
        const int cf = g_gtChosenFrames.load();
        const double window = static_cast<double>(WindowFor(cf));
        const float drop = g_gtDrop.load();
        if (drop >= 0.0f)
            LogLine("origins: armed variant f%d for drop %.2f m (target %.1f frames): %u slot(s) written, %u skipped (window %.2f s)",
                    cf, static_cast<double>(drop), static_cast<double>(g_gtTarget.load()),
                    static_cast<unsigned>(g_gtWritten.load()), static_cast<unsigned>(g_gtSkipped.load()), window);
        else
            LogLine("origins: armed variant f%d, drop not available (default variant): %u slot(s) written, %u skipped (window %.2f s)",
                    cf, static_cast<unsigned>(g_gtWritten.load()), static_cast<unsigned>(g_gtSkipped.load()), window);
        const uint32 rc = g_gtRootCount.load();
        if (rc > 0 && g_gtRootSane.load())
            LogLine("origins: root scaled for %u clip(s) to drop %.2f m, horiz %.2f m, gain %.2f (failed %u)",
                    static_cast<unsigned>(g_gtRootOk.load()), static_cast<double>(g_gtRootDrop.load()),
                    static_cast<double>(g_gtRootHoriz.load()), static_cast<double>(g_gtRootGain.load()),
                    static_cast<unsigned>(g_gtRootFailed.load()));
        else if (rc > 0)
            LogLine("origins: root template kept for %u clip(s): drop %.2f m, horiz %.2f m not usable (failed %u)",
                    static_cast<unsigned>(rc), static_cast<double>(g_gtRootDrop.load()),
                    static_cast<double>(g_gtRootHoriz.load()), static_cast<unsigned>(g_gtRootFailed.load()));
    }

    const int av = g_armedVariant.load();
    const int af = (av >= 0 && av < g_variantCount) ? g_variantFlight[av] : 0;
    if (g_armedAtomic.load() && now - g_armTimeAtomic.load() >= static_cast<double>(WindowFor(af)))
        DisarmAll("window elapsed", false);

    if (g_on.load() && !g_faulted.load() && playerPresent && g_playerSince >= 0.0
        && now - g_playerSince >= kPlayerSettleSec && now - g_lastPoll >= kPollSec)
    {
        g_lastPoll = now;
        Poll();
    }
}

// Menu: its own collapsible header inside DrawMenu.
void Menu()
{
    if (!ImGui::CollapsingHeader("Origins backflip (v0.26.0)")) return;
    if (!g_anyParsed) { ImGui::TextDisabled("Inactive: no origins\\mapping.json or no clip loaded (see log)"); return; }

    bool on = g_on.load();
    if (ImGui::Checkbox("Origins backflip animation", &on)) g_on.store(on);
    float secs = g_armSeconds.load();
    if (ImGui::SliderFloat("Arm duration (s)", &secs, kArmMinSec, kArmMaxSec, "%.1f")) g_armSeconds.store(secs);

    const bool armed = g_armedAtomic.load();
    const int av = g_armedVariant.load();
    const int af = (av >= 0 && av < g_variantCount) ? g_variantFlight[av] : 0;
    const double now = NowSec();
    const double left = armed ? std::max(0.0, static_cast<double>(WindowFor(af)) - (now - g_armTimeAtomic.load())) : 0.0;
    ImGui::Text("armed now: %s, variant %s, seconds left %.1f", armed ? "yes" : "no",
                armed ? FlightName(av).c_str() : "none", left);
    const float drop = g_gtDrop.load();
    if (drop >= 0.0f)
        ImGui::Text("last arm: drop %.2f m, target %.1f frames, chosen f%d", static_cast<double>(drop),
                    static_cast<double>(g_gtTarget.load()), g_gtChosenFrames.load());
    else
        ImGui::TextDisabled("last arm: none yet, or the drop was not available");
    if (g_gtRootCount.load() > 0)
        ImGui::Text("root template: %u clip(s) rescaled, %u failed%s", static_cast<unsigned>(g_gtRootOk.load()),
                    static_cast<unsigned>(g_gtRootFailed.load()), g_gtRootSane.load() ? "" : " (not rescaled: drop or distance unusable)");
    ImGui::Text("plans: %zu slot(s) and length value(s) in %d variant(s)", TotalPlanSize(), g_variantCount);
    for (int vi = 0; vi < g_variantCount; ++vi)
    {
        int total = 0, built = 0;
        for (const Entry& e : g_entries)
        {
            if (e.vi != vi || !e.parsed) continue;
            ++total;
            if (e.built) ++built;
        }
        ImGui::Text("  f%d: built %d / %d%s", g_variantFlight[vi], built, total,
                    (armed && vi == av) ? " (armed)" : "");
    }
    ImGui::Text("game-thread arms %u (last: %u written, %u skipped); disarms %llu; faults %u",
                static_cast<unsigned>(g_gtArmSerial.load()), static_cast<unsigned>(g_gtWritten.load()),
                static_cast<unsigned>(g_gtSkipped.load()), static_cast<unsigned long long>(g_disarmCount),
                static_cast<unsigned>(g_faults.load()));
    if (g_faulted.load()) ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "FAULTED: feature off for this session (see log)");
    if (ImGui::Button("Restore originals now")) g_restoreReq.store(true);
    for (const Entry& e : g_entries) ImGui::TextDisabled("  %s [f%d]: %s", e.name.c_str(), e.flight, e.state.c_str());
}

} // namespace origins
