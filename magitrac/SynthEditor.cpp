#include "SynthEditor.h"
#include "ServerPairing.h"
#include "MagiMsg.h"

static const int SE_HDR_H     = 50;
static const int SE_BTN_Y     = 5;
static const int SE_BTN_H     = 40;
static const int SE_PRESET_X0 = 12;
static const int SE_PRESET_W  = 122;
static const int SE_PRESET_GAP = 6;
static const int SE_BACK_X    = 820;
static const int SE_BACK_W    = 125;

static const int SE_LEFT_X  = 15;
static const int SE_RIGHT_X = 495;
static const int SE_ROW_H   = 58;
static const int SE_LABEL_W = 110;      // label column, then the track
static const int SE_TRACK_W = 230;
static const int SE_VALUE_X = 350;      // value readout, from the panel's left
static const int SE_KNOB_W  = 20;
static const int SE_KNOB_H  = 40;

static const int SE_WAVE_X  = 100;
static const int SE_WAVE_Y[2] = { 80, 132 };
static const int SE_WAVE_H  = 44;
static const int SE_WAVE_GAP = 4;
static const char* const SE_WAVE_LABELS[5] = { "SAW", "SQR", "TRI", "SINE", "NOISE" };

struct SeSlider { uint8_t param; int16_t x, y; const char* label; };
static const SeSlider SE_SLIDERS[] = {
    { SYN_MIX,     SE_LEFT_X,  186, "MIX" },
    { SYN_PITCH,   SE_LEFT_X,  244, "PITCH 2" },
    { SYN_DETUNE,  SE_LEFT_X,  302, "DETUNE" },
    { SYN_PW,      SE_LEFT_X,  360, "PULSE W" },
    { SYN_SUB,     SE_LEFT_X,  418, "SUB" },
    { SYN_CUTOFF,  SE_RIGHT_X,  80, "CUTOFF" },
    { SYN_RESO,    SE_RIGHT_X, 138, "RESO" },
    { SYN_ENV,     SE_RIGHT_X, 196, "ENV AMT" },
    { SYN_ATTACK,  SE_RIGHT_X, 284, "ATTACK" },
    { SYN_DECAY,   SE_RIGHT_X, 342, "DECAY" },
    { SYN_SUSTAIN, SE_RIGHT_X, 400, "SUSTAIN" },
    { SYN_RELEASE, SE_RIGHT_X, 458, "RELEASE" },
};
static const int SE_NSLIDERS = (int)(sizeof(SE_SLIDERS) / sizeof(SE_SLIDERS[0]));

static const SeSlider* seSlider(int param) {
    for (int i = 0; i < SE_NSLIDERS; i++) if (SE_SLIDERS[i].param == param) return &SE_SLIDERS[i];
    return nullptr;
}

static int seWaveCount(int osc) { return ORGAN_SYNTH_MAX[osc == 0 ? SYN_OSC1 : SYN_OSC2] + 1; }
static int seWaveW(int osc) {
    int n = seWaveCount(osc);
    return ((SE_RIGHT_X - 30 - SE_WAVE_X) - (n - 1) * SE_WAVE_GAP) / n;
}
static int seWaveBtnX(int osc, int i) { return SE_WAVE_X + i * (seWaveW(osc) + SE_WAVE_GAP); }

static void seTime(char* buf, size_t n, float ms) {
    if (ms < 1000.0f) snprintf(buf, n, "%d ms", (int)(ms + 0.5f));
    else              snprintf(buf, n, "%.1f s", ms / 1000.0f);
}

