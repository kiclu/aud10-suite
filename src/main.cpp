#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <thread>
#include <limits>

#include <ncurses.h>
#include <jack/jack.h>

#include "constants.hpp"
#include "config.hpp"
#include "fx/effects.hpp"

// ── JACK audio backend ──────────────────────────────────────
//
// jack_process_cb runs on JACK’s real-time thread.  All ncurses / keyboard
// handling runs on a dedicated UI thread (see std::thread in main).

static jack_client_t *g_client   = nullptr;
static jack_port_t   *g_in[2]       = {};
static jack_port_t   *g_out[2]      = {};
/** Extra stereo outputs carrying the same post-FX signal as output_1/2 — connect in QjackCtl etc. */
static jack_port_t   *g_virt_out[2] = {};

static std::array<std::unique_ptr<Effect>, CHAIN_SLOTS> g_fx_slots;

// Real-time effect order (filled when stopped or at Start; not changed while live).
static Effect *g_rt_fx[CHAIN_SLOTS];
static int     g_rt_n = 0;
static Compressor *g_gr_meter = nullptr; // last compressor in chain for GR meter

static std::atomic<bool>     g_active{false};
static std::atomic<bool>     g_jack_ok{true};
static std::atomic<unsigned> g_channels{1};
static std::atomic<unsigned> g_sr{48000};
static std::atomic<unsigned> g_bufsz{128};
static std::atomic<float>    g_peak_in{0.0f};
static std::atomic<float>    g_peak_out{0.0f};
static std::atomic<float>    g_vol_in{1.0f};
static std::atomic<float>    g_vol_out{1.0f};

// DSP timing (updated from JACK RT callback; read by UI thread).
static std::atomic<float>    g_dsp_ms_last{0.0f};
static std::atomic<float>    g_dsp_ms_ema{0.0f};
static std::atomic<uint64_t> g_xrun_count{0};

// JACK graph latency (frames); polled on UI thread via jack_port_get_latency_range.
static std::atomic<uint32_t> g_lat_in_min{0};
static std::atomic<uint32_t> g_lat_in_max{0};
static std::atomic<uint32_t> g_lat_out_min{0};
static std::atomic<uint32_t> g_lat_out_max{0};

static float g_ibuf[16384];

