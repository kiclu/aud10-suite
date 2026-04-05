#pragma once

#include "constants.hpp"
#include <array>
#include <string>
#include <vector>

struct CompressorSlotParams {
    bool enabled      = true;
    int  threshold    = -20;
    int  ratio_idx    = 3;
    int  attack_idx   = 5;
    int  release_idx  = 4;
    int  knee_idx     = 4;
    int  makeup       = 0;
};

struct EqSlotParams {
    bool enabled   = true;
    int  low_db    = 0;
    int  mid_db    = 0;
    int  high_db   = 0;
};

struct ReverbSlotParams {
    bool enabled = true;
    int  room    = 35;
    int  damp    = 50;
    int  wet     = 25;
};

struct DelaySlotParams {
    bool enabled  = true;
    int  time_ms  = 250;
    int  feedback = 25;
    int  wet      = 35;
};

struct DeEsserSlotParams {
    bool enabled      = true;
    int  freq_idx     = 2;
    int  threshold    = -22;
    int  amount       = 45;
    int  attack_idx   = 4;
    int  release_idx  = 4;
};

struct PeqSlotParams {
    bool enabled   = true;
    int  freq_idx  = 5;
    int  q_idx     = 2;
    int  gain_db   = 0;
};

struct HiloSlotParams {
    bool hpf_enabled = false;
    int  hpf_hz_idx  = 2;
    bool lpf_enabled = false;
    int  lpf_hz_idx  = 2;
};

struct PitchCorrectSlotParams {
    bool enabled   = true;
    int  wet       = 70;
    int  speed     = 45;
    int  key_root  = 0;
    int  scale_idx = 0;
    int  pull      = 40;
    /** Shorter buffers / faster pitch updates / tighter read lag — rougher, may glitch on low notes. */
    bool low_latency_x = false;
    /** Indices into PCOR_EXP_* tables (used only when low_latency_x). Defaults = prior fixed “exp” preset. */
    int exp_yin_i = 0;
    int exp_det_i = 2;
    int exp_warm_i = 1;
    int exp_gate_i = 1;
    int exp_lmin_i = 1;
    int exp_lmax_i = 3;
};

struct SaturatorSlotParams {
    bool enabled = true;
    int  drive   = 35; // 0–100 → gain into tanh
    int  mix     = 100; // 0–100 wet (shaped signal)
};

struct PitchShiftSlotParams {
    bool enabled = true;
    /** 0..PS_SEMI_STEPS-1 → semitones relative to PS_SEMI_CENTER (default = 0 st). */
    int semi_idx = PS_SEMI_CENTER;
    int wet      = 100;
};

struct Config {
    int  channel_idx     = 0;
    int  buffer_idx      = 2;
    int  vol_in          = 0;
    int  vol_out         = 0;
    std::string in_port_l, in_port_r;
    std::string out_port_l, out_port_r;
    bool comp_enabled    = true;
    int  comp_threshold  = -20;
    int  comp_ratio_idx  = 3;
    int  comp_attack_idx = 5;
    int  comp_release_idx= 4;
    int  comp_knee_idx   = 4;
    int  comp_makeup     = 0;
    std::array<int, CHAIN_SLOTS> chain_slot{};
    std::array<CompressorSlotParams, CHAIN_SLOTS> slot_comp{};
    std::array<EqSlotParams, CHAIN_SLOTS> slot_eq{};
    std::array<ReverbSlotParams, CHAIN_SLOTS> slot_rv{};
    std::array<DelaySlotParams, CHAIN_SLOTS> slot_dl{};
    std::array<DeEsserSlotParams, CHAIN_SLOTS> slot_dss{};
    std::array<PeqSlotParams, CHAIN_SLOTS> slot_peq{};
    std::array<HiloSlotParams, CHAIN_SLOTS> slot_hilo{};
    std::array<PitchCorrectSlotParams, CHAIN_SLOTS> slot_pc{};
    std::array<SaturatorSlotParams, CHAIN_SLOTS> slot_sat{};
    std::array<PitchShiftSlotParams, CHAIN_SLOTS> slot_ps{};
};

std::string config_path();
void save_config(const std::string &path, const Config &c);
Config load_config(const std::string &path);

/** Saved under <config_dir>/profiles/<slug>.conf — chain + all per-slot effect parameters. */
std::string profiles_directory();
std::string sanitize_profile_slug(std::string name);
bool save_effect_chain_profile(const std::string &slug, const Config &c);
/** Merges file over a copy of @a c (so omitted keys keep previous values), then copies chain data back. */
bool load_effect_chain_profile(const std::string &path, Config &c);
std::vector<std::string> list_profile_slugs();
bool delete_effect_chain_profile(const std::string &slug);
void copy_effect_chain(const Config &src, Config &dst);
