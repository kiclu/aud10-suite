#include "config.hpp"
#include "constants.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

std::string config_path() {
    const char *xdg  = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    std::string dir;
    if (xdg && xdg[0])
        dir = std::string(xdg) + "/aud10-suite";
    else if (home && home[0])
        dir = std::string(home) + "/.config/aud10-suite";
    else
        return "./aud10-suite.conf";
    mkdir(dir.c_str(), 0755);
    return dir + "/aud10-suite.conf";
}

std::string profiles_directory() {
    std::string base = config_path();
    std::size_t slash = base.find_last_of('/');
    std::string dir =
        (slash == std::string::npos) ? std::string("profiles")
                                     : base.substr(0, slash + 1) + "profiles";
    mkdir(dir.c_str(), 0755);
    return dir;
}

std::string sanitize_profile_slug(std::string s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char uc : s) {
        char c = static_cast<char>(uc);
        if (std::isalnum(uc) || c == '_' || c == '.' || c == '-')
            o += c;
        else if (c == ' ')
            o += '_';
    }
    if (o.size() > 64)
        o.resize(64);
    if (o.empty())
        o = "untitled";
    return o;
}

static void fwrite_effect_chain(FILE *f, const Config &c) {
    for (int i = 0; i < CHAIN_SLOTS; i++)
        std::fprintf(f, "chain_%d=%d\n", i, c.chain_slot[static_cast<std::size_t>(i)]);
    for (int i = 0; i < CHAIN_SLOTS; i++) {
        const auto &p = c.slot_comp[static_cast<std::size_t>(i)];
        std::fprintf(f, "slot_%d_enabled=%d\n", i, p.enabled ? 1 : 0);
        std::fprintf(f, "slot_%d_threshold=%d\n", i, p.threshold);
        std::fprintf(f, "slot_%d_ratio_idx=%d\n", i, p.ratio_idx);
        std::fprintf(f, "slot_%d_attack_idx=%d\n", i, p.attack_idx);
        std::fprintf(f, "slot_%d_release_idx=%d\n", i, p.release_idx);
        std::fprintf(f, "slot_%d_knee_idx=%d\n", i, p.knee_idx);
        std::fprintf(f, "slot_%d_makeup=%d\n", i, p.makeup);
        const auto &e = c.slot_eq[static_cast<std::size_t>(i)];
        std::fprintf(f, "slot_%d_eq_enabled=%d\n", i, e.enabled ? 1 : 0);
        std::fprintf(f, "slot_%d_eq_low_db=%d\n", i, e.low_db);
        std::fprintf(f, "slot_%d_eq_mid_db=%d\n", i, e.mid_db);
        std::fprintf(f, "slot_%d_eq_high_db=%d\n", i, e.high_db);
        const auto &r = c.slot_rv[static_cast<std::size_t>(i)];
        std::fprintf(f, "slot_%d_rv_enabled=%d\n", i, r.enabled ? 1 : 0);
        std::fprintf(f, "slot_%d_rv_room=%d\n", i, r.room);
        std::fprintf(f, "slot_%d_rv_damp=%d\n", i, r.damp);
        std::fprintf(f, "slot_%d_rv_wet=%d\n", i, r.wet);
        const auto &d = c.slot_dl[static_cast<std::size_t>(i)];
        std::fprintf(f, "slot_%d_dl_enabled=%d\n", i, d.enabled ? 1 : 0);
        std::fprintf(f, "slot_%d_dl_time_ms=%d\n", i, d.time_ms);
        std::fprintf(f, "slot_%d_dl_feedback=%d\n", i, d.feedback);
        std::fprintf(f, "slot_%d_dl_wet=%d\n", i, d.wet);
        const auto &ds = c.slot_dss[static_cast<std::size_t>(i)];
        std::fprintf(f, "slot_%d_dss_enabled=%d\n", i, ds.enabled ? 1 : 0);
        std::fprintf(f, "slot_%d_dss_freq_idx=%d\n", i, ds.freq_idx);
        std::fprintf(f, "slot_%d_dss_threshold=%d\n", i, ds.threshold);
        std::fprintf(f, "slot_%d_dss_amount=%d\n", i, ds.amount);
        std::fprintf(f, "slot_%d_dss_attack_idx=%d\n", i, ds.attack_idx);
        std::fprintf(f, "slot_%d_dss_release_idx=%d\n", i, ds.release_idx);
        const auto &pq = c.slot_peq[static_cast<std::size_t>(i)];
        std::fprintf(f, "slot_%d_peq_enabled=%d\n", i, pq.enabled ? 1 : 0);
        std::fprintf(f, "slot_%d_peq_freq_idx=%d\n", i, pq.freq_idx);
        std::fprintf(f, "slot_%d_peq_q_idx=%d\n", i, pq.q_idx);
        std::fprintf(f, "slot_%d_peq_gain_db=%d\n", i, pq.gain_db);
        const auto &hl = c.slot_hilo[static_cast<std::size_t>(i)];
        std::fprintf(f, "slot_%d_hl_hpf_en=%d\n", i, hl.hpf_enabled ? 1 : 0);
        std::fprintf(f, "slot_%d_hl_hpf_idx=%d\n", i, hl.hpf_hz_idx);
        std::fprintf(f, "slot_%d_hl_lpf_en=%d\n", i, hl.lpf_enabled ? 1 : 0);
        std::fprintf(f, "slot_%d_hl_lpf_idx=%d\n", i, hl.lpf_hz_idx);
        const auto &pc = c.slot_pc[static_cast<std::size_t>(i)];
        std::fprintf(f, "slot_%d_pc_enabled=%d\n", i, pc.enabled ? 1 : 0);
        std::fprintf(f, "slot_%d_pc_wet=%d\n", i, pc.wet);
        std::fprintf(f, "slot_%d_pc_speed=%d\n", i, pc.speed);
        std::fprintf(f, "slot_%d_pc_key=%d\n", i, pc.key_root);
        std::fprintf(f, "slot_%d_pc_scale=%d\n", i, pc.scale_idx);
        std::fprintf(f, "slot_%d_pc_pull=%d\n", i, pc.pull);
        std::fprintf(f, "slot_%d_pc_lowlat_x=%d\n", i, pc.low_latency_x ? 1 : 0);
        std::fprintf(f, "slot_%d_pc_exp_yin_i=%d\n", i, pc.exp_yin_i);
        std::fprintf(f, "slot_%d_pc_exp_det_i=%d\n", i, pc.exp_det_i);
        std::fprintf(f, "slot_%d_pc_exp_warm_i=%d\n", i, pc.exp_warm_i);
        std::fprintf(f, "slot_%d_pc_exp_gate_i=%d\n", i, pc.exp_gate_i);
        std::fprintf(f, "slot_%d_pc_exp_lmin_i=%d\n", i, pc.exp_lmin_i);
        std::fprintf(f, "slot_%d_pc_exp_lmax_i=%d\n", i, pc.exp_lmax_i);
        const auto &st = c.slot_sat[static_cast<std::size_t>(i)];
        std::fprintf(f, "slot_%d_sat_enabled=%d\n", i, st.enabled ? 1 : 0);
        std::fprintf(f, "slot_%d_sat_drive=%d\n", i, st.drive);
        std::fprintf(f, "slot_%d_sat_mix=%d\n", i, st.mix);
        const auto &ps = c.slot_ps[static_cast<std::size_t>(i)];
        std::fprintf(f, "slot_%d_ps_enabled=%d\n", i, ps.enabled ? 1 : 0);
        std::fprintf(f, "slot_%d_ps_semi_idx=%d\n", i, ps.semi_idx);
        std::fprintf(f, "slot_%d_ps_wet=%d\n", i, ps.wet);
    }
}

