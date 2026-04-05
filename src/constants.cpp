#include "constants.hpp"

const unsigned CHANNEL_OPTS[] = {1, 2};
const char *const CHANNEL_NAMES[] = {"Mono", "Stereo"};
const unsigned JACK_BUFSIZES[] = {32, 64, 128, 256, 512, 1024, 2048};

const float COMP_RATIOS[]   = {1.0f,1.5f,2.0f,3.0f,4.0f,6.0f,8.0f,12.0f,20.0f};
const float COMP_ATTACKS[]  = {0.1f,0.5f,1.0f,2.0f,5.0f,10.0f,20.0f,50.0f,100.0f};
const float COMP_RELEASES[] = {5.0f,10.0f,20.0f,50.0f,100.0f,200.0f,500.0f,1000.0f,2000.0f};
const float COMP_KNEES[]    = {0.0f,1.0f,2.0f,3.0f,6.0f,9.0f,12.0f};

const char *const SLOT_TYPE_NAMES[] = {
    "Empty", "Compressor", "EQ", "Reverb", "Delay",
    "De-esser", "Param EQ", "Hi/Lo pass", "Pitch correct", "Saturator"};

const char *const PCOR_SCALE_NAMES[] = {"Chromatic", "Major", "Minor"};

const unsigned PCOR_EXP_YIN_SAMPLES[PCOR_EXP_YIN_N]       = {512, 768, 1024};
const unsigned PCOR_EXP_DETECT_PER[PCOR_EXP_DETECT_N]     = {64, 96, 128, 192, 256, 384, 512};
const unsigned PCOR_EXP_WARMUP_SMPS[PCOR_EXP_WARMUP_N]   = {1024, 2048, 4096, 6144, 8192};
const unsigned PCOR_EXP_YIN_GATE_SMPS[PCOR_EXP_YIN_GATE_N] = {512, 640, 1024, 1536, 2048, 3072, 4096};
const unsigned PCOR_EXP_LAG_MIN_SMPS[PCOR_EXP_LAG_MIN_N]   = {256, 380, 512, 640, 900};
const unsigned PCOR_EXP_LAG_MAX_SMPS[PCOR_EXP_LAG_MAX_N]   = {1536, 2048, 2500, 3000, 4000, 5600, 8192};
const char *const NOTE_NAMES[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

const float DEESS_FREQ_HZ[] = {4000.0f, 5000.0f, 6000.0f, 7000.0f, 8000.0f, 9000.0f};
const float PEQ_FREQ_HZ[] = {
    100.0f, 200.0f, 400.0f, 800.0f, 1600.0f, 2500.0f, 4000.0f, 6000.0f, 9000.0f, 12000.0f};
const float PEQ_Q_VALS[] = {0.5f, 0.71f, 1.0f, 1.41f, 2.0f, 3.0f};
const float HPF_FREQ_HZ[] = {40.0f, 80.0f, 120.0f, 180.0f, 240.0f, 350.0f, 500.0f};
const float LPF_FREQ_HZ[] = {20000.0f, 16000.0f, 12000.0f, 10000.0f, 8000.0f, 6500.0f, 5000.0f};
