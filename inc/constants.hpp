#pragma once

#include <cstddef>

constexpr int CHAIN_SLOTS = 16;

constexpr int FX_NONE = 0;
constexpr int FX_COMP   = 1;
constexpr int FX_EQ     = 2;
constexpr int FX_REV    = 3;
constexpr int FX_DLY    = 4;
constexpr int FX_DESS   = 5;
constexpr int FX_PEQ    = 6;
constexpr int FX_HILO   = 7;
constexpr int FX_PCOR   = 8;
constexpr int FX_SAT    = 9;
constexpr int FX_PSHIFT = 10;

enum Field : int {
    F_RATE, F_CHAN, F_BUF, F_VOL_IN, F_VOL_OUT,
    F_IN_L, F_IN_R, F_OUT_L, F_OUT_R,
    F_CHAIN_0,
    F_START = F_CHAIN_0 + CHAIN_SLOTS,
    F_QUIT,
    F_COUNT
};

inline constexpr int field_chain_last_int() {
    return static_cast<int>(F_CHAIN_0) + CHAIN_SLOTS - 1;
}

inline constexpr bool field_is_chain_slot(int f) {
    return f >= static_cast<int>(F_CHAIN_0) && f <= field_chain_last_int();
}

enum SlotEditField {
    SE_TYPE = 0,
    SE_C_EN, SE_C_THRESH, SE_C_RATIO, SE_C_ATTACK, SE_C_RELEASE, SE_C_KNEE, SE_C_MAKEUP,
    SE_EQ_EN, SE_EQ_LOW, SE_EQ_MID, SE_EQ_HIGH,
    SE_RV_EN, SE_RV_ROOM, SE_RV_DAMP, SE_RV_WET,
    SE_DL_EN, SE_DL_TIME, SE_DL_FB, SE_DL_WET,
    SE_DS_EN, SE_DS_FREQ, SE_DS_THRESH, SE_DS_AMT, SE_DS_ATTACK, SE_DS_RELEASE,
    SE_PQ_EN, SE_PQ_FREQ, SE_PQ_Q, SE_PQ_GAIN,
    SE_HL_HPEN, SE_HL_HPF, SE_HL_LPEN, SE_HL_LPF,
    SE_PC_EN, SE_PC_WET, SE_PC_SPD, SE_PC_KEY, SE_PC_SCALE, SE_PC_PULL, SE_PC_LOWLAT_X,
    SE_PC_EXP_YIN, SE_PC_EXP_DET, SE_PC_EXP_WARM, SE_PC_EXP_GATE, SE_PC_EXP_LMIN,
    SE_PC_EXP_LMAX,
    SE_SAT_EN, SE_SAT_DRIVE, SE_SAT_MIX,
    SE_PS_EN, SE_PS_SEMI, SE_PS_WET,
    SE_BACK,
    SE_COUNT
};

template <typename T, std::size_t N>
constexpr int alen(const T (&)[N]) { return static_cast<int>(N); }

inline int wrap(int v, int d, int n) { return ((v + d) % n + n) % n; }

inline void nav_main_ud(int &cur, int d) {
    constexpr int lo = F_RATE;
    constexpr int hi = field_chain_last_int();
    if (cur == F_START || cur == F_QUIT) {
        if (d < 0)
            cur = hi;
        else
            cur = lo;
        return;
    }
    if (d < 0) {
        if (cur <= lo)
            cur = hi;
        else
            cur = cur - 1;
    } else {
        if (cur >= hi)
            cur = F_START;
        else
            cur = cur + 1;
    }
}