static void clamp_effect_chain_config(Config &c) {
    for (int i = 0; i < CHAIN_SLOTS; i++)
        c.chain_slot[static_cast<std::size_t>(i)] =
            std::clamp(c.chain_slot[static_cast<std::size_t>(i)], 0, SLOT_TYPES - 1);
    for (int i = 0; i < CHAIN_SLOTS; i++) {
        auto &p = c.slot_comp[static_cast<std::size_t>(i)];
        p.threshold   = std::clamp(p.threshold, -60, 0);
        p.ratio_idx   = std::clamp(p.ratio_idx, 0, alen(COMP_RATIOS) - 1);
        p.attack_idx  = std::clamp(p.attack_idx, 0, alen(COMP_ATTACKS) - 1);
        p.release_idx = std::clamp(p.release_idx, 0, alen(COMP_RELEASES) - 1);
        p.knee_idx    = std::clamp(p.knee_idx, 0, alen(COMP_KNEES) - 1);
        p.makeup      = std::clamp(p.makeup, -12, 36);
        auto &e = c.slot_eq[static_cast<std::size_t>(i)];
        e.low_db  = std::clamp(e.low_db, -12, 12);
        e.mid_db  = std::clamp(e.mid_db, -12, 12);
        e.high_db = std::clamp(e.high_db, -12, 12);
        auto &r = c.slot_rv[static_cast<std::size_t>(i)];
        r.room = std::clamp(r.room, 0, 100);
        r.damp = std::clamp(r.damp, 0, 100);
        r.wet  = std::clamp(r.wet, 0, 100);
        auto &d = c.slot_dl[static_cast<std::size_t>(i)];
        d.time_ms  = std::clamp(d.time_ms, 1, 2000);
        d.feedback = std::clamp(d.feedback, 0, 95);
        d.wet      = std::clamp(d.wet, 0, 100);
        auto &ds = c.slot_dss[static_cast<std::size_t>(i)];
        ds.freq_idx    = std::clamp(ds.freq_idx, 0, alen(DEESS_FREQ_HZ) - 1);
        ds.threshold   = std::clamp(ds.threshold, -50, -4);
        ds.amount      = std::clamp(ds.amount, 0, 100);
        ds.attack_idx  = std::clamp(ds.attack_idx, 0, alen(COMP_ATTACKS) - 1);
        ds.release_idx = std::clamp(ds.release_idx, 0, alen(COMP_RELEASES) - 1);
        auto &pq = c.slot_peq[static_cast<std::size_t>(i)];
        pq.freq_idx  = std::clamp(pq.freq_idx, 0, alen(PEQ_FREQ_HZ) - 1);
        pq.q_idx     = std::clamp(pq.q_idx, 0, alen(PEQ_Q_VALS) - 1);
        pq.gain_db   = std::clamp(pq.gain_db, -12, 12);
        auto &hl = c.slot_hilo[static_cast<std::size_t>(i)];
        hl.hpf_hz_idx = std::clamp(hl.hpf_hz_idx, 0, alen(HPF_FREQ_HZ) - 1);
        hl.lpf_hz_idx = std::clamp(hl.lpf_hz_idx, 0, alen(LPF_FREQ_HZ) - 1);
        auto &pc = c.slot_pc[static_cast<std::size_t>(i)];
        pc.wet       = std::clamp(pc.wet, 0, 100);
        pc.speed     = std::clamp(pc.speed, 0, 100);
        pc.key_root  = std::clamp(pc.key_root, 0, NOTE_NAMES_N - 1);
        pc.scale_idx = std::clamp(pc.scale_idx, 0, PCOR_SCALES - 1);
        pc.pull      = std::clamp(pc.pull, 5, 100);
        pc.exp_yin_i  = std::clamp(pc.exp_yin_i, 0, PCOR_EXP_YIN_N - 1);
        pc.exp_det_i  = std::clamp(pc.exp_det_i, 0, PCOR_EXP_DETECT_N - 1);
        pc.exp_warm_i = std::clamp(pc.exp_warm_i, 0, PCOR_EXP_WARMUP_N - 1);
        pc.exp_gate_i = std::clamp(pc.exp_gate_i, 0, PCOR_EXP_YIN_GATE_N - 1);
        pc.exp_lmin_i = std::clamp(pc.exp_lmin_i, 0, PCOR_EXP_LAG_MIN_N - 1);
        pc.exp_lmax_i = std::clamp(pc.exp_lmax_i, 0, PCOR_EXP_LAG_MAX_N - 1);
        {
            unsigned lmin = PCOR_EXP_LAG_MIN_SMPS[pc.exp_lmin_i];
            unsigned lmax = PCOR_EXP_LAG_MAX_SMPS[pc.exp_lmax_i];
            if (lmax <= lmin) {
                for (int k = 0; k < PCOR_EXP_LAG_MAX_N; k++) {
                    if (PCOR_EXP_LAG_MAX_SMPS[k] > lmin) {
                        pc.exp_lmax_i = k;
                        break;
                    }
                }
            }
        }
        auto &st = c.slot_sat[static_cast<std::size_t>(i)];
        st.drive = std::clamp(st.drive, 0, 100);
        st.mix   = std::clamp(st.mix, 0, 100);
        auto &ps = c.slot_ps[static_cast<std::size_t>(i)];
        ps.semi_idx = std::clamp(ps.semi_idx, 0, PS_SEMI_STEPS - 1);
        ps.wet      = std::clamp(ps.wet, 0, 100);
    }
}

