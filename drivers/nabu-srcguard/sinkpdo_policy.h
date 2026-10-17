#pragma once

namespace srcguard::sinkpdo {

constexpr unsigned kIoctlAcpiEvalMethodEx = 0x0032C018u;
constexpr unsigned kInputSignature = 0x41696541u;
constexpr unsigned kOutputSignature = 0x426F6541u;
constexpr unsigned kTypeInteger = 0;
constexpr unsigned kTypeBuffer = 2;
constexpr unsigned kTypePackage = 3;
constexpr unsigned kKeySinkPdos = 7;
constexpr unsigned kFromPdo = 0x0002D0C8u;
constexpr unsigned kToPdo = 0x0002D12Cu;
constexpr unsigned kMaxLoggedPdos = 4;

constexpr char kMethodFull[] = "\\_SB.UCP0.CON0._DSD";
constexpr char kMethodRelative[] = "CON0._DSD";

constexpr unsigned char kConnectorGuid[16] = {
    0x62, 0x6E, 0x85, 0x6B, 0xF4, 0x40, 0x88, 0x46, 0xBD, 0x46, 0x5E, 0x88, 0x8A, 0x22, 0x60, 0xDE};

using Size = decltype(sizeof(0));

enum class Outcome : unsigned {
    Patched = 0,
    AlreadyTarget = 1,
    NoMatch = 2,
    NoKey = 3,
    NoGuid = 4,
    Ambiguous = 5,
    Malformed = 6,
    NoReply = 7,
};

struct Result {
    Outcome Status;
    unsigned PdoCount;
    unsigned Pdos[kMaxLoggedPdos];
    Size PatchOffset;
};

inline unsigned Rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }
inline unsigned Rd32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<unsigned>(p[3]) << 24);
}
inline void Wr32(unsigned char* p, unsigned v) {
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
    p[2] = static_cast<unsigned char>(v >> 16);
    p[3] = static_cast<unsigned char>(v >> 24);
}

inline bool NameIs(const unsigned char* p, Size len, const char* name, Size nameLen) {
    if (len < 4 + nameLen) return false;
    for (Size i = 0; i < nameLen; ++i) {
        if (p[4 + i] != static_cast<unsigned char>(name[i])) return false;
    }
    return true;
}

inline bool IsConnectorDsdRequest(const void* in, Size len) {
    const auto* p = static_cast<const unsigned char*>(in);
    if (!p || len < 4 || Rd32(p) != kInputSignature) return false;
    return NameIs(p, len, kMethodFull, sizeof(kMethodFull)) ||
           NameIs(p, len, kMethodRelative, sizeof(kMethodRelative));
}

namespace detail {

struct Arg {
    unsigned Type;
    Size DataOffset;
    Size DataLength;
    Size Next;
};

inline bool ReadArg(const unsigned char* b, Size at, Size end, Arg* a) {
    if (at > end || end - at < 4) return false;
    a->Type = Rd16(b + at);
    a->DataLength = Rd16(b + at + 2);
    a->DataOffset = at + 4;
    const Size span = a->DataLength < 4 ? 4 : a->DataLength;
    if (end - a->DataOffset < span) return false;
    a->Next = a->DataOffset + span;
    return true;
}

inline bool ReadInt(const unsigned char* b, const Arg& a, unsigned* v) {
    if (a.Type != kTypeInteger || (a.DataLength != 4 && a.DataLength != 8)) return false;
    if (a.DataLength == 8 && Rd32(b + a.DataOffset + 4) != 0) return false;
    *v = Rd32(b + a.DataOffset);
    return true;
}

inline Outcome ScanPdos(const unsigned char* b, const Arg& value, Result* r) {
    if (value.Type != kTypePackage) return Outcome::Malformed;
    const Size end = value.DataOffset + value.DataLength;
    unsigned matches = 0;
    bool target = false;
    for (Size at = value.DataOffset; at < end;) {
        Arg e;
        unsigned pdo = 0;
        if (!ReadArg(b, at, end, &e) || !ReadInt(b, e, &pdo)) return Outcome::Malformed;
        if (r->PdoCount < kMaxLoggedPdos) r->Pdos[r->PdoCount] = pdo;
        ++r->PdoCount;
        if (pdo == kFromPdo) { ++matches; r->PatchOffset = e.DataOffset; }
        if (pdo == kToPdo) target = true;
        at = e.Next;
    }
    if (matches > 1) return Outcome::Ambiguous;
    if (matches == 1) return target ? Outcome::Ambiguous : Outcome::Patched;
    return target ? Outcome::AlreadyTarget : Outcome::NoMatch;
}

inline Outcome ScanProperties(const unsigned char* b, const Arg& props, Result* r) {
    if (props.Type != kTypePackage) return Outcome::Malformed;
    const Size end = props.DataOffset + props.DataLength;
    bool seen = false;
    Outcome found = Outcome::NoKey;
    for (Size at = props.DataOffset; at < end;) {
        Arg pair, key, value;
        unsigned k = 0;
        if (!ReadArg(b, at, end, &pair) || pair.Type != kTypePackage) return Outcome::Malformed;
        const Size pairEnd = pair.DataOffset + pair.DataLength;
        if (!ReadArg(b, pair.DataOffset, pairEnd, &key) || !ReadInt(b, key, &k)) return Outcome::Malformed;
        if (!ReadArg(b, key.Next, pairEnd, &value) || value.Next != pairEnd) return Outcome::Malformed;
        if (k == kKeySinkPdos) {
            if (seen) return Outcome::Ambiguous;
            seen = true;
            found = ScanPdos(b, value, r);
            if (found == Outcome::Malformed) return found;
        }
        at = pair.Next;
    }
    return found;
}

}

inline Result PatchSinkPdos(unsigned char* buffer, Size len, bool apply) {
    using namespace detail;
    Result r{};
    r.Status = Outcome::Malformed;
    if (!buffer || len < 12 || Rd32(buffer) != kOutputSignature) return r;
    const Size declared = Rd32(buffer + 4);
    if (declared < 12) return r;
    const Size end = declared < len ? declared : len;
    const unsigned count = Rd32(buffer + 8);
    if (count > end / 8) return r;

    bool guidSeen = false;
    bool afterGuid = false;
    Outcome found = Outcome::NoGuid;
    Result best{};
    Size at = 12;
    for (unsigned i = 0; i < count; ++i) {
        Arg a;
        if (!ReadArg(buffer, at, end, &a)) return r;
        if ((i & 1) == 0) {
            if (a.Type != kTypeBuffer) return r;
            afterGuid = false;
            if (a.DataLength == 16) {
                bool same = true;
                for (unsigned j = 0; j < 16; ++j) same = same && buffer[a.DataOffset + j] == kConnectorGuid[j];
                if (same) {
                    if (guidSeen) { r.Status = Outcome::Ambiguous; return r; }
                    guidSeen = afterGuid = true;
                }
            }
        } else if (afterGuid) {
            Result section{};
            found = ScanProperties(buffer, a, &section);
            if (found == Outcome::Malformed) return r;
            best = section;
        } else if (a.Type != kTypePackage) {
            return r;
        }
        at = a.Next;
    }
    if (count & 1) return r;

    r = best;
    r.Status = found;
    if (r.Status != Outcome::Patched) r.PatchOffset = 0;
    if (apply && r.Status == Outcome::Patched && Rd32(buffer + r.PatchOffset) == kFromPdo) {
        Wr32(buffer + r.PatchOffset, kToPdo);
    }
    return r;
}

}