// The setting in the units it means.
static void seValueText(int param, int v, char* buf, size_t n) {
    switch (param) {
        case SYN_MIX:
            if (v == 0)       snprintf(buf, n, "OSC 1");
            else if (v == 16) snprintf(buf, n, "OSC 2");
            else              snprintf(buf, n, "%d%% 2", v * 100 / 16);
            break;
        case SYN_PITCH:   if (v == 12) snprintf(buf, n, "0"); else snprintf(buf, n, "%+d st", v - 12); break;
        case SYN_DETUNE:  snprintf(buf, n, "%d c", (int)(synthDetuneCents(v) + 0.5f)); break;
        case SYN_PW:      snprintf(buf, n, "%d%%", (int)(synthPulseWidth(v) * 100.0f + 0.5f)); break;
        case SYN_SUB:
        case SYN_RESO:    if (v == 0) snprintf(buf, n, "OFF"); else snprintf(buf, n, "%d", v); break;
        case SYN_CUTOFF: {
            float hz = synthCutoffHz(v);
            if (v == 16)          snprintf(buf, n, "OPEN");
            else if (hz < 1000.0f) snprintf(buf, n, "%d Hz", (int)(hz + 0.5f));
            else                   snprintf(buf, n, "%.1f k", hz / 1000.0f);
            break;
        }
        case SYN_ENV:     if (v == 0) snprintf(buf, n, "OFF"); else snprintf(buf, n, "%.1f oct", synthEnvOctaves(v)); break;
        case SYN_ATTACK:  seTime(buf, n, synthAttackMs(v));  break;
        case SYN_DECAY:   seTime(buf, n, synthDecayMs(v));   break;
        case SYN_RELEASE: seTime(buf, n, synthReleaseMs(v)); break;
        case SYN_SUSTAIN: snprintf(buf, n, "%d%%", v * 100 / 16); break;
        default:          snprintf(buf, n, "%d", v); break;
    }
}

void SynthEditor::open(uint8_t* params) {
    _p       = params;
    _wasDown = false;
    _drag    = -1;
    _d.clear();
    draw();
    _d.paint();
}

void SynthEditor::draw() {
    if (!_p) return;
    _d.fillRect(0, 0, 960, 540, COL_WHITE);
    drawHeader();
    drawTitles();
    drawWaves(0);
    drawWaves(1);
    for (int i = 0; i < SE_NSLIDERS; i++) drawSlider(SE_SLIDERS[i].param);
}

void SynthEditor::drawHeader() {
    _d.fillRect(0, 0, 960, SE_HDR_H, COL_BLACK);
    for (int i = 0; i < ORGAN_SYNTH_PRESET_N; i++)
        uiButton(_d, SE_PRESET_X0 + i * (SE_PRESET_W + SE_PRESET_GAP), SE_BTN_Y, SE_PRESET_W, SE_BTN_H,
                 ORGAN_SYNTH_PRESETS[i].name, COL_WHITE, COL_BLACK, 2);
    uiButton(_d, SE_BACK_X, SE_BTN_Y, SE_BACK_W, SE_BTN_H, "BACK", COL_WHITE, COL_BLACK, 3);
}

void SynthEditor::drawTitles() {
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(SE_LEFT_X, 56);   _d.print("OSCILLATORS");
    _d.setCursor(SE_RIGHT_X, 56);  _d.print("FILTER");
    _d.setCursor(SE_RIGHT_X, 262); _d.print("ENVELOPE");
    _d.setCursor(SE_LEFT_X, 500);  _d.print("Play your keyboard to hear it");
    _d.fillRect(SE_RIGHT_X - 16, 58, 2, 460, COL_LTGREY);
}

void SynthEditor::drawWaves(int osc) {
    int y = SE_WAVE_Y[osc];
    _d.fillRect(SE_LEFT_X, y, SE_RIGHT_X - 30 - SE_LEFT_X, SE_WAVE_H, COL_WHITE);
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(SE_LEFT_X, y + (SE_WAVE_H - 16) / 2);
    _d.print(osc == 0 ? "OSC 1" : "OSC 2");
    int sel = _p[osc == 0 ? SYN_OSC1 : SYN_OSC2];
    for (int i = 0; i < seWaveCount(osc); i++) {
        bool on = (i == sel);
        uiButton(_d, seWaveBtnX(osc, i), y, seWaveW(osc), SE_WAVE_H, SE_WAVE_LABELS[i],
                 on ? COL_BLACK : COL_WHITE, on ? COL_WHITE : COL_BLACK, 2);
    }
}