static void apply_chain_or_slot_key(const char *key, const char *val, Config &c,
                                    bool *any_chain_key, bool *any_slot_param) {
    if (!std::strncmp(key, "chain_", 6)) {
        int si = std::atoi(key + 6);
        if (si >= 0 && si < CHAIN_SLOTS) {
            c.chain_slot[static_cast<std::size_t>(si)] = std::atoi(val);
            *any_chain_key                             = true;
        }
        return;
    }
    if (std::strncmp(key, "slot_", 5))
        return;
    int si = -1;
    char sub[80] = {};
    if (std::sscanf(key + 5, "%d_%79s", &si, sub) != 2 || si < 0 || si >= CHAIN_SLOTS)
        return;
    *any_slot_param = true;
    if (!std::strncmp(sub, "eq_", 3)) {
        auto &e = c.slot_eq[static_cast<std::size_t>(si)];
        if (!std::strcmp(sub, "eq_enabled"))
            e.enabled = std::atoi(val) != 0;
        else if (!std::strcmp(sub, "eq_low_db"))
            e.low_db = std::atoi(val);
        else if (!std::strcmp(sub, "eq_mid_db"))
            e.mid_db = std::atoi(val);
        else if (!std::strcmp(sub, "eq_high_db"))
            e.high_db = std::atoi(val);
    } else if (!std::strncmp(sub, "rv_", 3)) {
        auto &r = c.slot_rv[static_cast<std::size_t>(si)];
        if (!std::strcmp(sub, "rv_enabled"))
            r.enabled = std::atoi(val) != 0;
        else if (!std::strcmp(sub, "rv_room"))
            r.room = std::atoi(val);
        else if (!std::strcmp(sub, "rv_damp"))
            r.damp = std::atoi(val);
        else if (!std::strcmp(sub, "rv_wet"))
            r.wet = std::atoi(val);
    } else if (!std::strncmp(sub, "dl_", 3)) {
        auto &d = c.slot_dl[static_cast<std::size_t>(si)];
        if (!std::strcmp(sub, "dl_enabled"))
            d.enabled = std::atoi(val) != 0;
        else if (!std::strcmp(sub, "dl_time_ms"))
            d.time_ms = std::atoi(val);
        else if (!std::strcmp(sub, "dl_feedback"))
            d.feedback = std::atoi(val);
        else if (!std::strcmp(sub, "dl_wet"))
            d.wet = std::atoi(val);
    } else if (!std::strncmp(sub, "dss_", 4)) {
        auto &ds = c.slot_dss[static_cast<std::size_t>(si)];
        if (!std::strcmp(sub, "dss_enabled"))
            ds.enabled = std::atoi(val) != 0;
        else if (!std::strcmp(sub, "dss_freq_idx"))
            ds.freq_idx = std::atoi(val);
        else if (!std::strcmp(sub, "dss_threshold"))
            ds.threshold = std::atoi(val);
        else if (!std::strcmp(sub, "dss_amount"))
            ds.amount = std::atoi(val);
        else if (!std::strcmp(sub, "dss_attack_idx"))
            ds.attack_idx = std::atoi(val);
        else if (!std::strcmp(sub, "dss_release_idx"))
            ds.release_idx = std::atoi(val);
    } else if (!std::strncmp(sub, "peq_", 4)) {
        auto &pq = c.slot_peq[static_cast<std::size_t>(si)];
        if (!std::strcmp(sub, "peq_enabled"))
            pq.enabled = std::atoi(val) != 0;
        else if (!std::strcmp(sub, "peq_freq_idx"))
            pq.freq_idx = std::atoi(val);
        else if (!std::strcmp(sub, "peq_q_idx"))
            pq.q_idx = std::atoi(val);
        else if (!std::strcmp(sub, "peq_gain_db"))
            pq.gain_db = std::atoi(val);
    } else if (!std::strncmp(sub, "hl_", 3)) {
        auto &hl = c.slot_hilo[static_cast<std::size_t>(si)];
        if (!std::strcmp(sub, "hl_hpf_en"))
            hl.hpf_enabled = std::atoi(val) != 0;
        else if (!std::strcmp(sub, "hl_hpf_idx"))
            hl.hpf_hz_idx = std::atoi(val);
        else if (!std::strcmp(sub, "hl_lpf_en"))
            hl.lpf_enabled = std::atoi(val) != 0;
        else if (!std::strcmp(sub, "hl_lpf_idx"))
            hl.lpf_hz_idx = std::atoi(val);
    } else if (!std::strncmp(sub, "pc_", 3)) {
        auto &pc = c.slot_pc[static_cast<std::size_t>(si)];
        if (!std::strcmp(sub, "pc_enabled"))
            pc.enabled = std::atoi(val) != 0;
        else if (!std::strcmp(sub, "pc_wet"))
            pc.wet = std::atoi(val);
        else if (!std::strcmp(sub, "pc_speed"))
            pc.speed = std::atoi(val);
        else if (!std::strcmp(sub, "pc_key"))
            pc.key_root = std::atoi(val);
        else if (!std::strcmp(sub, "pc_scale"))
            pc.scale_idx = std::atoi(val);
        else if (!std::strcmp(sub, "pc_pull"))
            pc.pull = std::atoi(val);
        else if (!std::strcmp(sub, "pc_lowlat_x"))
            pc.low_latency_x = std::atoi(val) != 0;
        else if (!std::strcmp(sub, "pc_exp_yin_i"))
            pc.exp_yin_i = std::atoi(val);
        else if (!std::strcmp(sub, "pc_exp_det_i"))
            pc.exp_det_i = std::atoi(val);
        else if (!std::strcmp(sub, "pc_exp_warm_i"))
            pc.exp_warm_i = std::atoi(val);
        else if (!std::strcmp(sub, "pc_exp_gate_i"))
            pc.exp_gate_i = std::atoi(val);
        else if (!std::strcmp(sub, "pc_exp_lmin_i"))
            pc.exp_lmin_i = std::atoi(val);
        else if (!std::strcmp(sub, "pc_exp_lmax_i"))
            pc.exp_lmax_i = std::atoi(val);
    } else if (!std::strncmp(sub, "sat_", 4)) {
        auto &st = c.slot_sat[static_cast<std::size_t>(si)];
        if (!std::strcmp(sub, "sat_enabled"))
            st.enabled = std::atoi(val) != 0;
        else if (!std::strcmp(sub, "sat_drive"))
            st.drive = std::atoi(val);
        else if (!std::strcmp(sub, "sat_mix"))
            st.mix = std::atoi(val);
    } else if (!std::strncmp(sub, "ps_", 3)) {
        auto &ps = c.slot_ps[static_cast<std::size_t>(si)];
        if (!std::strcmp(sub, "ps_enabled"))
            ps.enabled = std::atoi(val) != 0;
        else if (!std::strcmp(sub, "ps_semi_idx"))
            ps.semi_idx = std::atoi(val);
        else if (!std::strcmp(sub, "ps_wet"))
            ps.wet = std::atoi(val);
    } else {
        auto &p = c.slot_comp[static_cast<std::size_t>(si)];
        if (!std::strcmp(sub, "enabled"))
            p.enabled = std::atoi(val) != 0;
        else if (!std::strcmp(sub, "threshold"))
            p.threshold = std::atoi(val);
        else if (!std::strcmp(sub, "ratio_idx"))
            p.ratio_idx = std::atoi(val);
        else if (!std::strcmp(sub, "attack_idx"))
            p.attack_idx = std::atoi(val);
        else if (!std::strcmp(sub, "release_idx"))
            p.release_idx = std::atoi(val);
        else if (!std::strcmp(sub, "knee_idx"))
            p.knee_idx = std::atoi(val);
        else if (!std::strcmp(sub, "makeup"))
            p.makeup = std::atoi(val);
    }
}