inline void se_advance(SlotEditField &c, int fx, bool empty, int dir,
                       bool pcor_exp_layers = false) {
    static const SlotEditField T_EMPTY[] = { SE_TYPE, SE_BACK };
    static const SlotEditField T_COMP[] = {
        SE_TYPE, SE_C_EN, SE_C_THRESH, SE_C_RATIO, SE_C_ATTACK, SE_C_RELEASE,
        SE_C_KNEE, SE_C_MAKEUP, SE_BACK};
    static const SlotEditField T_EQ[] = {
        SE_TYPE, SE_EQ_EN, SE_EQ_LOW, SE_EQ_MID, SE_EQ_HIGH, SE_BACK};
    static const SlotEditField T_RV[] = {
        SE_TYPE, SE_RV_EN, SE_RV_ROOM, SE_RV_DAMP, SE_RV_WET, SE_BACK};
    static const SlotEditField T_DL[] = {
        SE_TYPE, SE_DL_EN, SE_DL_TIME, SE_DL_FB, SE_DL_WET, SE_BACK};
    static const SlotEditField T_DS[] = {
        SE_TYPE, SE_DS_EN, SE_DS_FREQ, SE_DS_THRESH, SE_DS_AMT, SE_DS_ATTACK,
        SE_DS_RELEASE, SE_BACK};
    static const SlotEditField T_PQ[] = {
        SE_TYPE, SE_PQ_EN, SE_PQ_FREQ, SE_PQ_Q, SE_PQ_GAIN, SE_BACK};
    static const SlotEditField T_HL[] = {
        SE_TYPE, SE_HL_HPEN, SE_HL_HPF, SE_HL_LPEN, SE_HL_LPF, SE_BACK};
    static const SlotEditField T_PC[] = {
        SE_TYPE, SE_PC_EN, SE_PC_WET, SE_PC_SPD, SE_PC_KEY, SE_PC_SCALE, SE_PC_PULL,
        SE_PC_LOWLAT_X, SE_BACK};
    static const SlotEditField T_PC_EXP[] = {
        SE_TYPE, SE_PC_EN, SE_PC_WET, SE_PC_SPD, SE_PC_KEY, SE_PC_SCALE, SE_PC_PULL,
        SE_PC_LOWLAT_X, SE_PC_EXP_YIN, SE_PC_EXP_DET, SE_PC_EXP_WARM, SE_PC_EXP_GATE,
        SE_PC_EXP_LMIN, SE_PC_EXP_LMAX, SE_BACK};
    static const SlotEditField T_SAT[] = {
        SE_TYPE, SE_SAT_EN, SE_SAT_DRIVE, SE_SAT_MIX, SE_BACK};
    static const SlotEditField T_PS[] = {
        SE_TYPE, SE_PS_EN, SE_PS_SEMI, SE_PS_WET, SE_BACK};

    const SlotEditField *tab = T_EMPTY;
    int n = 2;
    if (!empty) {
        if (fx == FX_COMP) { tab = T_COMP; n = 9; }
        else if (fx == FX_EQ) { tab = T_EQ; n = 6; }
        else if (fx == FX_REV) { tab = T_RV; n = 6; }
        else if (fx == FX_DLY) { tab = T_DL; n = 6; }
        else if (fx == FX_DESS) { tab = T_DS; n = 8; }
        else if (fx == FX_PEQ) { tab = T_PQ; n = 6; }
        else if (fx == FX_HILO) { tab = T_HL; n = 6; }
        else if (fx == FX_PCOR) {
            tab = pcor_exp_layers ? T_PC_EXP : T_PC;
            n   = pcor_exp_layers ? 15 : 9;
        }
        else if (fx == FX_SAT) { tab = T_SAT; n = 5; }
        else if (fx == FX_PSHIFT) { tab = T_PS; n = 5; }
    }
    int i = 0;
    for (; i < n; i++)
        if (tab[i] == c) break;
    if (i >= n) i = 0;
    i = (i + dir + n * 64) % n;
    c = tab[i];
}

extern const unsigned CHANNEL_OPTS[2];
extern const char *const CHANNEL_NAMES[2];
extern const unsigned JACK_BUFSIZES[7];
extern const float COMP_RATIOS[9];
extern const float COMP_ATTACKS[9];
extern const float COMP_RELEASES[9];
extern const float COMP_KNEES[7];
constexpr int SLOT_TYPES = 11;
extern const char *const SLOT_TYPE_NAMES[SLOT_TYPES];
/** Pitch shift: semi_idx maps 0..PS_SEMI_STEPS-1 → (idx - PS_SEMI_CENTER) semitones. */
constexpr int PS_SEMI_STEPS  = 25;
constexpr int PS_SEMI_CENTER = 12;
constexpr int PCOR_SCALES = 3;
extern const char *const PCOR_SCALE_NAMES[PCOR_SCALES];
/** Pitch-correct experimental mode: discrete knob values (indices in PitchCorrectSlotParams). */
constexpr int PCOR_EXP_YIN_N      = 3;
constexpr int PCOR_EXP_DETECT_N   = 7;
constexpr int PCOR_EXP_WARMUP_N   = 5;
constexpr int PCOR_EXP_YIN_GATE_N  = 7;
constexpr int PCOR_EXP_LAG_MIN_N   = 5;
constexpr int PCOR_EXP_LAG_MAX_N   = 7;
extern const unsigned PCOR_EXP_YIN_SAMPLES[PCOR_EXP_YIN_N];
extern const unsigned PCOR_EXP_DETECT_PER[PCOR_EXP_DETECT_N];
extern const unsigned PCOR_EXP_WARMUP_SMPS[PCOR_EXP_WARMUP_N];
extern const unsigned PCOR_EXP_YIN_GATE_SMPS[PCOR_EXP_YIN_GATE_N];
extern const unsigned PCOR_EXP_LAG_MIN_SMPS[PCOR_EXP_LAG_MIN_N];
extern const unsigned PCOR_EXP_LAG_MAX_SMPS[PCOR_EXP_LAG_MAX_N];
constexpr int NOTE_NAMES_N = 12;
extern const char *const NOTE_NAMES[NOTE_NAMES_N];
extern const float DEESS_FREQ_HZ[6];
extern const float PEQ_FREQ_HZ[10];
extern const float PEQ_Q_VALS[6];
extern const float HPF_FREQ_HZ[7];
extern const float LPF_FREQ_HZ[7];