static int jack_xrun_cb(void *) {
    g_xrun_count.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

static int jack_process_cb(jack_nframes_t nframes, void *) {
    unsigned ch = g_channels.load(std::memory_order_relaxed);

    for (unsigned c = 0; c < 2; c++) {
        float *o = (float *)jack_port_get_buffer(g_out[c], nframes);
        std::memset(o, 0, nframes * sizeof(float));
        if (g_virt_out[c]) {
            float *v = (float *)jack_port_get_buffer(g_virt_out[c], nframes);
            std::memset(v, 0, nframes * sizeof(float));
        }
    }

    if (!g_active.load(std::memory_order_relaxed))
        return 0;

    const auto t_dsp0 = std::chrono::steady_clock::now();

    unsigned sr   = g_sr.load(std::memory_order_relaxed);
    float    vin  = g_vol_in.load(std::memory_order_relaxed);
    float    vout = g_vol_out.load(std::memory_order_relaxed);

    if (ch == 1) {
        float *in  = (float *)jack_port_get_buffer(g_in[0], nframes);
        float *out = (float *)jack_port_get_buffer(g_out[0], nframes);

        float ipk = 0.0f;
        for (jack_nframes_t i = 0; i < nframes; i++) {
            out[i] = in[i] * vin;
            ipk = std::max(ipk, std::fabs(out[i]));
        }
        g_peak_in.store(ipk, std::memory_order_relaxed);

        for (int k = 0; k < g_rt_n; k++) {
            Effect *e = g_rt_fx[k];
            if (e->enabled.load(std::memory_order_relaxed))
                e->process(out, nframes, 1, sr);
        }

        float opk = 0.0f;
        for (jack_nframes_t i = 0; i < nframes; i++) {
            out[i] *= vout;
            opk = std::max(opk, std::fabs(out[i]));
        }
        g_peak_out.store(opk, std::memory_order_relaxed);

        if (g_virt_out[0] && g_virt_out[1]) {
            float *v0 = (float *)jack_port_get_buffer(g_virt_out[0], nframes);
            float *v1 = (float *)jack_port_get_buffer(g_virt_out[1], nframes);
            for (jack_nframes_t i = 0; i < nframes; i++) {
                v0[i] = out[i];
                v1[i] = out[i];
            }
        }
    } else {
        float *in0  = (float *)jack_port_get_buffer(g_in[0], nframes);
        float *in1  = (float *)jack_port_get_buffer(g_in[1], nframes);
        float *out0 = (float *)jack_port_get_buffer(g_out[0], nframes);
        float *out1 = (float *)jack_port_get_buffer(g_out[1], nframes);

        jack_nframes_t n = std::min(nframes, jack_nframes_t(8192));
        float ipk = 0.0f;
        for (jack_nframes_t i = 0; i < n; i++) {
            float l = in0[i] * vin, r = in1[i] * vin;
            g_ibuf[i * 2]     = l;
            g_ibuf[i * 2 + 1] = r;
            ipk = std::max(ipk, std::max(std::fabs(l), std::fabs(r)));
        }
        g_peak_in.store(ipk, std::memory_order_relaxed);

        for (int k = 0; k < g_rt_n; k++) {
            Effect *e = g_rt_fx[k];
            if (e->enabled.load(std::memory_order_relaxed))
                e->process(g_ibuf, n, 2, sr);
        }

        float opk = 0.0f;
        for (jack_nframes_t i = 0; i < n; i++) {
            out0[i] = g_ibuf[i * 2]     * vout;
            out1[i] = g_ibuf[i * 2 + 1] * vout;
            opk = std::max(opk, std::max(std::fabs(out0[i]), std::fabs(out1[i])));
        }
        g_peak_out.store(opk, std::memory_order_relaxed);

        if (g_virt_out[0] && g_virt_out[1]) {
            float *v0 = (float *)jack_port_get_buffer(g_virt_out[0], nframes);
            float *v1 = (float *)jack_port_get_buffer(g_virt_out[1], nframes);
            for (jack_nframes_t i = 0; i < nframes; i++) {
                v0[i] = out0[i];
                v1[i] = out1[i];
            }
        }
    }

    const auto t_dsp1 = std::chrono::steady_clock::now();
    const float ms =
        std::chrono::duration<float, std::milli>(t_dsp1 - t_dsp0).count();
    g_dsp_ms_last.store(ms, std::memory_order_relaxed);
    float ema = g_dsp_ms_ema.load(std::memory_order_relaxed);
    g_dsp_ms_ema.store(ema * 0.92f + ms * 0.08f, std::memory_order_relaxed);

    return 0;
}

static int jack_bufsz_cb(jack_nframes_t nframes, void *) {
    g_bufsz.store(nframes, std::memory_order_relaxed);
    return 0;
}

static void jack_shutdown_cb(void *) {
    g_jack_ok.store(false, std::memory_order_relaxed);
    g_active.store(false, std::memory_order_relaxed);
}

static bool jack_port_has_connections(jack_port_t *p) {
    if (!p)
        return false;
    const char **co = jack_port_get_connections(p);
    bool r = co && co[0];
    if (co)
        jack_free(co);
    return r;
}

/** Aggregate capture latency at our inputs and playback latency at our outputs (connected ports only). */
static void poll_jack_port_latencies() {
    if (!g_client)
        return;
    jack_latency_range_t lr{};
    jack_nframes_t in_lo = std::numeric_limits<jack_nframes_t>::max(), in_hi = 0;
    jack_nframes_t out_lo = std::numeric_limits<jack_nframes_t>::max(), out_hi = 0;
    int nin = 0, nout = 0;

    for (int c = 0; c < 2; c++) {
        jack_port_t *pi = g_in[c];
        if (pi && jack_port_has_connections(pi)) {
            jack_port_get_latency_range(pi, JackCaptureLatency, &lr);
            in_lo = std::min(in_lo, lr.min);
            in_hi = std::max(in_hi, lr.max);
            nin++;
        }
        jack_port_t *po = g_out[c];
        if (po && jack_port_has_connections(po)) {
            jack_port_get_latency_range(po, JackPlaybackLatency, &lr);
            out_lo = std::min(out_lo, lr.min);
            out_hi = std::max(out_hi, lr.max);
            nout++;
        }
        jack_port_t *pv = g_virt_out[c];
        if (pv && jack_port_has_connections(pv)) {
            jack_port_get_latency_range(pv, JackPlaybackLatency, &lr);
            out_lo = std::min(out_lo, lr.min);
            out_hi = std::max(out_hi, lr.max);
            nout++;
        }
    }

    if (nin == 0) {
        in_lo = in_hi = 0;
    } else if (in_lo == std::numeric_limits<jack_nframes_t>::max()) {
        in_lo = 0;
    }
    if (nout == 0) {
        out_lo = out_hi = 0;
    } else if (out_lo == std::numeric_limits<jack_nframes_t>::max()) {
        out_lo = 0;
    }

    g_lat_in_min.store(static_cast<uint32_t>(in_lo), std::memory_order_relaxed);
    g_lat_in_max.store(static_cast<uint32_t>(in_hi), std::memory_order_relaxed);
    g_lat_out_min.store(static_cast<uint32_t>(out_lo), std::memory_order_relaxed);
    g_lat_out_max.store(static_cast<uint32_t>(out_hi), std::memory_order_relaxed);
}

// ── Port helpers ────────────────────────────────────────────

struct PortInfo {
    std::string name;
    std::string display;
};

static std::vector<PortInfo> enum_ports(unsigned long flags) {
    std::vector<PortInfo> out;
    const char **ports = jack_get_ports(g_client, nullptr,
                                        JACK_DEFAULT_AUDIO_TYPE,
                                        JackPortIsPhysical | flags);
    if (ports) {
        for (int i = 0; ports[i]; i++)
            out.push_back({ports[i], ports[i]});
        jack_free(ports);
    }
    return out;
}

static void disconnect_all() {
    for (int c = 0; c < 2; c++) {
        const char **conns = jack_port_get_connections(g_in[c]);
        if (conns) {
            for (int i = 0; conns[i]; i++)
                jack_disconnect(g_client, conns[i], jack_port_name(g_in[c]));
            jack_free(conns);
        }
        conns = jack_port_get_connections(g_out[c]);
        if (conns) {
            for (int i = 0; conns[i]; i++)
                jack_disconnect(g_client, jack_port_name(g_out[c]), conns[i]);
            jack_free(conns);
        }
        if (g_virt_out[c]) {
            conns = jack_port_get_connections(g_virt_out[c]);
            if (conns) {
                for (int i = 0; conns[i]; i++)
                    jack_disconnect(g_client, jack_port_name(g_virt_out[c]), conns[i]);
                jack_free(conns);
            }
        }
    }
}

static void connect_ports(unsigned ch,
                           const std::string &cap_l, const std::string &cap_r,
                           const std::string &play_l, const std::string &play_r) {
    if (!cap_l.empty())
        jack_connect(g_client, cap_l.c_str(), jack_port_name(g_in[0]));
    if (ch > 1 && !cap_r.empty())
        jack_connect(g_client, cap_r.c_str(), jack_port_name(g_in[1]));
    if (!play_l.empty())
        jack_connect(g_client, jack_port_name(g_out[0]), play_l.c_str());
    if (ch > 1 && !play_r.empty())
        jack_connect(g_client, jack_port_name(g_out[1]), play_r.c_str());
}

// slot_type[i]: FX_NONE..FX_PSHIFT
static void rebuild_fx_chain(const int slot_type[CHAIN_SLOTS]) {
    g_rt_n = 0;
    g_gr_meter = nullptr;
    for (int i = 0; i < CHAIN_SLOTS; i++) {
        int t = slot_type[i];
        if (t <= FX_NONE || t > FX_PSHIFT) {
            g_fx_slots[i].reset();
            continue;
        }
        bool need = !g_fx_slots[i] || g_fx_slots[i]->effect_kind() != t;
        if (need) {
            if (t == FX_COMP)      g_fx_slots[i] = std::make_unique<Compressor>();
            else if (t == FX_EQ)   g_fx_slots[i] = std::make_unique<GraphicEQ>();
            else if (t == FX_REV)  g_fx_slots[i] = std::make_unique<ReverbFX>();
            else if (t == FX_DLY)  g_fx_slots[i] = std::make_unique<DelayFX>();
            else if (t == FX_DESS) g_fx_slots[i] = std::make_unique<DeesserFX>();
            else if (t == FX_PEQ)  g_fx_slots[i] = std::make_unique<ParametricEQ>();
            else if (t == FX_HILO) g_fx_slots[i] = std::make_unique<HiLoPassFX>();
            else if (t == FX_PCOR) g_fx_slots[i] = std::make_unique<PitchCorrectFX>();
            else if (t == FX_SAT)  g_fx_slots[i] = std::make_unique<SaturatorFX>();
            else if (t == FX_PSHIFT) g_fx_slots[i] = std::make_unique<PitchShiftFX>();
        }
        Effect *e = g_fx_slots[i].get();
        g_rt_fx[g_rt_n++] = e;
        if (t == FX_COMP)
            g_gr_meter = static_cast<Compressor *>(e);
    }
}

// ── TUI ─────────────────────────────────────────────────────

static std::string trunc(const std::string &s, std::size_t mx) {
    return s.size() <= mx ? s : s.substr(0, mx - 2) + "..";
}

static void fmt_pitch_readout(char *dst, size_t cap, float hz) {
    if (!(hz > 45.0f && hz < 2500.0f)) {
        std::snprintf(dst, cap, "—");
        return;
    }
    float m = 69.0f + 12.0f * std::log2(hz / 440.0f);
    int n = (int)std::lround(m);
    int pc = ((n % 12) + 12) % 12;
    int oct = n / 12 - 1;
    std::snprintf(dst, cap, "%.1f Hz  ~ %s%d", hz, NOTE_NAMES[pc], oct);
}

static void pc_normalize_exp_lag(PitchCorrectSlotParams &p) {
    int ni = std::clamp(p.exp_lmin_i, 0, PCOR_EXP_LAG_MIN_N - 1);
    int mi = std::clamp(p.exp_lmax_i, 0, PCOR_EXP_LAG_MAX_N - 1);
    unsigned lmin = PCOR_EXP_LAG_MIN_SMPS[ni];
    unsigned lmax = PCOR_EXP_LAG_MAX_SMPS[mi];
    if (lmax > lmin)
        return;
    for (int k = 0; k < PCOR_EXP_LAG_MAX_N; k++) {
        if (PCOR_EXP_LAG_MAX_SMPS[k] > lmin) {
            p.exp_lmax_i = k;
            return;
        }
    }
    p.exp_lmin_i = std::max(0, ni - 1);
}

static void pc_clamp_pc_exp_cursor(SlotEditField &c, bool lowlat_on) {
    if (!lowlat_on && c >= SE_PC_EXP_YIN && c <= SE_PC_EXP_LMAX)
        c = SE_PC_LOWLAT_X;
}

static int find_port_idx(const std::vector<PortInfo> &ports, const std::string &name,
                          int fallback = -1) {
    for (int i = 0; i < (int)ports.size(); i++)
        if (ports[i].name == name) return i;
    return fallback >= 0 ? fallback : (ports.size() > 1 ? 1 : 0);
}

// ── Effect chain clipboard (copy / paste / cut / swap) ──────

struct SlotClipboard {
    bool valid = false;
    int ty = FX_NONE;
    CompressorSlotParams comp{};
    EqSlotParams eq{};
    ReverbSlotParams rv{};
    DelaySlotParams dl{};
    DeEsserSlotParams dss{};
    PeqSlotParams peq{};
    HiloSlotParams hilo{};
    PitchCorrectSlotParams pc{};
    SaturatorSlotParams sat{};
    PitchShiftSlotParams ps{};
};

static SlotClipboard g_chain_clip;

static void slot_clip_copy(SlotClipboard &cb, int si,
                           const std::array<int, CHAIN_SLOTS> &slot_type,
                           const std::array<CompressorSlotParams, CHAIN_SLOTS> &slot_param,
                           const std::array<EqSlotParams, CHAIN_SLOTS> &slot_eq,
                           const std::array<ReverbSlotParams, CHAIN_SLOTS> &slot_rv,
                           const std::array<DelaySlotParams, CHAIN_SLOTS> &slot_dl,
                           const std::array<DeEsserSlotParams, CHAIN_SLOTS> &slot_dss,
                           const std::array<PeqSlotParams, CHAIN_SLOTS> &slot_peq,
                           const std::array<HiloSlotParams, CHAIN_SLOTS> &slot_hilo,
                           const std::array<PitchCorrectSlotParams, CHAIN_SLOTS> &slot_pc,
                           const std::array<SaturatorSlotParams, CHAIN_SLOTS> &slot_sat,
                           const std::array<PitchShiftSlotParams, CHAIN_SLOTS> &slot_ps)
{
    if (si < 0 || si >= CHAIN_SLOTS) return;
    const std::size_t u = static_cast<std::size_t>(si);
    cb.valid = true;
    cb.ty = slot_type[u];
    cb.comp = slot_param[u];
    cb.eq = slot_eq[u];
    cb.rv = slot_rv[u];
    cb.dl = slot_dl[u];
    cb.dss = slot_dss[u];
    cb.peq = slot_peq[u];
    cb.hilo = slot_hilo[u];
    cb.pc = slot_pc[u];
    cb.sat = slot_sat[u];
    cb.ps = slot_ps[u];
}

static void slot_clip_paste(SlotClipboard &cb, int si,
                            std::array<int, CHAIN_SLOTS> &slot_type,
                            std::array<CompressorSlotParams, CHAIN_SLOTS> &slot_param,
                            std::array<EqSlotParams, CHAIN_SLOTS> &slot_eq,
                            std::array<ReverbSlotParams, CHAIN_SLOTS> &slot_rv,
                            std::array<DelaySlotParams, CHAIN_SLOTS> &slot_dl,
                            std::array<DeEsserSlotParams, CHAIN_SLOTS> &slot_dss,
                            std::array<PeqSlotParams, CHAIN_SLOTS> &slot_peq,
                            std::array<HiloSlotParams, CHAIN_SLOTS> &slot_hilo,
                            std::array<PitchCorrectSlotParams, CHAIN_SLOTS> &slot_pc,
                            std::array<SaturatorSlotParams, CHAIN_SLOTS> &slot_sat,
                            std::array<PitchShiftSlotParams, CHAIN_SLOTS> &slot_ps)
{
    if (!cb.valid || si < 0 || si >= CHAIN_SLOTS) return;
    const std::size_t u = static_cast<std::size_t>(si);
    slot_type[u] = std::clamp(cb.ty, 0, SLOT_TYPES - 1);
    slot_param[u] = cb.comp;
    slot_eq[u] = cb.eq;
    slot_rv[u] = cb.rv;
    slot_dl[u] = cb.dl;
    slot_dss[u] = cb.dss;
    slot_peq[u] = cb.peq;
    slot_hilo[u] = cb.hilo;
    slot_pc[u] = cb.pc;
    slot_sat[u] = cb.sat;
    slot_ps[u] = cb.ps;
}

static void slot_clip_cut(SlotClipboard &cb, int si,
                          std::array<int, CHAIN_SLOTS> &slot_type,
                          std::array<CompressorSlotParams, CHAIN_SLOTS> &slot_param,
                          std::array<EqSlotParams, CHAIN_SLOTS> &slot_eq,
                          std::array<ReverbSlotParams, CHAIN_SLOTS> &slot_rv,
                          std::array<DelaySlotParams, CHAIN_SLOTS> &slot_dl,
                          std::array<DeEsserSlotParams, CHAIN_SLOTS> &slot_dss,
                          std::array<PeqSlotParams, CHAIN_SLOTS> &slot_peq,
                          std::array<HiloSlotParams, CHAIN_SLOTS> &slot_hilo,
                          std::array<PitchCorrectSlotParams, CHAIN_SLOTS> &slot_pc,
                          std::array<SaturatorSlotParams, CHAIN_SLOTS> &slot_sat,
                            std::array<PitchShiftSlotParams, CHAIN_SLOTS> &slot_ps)
{
    slot_clip_copy(cb, si, slot_type, slot_param, slot_eq, slot_rv, slot_dl, slot_dss,
                   slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
    slot_type[static_cast<std::size_t>(si)] = FX_NONE;
}

static void chain_swap_slots(int a, int b,
                             std::array<int, CHAIN_SLOTS> &slot_type,
                             std::array<CompressorSlotParams, CHAIN_SLOTS> &slot_param,
                             std::array<EqSlotParams, CHAIN_SLOTS> &slot_eq,
                             std::array<ReverbSlotParams, CHAIN_SLOTS> &slot_rv,
                             std::array<DelaySlotParams, CHAIN_SLOTS> &slot_dl,
                             std::array<DeEsserSlotParams, CHAIN_SLOTS> &slot_dss,
                             std::array<PeqSlotParams, CHAIN_SLOTS> &slot_peq,
                             std::array<HiloSlotParams, CHAIN_SLOTS> &slot_hilo,
                             std::array<PitchCorrectSlotParams, CHAIN_SLOTS> &slot_pc,
                             std::array<SaturatorSlotParams, CHAIN_SLOTS> &slot_sat,
                            std::array<PitchShiftSlotParams, CHAIN_SLOTS> &slot_ps)
{
    if (a < 0 || b < 0 || a >= CHAIN_SLOTS || b >= CHAIN_SLOTS) return;
    using std::swap;
    const std::size_t ua = static_cast<std::size_t>(a);
    const std::size_t ub = static_cast<std::size_t>(b);
    swap(slot_type[ua], slot_type[ub]);
    swap(slot_param[ua], slot_param[ub]);
    swap(slot_eq[ua], slot_eq[ub]);
    swap(slot_rv[ua], slot_rv[ub]);
    swap(slot_dl[ua], slot_dl[ub]);
    swap(slot_dss[ua], slot_dss[ub]);
    swap(slot_peq[ua], slot_peq[ub]);
    swap(slot_hilo[ua], slot_hilo[ub]);
    swap(slot_pc[ua], slot_pc[ub]);
    swap(slot_sat[ua], slot_sat[ub]);
    swap(slot_ps[ua], slot_ps[ub]);
}

// Per-slot bypass (matches each effect’s “Enabled” / Hi–Lo filters).
static bool slot_effect_is_on(
    int st, std::size_t si,
    const std::array<CompressorSlotParams, CHAIN_SLOTS> &slot_param,
    const std::array<EqSlotParams, CHAIN_SLOTS> &slot_eq,
    const std::array<ReverbSlotParams, CHAIN_SLOTS> &slot_rv,
    const std::array<DelaySlotParams, CHAIN_SLOTS> &slot_dl,
    const std::array<DeEsserSlotParams, CHAIN_SLOTS> &slot_dss,
    const std::array<PeqSlotParams, CHAIN_SLOTS> &slot_peq,
    const std::array<HiloSlotParams, CHAIN_SLOTS> &slot_hilo,
    const std::array<PitchCorrectSlotParams, CHAIN_SLOTS> &slot_pc,
    const std::array<SaturatorSlotParams, CHAIN_SLOTS> &slot_sat,
                           const std::array<PitchShiftSlotParams, CHAIN_SLOTS> &slot_ps)
{
    st = std::clamp(st, 0, SLOT_TYPES - 1);
    switch (st) {
    case FX_NONE: return false;
    case FX_COMP: return slot_param[si].enabled;
    case FX_EQ: return slot_eq[si].enabled;
    case FX_REV: return slot_rv[si].enabled;
    case FX_DLY: return slot_dl[si].enabled;
    case FX_DESS: return slot_dss[si].enabled;
    case FX_PEQ: return slot_peq[si].enabled;
    case FX_HILO: return slot_hilo[si].hpf_enabled || slot_hilo[si].lpf_enabled;
    case FX_PCOR: return slot_pc[si].enabled;
    case FX_SAT: return slot_sat[si].enabled;
    case FX_PSHIFT: return slot_ps[si].enabled;
    default: return false;
    }
}

static void slot_toggle_effect_enabled(
    int st, std::size_t si,
    std::array<CompressorSlotParams, CHAIN_SLOTS> &slot_param,
    std::array<EqSlotParams, CHAIN_SLOTS> &slot_eq,
    std::array<ReverbSlotParams, CHAIN_SLOTS> &slot_rv,
    std::array<DelaySlotParams, CHAIN_SLOTS> &slot_dl,
    std::array<DeEsserSlotParams, CHAIN_SLOTS> &slot_dss,
    std::array<PeqSlotParams, CHAIN_SLOTS> &slot_peq,
    std::array<HiloSlotParams, CHAIN_SLOTS> &slot_hilo,
    std::array<PitchCorrectSlotParams, CHAIN_SLOTS> &slot_pc,
    std::array<SaturatorSlotParams, CHAIN_SLOTS> &slot_sat,
                            std::array<PitchShiftSlotParams, CHAIN_SLOTS> &slot_ps)
{
    st = std::clamp(st, 0, SLOT_TYPES - 1);
    switch (st) {
    case FX_NONE: break;
    case FX_COMP: slot_param[si].enabled = !slot_param[si].enabled; break;
    case FX_EQ: slot_eq[si].enabled = !slot_eq[si].enabled; break;
    case FX_REV: slot_rv[si].enabled = !slot_rv[si].enabled; break;
    case FX_DLY: slot_dl[si].enabled = !slot_dl[si].enabled; break;
    case FX_DESS: slot_dss[si].enabled = !slot_dss[si].enabled; break;
    case FX_PEQ: slot_peq[si].enabled = !slot_peq[si].enabled; break;
    case FX_HILO: {
        auto &h = slot_hilo[si];
        if (h.hpf_enabled || h.lpf_enabled) {
            h.hpf_enabled = false;
            h.lpf_enabled = false;
        } else {
            h.hpf_enabled = true;
            h.lpf_enabled = true;
        }
    } break;
    case FX_PCOR: slot_pc[si].enabled = !slot_pc[si].enabled; break;
    case FX_SAT: slot_sat[si].enabled = !slot_sat[si].enabled; break;
    case FX_PSHIFT: slot_ps[si].enabled = !slot_ps[si].enabled; break;
    default: break;
    }
}

static Config config_snapshot_chain(
    const std::array<int, CHAIN_SLOTS> &slot_type,
    const std::array<CompressorSlotParams, CHAIN_SLOTS> &slot_param,
    const std::array<EqSlotParams, CHAIN_SLOTS> &slot_eq,
    const std::array<ReverbSlotParams, CHAIN_SLOTS> &slot_rv,
    const std::array<DelaySlotParams, CHAIN_SLOTS> &slot_dl,
    const std::array<DeEsserSlotParams, CHAIN_SLOTS> &slot_dss,
    const std::array<PeqSlotParams, CHAIN_SLOTS> &slot_peq,
    const std::array<HiloSlotParams, CHAIN_SLOTS> &slot_hilo,
    const std::array<PitchCorrectSlotParams, CHAIN_SLOTS> &slot_pc,
    const std::array<SaturatorSlotParams, CHAIN_SLOTS> &slot_sat,
                           const std::array<PitchShiftSlotParams, CHAIN_SLOTS> &slot_ps)
{
    Config c{};
    c.chain_slot = slot_type;
    c.slot_comp  = slot_param;
    c.slot_eq    = slot_eq;
    c.slot_rv    = slot_rv;
    c.slot_dl    = slot_dl;
    c.slot_dss   = slot_dss;
    c.slot_peq   = slot_peq;
    c.slot_hilo  = slot_hilo;
    c.slot_pc    = slot_pc;
    c.slot_sat   = slot_sat;
    c.slot_ps    = slot_ps;
    return c;
}

static void apply_chain_snapshot(Config &c, std::array<int, CHAIN_SLOTS> &slot_type,
                                 std::array<CompressorSlotParams, CHAIN_SLOTS> &slot_param,
                                 std::array<EqSlotParams, CHAIN_SLOTS> &slot_eq,
                                 std::array<ReverbSlotParams, CHAIN_SLOTS> &slot_rv,
                                 std::array<DelaySlotParams, CHAIN_SLOTS> &slot_dl,
                                 std::array<DeEsserSlotParams, CHAIN_SLOTS> &slot_dss,
                                 std::array<PeqSlotParams, CHAIN_SLOTS> &slot_peq,
                                 std::array<HiloSlotParams, CHAIN_SLOTS> &slot_hilo,
                                 std::array<PitchCorrectSlotParams, CHAIN_SLOTS> &slot_pc,
                                 std::array<SaturatorSlotParams, CHAIN_SLOTS> &slot_sat,
                            std::array<PitchShiftSlotParams, CHAIN_SLOTS> &slot_ps)
{
    slot_type = c.chain_slot;
    slot_param = c.slot_comp;
    slot_eq    = c.slot_eq;
    slot_rv    = c.slot_rv;
    slot_dl    = c.slot_dl;
    slot_dss   = c.slot_dss;
    slot_peq   = c.slot_peq;
    slot_hilo  = c.slot_hilo;
    slot_pc    = c.slot_pc;
    slot_sat   = c.slot_sat;
    slot_ps    = c.slot_ps;
}

static void run_profiles_screen(std::array<int, CHAIN_SLOTS> &slot_type,
                                std::array<CompressorSlotParams, CHAIN_SLOTS> &slot_param,
                                std::array<EqSlotParams, CHAIN_SLOTS> &slot_eq,
                                std::array<ReverbSlotParams, CHAIN_SLOTS> &slot_rv,
                                std::array<DelaySlotParams, CHAIN_SLOTS> &slot_dl,
                                std::array<DeEsserSlotParams, CHAIN_SLOTS> &slot_dss,
                                std::array<PeqSlotParams, CHAIN_SLOTS> &slot_peq,
                                std::array<HiloSlotParams, CHAIN_SLOTS> &slot_hilo,
                                std::array<PitchCorrectSlotParams, CHAIN_SLOTS> &slot_pc,
                                std::array<SaturatorSlotParams, CHAIN_SLOTS> &slot_sat,
                            std::array<PitchShiftSlotParams, CHAIN_SLOTS> &slot_ps)
{
    int sel = 0;
    timeout(-1);
    for (;;) {
        std::vector<std::string> list = list_profile_slugs();
        if (!list.empty())
            sel = std::clamp(sel, 0, static_cast<int>(list.size()) - 1);
        else
            sel = 0;

        erase();
        int x = 2;
        attron(COLOR_PAIR(1) | A_BOLD);
        mvprintw(1, x, " Effect chain profiles ");
        attroff(A_BOLD);
        printw(" (F2 from main / slot editor)");
        attroff(COLOR_PAIR(1));
        attron(A_DIM);
        mvprintw(2, x,
                 "Up/Dn: select   Enter: load   W: save current as…   X: delete   Esc/q: close");
        attroff(A_DIM);

        int row = 4;
        if (list.empty()) {
            mvprintw(row++, x, "(No profiles yet. Press W to save the current chain.)");
        } else {
            int maxrows = std::max(3, LINES - 8);
            int top     = 0;
            if (static_cast<int>(list.size()) > maxrows) {
                top = std::clamp(sel - maxrows / 2, 0,
                                 static_cast<int>(list.size()) - maxrows);
            }
            for (int i = 0; i < maxrows && top + i < static_cast<int>(list.size()); i++) {
                int li = top + i;
                if (li == sel)
                    attron(A_REVERSE | A_BOLD);
                mvprintw(row + i, x, " %s", list[static_cast<std::size_t>(li)].c_str());
                attroff(A_REVERSE | A_BOLD);
            }
        }

        refresh();
        int ch = getch();
        if (ch == ERR)
            continue;
        if (ch == KEY_RESIZE) {
            clearok(stdscr, TRUE);
            continue;
        }
        if (ch == 27 || ch == 'q' || ch == 'Q')
            break;

        if (ch == KEY_UP && !list.empty()) {
            sel = (sel - 1 + static_cast<int>(list.size())) % static_cast<int>(list.size());
            continue;
        }
        if (ch == KEY_DOWN && !list.empty()) {
            sel = (sel + 1) % static_cast<int>(list.size());
            continue;
        }

        if ((ch == '\n' || ch == KEY_ENTER) && !list.empty()) {
            std::string path =
                profiles_directory() + "/" + list[static_cast<std::size_t>(sel)] + ".conf";
            Config c = config_snapshot_chain(slot_type, slot_param, slot_eq, slot_rv, slot_dl,
                                             slot_dss, slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
            if (load_effect_chain_profile(path, c)) {
                apply_chain_snapshot(c, slot_type, slot_param, slot_eq, slot_rv, slot_dl, slot_dss,
                                     slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                rebuild_fx_chain(slot_type.data());
            }
            continue;
        }

        if (ch == 'w' || ch == 'W') {
            echo();
            curs_set(1);
            mvprintw(LINES - 2, x, "Profile name: ");
            clrtoeol();
            char namebuf[72]{};
            move(LINES - 2, x + 14);
            refresh();
            if (mvgetnstr(LINES - 2, x + 14, namebuf, int(sizeof namebuf) - 2) != ERR
                && namebuf[0]) {
                Config snap = config_snapshot_chain(slot_type, slot_param, slot_eq, slot_rv,
                                                    slot_dl, slot_dss, slot_peq, slot_hilo, slot_pc,
                                                    slot_sat, slot_ps);
                (void)save_effect_chain_profile(namebuf, snap);
            }
            noecho();
            curs_set(0);
            continue;
        }

        if ((ch == 'x' || ch == 'X') && !list.empty()) {
            attron(COLOR_PAIR(3));
            mvprintw(LINES - 2, x, "Delete \"%s\" ?  [y] yes   other: cancel",
                     list[static_cast<std::size_t>(sel)].c_str());
            attroff(COLOR_PAIR(3));
            clrtoeol();
            refresh();
            int c2 = getch();
            if (c2 == 'y' || c2 == 'Y') {
                (void)delete_effect_chain_profile(list[static_cast<std::size_t>(sel)]);
                sel = 0;
            }
            continue;
        }
    }
}

static void run_help_screen() {
    erase();
    int y = 1, x = 2;
    attron(COLOR_PAIR(1) | A_BOLD);
    mvprintw(y++, x, " Aud10 — keyboard help ");
    attroff(A_BOLD);
    printw(" (any key closes)");
    attroff(COLOR_PAIR(1));
    y++;
    auto L = [&](const char *s) { mvprintw(y++, x, "%s", s); };
    L("Main screen");
    L("  Up/Down       Navigate fields (wraps through Start / Quit)");
    L("  Left/Right    Adjust value; on chain slots (stopped): change effect type");
    L("  Enter         Start or Stop (on button); open slot editor (on slot)");
    L("  s             Start or Stop transport (main or slot editor)");
    L("  d             Toggle focused slot on/bypass (# green=on, dim=off in chain list)");
    L("  q             Quit");
    L("");
    L("JACK — virtual bus (not wired automatically)");
    L("  Ports virtual_out_1 / virtual_out_2 mirror output_1/2 (post-FX, same gain).");
    L("  Connect them to another JACK/PipeWire client's inputs in your patchbay.");
    L("");
    L("Chain clipboard & order (only while transport is stopped)");
    L("  y             Copy focused slot (type + all parameters)");
    L("  p             Paste clipboard into focused slot");
    L("  x             Cut focused slot (copy, then clear to Empty)");
    L("  [  ]          Move slot earlier / later (swap with neighbor)");
    L("");
    L("Slot editor");
    L("  Up/Down       Previous / next parameter row");
    L("  Left/Right    Adjust value (Esc / q back to main)");
    L("  Enter         Toggle some toggles; Back finishes");
    L("  s             Start or Stop transport (same as main)");
    L("  d             Toggle this slot on/bypass (same as main chain)");
    L("  y p x [ ]     Same clipboard / move for this slot when stopped");
    L("");
    L("  ?   F1        Open this help from main or slot editor");
    L("  F2            Profiles: save/load full effect chain + all slot parameters");
    L("");
    L("Pitch correct is monophonic (one melody). Polyphonic mix confuses tracking.");
    y++;
    attron(A_DIM);
    mvprintw(std::min(LINES - 2, y + 1), x,
             "Tip: copy a tuned effect, paste into another slot, then reorder with [ ].");
    attroff(A_DIM);
    refresh();
    timeout(-1);
    (void)getch();
}

int main() {
    // ── open JACK client ──
    jack_status_t jstat;
    g_client = jack_client_open("aud10-suite", JackNoStartServer, &jstat);
    if (!g_client) {
        std::fprintf(stderr, "Cannot connect to JACK/PipeWire (0x%x).\n", jstat);
        std::fprintf(stderr, "Make sure PipeWire is running.\n");
        return 1;
    }

    g_sr.store(jack_get_sample_rate(g_client), std::memory_order_relaxed);
    g_bufsz.store(jack_get_buffer_size(g_client), std::memory_order_relaxed);

    g_in[0]  = jack_port_register(g_client, "input_1",  JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput,  0);
    g_in[1]  = jack_port_register(g_client, "input_2",  JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput,  0);
    g_out[0] = jack_port_register(g_client, "output_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_out[1] = jack_port_register(g_client, "output_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_virt_out[0] =
        jack_port_register(g_client, "virtual_out_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_virt_out[1] =
        jack_port_register(g_client, "virtual_out_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!g_virt_out[0] || !g_virt_out[1])
        std::fprintf(stderr,
                     "aud10-suite: could not register virtual_out_1/2 (virtual bus disabled).\n");

    jack_set_process_callback(g_client, jack_process_cb, nullptr);
    jack_set_buffer_size_callback(g_client, jack_bufsz_cb, nullptr);
    jack_set_xrun_callback(g_client, jack_xrun_cb, nullptr);
    jack_on_shutdown(g_client, jack_shutdown_cb, nullptr);

    if (jack_activate(g_client)) {
        std::fprintf(stderr, "Failed to activate JACK client.\n");
        jack_client_close(g_client);
        return 1;
    }

    auto captures  = enum_ports(JackPortIsOutput);
    auto playbacks = enum_ports(JackPortIsInput);
    captures.insert(captures.begin(),   {"", "None"});
    playbacks.insert(playbacks.begin(), {"", "None"});

    // ── load config ──
    std::string cfg_path = config_path();
    Config cfg = load_config(cfg_path);

    int ci  = cfg.channel_idx;
    int bi  = cfg.buffer_idx;
    int vi  = cfg.vol_in;
    int vo  = cfg.vol_out;
    int pil = cfg.in_port_l.empty()  ? (captures.size()  > 1 ? 1 : 0)
                                     : find_port_idx(captures, cfg.in_port_l);
    int pir = cfg.in_port_r.empty()  ? 0 : find_port_idx(captures,  cfg.in_port_r, 0);
    int qol = cfg.out_port_l.empty() ? (playbacks.size() > 1 ? 1 : 0)
                                     : find_port_idx(playbacks, cfg.out_port_l);
    int qor = cfg.out_port_r.empty() ? 0 : find_port_idx(playbacks, cfg.out_port_r, 0);
    int cur = F_IN_L;

    std::array<CompressorSlotParams, CHAIN_SLOTS> slot_param = cfg.slot_comp;
    std::array<EqSlotParams, CHAIN_SLOTS>        slot_eq    = cfg.slot_eq;
    std::array<ReverbSlotParams, CHAIN_SLOTS>    slot_rv    = cfg.slot_rv;
    std::array<DelaySlotParams, CHAIN_SLOTS>     slot_dl    = cfg.slot_dl;
    std::array<DeEsserSlotParams, CHAIN_SLOTS>   slot_dss   = cfg.slot_dss;
    std::array<PeqSlotParams, CHAIN_SLOTS>       slot_peq   = cfg.slot_peq;
    std::array<HiloSlotParams, CHAIN_SLOTS>      slot_hilo  = cfg.slot_hilo;
    std::array<PitchCorrectSlotParams, CHAIN_SLOTS> slot_pc  = cfg.slot_pc;
    std::array<SaturatorSlotParams, CHAIN_SLOTS>     slot_sat = cfg.slot_sat;
    std::array<PitchShiftSlotParams, CHAIN_SLOTS>    slot_ps  = cfg.slot_ps;
    std::array<int, CHAIN_SLOTS> slot_type = cfg.chain_slot;
    rebuild_fx_chain(slot_type.data());

    int           edit_slot = -1;
    SlotEditField se_cur    = SE_TYPE;

    jack_set_buffer_size(g_client, JACK_BUFSIZES[bi]);

    int app_exit = 0;
    std::thread ui_thread([&]() {
    // ── ncurses (dedicated UI thread; never call from JACK callback) ──
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN,    -1);
        init_pair(2, COLOR_GREEN,   -1);
        init_pair(3, COLOR_RED,     -1);
        init_pair(4, COLOR_YELLOW,  -1);
        init_pair(5, COLOR_MAGENTA, -1);
        /* dim white reads as gray for chain “off” # marker */
        init_pair(6, COLOR_WHITE,   -1);
    }

    auto shutdown = [&]() {
        cfg.channel_idx      = ci;
        cfg.buffer_idx       = bi;
        cfg.vol_in           = vi;
        cfg.vol_out          = vo;
        cfg.in_port_l        = captures[pil].name;
        cfg.in_port_r        = captures[pir].name;
        cfg.out_port_l       = playbacks[qol].name;
        cfg.out_port_r       = playbacks[qor].name;
        cfg.chain_slot       = slot_type;
        cfg.slot_comp        = slot_param;
        cfg.slot_eq          = slot_eq;
        cfg.slot_rv          = slot_rv;
        cfg.slot_dl          = slot_dl;
        cfg.slot_dss         = slot_dss;
        cfg.slot_peq         = slot_peq;
        cfg.slot_hilo        = slot_hilo;
        cfg.slot_pc          = slot_pc;
        cfg.slot_sat         = slot_sat;
        cfg.slot_ps          = slot_ps;
        cfg.comp_enabled     = slot_param[0].enabled;
        cfg.comp_threshold   = slot_param[0].threshold;
        cfg.comp_ratio_idx   = slot_param[0].ratio_idx;
        cfg.comp_attack_idx  = slot_param[0].attack_idx;
        cfg.comp_release_idx = slot_param[0].release_idx;
        cfg.comp_knee_idx    = slot_param[0].knee_idx;
        cfg.comp_makeup      = slot_param[0].makeup;
        save_config(cfg_path, cfg);

        g_active = false;
        disconnect_all();
        jack_deactivate(g_client);
        jack_client_close(g_client);
        g_client = nullptr;
        endwin();
    };

    constexpr int kMinH = CHAIN_SLOTS + 24;
    constexpr int kMinW = 66;

    static float dsp_peak_display = 0.f;

    for (;;) {
        // ── sync RT parameters ──
        g_vol_in.store(std::pow(10.0f, float(vi) / 20.0f), std::memory_order_relaxed);
        g_vol_out.store(std::pow(10.0f, float(vo) / 20.0f), std::memory_order_relaxed);
        for (int i = 0; i < CHAIN_SLOTS; i++) {
            if (!g_fx_slots[i]) continue;
            int t = slot_type[static_cast<std::size_t>(i)];
            if (t == FX_COMP) {
                auto *x = static_cast<Compressor *>(g_fx_slots[i].get());
                const auto &p = slot_param[static_cast<std::size_t>(i)];
                x->enabled.store(p.enabled, std::memory_order_relaxed);
                x->threshold.store(float(p.threshold), std::memory_order_relaxed);
                x->ratio.store(COMP_RATIOS[p.ratio_idx], std::memory_order_relaxed);
                x->attack_ms.store(COMP_ATTACKS[p.attack_idx], std::memory_order_relaxed);
                x->release_ms.store(COMP_RELEASES[p.release_idx], std::memory_order_relaxed);
                x->knee.store(COMP_KNEES[p.knee_idx], std::memory_order_relaxed);
                x->makeup.store(float(p.makeup), std::memory_order_relaxed);
            } else if (t == FX_EQ) {
                auto *x = static_cast<GraphicEQ *>(g_fx_slots[i].get());
                const auto &e = slot_eq[static_cast<std::size_t>(i)];
                x->enabled.store(e.enabled, std::memory_order_relaxed);
                x->low_db.store(float(e.low_db), std::memory_order_relaxed);
                x->mid_db.store(float(e.mid_db), std::memory_order_relaxed);
                x->high_db.store(float(e.high_db), std::memory_order_relaxed);
            } else if (t == FX_REV) {
                auto *x = static_cast<ReverbFX *>(g_fx_slots[i].get());
                const auto &r = slot_rv[static_cast<std::size_t>(i)];
                x->enabled.store(r.enabled, std::memory_order_relaxed);
                x->room.store(std::clamp(r.room / 100.0f, 0.0f, 0.99f),
                              std::memory_order_relaxed);
                x->damp.store(std::clamp(r.damp / 100.0f, 0.0f, 0.99f),
                              std::memory_order_relaxed);
                x->wet.store(std::clamp(r.wet / 100.0f, 0.0f, 1.0f),
                             std::memory_order_relaxed);
            } else if (t == FX_DLY) {
                auto *x = static_cast<DelayFX *>(g_fx_slots[i].get());
                const auto &d = slot_dl[static_cast<std::size_t>(i)];
                x->enabled.store(d.enabled, std::memory_order_relaxed);
                x->time_ms.store(float(d.time_ms), std::memory_order_relaxed);
                x->feedback.store(std::clamp(d.feedback / 100.0f, 0.0f, 0.95f),
                                  std::memory_order_relaxed);
                x->wet.store(std::clamp(d.wet / 100.0f, 0.0f, 1.0f),
                             std::memory_order_relaxed);
            } else if (t == FX_DESS) {
                auto *x = static_cast<DeesserFX *>(g_fx_slots[i].get());
                const auto &d = slot_dss[static_cast<std::size_t>(i)];
                x->enabled.store(d.enabled, std::memory_order_relaxed);
                x->center_hz.store(DEESS_FREQ_HZ[d.freq_idx], std::memory_order_relaxed);
                x->threshold_db.store(float(d.threshold), std::memory_order_relaxed);
                x->amount.store(std::clamp(d.amount / 100.0f, 0.0f, 1.0f),
                                std::memory_order_relaxed);
                x->attack_ms.store(COMP_ATTACKS[d.attack_idx], std::memory_order_relaxed);
                x->release_ms.store(COMP_RELEASES[d.release_idx], std::memory_order_relaxed);
            } else if (t == FX_PEQ) {
                auto *x = static_cast<ParametricEQ *>(g_fx_slots[i].get());
                const auto &p = slot_peq[static_cast<std::size_t>(i)];
                x->enabled.store(p.enabled, std::memory_order_relaxed);
                x->freq_hz.store(PEQ_FREQ_HZ[p.freq_idx], std::memory_order_relaxed);
                x->Q.store(PEQ_Q_VALS[p.q_idx], std::memory_order_relaxed);
                x->gain_db.store(float(p.gain_db), std::memory_order_relaxed);
            } else if (t == FX_HILO) {
                auto *x = static_cast<HiLoPassFX *>(g_fx_slots[i].get());
                const auto &h = slot_hilo[static_cast<std::size_t>(i)];
                x->enabled.store(h.hpf_enabled || h.lpf_enabled, std::memory_order_relaxed);
                x->hpf_on.store(h.hpf_enabled, std::memory_order_relaxed);
                x->lpf_on.store(h.lpf_enabled, std::memory_order_relaxed);
                x->hpf_hz.store(HPF_FREQ_HZ[h.hpf_hz_idx], std::memory_order_relaxed);
                x->lpf_hz.store(LPF_FREQ_HZ[h.lpf_hz_idx], std::memory_order_relaxed);
            } else if (t == FX_PCOR) {
                auto *x = static_cast<PitchCorrectFX *>(g_fx_slots[i].get());
                const auto &p = slot_pc[static_cast<std::size_t>(i)];
                x->enabled.store(p.enabled, std::memory_order_relaxed);
                x->wet.store(std::clamp(p.wet / 100.0f, 0.0f, 1.0f),
                             std::memory_order_relaxed);
                x->speed_ms.store(25.0f + float(p.speed) * 4.75f, std::memory_order_relaxed);
                x->key_root.store(p.key_root, std::memory_order_relaxed);
                x->scale_.store(p.scale_idx, std::memory_order_relaxed);
                x->pull_semi.store(0.2f + float(p.pull) / 100.0f * 3.3f,
                                   std::memory_order_relaxed);
                x->low_latency_x.store(p.low_latency_x, std::memory_order_relaxed);
                if (p.low_latency_x) {
                    int iy = std::clamp(p.exp_yin_i, 0, PCOR_EXP_YIN_N - 1);
                    int id = std::clamp(p.exp_det_i, 0, PCOR_EXP_DETECT_N - 1);
                    int iw = std::clamp(p.exp_warm_i, 0, PCOR_EXP_WARMUP_N - 1);
                    int ig = std::clamp(p.exp_gate_i, 0, PCOR_EXP_YIN_GATE_N - 1);
                    int iln = std::clamp(p.exp_lmin_i, 0, PCOR_EXP_LAG_MIN_N - 1);
                    int ilx = std::clamp(p.exp_lmax_i, 0, PCOR_EXP_LAG_MAX_N - 1);
                    x->exp_yin_samples.store(PCOR_EXP_YIN_SAMPLES[iy],
                                             std::memory_order_relaxed);
                    x->exp_detect_period.store(PCOR_EXP_DETECT_PER[id],
                                               std::memory_order_relaxed);
                    x->exp_warmup_smps.store(PCOR_EXP_WARMUP_SMPS[iw],
                                             std::memory_order_relaxed);
                    x->exp_yin_gate_smps.store(PCOR_EXP_YIN_GATE_SMPS[ig],
                                               std::memory_order_relaxed);
                    x->exp_lag_min.store(float(PCOR_EXP_LAG_MIN_SMPS[iln]),
                                         std::memory_order_relaxed);
                    x->exp_lag_max.store(float(PCOR_EXP_LAG_MAX_SMPS[ilx]),
                                         std::memory_order_relaxed);
                }
            } else if (t == FX_SAT) {
                auto *x = static_cast<SaturatorFX *>(g_fx_slots[i].get());
                const auto &s = slot_sat[static_cast<std::size_t>(i)];
                x->enabled.store(s.enabled, std::memory_order_relaxed);
                float g = 0.25f + float(s.drive) / 100.0f * 13.75f;
                x->drive.store(g, std::memory_order_relaxed);
                x->mix.store(std::clamp(float(s.mix) / 100.0f, 0.0f, 1.0f),
                             std::memory_order_relaxed);
            } else if (t == FX_PSHIFT) {
                auto *x = static_cast<PitchShiftFX *>(g_fx_slots[i].get());
                const auto &p = slot_ps[static_cast<std::size_t>(i)];
                x->enabled.store(p.enabled, std::memory_order_relaxed);
                x->semitones_idx.store(
                    std::clamp(p.semi_idx, 0, PS_SEMI_STEPS - 1), std::memory_order_relaxed);
                x->wet_mix.store(std::clamp(float(p.wet) / 100.0f, 0.0f, 1.0f),
                                 std::memory_order_relaxed);
            }
        }

        unsigned actual_buf = g_bufsz.load(std::memory_order_relaxed);
        for (int i = 0; i < alen(JACK_BUFSIZES); i++)
            if (JACK_BUFSIZES[i] == actual_buf) { bi = i; break; }

        bool live   = g_active;
        bool ok     = g_jack_ok;
        bool stereo = CHANNEL_OPTS[ci] > 1;
        unsigned sr = g_sr.load(std::memory_order_relaxed);

        if (ok && g_client)
            poll_jack_port_latencies();

        // ── slot parameter editor (full window) ──
        if (edit_slot >= 0) {
            const int es = edit_slot;
            int st = std::clamp(slot_type[static_cast<std::size_t>(es)], 0, SLOT_TYPES - 1);
            bool      empty = (st == FX_NONE);
            int       fx    = st;
            auto     &cp    = slot_param[static_cast<std::size_t>(es)];
            auto     &eq    = slot_eq[static_cast<std::size_t>(es)];
            auto     &rv    = slot_rv[static_cast<std::size_t>(es)];
            auto     &dl    = slot_dl[static_cast<std::size_t>(es)];
            auto     &ds    = slot_dss[static_cast<std::size_t>(es)];
            auto     &pq    = slot_peq[static_cast<std::size_t>(es)];
            auto     &hl    = slot_hilo[static_cast<std::size_t>(es)];
            auto     &pc    = slot_pc[static_cast<std::size_t>(es)];
            auto     &sat   = slot_sat[static_cast<std::size_t>(es)];
            auto     &psh   = slot_ps[static_cast<std::size_t>(es)];

            if (st == FX_PCOR)
                pc_clamp_pc_exp_cursor(se_cur, pc.low_latency_x);

            erase();
            int ex0 = 1, ey0 = 0;
            int eW  = std::min(COLS - 2, 74);
            attron(COLOR_PAIR(1) | A_BOLD);
            mvprintw(ey0, ex0, " Slot %d ", es + 1);
            attroff(A_BOLD);
            if (st == FX_NONE) {
                printw("— Empty");
            } else {
                bool on = slot_effect_is_on(st, static_cast<std::size_t>(es), slot_param, slot_eq,
                                            slot_rv, slot_dl, slot_dss, slot_peq, slot_hilo, slot_pc,
                                            slot_sat, slot_ps);
                printw("— ");
                if (has_colors()) {
                    if (on)
                        attron(COLOR_PAIR(2));
                    else {
                        attron(COLOR_PAIR(6));
                        attron(A_DIM);
                    }
                    addch('#');
                    attroff(A_DIM | COLOR_PAIR(2) | COLOR_PAIR(6));
                } else {
                    if (!on)
                        attron(A_DIM);
                    addch('#');
                    attroff(A_DIM);
                }
                attron(COLOR_PAIR(1));
                printw(" %s", SLOT_TYPE_NAMES[st]);
            }
            attroff(COLOR_PAIR(1));
            attron(A_DIM);
            mvprintw(ey0 + 1, ex0, "%s",
                     live ? "Effect type locked while running (params still editable)"
                          : "L/R: values  Esc/q: back  F2: profiles  S/D: transport  y/p/x/[ ]  ?: help");
            attroff(A_DIM);

            int ly  = ey0 + 3;
            int evx = ex0 + 22;
            int eVW = std::max(22, eW - 26);

            auto row_ed = [&](int y, SlotEditField f, const char *label, const char *val,
                              bool editable) {
                bool sel = se_cur == f;
                if (sel)            attron(A_REVERSE | A_BOLD);
                else if (!editable) attron(A_DIM);
                mvprintw(y, ex0, "%-18s", label);
                if (sel && editable) {
                    attroff(A_REVERSE | A_BOLD);
                    attron(COLOR_PAIR(4));
                    mvprintw(y, evx - 2, "< ");
                    attroff(COLOR_PAIR(4));
                    attron(A_REVERSE | A_BOLD);
                }
                mvprintw(y, evx, "%-*s", eVW, val);
                if (sel && editable) {
                    attroff(A_REVERSE | A_BOLD);
                    attron(COLOR_PAIR(4));
                    printw(" >");
                    attroff(COLOR_PAIR(4));
                }
                attroff(A_REVERSE | A_BOLD | A_DIM);
            };

            char etmp[64];
            row_ed(ly++, SE_TYPE, "Effect type", SLOT_TYPE_NAMES[st], !live);

            if (st == FX_COMP) {
                row_ed(ly++, SE_C_EN, "Enabled", cp.enabled ? "ON" : "OFF", true);
                std::snprintf(etmp, sizeof etmp, "%d dB", cp.threshold);
                row_ed(ly++, SE_C_THRESH, "Threshold", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%.1f:1", COMP_RATIOS[cp.ratio_idx]);
                row_ed(ly++, SE_C_RATIO, "Ratio", etmp, true);
                if (COMP_ATTACKS[cp.attack_idx] < 1.0f)
                    std::snprintf(etmp, sizeof etmp, "%.1f ms",
                                  COMP_ATTACKS[cp.attack_idx]);
                else
                    std::snprintf(etmp, sizeof etmp, "%.0f ms",
                                  COMP_ATTACKS[cp.attack_idx]);
                row_ed(ly++, SE_C_ATTACK, "Attack", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%.0f ms",
                              COMP_RELEASES[cp.release_idx]);
                row_ed(ly++, SE_C_RELEASE, "Release", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%.0f dB", COMP_KNEES[cp.knee_idx]);
                row_ed(ly++, SE_C_KNEE, "Knee", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%+d dB", cp.makeup);
                row_ed(ly++, SE_C_MAKEUP, "Makeup gain", etmp, true);
            } else if (st == FX_EQ) {
                row_ed(ly++, SE_EQ_EN, "Enabled", eq.enabled ? "ON" : "OFF", true);
                std::snprintf(etmp, sizeof etmp, "%+d dB (low)", eq.low_db);
                row_ed(ly++, SE_EQ_LOW, "Low ~180 Hz", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%+d dB (mid)", eq.mid_db);
                row_ed(ly++, SE_EQ_MID, "Mid ~1.2 kHz", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%+d dB (high)", eq.high_db);
                row_ed(ly++, SE_EQ_HIGH, "High ~6.5 kHz", etmp, true);
            } else if (st == FX_REV) {
                row_ed(ly++, SE_RV_EN, "Enabled", rv.enabled ? "ON" : "OFF", true);
                std::snprintf(etmp, sizeof etmp, "%d%%", rv.room);
                row_ed(ly++, SE_RV_ROOM, "Room / decay", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%d%%", rv.damp);
                row_ed(ly++, SE_RV_DAMP, "Damping (HF)", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%d%%", rv.wet);
                row_ed(ly++, SE_RV_WET, "Wet mix", etmp, true);
            } else if (st == FX_DLY) {
                row_ed(ly++, SE_DL_EN, "Enabled", dl.enabled ? "ON" : "OFF", true);
                std::snprintf(etmp, sizeof etmp, "%d ms", dl.time_ms);
                row_ed(ly++, SE_DL_TIME, "Delay time", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%d%%", dl.feedback);
                row_ed(ly++, SE_DL_FB, "Feedback", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%d%%", dl.wet);
                row_ed(ly++, SE_DL_WET, "Wet mix", etmp, true);
            } else if (st == FX_DESS) {
                row_ed(ly++, SE_DS_EN, "Enabled", ds.enabled ? "ON" : "OFF", true);
                std::snprintf(etmp, sizeof etmp, "%.0f Hz", DEESS_FREQ_HZ[ds.freq_idx]);
                row_ed(ly++, SE_DS_FREQ, "Sib band center", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%d dB", ds.threshold);
                row_ed(ly++, SE_DS_THRESH, "Detect threshold", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%d%%", ds.amount);
                row_ed(ly++, SE_DS_AMT, "Reduction", etmp, true);
                if (COMP_ATTACKS[ds.attack_idx] < 1.0f)
                    std::snprintf(etmp, sizeof etmp, "%.1f ms",
                                  COMP_ATTACKS[ds.attack_idx]);
                else
                    std::snprintf(etmp, sizeof etmp, "%.0f ms",
                                  COMP_ATTACKS[ds.attack_idx]);
                row_ed(ly++, SE_DS_ATTACK, "Attack", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%.0f ms",
                              COMP_RELEASES[ds.release_idx]);
                row_ed(ly++, SE_DS_RELEASE, "Release", etmp, true);
            } else if (st == FX_PEQ) {
                row_ed(ly++, SE_PQ_EN, "Enabled", pq.enabled ? "ON" : "OFF", true);
                std::snprintf(etmp, sizeof etmp, "%.0f Hz", PEQ_FREQ_HZ[pq.freq_idx]);
                row_ed(ly++, SE_PQ_FREQ, "Frequency", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%.2f", PEQ_Q_VALS[pq.q_idx]);
                row_ed(ly++, SE_PQ_Q, "Q", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%+d dB", pq.gain_db);
                row_ed(ly++, SE_PQ_GAIN, "Gain", etmp, true);
            } else if (st == FX_HILO) {
                row_ed(ly++, SE_HL_HPEN, "High-pass", hl.hpf_enabled ? "ON" : "OFF", true);
                std::snprintf(etmp, sizeof etmp, "%.0f Hz", HPF_FREQ_HZ[hl.hpf_hz_idx]);
                row_ed(ly++, SE_HL_HPF, "HPF corner", etmp, true);
                row_ed(ly++, SE_HL_LPEN, "Low-pass", hl.lpf_enabled ? "ON" : "OFF", true);
                std::snprintf(etmp, sizeof etmp, "%.0f Hz", LPF_FREQ_HZ[hl.lpf_hz_idx]);
                row_ed(ly++, SE_HL_LPF, "LPF corner", etmp, true);
            } else if (st == FX_PCOR) {
                row_ed(ly++, SE_PC_EN, "Enabled", pc.enabled ? "ON" : "OFF", true);
                std::snprintf(etmp, sizeof etmp, "%d%%", pc.wet);
                row_ed(ly++, SE_PC_WET, "Correct mix", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%.0f ms",
                              25.0f + float(pc.speed) * 4.75f);
                row_ed(ly++, SE_PC_SPD, "Correction speed", etmp, true);
                row_ed(ly++, SE_PC_KEY, "Key", NOTE_NAMES[pc.key_root], true);
                row_ed(ly++, SE_PC_SCALE, "Scale", PCOR_SCALE_NAMES[pc.scale_idx], true);
                std::snprintf(etmp, sizeof etmp, "±%.2f st",
                              0.2f + float(pc.pull) / 100.0f * 3.3f);
                row_ed(ly++, SE_PC_PULL, "Max pull", etmp, true);
                row_ed(ly++, SE_PC_LOWLAT_X, "Exp. low latency",
                       pc.low_latency_x ? "ON  (extra knobs below)" : "OFF (default)", true);
                if (pc.low_latency_x) {
                    int iy = std::clamp(pc.exp_yin_i, 0, PCOR_EXP_YIN_N - 1);
                    std::snprintf(etmp, sizeof etmp, "%u smp", PCOR_EXP_YIN_SAMPLES[iy]);
                    row_ed(ly++, SE_PC_EXP_YIN, "  YIN window", etmp, true);
                    int id = std::clamp(pc.exp_det_i, 0, PCOR_EXP_DETECT_N - 1);
                    std::snprintf(etmp, sizeof etmp, "every %u smp", PCOR_EXP_DETECT_PER[id]);
                    row_ed(ly++, SE_PC_EXP_DET, "  Pitch detect", etmp, true);
                    int iw = std::clamp(pc.exp_warm_i, 0, PCOR_EXP_WARMUP_N - 1);
                    std::snprintf(etmp, sizeof etmp, "%u smp", PCOR_EXP_WARMUP_SMPS[iw]);
                    row_ed(ly++, SE_PC_EXP_WARM, "  Warm-up", etmp, true);
                    int ig = std::clamp(pc.exp_gate_i, 0, PCOR_EXP_YIN_GATE_N - 1);
                    std::snprintf(etmp, sizeof etmp, "%u smp", PCOR_EXP_YIN_GATE_SMPS[ig]);
                    row_ed(ly++, SE_PC_EXP_GATE, "  Detect gate", etmp, true);
                    int iln = std::clamp(pc.exp_lmin_i, 0, PCOR_EXP_LAG_MIN_N - 1);
                    std::snprintf(etmp, sizeof etmp, "%u fr", PCOR_EXP_LAG_MIN_SMPS[iln]);
                    row_ed(ly++, SE_PC_EXP_LMIN, "  Read lag min", etmp, true);
                    int ilx = std::clamp(pc.exp_lmax_i, 0, PCOR_EXP_LAG_MAX_N - 1);
                    std::snprintf(etmp, sizeof etmp, "%u fr", PCOR_EXP_LAG_MAX_SMPS[ilx]);
                    row_ed(ly++, SE_PC_EXP_LMAX, "  Read lag max", etmp, true);
                }
            } else if (st == FX_SAT) {
                row_ed(ly++, SE_SAT_EN, "Enabled", sat.enabled ? "ON" : "OFF", true);
                std::snprintf(etmp, sizeof etmp, "%d%%", sat.drive);
                row_ed(ly++, SE_SAT_DRIVE, "Drive (into tanh)", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%d%% wet", sat.mix);
                row_ed(ly++, SE_SAT_MIX, "Saturation mix", etmp, true);
            } else if (st == FX_PSHIFT) {
                row_ed(ly++, SE_PS_EN, "Enabled", psh.enabled ? "ON" : "OFF", true);
                int si = std::clamp(psh.semi_idx, 0, PS_SEMI_STEPS - 1);
                int st_semi = si - PS_SEMI_CENTER;
                std::snprintf(etmp, sizeof etmp, "%+d st", st_semi);
                row_ed(ly++, SE_PS_SEMI, "Semitones", etmp, true);
                std::snprintf(etmp, sizeof etmp, "%d%% wet", psh.wet);
                row_ed(ly++, SE_PS_WET, "Wet mix", etmp, true);
            }

            if (st == FX_PCOR && g_fx_slots[es] && g_fx_slots[es]->effect_kind() == FX_PCOR) {
                auto *pc_fx = static_cast<PitchCorrectFX *>(g_fx_slots[es].get());
                ly++;
                attron(COLOR_PAIR(2) | A_BOLD);
                mvprintw(ly++, ex0, " Pitch tracker ");
                attroff(A_BOLD | COLOR_PAIR(2));
                if (live && pc.enabled) {
                    float dh = pc_fx->meter_detect_hz.load(std::memory_order_relaxed);
                    float th = pc_fx->meter_target_hz.load(std::memory_order_relaxed);
                    char ln_a[72], ln_b[72];
                    if (dh > 1.0f) {
                        fmt_pitch_readout(ln_a, sizeof ln_a, dh);
                        mvprintw(ly++, ex0, "  Detected:  %s", ln_a);
                        if (th > 1.0f) {
                            fmt_pitch_readout(ln_b, sizeof ln_b, th);
                            mvprintw(ly++, ex0, "  Target:    %s", ln_b);
                            float cents = 1200.0f * std::log2(th / dh);
                            std::snprintf(ln_a, sizeof ln_a, "%+.0f cents", cents);
                            mvprintw(ly++, ex0, "  Delta:     %s", ln_a);
                        } else {
                            attron(A_DIM);
                            mvprintw(ly++, ex0, "  Target:    —");
                            attroff(A_DIM);
                        }
                    } else {
                        attron(A_DIM);
                        mvprintw(ly++, ex0, "  Detected:  (no stable pitch yet)");
                        mvprintw(ly++, ex0, "  Target:    —");
                        attroff(A_DIM);
                    }
                } else if (live && !pc.enabled) {
                    attron(A_DIM);
                    mvprintw(ly++, ex0, "  Enable this slot to run the detector.");
                    attroff(A_DIM);
                } else {
                    float dh = pc_fx->meter_detect_hz.load(std::memory_order_relaxed);
                    float th = pc_fx->meter_target_hz.load(std::memory_order_relaxed);
                    attron(A_DIM);
                    mvprintw(ly++, ex0, "  Start transport for live readout.");
                    attroff(A_DIM);
                    if (dh > 1.0f) {
                        char ln_a[72], ln_b[72];
                        fmt_pitch_readout(ln_a, sizeof ln_a, dh);
                        attron(A_DIM);
                        mvprintw(ly++, ex0, "  Last:      %s", ln_a);
                        if (th > 1.0f) {
                            fmt_pitch_readout(ln_b, sizeof ln_b, th);
                            mvprintw(ly++, ex0, "  Last tgt:  %s", ln_b);
                        }
                        attroff(A_DIM);
                    }
                }
            }

            ly++;
            if (se_cur == SE_BACK) attron(A_REVERSE | A_BOLD);
            attron(COLOR_PAIR(2));
            mvprintw(ly, ex0, " [ Done — back to main ] ");
            attroff(A_REVERSE | A_BOLD | COLOR_PAIR(2));

            if (live) {
                ly += 2;
                int mw = eW - 6;
                if (mw > 8 && g_gr_meter) {
                    float gr = g_gr_meter->gr_db.load(std::memory_order_relaxed);
                    bool  ge = g_gr_meter->enabled.load(std::memory_order_relaxed);
                    int   grf = ge ? std::clamp(int(-gr / 24.0f * mw), 0, mw) : 0;
                    mvprintw(ly, ex0, "GR (last compressor in chain): ");
                    for (int i = 0; i < mw; i++) {
                        if (i < grf) {
                            attron(COLOR_PAIR(5));
                            addch(ACS_BLOCK);
                            attroff(COLOR_PAIR(5));
                        } else {
                            attron(A_DIM);
                            addch(ACS_BULLET);
                            attroff(A_DIM);
                        }
                    }
                    std::snprintf(etmp, sizeof etmp, ge ? " %.0f dB" : " -- ", gr);
                    addstr(etmp);
                }
            }

            refresh();
            timeout(live ? 50 : -1);
            int ech = getch();
            if (ech == ERR) continue;
            if (ech == KEY_RESIZE) {
                clearok(stdscr, TRUE);
                continue;
            }
            if (ech == 27 || ech == 'q' || ech == 'Q') {
                edit_slot = -1;
                continue;
            }
            if (ech == '?' || ech == KEY_F(1)) {
                run_help_screen();
                continue;
            }
            if (ech == KEY_F(2)) {
                run_profiles_screen(slot_type, slot_param, slot_eq, slot_rv, slot_dl, slot_dss,
                                    slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                continue;
            }
            if (ech == 's' || ech == 'S') {
                if (live) {
                    g_active = false;
                    disconnect_all();
                } else if (ok && (pil > 0 || pir > 0) && (qol > 0 || qor > 0)) {
                    rebuild_fx_chain(slot_type.data());
                    for (int k = 0; k < g_rt_n; k++)
                        g_rt_fx[k]->reset();
                    g_channels.store(CHANNEL_OPTS[ci], std::memory_order_relaxed);
                    connect_ports(CHANNEL_OPTS[ci],
                                  captures[pil].name,  captures[pir].name,
                                  playbacks[qol].name, playbacks[qor].name);
                    g_active = true;
                }
                continue;
            }
            if (ech == 'd' || ech == 'D') {
                int tst = std::clamp(slot_type[static_cast<std::size_t>(es)], 0, SLOT_TYPES - 1);
                if (tst != FX_NONE) {
                    slot_toggle_effect_enabled(tst, static_cast<std::size_t>(es), slot_param, slot_eq,
                                               slot_rv, slot_dl, slot_dss, slot_peq, slot_hilo,
                                               slot_pc, slot_sat, slot_ps);
                }
                continue;
            }

            switch (ech) {
            case KEY_UP:
                se_advance(se_cur, fx, empty, -1, st == FX_PCOR && pc.low_latency_x);
                break;
            case KEY_DOWN:
                se_advance(se_cur, fx, empty, 1, st == FX_PCOR && pc.low_latency_x);
                break;

            case KEY_LEFT:
                switch (se_cur) {
                case SE_TYPE:
                    if (!live) {
                        slot_type[static_cast<std::size_t>(es)] =
                            wrap(slot_type[static_cast<std::size_t>(es)], -1, SLOT_TYPES);
                        rebuild_fx_chain(slot_type.data());
                        st   = slot_type[static_cast<std::size_t>(es)];
                        empty = (st == FX_NONE);
                        fx   = st;
                        se_cur = SE_TYPE;
                    }
                    break;
                case SE_C_EN:      cp.enabled = !cp.enabled; break;
                case SE_C_THRESH:  cp.threshold = std::max(-60, cp.threshold - 1); break;
                case SE_C_RATIO:   cp.ratio_idx = wrap(cp.ratio_idx, -1, alen(COMP_RATIOS)); break;
                case SE_C_ATTACK:  cp.attack_idx = wrap(cp.attack_idx, -1, alen(COMP_ATTACKS)); break;
                case SE_C_RELEASE: cp.release_idx = wrap(cp.release_idx, -1, alen(COMP_RELEASES)); break;
                case SE_C_KNEE:    cp.knee_idx = wrap(cp.knee_idx, -1, alen(COMP_KNEES)); break;
                case SE_C_MAKEUP:  cp.makeup = std::max(-12, cp.makeup - 1); break;
                case SE_EQ_EN:     eq.enabled = !eq.enabled; break;
                case SE_EQ_LOW:    eq.low_db  = std::max(-12, eq.low_db - 1); break;
                case SE_EQ_MID:    eq.mid_db  = std::max(-12, eq.mid_db - 1); break;
                case SE_EQ_HIGH:   eq.high_db = std::max(-12, eq.high_db - 1); break;
                case SE_RV_EN:     rv.enabled = !rv.enabled; break;
                case SE_RV_ROOM:   rv.room = std::max(0, rv.room - 1); break;
                case SE_RV_DAMP:   rv.damp = std::max(0, rv.damp - 1); break;
                case SE_RV_WET:    rv.wet  = std::max(0, rv.wet - 1); break;
                case SE_DL_EN:     dl.enabled = !dl.enabled; break;
                case SE_DL_TIME:   dl.time_ms = std::max(1, dl.time_ms - 5); break;
                case SE_DL_FB:     dl.feedback = std::max(0, dl.feedback - 1); break;
                case SE_DL_WET:    dl.wet = std::max(0, dl.wet - 1); break;
                case SE_DS_EN:     ds.enabled = !ds.enabled; break;
                case SE_DS_FREQ:   ds.freq_idx = wrap(ds.freq_idx, -1, alen(DEESS_FREQ_HZ)); break;
                case SE_DS_THRESH: ds.threshold = std::max(-50, ds.threshold - 1); break;
                case SE_DS_AMT:    ds.amount = std::max(0, ds.amount - 1); break;
                case SE_DS_ATTACK: ds.attack_idx = wrap(ds.attack_idx, -1, alen(COMP_ATTACKS)); break;
                case SE_DS_RELEASE: ds.release_idx = wrap(ds.release_idx, -1, alen(COMP_RELEASES)); break;
                case SE_PQ_EN:     pq.enabled = !pq.enabled; break;
                case SE_PQ_FREQ:   pq.freq_idx = wrap(pq.freq_idx, -1, alen(PEQ_FREQ_HZ)); break;
                case SE_PQ_Q:      pq.q_idx = wrap(pq.q_idx, -1, alen(PEQ_Q_VALS)); break;
                case SE_PQ_GAIN:   pq.gain_db = std::max(-12, pq.gain_db - 1); break;
                case SE_HL_HPEN:   hl.hpf_enabled = !hl.hpf_enabled; break;
                case SE_HL_HPF:    hl.hpf_hz_idx = wrap(hl.hpf_hz_idx, -1, alen(HPF_FREQ_HZ)); break;
                case SE_HL_LPEN:   hl.lpf_enabled = !hl.lpf_enabled; break;
                case SE_HL_LPF:    hl.lpf_hz_idx = wrap(hl.lpf_hz_idx, -1, alen(LPF_FREQ_HZ)); break;
                case SE_PC_EN:     pc.enabled = !pc.enabled; break;
                case SE_PC_WET:    pc.wet = std::max(0, pc.wet - 1); break;
                case SE_PC_SPD:    pc.speed = std::max(0, pc.speed - 1); break;
                case SE_PC_KEY:    pc.key_root = wrap(pc.key_root, -1, NOTE_NAMES_N); break;
                case SE_PC_SCALE:  pc.scale_idx = wrap(pc.scale_idx, -1, PCOR_SCALES); break;
                case SE_PC_PULL:   pc.pull = std::max(5, pc.pull - 1); break;
                case SE_PC_LOWLAT_X:
                    pc.low_latency_x = !pc.low_latency_x;
                    pc_clamp_pc_exp_cursor(se_cur, pc.low_latency_x);
                    break;
                case SE_PC_EXP_YIN:
                    pc.exp_yin_i = wrap(pc.exp_yin_i, -1, PCOR_EXP_YIN_N);
                    break;
                case SE_PC_EXP_DET:
                    pc.exp_det_i = wrap(pc.exp_det_i, -1, PCOR_EXP_DETECT_N);
                    break;
                case SE_PC_EXP_WARM:
                    pc.exp_warm_i = wrap(pc.exp_warm_i, -1, PCOR_EXP_WARMUP_N);
                    break;
                case SE_PC_EXP_GATE:
                    pc.exp_gate_i = wrap(pc.exp_gate_i, -1, PCOR_EXP_YIN_GATE_N);
                    break;
                case SE_PC_EXP_LMIN:
                    pc.exp_lmin_i = wrap(pc.exp_lmin_i, -1, PCOR_EXP_LAG_MIN_N);
                    pc_normalize_exp_lag(pc);
                    break;
                case SE_PC_EXP_LMAX:
                    pc.exp_lmax_i = wrap(pc.exp_lmax_i, -1, PCOR_EXP_LAG_MAX_N);
                    pc_normalize_exp_lag(pc);
                    break;
                case SE_SAT_EN:    sat.enabled = !sat.enabled; break;
                case SE_SAT_DRIVE: sat.drive = std::max(0, sat.drive - 2); break;
                case SE_SAT_MIX:   sat.mix = std::max(0, sat.mix - 2); break;
                case SE_PS_EN:     psh.enabled = !psh.enabled; break;
                case SE_PS_SEMI:   psh.semi_idx = wrap(psh.semi_idx, -1, PS_SEMI_STEPS); break;
                case SE_PS_WET:    psh.wet = std::max(0, psh.wet - 2); break;
                default: break;
                }
                break;

            case KEY_RIGHT:
                switch (se_cur) {
                case SE_TYPE:
                    if (!live) {
                        slot_type[static_cast<std::size_t>(es)] =
                            wrap(slot_type[static_cast<std::size_t>(es)], 1, SLOT_TYPES);
                        rebuild_fx_chain(slot_type.data());
                        st   = slot_type[static_cast<std::size_t>(es)];
                        empty = (st == FX_NONE);
                        fx   = st;
                        se_cur = SE_TYPE;
                    }
                    break;
                case SE_C_EN:      cp.enabled = !cp.enabled; break;
                case SE_C_THRESH:  cp.threshold = std::min(0, cp.threshold + 1); break;
                case SE_C_RATIO:   cp.ratio_idx = wrap(cp.ratio_idx, 1, alen(COMP_RATIOS)); break;
                case SE_C_ATTACK:  cp.attack_idx = wrap(cp.attack_idx, 1, alen(COMP_ATTACKS)); break;
                case SE_C_RELEASE: cp.release_idx = wrap(cp.release_idx, 1, alen(COMP_RELEASES)); break;
                case SE_C_KNEE:    cp.knee_idx = wrap(cp.knee_idx, 1, alen(COMP_KNEES)); break;
                case SE_C_MAKEUP:  cp.makeup = std::min(36, cp.makeup + 1); break;
                case SE_EQ_EN:     eq.enabled = !eq.enabled; break;
                case SE_EQ_LOW:    eq.low_db  = std::min(12, eq.low_db + 1); break;
                case SE_EQ_MID:    eq.mid_db  = std::min(12, eq.mid_db + 1); break;
                case SE_EQ_HIGH:   eq.high_db = std::min(12, eq.high_db + 1); break;
                case SE_RV_EN:     rv.enabled = !rv.enabled; break;
                case SE_RV_ROOM:   rv.room = std::min(100, rv.room + 1); break;
                case SE_RV_DAMP:   rv.damp = std::min(100, rv.damp + 1); break;
                case SE_RV_WET:    rv.wet  = std::min(100, rv.wet + 1); break;
                case SE_DL_EN:     dl.enabled = !dl.enabled; break;
                case SE_DL_TIME:   dl.time_ms = std::min(2000, dl.time_ms + 5); break;
                case SE_DL_FB:     dl.feedback = std::min(95, dl.feedback + 1); break;
                case SE_DL_WET:    dl.wet = std::min(100, dl.wet + 1); break;
                case SE_DS_EN:     ds.enabled = !ds.enabled; break;
                case SE_DS_FREQ:   ds.freq_idx = wrap(ds.freq_idx, 1, alen(DEESS_FREQ_HZ)); break;
                case SE_DS_THRESH: ds.threshold = std::min(-4, ds.threshold + 1); break;
                case SE_DS_AMT:    ds.amount = std::min(100, ds.amount + 1); break;
                case SE_DS_ATTACK: ds.attack_idx = wrap(ds.attack_idx, 1, alen(COMP_ATTACKS)); break;
                case SE_DS_RELEASE: ds.release_idx = wrap(ds.release_idx, 1, alen(COMP_RELEASES)); break;
                case SE_PQ_EN:     pq.enabled = !pq.enabled; break;
                case SE_PQ_FREQ:   pq.freq_idx = wrap(pq.freq_idx, 1, alen(PEQ_FREQ_HZ)); break;
                case SE_PQ_Q:      pq.q_idx = wrap(pq.q_idx, 1, alen(PEQ_Q_VALS)); break;
                case SE_PQ_GAIN:   pq.gain_db = std::min(12, pq.gain_db + 1); break;
                case SE_HL_HPEN:   hl.hpf_enabled = !hl.hpf_enabled; break;
                case SE_HL_HPF:    hl.hpf_hz_idx = wrap(hl.hpf_hz_idx, 1, alen(HPF_FREQ_HZ)); break;
                case SE_HL_LPEN:   hl.lpf_enabled = !hl.lpf_enabled; break;
                case SE_HL_LPF:    hl.lpf_hz_idx = wrap(hl.lpf_hz_idx, 1, alen(LPF_FREQ_HZ)); break;
                case SE_PC_EN:     pc.enabled = !pc.enabled; break;
                case SE_PC_WET:    pc.wet = std::min(100, pc.wet + 1); break;
                case SE_PC_SPD:    pc.speed = std::min(100, pc.speed + 1); break;
                case SE_PC_KEY:    pc.key_root = wrap(pc.key_root, 1, NOTE_NAMES_N); break;
                case SE_PC_SCALE:  pc.scale_idx = wrap(pc.scale_idx, 1, PCOR_SCALES); break;
                case SE_PC_PULL:   pc.pull = std::min(100, pc.pull + 1); break;
                case SE_PC_LOWLAT_X:
                    pc.low_latency_x = !pc.low_latency_x;
                    pc_clamp_pc_exp_cursor(se_cur, pc.low_latency_x);
                    break;
                case SE_PC_EXP_YIN:
                    pc.exp_yin_i = wrap(pc.exp_yin_i, 1, PCOR_EXP_YIN_N);
                    break;
                case SE_PC_EXP_DET:
                    pc.exp_det_i = wrap(pc.exp_det_i, 1, PCOR_EXP_DETECT_N);
                    break;
                case SE_PC_EXP_WARM:
                    pc.exp_warm_i = wrap(pc.exp_warm_i, 1, PCOR_EXP_WARMUP_N);
                    break;
                case SE_PC_EXP_GATE:
                    pc.exp_gate_i = wrap(pc.exp_gate_i, 1, PCOR_EXP_YIN_GATE_N);
                    break;
                case SE_PC_EXP_LMIN:
                    pc.exp_lmin_i = wrap(pc.exp_lmin_i, 1, PCOR_EXP_LAG_MIN_N);
                    pc_normalize_exp_lag(pc);
                    break;
                case SE_PC_EXP_LMAX:
                    pc.exp_lmax_i = wrap(pc.exp_lmax_i, 1, PCOR_EXP_LAG_MAX_N);
                    pc_normalize_exp_lag(pc);
                    break;
                case SE_SAT_EN:    sat.enabled = !sat.enabled; break;
                case SE_SAT_DRIVE: sat.drive = std::min(100, sat.drive + 2); break;
                case SE_SAT_MIX:   sat.mix = std::min(100, sat.mix + 2); break;
                case SE_PS_EN:     psh.enabled = !psh.enabled; break;
                case SE_PS_SEMI:   psh.semi_idx = wrap(psh.semi_idx, 1, PS_SEMI_STEPS); break;
                case SE_PS_WET:    psh.wet = std::min(100, psh.wet + 2); break;
                default: break;
                }
                break;

            case '\n':
            case KEY_ENTER:
                if (se_cur == SE_BACK)
                    edit_slot = -1;
                else if (se_cur == SE_C_EN)
                    cp.enabled = !cp.enabled;
                else if (se_cur == SE_EQ_EN)
                    eq.enabled = !eq.enabled;
                else if (se_cur == SE_RV_EN)
                    rv.enabled = !rv.enabled;
                else if (se_cur == SE_DL_EN)
                    dl.enabled = !dl.enabled;
                else if (se_cur == SE_DS_EN)
                    ds.enabled = !ds.enabled;
                else if (se_cur == SE_PQ_EN)
                    pq.enabled = !pq.enabled;
                else if (se_cur == SE_HL_HPEN)
                    hl.hpf_enabled = !hl.hpf_enabled;
                else if (se_cur == SE_HL_LPEN)
                    hl.lpf_enabled = !hl.lpf_enabled;
                else if (se_cur == SE_PC_EN)
                    pc.enabled = !pc.enabled;
                else if (se_cur == SE_PC_LOWLAT_X) {
                    pc.low_latency_x = !pc.low_latency_x;
                    pc_clamp_pc_exp_cursor(se_cur, pc.low_latency_x);
                } else if (se_cur == SE_SAT_EN)
                    sat.enabled = !sat.enabled;
                else if (se_cur == SE_PS_EN)
                    psh.enabled = !psh.enabled;
                break;

            case 'y':
            case 'Y':
                if (!live) {
                    slot_clip_copy(g_chain_clip, es, slot_type, slot_param, slot_eq, slot_rv,
                                   slot_dl, slot_dss, slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                }
                break;
            case 'p':
            case 'P':
                if (!live && g_chain_clip.valid) {
                    slot_clip_paste(g_chain_clip, es, slot_type, slot_param, slot_eq, slot_rv,
                                    slot_dl, slot_dss, slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                    rebuild_fx_chain(slot_type.data());
                    st = std::clamp(slot_type[static_cast<std::size_t>(es)], 0, SLOT_TYPES - 1);
                    empty = (st == FX_NONE);
                    fx = st;
                }
                break;
            case 'x':
            case 'X':
                if (!live) {
                    slot_clip_cut(g_chain_clip, es, slot_type, slot_param, slot_eq, slot_rv,
                                  slot_dl, slot_dss, slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                    rebuild_fx_chain(slot_type.data());
                    st = std::clamp(slot_type[static_cast<std::size_t>(es)], 0, SLOT_TYPES - 1);
                    empty = (st == FX_NONE);
                    fx = st;
                }
                break;
            case '[':
                if (!live && es > 0) {
                    chain_swap_slots(es, es - 1, slot_type, slot_param, slot_eq, slot_rv, slot_dl,
                                     slot_dss, slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                    rebuild_fx_chain(slot_type.data());
                    edit_slot = es - 1;
                }
                break;
            case ']':
                if (!live && es < CHAIN_SLOTS - 1) {
                    chain_swap_slots(es, es + 1, slot_type, slot_param, slot_eq, slot_rv, slot_dl,
                                     slot_dss, slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                    rebuild_fx_chain(slot_type.data());
                    edit_slot = es + 1;
                }
                break;

            default: break;
            }
            continue;
        }

        constexpr int R_CHAIN_SEP = 10;
        constexpr int R_CHAIN0    = 11;
        constexpr int R_CLIP      = R_CHAIN0 + CHAIN_SLOTS;
        constexpr int R_HINT      = R_CLIP + 1;
        constexpr int R_BTN       = R_HINT + 1;
        constexpr int R_STAT      = R_BTN + 1;
        constexpr int need_h      = R_STAT + 10;

        int H = std::max(3, std::min(std::max(need_h, kMinH), LINES));
        int W = std::min(std::max(kMinW, COLS - 4), COLS);

        erase();
        int y0 = std::max(0, (LINES - H) / 2);
        int x0 = std::max(0, (COLS  - W) / 2);

        // ── border ──
        attron(COLOR_PAIR(1));
        mvaddch(y0, x0, ACS_ULCORNER);
        mvhline(y0, x0+1, ACS_HLINE, W-2);
        mvaddch(y0, x0+W-1, ACS_URCORNER);
        for (int r = 1; r < H-1; r++) {
            mvaddch(y0+r, x0, ACS_VLINE);
            mvaddch(y0+r, x0+W-1, ACS_VLINE);
        }
        mvaddch(y0+H-1, x0, ACS_LLCORNER);
        mvhline(y0+H-1, x0+1, ACS_HLINE, W-2);
        mvaddch(y0+H-1, x0+W-1, ACS_LRCORNER);
        attron(A_BOLD);
        mvprintw(y0, x0+(W-18)/2, " Aud10 Suite ");
        attroff(A_BOLD | COLOR_PAIR(1));

        int lx = x0 + 3;
        int vx = x0 + 22;
        int VW = std::max(24, W - 28);

        auto row = [&](int y, int f, const char *label, const char *val, bool editable) {
            bool sel = cur == f;
            if (sel)            attron(A_REVERSE | A_BOLD);
            else if (!editable) attron(A_DIM);

            mvprintw(y, lx, "%-16s", label);
            if (sel && editable) {
                attroff(A_REVERSE | A_BOLD);
                attron(COLOR_PAIR(4));
                mvprintw(y, vx-2, "< ");
                attroff(COLOR_PAIR(4));
                attron(A_REVERSE | A_BOLD);
            }
            mvprintw(y, vx, "%-*s", VW, val);
            if (sel && editable) {
                attroff(A_REVERSE | A_BOLD);
                attron(COLOR_PAIR(4));
                printw(" >");
                attroff(COLOR_PAIR(4));
            }
            attroff(A_REVERSE | A_BOLD | A_DIM);
        };

        auto chain_row = [&](int y, Field f, const char *label, int st, std::size_t ui,
                             bool editable) {
            bool sel = cur == f;
            st = std::clamp(st, 0, SLOT_TYPES - 1);
            if (sel)
                attron(A_REVERSE | A_BOLD);
            else if (!editable)
                attron(A_DIM);

            mvprintw(y, lx, "%-16s", label);
            if (sel && editable) {
                attroff(A_REVERSE | A_BOLD);
                attron(COLOR_PAIR(4));
                mvprintw(y, vx - 2, "< ");
                attroff(COLOR_PAIR(4));
                attron(A_REVERSE | A_BOLD);
            }

            const int vcol = vx;
            if (st == FX_NONE) {
                mvprintw(y, vcol, "%-*s", VW, "  Empty");
            } else {
                const bool on =
                    slot_effect_is_on(st, ui, slot_param, slot_eq, slot_rv, slot_dl, slot_dss,
                                      slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                if (has_colors()) {
                    if (sel)
                        attroff(A_REVERSE | A_BOLD);
                    if (on)
                        attron(COLOR_PAIR(2));
                    else {
                        attron(COLOR_PAIR(6));
                        attron(A_DIM);
                    }
                    mvaddch(y, vcol, '#');
                    attroff(A_DIM | COLOR_PAIR(2) | COLOR_PAIR(6));
                    if (sel)
                        attron(A_REVERSE | A_BOLD);
                    else if (!editable)
                        attron(A_DIM);
                } else {
                    if (!on)
                        attron(A_DIM);
                    mvaddch(y, vcol, '#');
                    attroff(A_DIM);
                }
                char rest[72];
                std::snprintf(rest, sizeof rest, " %s", SLOT_TYPE_NAMES[st]);
                mvprintw(y, vcol + 1, "%-*s", VW - 1,
                         trunc(std::string(rest), static_cast<std::size_t>(VW - 1)).c_str());
            }

            if (sel && editable) {
                attroff(A_REVERSE | A_BOLD);
                attron(COLOR_PAIR(4));
                mvprintw(y, vx + VW, " >");
                attroff(COLOR_PAIR(4));
                attron(A_REVERSE | A_BOLD);
            }
            attroff(A_REVERSE | A_BOLD | A_DIM);
        };

        char tmp[64];

        std::snprintf(tmp, sizeof tmp, "%u Hz (server)", sr);
        row(y0+1, F_RATE, "Sample Rate", tmp, false);
        row(y0+2, F_CHAN, "Channels", CHANNEL_NAMES[ci], !live);
        float lat_ms = float(JACK_BUFSIZES[bi]) / float(sr) * 2000.0f;
        std::snprintf(tmp, sizeof tmp, "%u smp (~%.1f ms RT)", JACK_BUFSIZES[bi], lat_ms);
        row(y0+3, F_BUF, "Buffer Size", tmp, true);

        std::snprintf(tmp, sizeof tmp, "%+d dB", vi);
        row(y0+4, F_VOL_IN, "In Gain", tmp, true);
        std::snprintf(tmp, sizeof tmp, "%+d dB", vo);
        row(y0+5, F_VOL_OUT, "Out Gain", tmp, true);

        row(y0+6, F_IN_L,  "Input L",  trunc(captures[pil].display, VW).c_str(), !live);
        row(y0+7, F_IN_R,  "Input R",  trunc(captures[pir].display, VW).c_str(), !live && stereo);
        row(y0+8, F_OUT_L, "Output L", trunc(playbacks[qol].display, VW).c_str(), !live);
        row(y0+9, F_OUT_R, "Output R", trunc(playbacks[qor].display, VW).c_str(), !live && stereo);

        // ── effect chain (signal flows 1 → N) ──
        {
            int hl = std::max(4, W - 26);
            attron(COLOR_PAIR(1) | A_DIM);
            mvhline(y0 + R_CHAIN_SEP, lx, ACS_HLINE, 2);
            attroff(A_DIM);
            mvprintw(y0 + R_CHAIN_SEP, lx + 2, " Effect chain ");
            attron(A_DIM);
            mvhline(y0 + R_CHAIN_SEP, lx + 16, ACS_HLINE, hl);
            attroff(COLOR_PAIR(1) | A_DIM);
        }
        for (int i = 0; i < CHAIN_SLOTS; i++) {
            std::snprintf(tmp, sizeof tmp, "Slot %d", i + 1);
            int st = slot_type[static_cast<std::size_t>(i)];
            chain_row(y0 + R_CHAIN0 + i, static_cast<Field>(F_CHAIN_0 + i), tmp,
                      st, static_cast<std::size_t>(i), !live);
        }

        attron(A_DIM);
        {
            const char *cn = g_chain_clip.valid
                ? SLOT_TYPE_NAMES[std::clamp(g_chain_clip.ty, 0, SLOT_TYPES - 1)]
                : "(empty)";
            mvprintw(y0 + R_CLIP, lx,
                     "Clipboard: %-14s  y/p/x/[ ]  D=bypass  # green=on dim=off",
                     cn);
        }
        mvprintw(y0 + R_HINT, lx,
                 "Slots: L/R=type  D=on/off  Enter=edit  S=Start/Stop  ?=help  F1=help");
        attroff(A_DIM);

        // ── buttons ──
        int by = y0 + R_BTN;
        auto btn = [&](int bx, int f, int pair, const char *lbl) {
            if (cur == f) attron(A_REVERSE | A_BOLD);
            attron(COLOR_PAIR(pair));
            mvprintw(by, bx, " %s ", lbl);
            attroff(A_REVERSE | A_BOLD | COLOR_PAIR(pair));
        };
        btn(x0+W/2-14, F_START, live?3:2, live?"[ Stop  ]":"[ Start ]");
        btn(x0+W/2+4,  F_QUIT,  3,        "[ Quit  ]");

        // ── status / meters ──
        int sy = y0 + R_STAT;
        if (!ok) {
            attron(COLOR_PAIR(3) | A_BOLD);
            mvprintw(sy, lx, "JACK server disconnected!");
            attroff(COLOR_PAIR(3) | A_BOLD);
        } else if (live) {
            attron(COLOR_PAIR(2) | A_BOLD);
            std::snprintf(tmp, sizeof tmp, "Running  %u smp / %.1f ms RT",
                          actual_buf, float(actual_buf)/float(sr)*2000.0f);
            mvprintw(sy, lx, "Status: %s", tmp);
            attroff(COLOR_PAIR(2) | A_BOLD);

            int mw = W - 22;

            auto meter = [&](int y, const char *label, float pk_lin, int color) {
                float db = (pk_lin > 1e-7f) ? 20.0f*std::log10(pk_lin) : -140.0f;
                int filled = std::clamp(int((db+60.0f)/60.0f*mw), 0, mw);
                mvprintw(y, lx, "%-5s", label);
                for (int i = 0; i < mw; i++) {
                    if (i < filled) {
                        int p = color ? color : ((i>mw*3/4)?3:(i>mw/2)?4:2);
                        attron(COLOR_PAIR(p)); addch(ACS_BLOCK); attroff(COLOR_PAIR(p));
                    } else { attron(A_DIM); addch(ACS_BULLET); attroff(A_DIM); }
                }
                std::snprintf(tmp, sizeof tmp, " %+.0f dB", db);
                addstr(tmp);
            };

            meter(sy+1, "In:  ", g_peak_in.load(std::memory_order_relaxed),  0);
            meter(sy+2, "Out: ", g_peak_out.load(std::memory_order_relaxed), 0);

            float gr = 0.0f;
            if (g_gr_meter)
                gr = g_gr_meter->gr_db.load(std::memory_order_relaxed);
            bool show_gr = g_gr_meter
                && g_gr_meter->enabled.load(std::memory_order_relaxed);
            int grf = show_gr ? std::clamp(int(-gr/24.0f*mw), 0, mw) : 0;
            mvprintw(sy+3, lx, "GR:  ");
            for (int i = 0; i < mw; i++) {
                if (i < grf) {
                    attron(COLOR_PAIR(5)); addch(ACS_BLOCK); attroff(COLOR_PAIR(5));
                } else { attron(A_DIM); addch(ACS_BULLET); attroff(A_DIM); }
            }
            std::snprintf(tmp, sizeof tmp, show_gr ? " %.0f dB" : " -- ", gr);
            addstr(tmp);

            float dsp_last = g_dsp_ms_last.load(std::memory_order_relaxed);
            dsp_peak_display = std::max(dsp_peak_display * 0.987f, dsp_last);
            float dsp_ema = g_dsp_ms_ema.load(std::memory_order_relaxed);
            uint64_t xr = g_xrun_count.load(std::memory_order_relaxed);
            float period_ms = (sr > 0u && actual_buf > 0u)
                ? float(actual_buf) * 1000.0f / float(sr)
                : 0.f;
            float dsp_pct = (period_ms > 1e-4f) ? 100.0f * dsp_ema / period_ms : 0.f;
            char perf[96];
            if (xr > 0)
                std::snprintf(perf, sizeof perf,
                              "DSP: %.2f ms avg  %.2f ms peak  ~%.0f%% of %.2f ms  xruns: %llu",
                              dsp_ema, dsp_peak_display, dsp_pct, period_ms,
                              static_cast<unsigned long long>(xr));
            else
                std::snprintf(perf, sizeof perf,
                              "DSP: %.2f ms avg  %.2f ms peak  ~%.0f%% of %.2f ms buffer",
                              dsp_ema, dsp_peak_display, dsp_pct, period_ms);
            attron(A_DIM);
            mvprintw(sy + 4, lx, "%.*s", std::max(1, W - 6), perf);
            attroff(A_DIM);

            uint32_t ilo = g_lat_in_min.load(std::memory_order_relaxed);
            uint32_t ihi = g_lat_in_max.load(std::memory_order_relaxed);
            uint32_t olo = g_lat_out_min.load(std::memory_order_relaxed);
            uint32_t ohi = g_lat_out_max.load(std::memory_order_relaxed);
            char latline[128];
            if (sr == 0u) {
                std::snprintf(latline, sizeof latline, "I/O graph latency: (no rate)");
            } else if (ihi == 0u && ohi == 0u) {
                std::snprintf(latline, sizeof latline,
                              "I/O graph latency: in —   out —   (connect ports)");
            } else {
                auto fmt_side = [](char *dst, std::size_t dst_sz, const char *tag, uint32_t lo,
                                   uint32_t hi, unsigned rate) {
                    if (hi == 0u) {
                        std::snprintf(dst, dst_sz, "%s —", tag);
                        return;
                    }
                    float mlo = float(lo) * 1000.0f / float(rate);
                    float mhi = float(hi) * 1000.0f / float(rate);
                    if (lo == hi)
                        std::snprintf(dst, dst_sz, "%s %.2f ms (%uf)", tag, mlo,
                                      static_cast<unsigned>(lo));
                    else
                        std::snprintf(dst, dst_sz, "%s %.2f–%.2f ms (%u–%uf)", tag, mlo, mhi,
                                      static_cast<unsigned>(lo), static_cast<unsigned>(hi));
                };
                char inpart[56], outpart[56];
                fmt_side(inpart, sizeof inpart, "in", ilo, ihi, sr);
                fmt_side(outpart, sizeof outpart, "out", olo, ohi, sr);
                std::snprintf(latline, sizeof latline, "I/O graph latency:  %s   %s  [JACK]", inpart,
                              outpart);
            }
            attron(A_DIM);
            mvprintw(sy + 5, lx, "%.*s", std::max(1, W - 6), latline);
            attroff(A_DIM);
        } else {
            attron(A_DIM);
            mvprintw(sy, lx, "Status: Stopped");
            attroff(A_DIM);
            dsp_peak_display = 0.f;
        }

        attron(A_DIM);
        mvprintw(y0+H-2, lx,
                 "Up/Dn: fields  L/R: adjust  D: bypass  S: Start/Stop  F2: profiles  ?=help  ypx[]");
        attroff(A_DIM);

        refresh();
        timeout(live ? 50 : -1);
        int ch = getch();
        if (ch == ERR) continue;
        if (ch == KEY_RESIZE) {
            clearok(stdscr, TRUE);
            continue;
        }

        switch (ch) {
        case '?':
        case KEY_F(1):
            run_help_screen();
            break;
        case KEY_F(2):
            run_profiles_screen(slot_type, slot_param, slot_eq, slot_rv, slot_dl, slot_dss, slot_peq,
                                slot_hilo, slot_pc, slot_sat, slot_ps);
            break;
        case 'y':
        case 'Y':
            if (!live && field_is_chain_slot(cur)) {
                int si = cur - F_CHAIN_0;
                slot_clip_copy(g_chain_clip, si, slot_type, slot_param, slot_eq, slot_rv, slot_dl,
                               slot_dss, slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
            }
            break;
        case 'p':
        case 'P':
            if (!live && field_is_chain_slot(cur) && g_chain_clip.valid) {
                int si = cur - F_CHAIN_0;
                slot_clip_paste(g_chain_clip, si, slot_type, slot_param, slot_eq, slot_rv, slot_dl,
                                slot_dss, slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                rebuild_fx_chain(slot_type.data());
            }
            break;
        case 'x':
        case 'X':
            if (!live && field_is_chain_slot(cur)) {
                int si = cur - F_CHAIN_0;
                slot_clip_cut(g_chain_clip, si, slot_type, slot_param, slot_eq, slot_rv, slot_dl,
                              slot_dss, slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                rebuild_fx_chain(slot_type.data());
            }
            break;
        case '[':
            if (!live && field_is_chain_slot(cur)) {
                int si = cur - F_CHAIN_0;
                if (si > 0) {
                    chain_swap_slots(si, si - 1, slot_type, slot_param, slot_eq, slot_rv, slot_dl,
                                     slot_dss, slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                    rebuild_fx_chain(slot_type.data());
                    cur = F_CHAIN_0 + si - 1;
                }
            }
            break;
        case ']':
            if (!live && field_is_chain_slot(cur)) {
                int si = cur - F_CHAIN_0;
                if (si < CHAIN_SLOTS - 1) {
                    chain_swap_slots(si, si + 1, slot_type, slot_param, slot_eq, slot_rv, slot_dl,
                                     slot_dss, slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                    rebuild_fx_chain(slot_type.data());
                    cur = F_CHAIN_0 + si + 1;
                }
            }
            break;

        case 'd':
        case 'D':
            if (field_is_chain_slot(cur)) {
                std::size_t si = static_cast<std::size_t>(cur - F_CHAIN_0);
                int st = std::clamp(slot_type[si], 0, SLOT_TYPES - 1);
                if (st != FX_NONE) {
                    slot_toggle_effect_enabled(st, si, slot_param, slot_eq, slot_rv, slot_dl, slot_dss,
                                               slot_peq, slot_hilo, slot_pc, slot_sat, slot_ps);
                }
            }
            break;

        case KEY_UP:   nav_main_ud(cur, -1); break;
        case KEY_DOWN: nav_main_ud(cur,  1); break;

        case KEY_LEFT:
            if (cur == F_QUIT) {
                cur = F_START;
                break;
            }
            if (cur == F_START)
                break;
            switch (cur) {
            case F_CHAN:   if (!live) ci = wrap(ci, -1, alen(CHANNEL_OPTS)); break;
            case F_BUF:
                bi = wrap(bi, -1, alen(JACK_BUFSIZES));
                jack_set_buffer_size(g_client, JACK_BUFSIZES[bi]);
                break;
            case F_VOL_IN:       vi = std::max(-60, vi - 1); break;
            case F_VOL_OUT:      vo = std::max(-60, vo - 1); break;
            case F_IN_L:   if (!live) pil = wrap(pil, -1, (int)captures.size());  break;
            case F_IN_R:   if (!live && stereo) pir = wrap(pir, -1, (int)captures.size());  break;
            case F_OUT_L:  if (!live) qol = wrap(qol, -1, (int)playbacks.size()); break;
            case F_OUT_R:  if (!live && stereo) qor = wrap(qor, -1, (int)playbacks.size()); break;
            default:
                if (field_is_chain_slot(cur) && !live) {
                    int si = cur - F_CHAIN_0;
                    slot_type[static_cast<std::size_t>(si)] =
                        wrap(slot_type[static_cast<std::size_t>(si)], -1, SLOT_TYPES);
                    rebuild_fx_chain(slot_type.data());
                }
                break;
            }
            break;

        case KEY_RIGHT:
            if (cur == F_START) {
                cur = F_QUIT;
                break;
            }
            if (cur == F_QUIT)
                break;
            switch (cur) {
            case F_CHAN:   if (!live) ci = wrap(ci, 1, alen(CHANNEL_OPTS)); break;
            case F_BUF:
                bi = wrap(bi, 1, alen(JACK_BUFSIZES));
                jack_set_buffer_size(g_client, JACK_BUFSIZES[bi]);
                break;
            case F_VOL_IN:       vi = std::min(12, vi + 1); break;
            case F_VOL_OUT:      vo = std::min(12, vo + 1); break;
            case F_IN_L:   if (!live) pil = wrap(pil, 1, (int)captures.size());  break;
            case F_IN_R:   if (!live && stereo) pir = wrap(pir, 1, (int)captures.size());  break;
            case F_OUT_L:  if (!live) qol = wrap(qol, 1, (int)playbacks.size()); break;
            case F_OUT_R:  if (!live && stereo) qor = wrap(qor, 1, (int)playbacks.size()); break;
            default:
                if (field_is_chain_slot(cur) && !live) {
                    int si = cur - F_CHAIN_0;
                    slot_type[static_cast<std::size_t>(si)] =
                        wrap(slot_type[static_cast<std::size_t>(si)], 1, SLOT_TYPES);
                    rebuild_fx_chain(slot_type.data());
                }
                break;
            }
            break;

        case '\n':
        case KEY_ENTER:
            if (cur == F_QUIT) { shutdown(); app_exit = 0; return; }
            if (cur == F_START) {
                if (live) {
                    g_active = false;
                    disconnect_all();
                } else if (ok && (pil > 0 || pir > 0) && (qol > 0 || qor > 0)) {
                    rebuild_fx_chain(slot_type.data());
                    for (int k = 0; k < g_rt_n; k++)
                        g_rt_fx[k]->reset();
                    g_channels.store(CHANNEL_OPTS[ci], std::memory_order_relaxed);
                    connect_ports(CHANNEL_OPTS[ci],
                                  captures[pil].name,  captures[pir].name,
                                  playbacks[qol].name, playbacks[qor].name);
                    g_active = true;
                }
            }
            if (field_is_chain_slot(cur)) {
                edit_slot = cur - F_CHAIN_0;
                se_cur    = SE_TYPE;
            }
            break;

        case 's':
        case 'S':
            if (live) {
                g_active = false;
                disconnect_all();
            } else if (ok && (pil > 0 || pir > 0) && (qol > 0 || qor > 0)) {
                rebuild_fx_chain(slot_type.data());
                for (int k = 0; k < g_rt_n; k++)
                    g_rt_fx[k]->reset();
                g_channels.store(CHANNEL_OPTS[ci], std::memory_order_relaxed);
                connect_ports(CHANNEL_OPTS[ci],
                              captures[pil].name,  captures[pir].name,
                              playbacks[qol].name, playbacks[qor].name);
                g_active = true;
            }
            break;

        case 'q': case 'Q':
            shutdown();
            app_exit = 0;
            return;
        }
    }
    });

    ui_thread.join();
    return app_exit;
}