static bool read_effect_chain_file(const std::string &path, Config &out, bool *any_chain,
                                   bool *any_slot) {
    *any_chain = false;
    *any_slot  = false;
    FILE *f    = std::fopen(path.c_str(), "r");
    if (!f)
        return false;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        char key[64] = {}, val[256] = {};
        if (std::sscanf(line, "%63[^=]=%255[^\n]", key, val) >= 1)
            apply_chain_or_slot_key(key, val, out, any_chain, any_slot);
    }
    std::fclose(f);
    return true;
}

void copy_effect_chain(const Config &src, Config &dst) {
    dst.chain_slot = src.chain_slot;
    dst.slot_comp  = src.slot_comp;
    dst.slot_eq    = src.slot_eq;
    dst.slot_rv    = src.slot_rv;
    dst.slot_dl    = src.slot_dl;
    dst.slot_dss   = src.slot_dss;
    dst.slot_peq   = src.slot_peq;
    dst.slot_hilo  = src.slot_hilo;
    dst.slot_pc    = src.slot_pc;
    dst.slot_sat   = src.slot_sat;
    dst.slot_ps    = src.slot_ps;
}

bool save_effect_chain_profile(const std::string &slug, const Config &c) {
    std::string safe = sanitize_profile_slug(slug);
    std::string path = profiles_directory() + "/" + safe + ".conf";
    std::string tmp  = path + ".tmp";
    FILE *f          = std::fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    std::fprintf(f, "# Aud10 effect chain profile (chain + all slot parameters)\n");
    fwrite_effect_chain(f, c);
    std::fclose(f);
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        return false;
    return true;
}