void SynthEditor::drawSlider(int param) {
    const SeSlider* s = seSlider(param);
    if (!s) return;
    int x = s->x, y = s->y, cy = y + SE_ROW_H / 2;
    _d.fillRect(x, y, SE_RIGHT_X - 30 - SE_LEFT_X, SE_ROW_H, COL_WHITE);

    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(x, cy - 8);
    _d.print(s->label);

    int v   = _p[param];
    int max = ORGAN_SYNTH_MAX[param];
    int tx  = x + SE_LABEL_W;
    int kx  = tx + v * SE_TRACK_W / max;
    _d.fillRect(tx, cy - 2, SE_TRACK_W, 4, COL_LTGREY);
    if (param == SYN_PITCH) _d.fillRect(tx + SE_TRACK_W / 2 - 1, cy - 10, 2, 20, COL_DKGREY);   // unison mark
    if (kx > tx) _d.fillRect(tx, cy - 3, kx - tx, 6, COL_DKGREY);
    _d.fillRect(kx - SE_KNOB_W / 2, cy - SE_KNOB_H / 2, SE_KNOB_W, SE_KNOB_H, COL_BLACK);

    char buf[16];
    seValueText(param, v, buf, sizeof(buf));
    _d.setCursor(x + SE_VALUE_X, cy - 8);
    _d.print(buf);
}

int SynthEditor::sliderAt(int sx, int sy) const {
    for (int i = 0; i < SE_NSLIDERS; i++) {
        const SeSlider& s = SE_SLIDERS[i];
        if (sy >= s.y && sy < s.y + SE_ROW_H &&
            sx >= s.x + SE_LABEL_W - 20 && sx < s.x + SE_LABEL_W + SE_TRACK_W + 20)
            return s.param;
    }
    return -1;
}

int SynthEditor::sliderValueAt(int param, int sx) const {
    const SeSlider* s = seSlider(param);
    int max = ORGAN_SYNTH_MAX[param];
    int v = ((sx - (s->x + SE_LABEL_W)) * max + SE_TRACK_W / 2) / SE_TRACK_W;
    return v < 0 ? 0 : (v > max ? max : v);
}

void SynthEditor::set(int param, int value) {
    _p[param] = organSynthClamp(param, value);
    gServerPairing.sendOrgan(ORGAN_OP_SYNTH, (uint8_t)param, _p[param]);
}

SynthEditor::Result SynthEditor::touch(bool down, int sx, int sy) {
    if (!_p) return Result::NONE;
    if (down) {
        if (!_wasDown) {
            _wasDown = true;
            _drag = sliderAt(sx, sy);
        }
        if (_drag >= 0) {
            int v = sliderValueAt(_drag, sx);
            if (v != _p[_drag]) {
                set(_drag, v);
                drawSlider(_drag);
                _d.paintLater();
                return Result::CHANGED;
            }
        }
        return Result::NONE;
    }
    if (!_wasDown) return Result::NONE;
    _wasDown = false;
    bool dragged = (_drag >= 0);
    _drag = -1;
    if (dragged) return Result::NONE;

    if (sy >= SE_BTN_Y && sy < SE_BTN_Y + SE_BTN_H) {
        if (sx >= SE_BACK_X && sx < SE_BACK_X + SE_BACK_W) return Result::BACK;
        for (int i = 0; i < ORGAN_SYNTH_PRESET_N; i++) {
            int x = SE_PRESET_X0 + i * (SE_PRESET_W + SE_PRESET_GAP);
            if (sx >= x && sx < x + SE_PRESET_W) {
                for (int k = 0; k < ORGAN_SYNTH_PARAMS; k++) set(k, ORGAN_SYNTH_PRESETS[i].p[k]);
                drawWaves(0);
                drawWaves(1);
                for (int j = 0; j < SE_NSLIDERS; j++) drawSlider(SE_SLIDERS[j].param);
                _d.paintLater();
                return Result::CHANGED;
            }
        }
        return Result::NONE;
    }
    for (int osc = 0; osc < 2; osc++) {
        int y = SE_WAVE_Y[osc];
        if (sy < y || sy >= y + SE_WAVE_H) continue;
        for (int i = 0; i < seWaveCount(osc); i++) {
            int x = seWaveBtnX(osc, i);
            if (sx >= x && sx < x + seWaveW(osc)) {
                int param = (osc == 0) ? SYN_OSC1 : SYN_OSC2;
                if (_p[param] == i) return Result::NONE;
                set(param, i);
                drawWaves(osc);
                _d.paintLater();
                return Result::CHANGED;
            }
        }
    }
    return Result::NONE;
}
