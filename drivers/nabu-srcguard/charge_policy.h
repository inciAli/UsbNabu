#pragma once

namespace srcguard::charge {

constexpr unsigned kCurrentStepMa = 50;
constexpr unsigned kFloatBaseMv = 3600;
constexpr unsigned kFloatStepMv = 10;

constexpr unsigned char Encode(unsigned value) { return static_cast<unsigned char>(value * 232u); }

constexpr unsigned Preimage(unsigned char byte) { return ((byte >> 3) * 21u) & 31u; }
constexpr unsigned char Reachable(unsigned code) {
    return static_cast<unsigned char>((code > 0xF8 ? 0xF8 : code) & ~7u);
}

constexpr unsigned char CurrentByte(unsigned ma) { return Reachable(ma / kCurrentStepMa); }
constexpr unsigned char FloatByte(unsigned mv) {
    return mv <= kFloatBaseMv ? 0 : Reachable((mv - kFloatBaseMv) / kFloatStepMv);
}

constexpr unsigned kFccMaxByte = 0xA0;
constexpr unsigned kIclMaxByte = 0x64;
constexpr unsigned kFloatMaxByte = 0x77;
constexpr unsigned char ExactCurrentByte(unsigned ma, unsigned maxByte) {
    return static_cast<unsigned char>(ma / kCurrentStepMa > maxByte ? maxByte : ma / kCurrentStepMa);
}
constexpr unsigned char ExactFloatByte(unsigned mv) {
    return mv <= kFloatBaseMv ? 0
         : static_cast<unsigned char>((mv - kFloatBaseMv) / kFloatStepMv > kFloatMaxByte
                                      ? kFloatMaxByte : (mv - kFloatBaseMv) / kFloatStepMv);
}

constexpr unsigned kRegFcc = 0x1061;
constexpr unsigned kRegFloat = 0x1070;
constexpr unsigned kRegIcl = 0x1370;

enum class Owner : unsigned { Manager = 0, Contract = 1, Exact = 2 };
constexpr unsigned kOwnerDefault = 2;

struct Zone { int MinDeciC; int MaxDeciC; unsigned FccMa; unsigned FloatMv; };
constexpr Zone kZones[] = {
    {-100,   0,  820, 4450},
    {   1,  50,  820, 4450},
    {  51, 100, 2500, 4450},
    { 101, 150, 5964, 4450},
    { 151, 480, 5900, 4480},
    { 481, 590, 4260, 4100},
};
constexpr int kTooColdDeciC = -100;
constexpr int kTooHotDeciC = 590;

struct Limits {
    unsigned FccCapMa = 4800;
    unsigned IclCapMa = 3000;
    unsigned FloatCapMv = 4450;
    unsigned IclMinForHcMa = 800;
};

struct Inputs {
    bool TempValid;
    int TempDeciC;
    bool SinkAttached;
    unsigned IclRequestMaxMa;
    bool ContractActive;
    unsigned ContractMa;
    unsigned OwnerMode;
};

struct Decision {
    bool Active;
    bool Stop;
    unsigned char Fcc;
    unsigned char Float;
    unsigned char Icl;
    int ZoneIndex;
    bool IclFromContract;
    bool Exact;
    unsigned char FccExact;
    unsigned char FloatExact;
    unsigned char IclExact;
};

constexpr unsigned Min(unsigned a, unsigned b) { return a < b ? a : b; }

constexpr int ZoneFor(int deciC) {
    for (int i = 0; i < static_cast<int>(sizeof(kZones) / sizeof(kZones[0])); ++i) {
        if (deciC >= kZones[i].MinDeciC && deciC <= kZones[i].MaxDeciC) return i;
    }
    return -1;
}

inline Decision Decide(const Inputs& in, const Limits& lim) {
    Decision d{};
    d.ZoneIndex = -1;
    d.Exact = in.OwnerMode >= static_cast<unsigned>(Owner::Exact);
    d.Float = FloatByte(lim.FloatCapMv);
    d.FloatExact = ExactFloatByte(lim.FloatCapMv);
    if (!in.TempValid) return d;
    d.ZoneIndex = ZoneFor(in.TempDeciC);
    if (d.ZoneIndex < 0 || in.TempDeciC < kTooColdDeciC || in.TempDeciC > kTooHotDeciC) {
        d.Stop = true;
        d.Fcc = 0;
        return d;
    }
    const Zone& z = kZones[d.ZoneIndex];
    const unsigned floatMv = Min(z.FloatMv, lim.FloatCapMv);
    const unsigned fccMa = Min(z.FccMa, lim.FccCapMa);
    d.Float = FloatByte(floatMv);
    d.FloatExact = ExactFloatByte(floatMv);
    d.Fcc = CurrentByte(fccMa);
    d.FccExact = ExactCurrentByte(fccMa, kFccMaxByte);
    const bool fromContract = in.OwnerMode >= static_cast<unsigned>(Owner::Contract) && in.ContractActive;
    const unsigned inputMa = fromContract ? in.ContractMa : in.SinkAttached ? in.IclRequestMaxMa : 0;
    if (inputMa >= lim.IclMinForHcMa) {
        d.Active = true;
        d.IclFromContract = fromContract;
        d.Icl = CurrentByte(Min(inputMa, lim.IclCapMa));
        d.IclExact = ExactCurrentByte(Min(inputMa, lim.IclCapMa), kIclMaxByte);
    }
    return d;
}

constexpr bool SameDecision(const Decision& a, const Decision& b) {
    return a.Active == b.Active && a.Stop == b.Stop && a.Fcc == b.Fcc && a.Float == b.Float && a.Icl == b.Icl &&
           a.IclFromContract == b.IclFromContract && a.Exact == b.Exact && a.FccExact == b.FccExact &&
           a.FloatExact == b.FloatExact && a.IclExact == b.IclExact;
}

struct Write { unsigned Addr; unsigned char Value; };
struct Writes { Write Items[3]; unsigned Count; };
inline Writes ExactWrites(const Decision& d) {
    Writes w{};
    if (!d.Exact) return w;
    w.Items[w.Count++] = Write{kRegFloat, d.FloatExact};
    if (d.Stop) w.Items[w.Count++] = Write{kRegFcc, 0};
    if (d.Active) {
        w.Items[w.Count++] = Write{kRegFcc, d.FccExact};
        w.Items[w.Count++] = Write{kRegIcl, d.IclExact};
    }
    return w;
}

inline bool RewriteChargeParams(unsigned* p, const Decision& d) {
    const unsigned before[4] = {p[0], p[1], p[2], p[3]};
    p[2] = Preimage(d.Float);
    if (d.Stop) p[1] = Preimage(0);
    if (d.Active) {
        p[1] = Preimage(d.Fcc);

        if (p[3] != 0 || d.IclFromContract) p[3] = Preimage(d.Icl);
    }
    return before[1] != p[1] || before[2] != p[2] || before[3] != p[3];
}

inline bool RewriteIclMode(unsigned* p, const Decision& d) {
    if (!d.Active) return false;
    const bool changed = p[1] != 0 || p[3] != 1;
    p[1] = 0;
    p[3] = 1;
    return changed;
}

}