bool load_effect_chain_profile(const std::string &path, Config &c) {
    Config tmp = c;
    bool any_chain = false, any_slot = false;
    if (!read_effect_chain_file(path, tmp, &any_chain, &any_slot))
        return false;
    if (!any_chain && !any_slot)
        return false;
    clamp_effect_chain_config(tmp);
    copy_effect_chain(tmp, c);
    return true;
}

std::vector<std::string> list_profile_slugs() {
    std::vector<std::string> out;
    std::string dir = profiles_directory();
    DIR *d          = opendir(dir.c_str());
    if (!d)
        return out;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        const char *n = e->d_name;
        std::size_t len = std::strlen(n);
        if (len > 5 && std::strcmp(n + len - 5, ".conf") == 0)
            out.emplace_back(std::string(n, len - 5));
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

bool delete_effect_chain_profile(const std::string &slug) {
    std::string path = profiles_directory() + "/" + sanitize_profile_slug(slug) + ".conf";
    return std::remove(path.c_str()) == 0;
}

void save_config(const std::string &path, const Config &c) {
    std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "channel_idx=%d\n", c.channel_idx);
    std::fprintf(f, "buffer_idx=%d\n", c.buffer_idx);
    std::fprintf(f, "vol_in=%d\n", c.vol_in);
    std::fprintf(f, "vol_out=%d\n", c.vol_out);
    std::fprintf(f, "in_port_l=%s\n", c.in_port_l.c_str());
    std::fprintf(f, "in_port_r=%s\n", c.in_port_r.c_str());
    std::fprintf(f, "out_port_l=%s\n", c.out_port_l.c_str());
    std::fprintf(f, "out_port_r=%s\n", c.out_port_r.c_str());
    std::fprintf(f, "comp_enabled=%d\n", c.comp_enabled ? 1 : 0);
    std::fprintf(f, "comp_threshold=%d\n", c.comp_threshold);
    std::fprintf(f, "comp_ratio_idx=%d\n", c.comp_ratio_idx);
    std::fprintf(f, "comp_attack_idx=%d\n", c.comp_attack_idx);
    std::fprintf(f, "comp_release_idx=%d\n", c.comp_release_idx);
    std::fprintf(f, "comp_knee_idx=%d\n", c.comp_knee_idx);
    std::fprintf(f, "comp_makeup=%d\n", c.comp_makeup);
    fwrite_effect_chain(f, c);
    std::fclose(f);
    std::rename(tmp.c_str(), path.c_str());
}

Config load_config(const std::string &path) {
    Config c;
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return c;
    char line[512];
    bool any_chain_key  = false;
    bool any_slot_param = false;
    while (std::fgets(line, sizeof line, f)) {
        char key[64] = {}, val[256] = {};
        if (std::sscanf(line, "%63[^=]=%255[^\n]", key, val) >= 1) {
            if (!std::strcmp(key, "channel_idx"))
                c.channel_idx = std::atoi(val);
            else if (!std::strcmp(key, "buffer_idx"))
                c.buffer_idx = std::atoi(val);
            else if (!std::strcmp(key, "vol_in"))
                c.vol_in = std::atoi(val);
            else if (!std::strcmp(key, "vol_out"))
                c.vol_out = std::atoi(val);
            else if (!std::strcmp(key, "in_port_l"))
                c.in_port_l = val;
            else if (!std::strcmp(key, "in_port_r"))
                c.in_port_r = val;
            else if (!std::strcmp(key, "out_port_l"))
                c.out_port_l = val;
            else if (!std::strcmp(key, "out_port_r"))
                c.out_port_r = val;
            else if (!std::strcmp(key, "comp_enabled"))
                c.comp_enabled = std::atoi(val) != 0;
            else if (!std::strcmp(key, "comp_threshold"))
                c.comp_threshold = std::atoi(val);
            else if (!std::strcmp(key, "comp_ratio_idx"))
                c.comp_ratio_idx = std::atoi(val);
            else if (!std::strcmp(key, "comp_attack_idx"))
                c.comp_attack_idx = std::atoi(val);
            else if (!std::strcmp(key, "comp_release_idx"))
                c.comp_release_idx = std::atoi(val);
            else if (!std::strcmp(key, "comp_knee_idx"))
                c.comp_knee_idx = std::atoi(val);
            else if (!std::strcmp(key, "comp_makeup"))
                c.comp_makeup = std::atoi(val);
            else if (!std::strncmp(key, "chain_", 6) || !std::strncmp(key, "slot_", 5))
                apply_chain_or_slot_key(key, val, c, &any_chain_key, &any_slot_param);
            else if (!std::strcmp(key, "input_port"))
                c.in_port_l = val;
            else if (!std::strcmp(key, "output_port"))
                c.out_port_l = val;
        }
    }
    std::fclose(f);

    if (!any_chain_key && c.comp_enabled)
        c.chain_slot[0] = 1;

    if (!any_slot_param) {
        CompressorSlotParams p;
        p.enabled     = c.comp_enabled;
        p.threshold   = c.comp_threshold;
        p.ratio_idx   = c.comp_ratio_idx;
        p.attack_idx  = c.comp_attack_idx;
        p.release_idx = c.comp_release_idx;
        p.knee_idx    = c.comp_knee_idx;
        p.makeup      = c.comp_makeup;
        for (int i = 0; i < CHAIN_SLOTS; i++)
            c.slot_comp[static_cast<std::size_t>(i)] = p;
    }

    c.channel_idx        = std::clamp(c.channel_idx, 0, alen(CHANNEL_OPTS) - 1);
    c.buffer_idx         = std::clamp(c.buffer_idx, 0, alen(JACK_BUFSIZES) - 1);
    c.vol_in             = std::clamp(c.vol_in, -60, 12);
    c.vol_out            = std::clamp(c.vol_out, -60, 12);
    c.comp_threshold     = std::clamp(c.comp_threshold, -60, 0);
    c.comp_ratio_idx     = std::clamp(c.comp_ratio_idx, 0, alen(COMP_RATIOS) - 1);
    c.comp_attack_idx    = std::clamp(c.comp_attack_idx, 0, alen(COMP_ATTACKS) - 1);
    c.comp_release_idx   = std::clamp(c.comp_release_idx, 0, alen(COMP_RELEASES) - 1);
    c.comp_knee_idx      = std::clamp(c.comp_knee_idx, 0, alen(COMP_KNEES) - 1);
    c.comp_makeup        = std::clamp(c.comp_makeup, -12, 36);
    clamp_effect_chain_config(c);
    return c;
}
