#include "OrganPage.h"
#include "ServerPairing.h"
#include "MagiMsg.h"   // OrganOp constants

// Footage labels, low harmonic → high (matches the Hammond panel order).
static const char* const OR_FOOTAGE[OR_N] =
    { "16", "5 1/3", "8", "4", "2 2/3", "2", "1 3/5", "1 1/3", "1" };

// Organ voice models — must line up with the server's ORGAN_TYPE_NAMES order.
static const char* const OR_TYPE_NAMES[OR_TYPE_N] =
    { "DRAWBAR", "TONEWHEEL", "CLAUDE", "NEBULA", "PROC" };
static const char* const OR_VC_NAMES[OR_VC_N]   = { "OFF", "V1", "V2", "V3", "C1", "C2", "C3" };
static const char* const OR_LES_NAMES[OR_LES_N] = { "STOP", "SLOW", "FAST" };
static const char* const OR_DRV_NAMES[OR_DRV_N] = { "OFF", "ON" };
static const char* const OR_RVB_NAMES[OR_RVB_N] = { "OFF", "ROOM", "HALL" };

static int orFxX(int i) { return OR_FX_X0 + i * (OR_FX_W + OR_FX_GAP); }

// Per-type knob column.  Order/meaning must match the server's s_param mapping.
struct OrKnobCfg { int n; const char* names[OR_NKNOB]; };
static const OrKnobCfg OR_KNOBCFG[OR_TYPE_N] = {
    { 0, { "" } },                                  // DRAWBAR  — drawbars only
    { 1, { "CLICK" } },                             // TONEWHEEL
    { 0, { "" } },                                  // CLAUDE   — knob-less on purpose
    { 3, { "DETUNE", "GLIDE", "BRIGHT" } },         // NEBULA
    { 0, { "" } },                                  // PROC — sliders come per-sound
};

// PROC sounds — name + sliders + defaults; mirror of the server's PROC_SOUNDS
// registry in proc_sounds.h (same order).
struct OrProcCfg { const char* name; OrKnobCfg knobs; uint8_t defs[OR_NKNOB]; };
static const OrProcCfg OR_PROC[OR_PROC_N] = {
    { "TWO TRIBES", { 5, { "ATTACK", "CHORUS", "BRIGHT", "SCOOP", "THROAT" } },
                    { 4, 4, 8, 4, 4 } },
    { "CHOIR",      { 5, { "BREATH", "ENSMBL", "BRIGHT", "ATTACK", "AHH" } },
                    { 4, 4, 8, 4, 4 } },
};

// PROC's 5-slider column starts further left (no drawbars in that mode).
static int orKnobX(int type, int k) {
    int x0 = (type == OR_TYPE_PROC) ? OR_KCOL_X0_PROC : OR_KCOL_X0;
    return x0 + k * (OR_KCOL_W + OR_KCOL_GAP);
}

// Classic Hammond registrations (drawbar values 0..8, low → high footage).
struct OrganPreset { const char* name; uint8_t bars[OR_N]; };
static const OrganPreset OR_PRESETS[OR_PRESET_N] = {
    { "FULL",   { 8, 8, 8, 8, 8, 8, 8, 8, 8 } },   // everything out — big and bright
    { "JAZZ",   { 8, 8, 8, 0, 0, 0, 0, 0, 0 } },   // Jimmy Smith comping
    { "GOSPEL", { 8, 8, 8, 8, 0, 0, 0, 8, 8 } },   // foundation + sparkle on top
    { "FLUTE",  { 0, 0, 8, 8, 0, 6, 0, 0, 0 } },   // mellow flutes
    { "ROCK",   { 8, 8, 8, 8, 0, 0, 0, 0, 0 } },   // Booker T / Green Onions
};

OrganPage::OrganPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song)
    : _d(display)
    , _touch(touch)
    , _song(song)
    , _open(false)
    , _wasDown(false)
    , _dragCol(-1)
    , _dragKnob(-1)
    , _activePreset(1)   // JAZZ — matches the default registration below
    , _type(0)           // DRAWBAR
    , _vibChorus(0)      // effects default OFF so the page opens dry
    , _leslie(0)
    , _drive(0)
    , _reverb(0)
    , _procSel(0)
{
    // Classic full-drawbar default: 16' 5 1/3' 8' pulled, rest in.
    const uint8_t init[OR_N] = { 8, 8, 8, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < OR_N; i++) _bars[i] = init[i];

    for (int t = 0; t < OR_TYPE_N; t++)
        for (int k = 0; k < OR_NKNOB; k++) _knob[t][k] = 0;
    _knob[1][0] = 4;                         // TONEWHEEL click
    _knob[3][0] = 4; _knob[3][1] = 5; _knob[3][2] = 6;   // NEBULA detune/glide/bright
    for (int p = 0; p < OR_PROC_N; p++)
        for (int k = 0; k < OR_NKNOB; k++) _procKnob[p][k] = OR_PROC[p].defs[k];
}

// The active slider set: the type's knobs, or the selected sound's in PROC.
static const OrKnobCfg& orActiveCfg(int type, int procSel) {
    return (type == OR_TYPE_PROC) ? OR_PROC[procSel].knobs : OR_KNOBCFG[type];
}

uint8_t& OrganPage::knobRef(int k) {
    return (_type == OR_TYPE_PROC) ? _procKnob[_procSel][k] : _knob[_type][k];
}

// One organ per song: the page's ENTIRE state (voice, drawbars, sliders,
// effects) is the song's OrganPatch.  Any change writes it and patches the
// 20-byte struct to the server, so a live tweak persists in the song — and
// the sequencer's block-transition reapply lands on the values just played.
// Only persists when the song actually uses the organ (has an ORGAN column);
// noodling on the page in an organ-less song changes nothing.
void OrganPage::applyToSong() {
    bool used = false;
    for (int c = 1; c < MAX_COLUMNS; c++)
        if (_song.columns[c].midiChannel == ORGAN_CHANNEL) { used = true; break; }
    if (!used) return;

    OrganPatch& op = _song.organ;
    op.flags = 1;
    op.voice = (_type < OR_TYPE_PROC) ? (uint8_t)_type
                                      : (uint8_t)(OR_TYPE_PROC + _procSel);
    for (int b = 0; b < ORGAN_PATCH_BARS; b++) op.bars[b] = _bars[b];
    const OrKnobCfg& cfg = orActiveCfg(_type, _procSel);
    for (int k = 0; k < ORGAN_PATCH_SLIDERS; k++)
        op.sliders[k] = (k < cfg.n) ? knobRef(k) : 0;
    op.vibChorus = (uint8_t)_vibChorus;
    op.leslie    = (uint8_t)_leslie;
    op.drive     = (uint8_t)_drive;
    op.reverb    = (uint8_t)_reverb;
    gServerPairing.sendSongPatch(_song, &op, (uint8_t)sizeof(OrganPatch));
}

// The song is master: opening the page adopts its OrganPatch when present.
void OrganPage::adoptFromSong() {
    const OrganPatch& op = _song.organ;
    if (!(op.flags & 1)) return;
    if (op.voice < OR_TYPE_PROC) {
        _type = op.voice;
    } else {
        _type    = OR_TYPE_PROC;
        _procSel = op.voice - OR_TYPE_PROC;
        if (_procSel >= OR_PROC_N) _procSel = OR_PROC_N - 1;
    }
    for (int b = 0; b < OR_N; b++) _bars[b] = (op.bars[b] > 8) ? 8 : op.bars[b];
    const OrKnobCfg& cfg = orActiveCfg(_type, _procSel);
    for (int k = 0; k < cfg.n; k++)
        knobRef(k) = (op.sliders[k] > 8) ? 8 : op.sliders[k];
    _vibChorus    = op.vibChorus % OR_VC_N;
    _leslie       = op.leslie    % OR_LES_N;
    _drive        = op.drive     % OR_DRV_N;
    _reverb       = op.reverb    % OR_RVB_N;
    _activePreset = -1;   // registration came from the song, not a preset
}

void OrganPage::open() {
    _open    = true;
    _wasDown = _touch.isTouched;
    _dragCol = -1;
    adoptFromSong();   // song's ORGAN columns (if any) override the page state
    _d.clear();
    draw();
    _d.paint();

    // Flip the server into organ mode and sync type, effects + registration.
    gServerPairing.sendOrgan(ORGAN_OP_ENTER);
    gServerPairing.sendOrgan(ORGAN_OP_TYPE,      0, (uint8_t)_type);
    if (_type == OR_TYPE_PROC)
        gServerPairing.sendOrgan(ORGAN_OP_PROCSEL, 0, (uint8_t)_procSel);
    gServerPairing.sendOrgan(ORGAN_OP_VIBCHORUS, 0, (uint8_t)_vibChorus);
    gServerPairing.sendOrgan(ORGAN_OP_LESLIE,    0, (uint8_t)_leslie);
    gServerPairing.sendOrgan(ORGAN_OP_DRIVE,     0, (uint8_t)_drive);
    gServerPairing.sendOrgan(ORGAN_OP_REVERB,    0, (uint8_t)_reverb);
    syncKnobs();
    for (int i = 0; i < OR_N; i++) gServerPairing.sendOrgan(ORGAN_OP_SET, i, _bars[i]);
}

void OrganPage::draw() {
    _d.fillRect(0, 0, 960, 540, COL_WHITE);
    drawHeader();
    if (_type == OR_TYPE_PROC) {
        drawProcList();
    } else {
        drawPresets();
        for (int i = 0; i < OR_N; i++) drawBar(i);
    }
    drawKnobs();
    drawFooter();
}

// PROC mode: the sound list takes the presets + drawbars area.
void OrganPage::drawProcList() {
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(OR_PRESET_X + 8, OR_HDR_H + 4);
    _d.print("SOUNDS");
    for (int i = 0; i < OR_PROC_N; i++) {
        int y = OR_PRESET_Y0 + i * (OR_PRESET_H + OR_PRESET_GAP);
        bool active = (i == _procSel);
        uint8_t bg = active ? COL_BLACK : COL_WHITE;
        uint8_t fg = active ? COL_WHITE : COL_BLACK;
        uiButton(_d, OR_PRESET_X, y, OR_PROCBTN_W, OR_PRESET_H, OR_PROC[i].name, bg, fg, 3);
    }
}

int OrganPage::hitProc(int sx, int sy) const {
    if (sx < OR_PRESET_X || sx >= OR_PRESET_X + OR_PROCBTN_W) return -1;
    for (int i = 0; i < OR_PROC_N; i++) {
        int y = OR_PRESET_Y0 + i * (OR_PRESET_H + OR_PRESET_GAP);
        if (sy >= y && sy < y + OR_PRESET_H) return i;
    }
    return -1;
}

void OrganPage::selectProc(int idx) {
    if (idx < 0 || idx >= OR_PROC_N || idx == _procSel) return;
    _procSel = idx;
    gServerPairing.sendOrgan(ORGAN_OP_PROCSEL, 0, (uint8_t)_procSel);
    syncKnobs();          // push the new sound's slider values
    applyToSong();
    drawProcList();
    drawKnobs();
    _d.paintLater();
}

void OrganPage::drawPresets() {
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(OR_PRESET_X + 8, OR_HDR_H + 4);
    _d.print("PRESETS");
    for (int i = 0; i < OR_PRESET_N; i++) {
        int y = OR_PRESET_Y0 + i * (OR_PRESET_H + OR_PRESET_GAP);
        bool active = (i == _activePreset);
        uint8_t bg = active ? COL_BLACK : COL_WHITE;
        uint8_t fg = active ? COL_WHITE : COL_BLACK;
        uiButton(_d, OR_PRESET_X, y, OR_PRESET_W, OR_PRESET_H, OR_PRESETS[i].name, bg, fg, 3);
    }
}

void OrganPage::applyPreset(int idx) {
    if (idx < 0 || idx >= OR_PRESET_N) return;
    for (int i = 0; i < OR_N; i++) _bars[i] = OR_PRESETS[idx].bars[i];
    _activePreset = idx;
    drawPresets();
    for (int i = 0; i < OR_N; i++) drawBar(i);
    _d.paintLater();
    for (int i = 0; i < OR_N; i++) gServerPairing.sendOrgan(ORGAN_OP_SET, (uint8_t)i, _bars[i]);
    applyToSong();
}

void OrganPage::drawHeader() {
    _d.fillRect(0, 0, 960, OR_HDR_H, COL_BLACK);

    // Four tap-to-cycle controls.  Effect buttons invert (black) when not OFF,
    // so an active effect reads at a glance.
    char buf[20];
    snprintf(buf, sizeof(buf), "%s", OR_TYPE_NAMES[_type]);
    uiButton(_d, orFxX(0), OR_BTN_Y, OR_FX_W, OR_BTN_H, buf, COL_WHITE, COL_BLACK, 2);

    bool vcOn  = (_vibChorus != 0), lesOn = (_leslie != 0);
    bool drvOn = (_drive != 0),     rvbOn = (_reverb != 0);
    snprintf(buf, sizeof(buf), "VIB %s", OR_VC_NAMES[_vibChorus]);
    uiButton(_d, orFxX(1), OR_BTN_Y, OR_FX_W, OR_BTN_H, buf,
             vcOn ? COL_BLACK : COL_WHITE, vcOn ? COL_WHITE : COL_BLACK, 2);
    snprintf(buf, sizeof(buf), "LES %s", OR_LES_NAMES[_leslie]);
    uiButton(_d, orFxX(2), OR_BTN_Y, OR_FX_W, OR_BTN_H, buf,
             lesOn ? COL_BLACK : COL_WHITE, lesOn ? COL_WHITE : COL_BLACK, 2);
    snprintf(buf, sizeof(buf), "DRIVE %s", OR_DRV_NAMES[_drive]);
    uiButton(_d, orFxX(3), OR_BTN_Y, OR_FX_W, OR_BTN_H, buf,
             drvOn ? COL_BLACK : COL_WHITE, drvOn ? COL_WHITE : COL_BLACK, 2);
    snprintf(buf, sizeof(buf), "VERB %s", OR_RVB_NAMES[_reverb]);
    uiButton(_d, orFxX(4), OR_BTN_Y, OR_FX_W, OR_BTN_H, buf,
             rvbOn ? COL_BLACK : COL_WHITE, rvbOn ? COL_WHITE : COL_BLACK, 2);

    uiButton(_d, OR_HOME_X, OR_BTN_Y, OR_HOME_W, OR_BTN_H, "HOME", COL_WHITE, COL_BLACK, 3);
}

void OrganPage::drawKnobs() {
    // Clear the column, then draw the active type's knobs (if any).
    int x0 = (_type == OR_TYPE_PROC) ? OR_KCOL_X0_PROC : OR_KCOL_X0;
    _d.fillRect(x0 - 6, OR_HDR_H + 2, 960 - (x0 - 6),
                (OR_LABEL_Y + 16) - (OR_HDR_H + 2), COL_WHITE);
    for (int k = 0; k < orActiveCfg(_type, _procSel).n; k++) drawKnob(k);
}

void OrganPage::drawKnob(int k) {
    int x  = orKnobX(_type, k);
    int cx = x + OR_KCOL_W / 2;
    int v  = knobRef(k);
    char b[4]; snprintf(b, sizeof(b), "%d", v);

    // Wipe this knob's slot first, else the old tab/number ghosts (e-paper).
    _d.fillRect(x, OR_HDR_H + 2, OR_KCOL_W, (OR_LABEL_Y + 16) - (OR_HDR_H + 2), COL_WHITE);

    _d.setTextSize(3); _d.setTextColor(COL_BLACK);
    int nw = (int)strlen(b) * 18;
    _d.setCursor(cx - nw / 2, OR_VAL_Y); _d.print(b);

    _d.fillRect(cx - 2, OR_TOP, 4, OR_BOT - OR_TOP, COL_LTGREY);     // groove
    int tabY = OR_TOP + v * (OR_BOT - OR_TOP) / 8;
    if (tabY > OR_TOP) _d.fillRect(cx - 3, OR_TOP, 6, tabY - OR_TOP, COL_DKGREY);
    int ky = tabY - OR_KNOB_H / 2;
    _d.fillRect(x + 2, ky, OR_KCOL_W - 4, OR_KNOB_H, COL_DKGREY);    // grey tab (≠ drawbars)
    _d.drawRect(x + 2, ky, OR_KCOL_W - 4, OR_KNOB_H, COL_BLACK);
    _d.setTextSize(2); _d.setTextColor(COL_WHITE);
    int kw = (int)strlen(b) * 12;
    _d.setCursor(cx - kw / 2, ky + (OR_KNOB_H - 16) / 2); _d.print(b);

    const char* lab = orActiveCfg(_type, _procSel).names[k];
    _d.setTextSize(1); _d.setTextColor(COL_BLACK);
    int lw = (int)strlen(lab) * 6;
    _d.setCursor(cx - lw / 2, OR_LABEL_Y + 2); _d.print(lab);
}

void OrganPage::syncKnobs() {
    for (int k = 0; k < orActiveCfg(_type, _procSel).n; k++)
        gServerPairing.sendOrgan(ORGAN_OP_PARAM, (uint8_t)k, knobRef(k));
}

int OrganPage::hitKnob(int sx, int sy) const {
    if (sy < OR_HDR_H || sy >= OR_LABEL_Y) return -1;
    for (int k = 0; k < orActiveCfg(_type, _procSel).n; k++) {
        int x = orKnobX(_type, k);
        if (sx >= x && sx < x + OR_KCOL_W) return k;
    }
    return -1;
}

void OrganPage::drawFooter() {
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(20, 510);
    _d.print(_type == OR_TYPE_PROC
                 ? "Pick a sound - play your MIDI keyboard to hear it"
                 : "Drag the drawbars - play your MIDI keyboard to sound the organ");
}

void OrganPage::drawBar(int i) {
    int x  = OR_MARGIN + i * OR_COL_W;
    int cx = x + OR_COL_W / 2;
    int v  = _bars[i];

    // Hammond colour code, mapped to the 4 e-paper shades.
    uint8_t knobBg, knobFg;
    switch (i) {
        case 0: case 1:         knobBg = COL_DKGREY; knobFg = COL_WHITE; break;  // 16',5 1/3' brown
        case 4: case 6: case 7: knobBg = COL_BLACK;  knobFg = COL_WHITE; break;  // mutations  black
        default:                knobBg = COL_WHITE;  knobFg = COL_BLACK; break;  // foundation white
    }

    // Clear the column body (down to just below the footage label, above the footer).
    _d.fillRect(x, OR_HDR_H + 2, OR_COL_W, (OR_LABEL_Y + 16) - (OR_HDR_H + 2), COL_WHITE);

    // Value number on top.
    char b[4]; snprintf(b, sizeof(b), "%d", v);
    int nw = (int)strlen(b) * 6 * 4;
    _d.setTextSize(4);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(cx - nw / 2, OR_VAL_Y);
    _d.print(b);

    // Groove + pulled stem + knob.
    _d.fillRect(cx - 2, OR_TOP, 4, OR_BOT - OR_TOP, COL_LTGREY);
    int tabY = OR_TOP + v * (OR_BOT - OR_TOP) / 8;
    if (tabY > OR_TOP) _d.fillRect(cx - 4, OR_TOP, 8, tabY - OR_TOP, COL_DKGREY);

    int ky = tabY - OR_KNOB_H / 2;
    _d.fillRect(cx - OR_KNOB_W / 2, ky, OR_KNOB_W, OR_KNOB_H, knobBg);
    _d.drawRect(cx - OR_KNOB_W / 2,     ky,     OR_KNOB_W,     OR_KNOB_H,     COL_BLACK);
    _d.drawRect(cx - OR_KNOB_W / 2 + 1, ky + 1, OR_KNOB_W - 2, OR_KNOB_H - 2, COL_BLACK);
    // A value tick on the knob face so the setting reads at a glance.
    _d.setTextSize(2);
    _d.setTextColor(knobFg);
    int kw = (int)strlen(b) * 6 * 2;
    _d.setCursor(cx - kw / 2, ky + (OR_KNOB_H - 16) / 2);
    _d.print(b);

    // Footage label below.
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    const char* lab = OR_FOOTAGE[i];
    int lw = (int)strlen(lab) * 6 * 2;
    _d.setCursor(cx - lw / 2, OR_LABEL_Y);
    _d.print(lab);
}

OrganPage::Result OrganPage::poll() {
    if (!_open || !_touch.read()) return Result::NONE;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    if (down) {
        if (!_wasDown) {
            _wasDown = true;
            _dragCol = (_type != OR_TYPE_PROC &&      // no drawbars in PROC mode
                        sy >= OR_HDR_H && sx >= OR_MARGIN &&
                        sx < OR_MARGIN + OR_COL_W * OR_N)
                       ? (sx - OR_MARGIN) / OR_COL_W : -1;
            _dragKnob = (_dragCol < 0) ? hitKnob(sx, sy) : -1;
        }
        int span = OR_BOT - OR_TOP;
        int v = ((sy - OR_TOP) * 8 + span / 2) / span;
        if (v < 0) v = 0;
        if (v > 8) v = 8;
        if (_dragCol >= 0 && _dragCol < OR_N) {
            if (v != _bars[_dragCol]) {
                _bars[_dragCol] = (uint8_t)v;
                gServerPairing.sendOrgan(ORGAN_OP_SET, (uint8_t)_dragCol, (uint8_t)v);
                applyToSong();
                drawBar(_dragCol);
                if (_activePreset != -1) { _activePreset = -1; drawPresets(); }  // hand-edited
                _d.paintLater();
            }
        } else if (_dragKnob >= 0) {
            if (v != knobRef(_dragKnob)) {
                knobRef(_dragKnob) = (uint8_t)v;
                gServerPairing.sendOrgan(ORGAN_OP_PARAM, (uint8_t)_dragKnob, (uint8_t)v);
                applyToSong();
                drawKnob(_dragKnob);
                _d.paintLater();
            }
        }
    } else if (_wasDown) {
        _wasDown = false;
        int col = _dragCol, knob = _dragKnob;
        _dragCol = -1; _dragKnob = -1;
        if (col < 0 && knob < 0) {
            if (hitHome(sx, sy)) {
                _open = false;
                gServerPairing.sendOrgan(ORGAN_OP_EXIT);
                return Result::HOME;
            }
            if (_type == OR_TYPE_PROC) {
                int pi = hitProc(sx, sy);
                if (pi >= 0) { selectProc(pi); return Result::NONE; }
            } else {
                int p = hitPreset(sx, sy);
                if (p >= 0) { applyPreset(p); return Result::NONE; }
            }

            int fx = hitFx(sx, sy);
            if (fx >= 0) {
                switch (fx) {
                    case 0: {
                            bool wasProc = (_type == OR_TYPE_PROC);
                            _type = (_type + 1) % OR_TYPE_N;
                            gServerPairing.sendOrgan(ORGAN_OP_TYPE,      0, (uint8_t)_type);
                            if (_type == OR_TYPE_PROC)
                                gServerPairing.sendOrgan(ORGAN_OP_PROCSEL, 0, (uint8_t)_procSel);
                            syncKnobs();          // push the new type's knob values
                            applyToSong();
                            if (wasProc || _type == OR_TYPE_PROC) draw();   // body layout swaps
                            else drawKnobs();     // its knob column differs
                            break;
                            }
                    case 1: _vibChorus = (_vibChorus + 1) % OR_VC_N;
                            gServerPairing.sendOrgan(ORGAN_OP_VIBCHORUS, 0, (uint8_t)_vibChorus);
                            applyToSong(); break;
                    case 2: _leslie    = (_leslie + 1) % OR_LES_N;
                            gServerPairing.sendOrgan(ORGAN_OP_LESLIE,    0, (uint8_t)_leslie);
                            applyToSong(); break;
                    case 3: _drive     = (_drive + 1) % OR_DRV_N;
                            gServerPairing.sendOrgan(ORGAN_OP_DRIVE,     0, (uint8_t)_drive);
                            applyToSong(); break;
                    case 4: _reverb    = (_reverb + 1) % OR_RVB_N;
                            gServerPairing.sendOrgan(ORGAN_OP_REVERB,    0, (uint8_t)_reverb);
                            applyToSong(); break;
                }
                drawHeader();
                _d.paintLater();
            }
        }
    }
    return Result::NONE;
}

int OrganPage::hitPreset(int sx, int sy) const {
    if (sx < OR_PRESET_X || sx >= OR_PRESET_X + OR_PRESET_W) return -1;
    for (int i = 0; i < OR_PRESET_N; i++) {
        int y = OR_PRESET_Y0 + i * (OR_PRESET_H + OR_PRESET_GAP);
        if (sy >= y && sy < y + OR_PRESET_H) return i;
    }
    return -1;
}

int OrganPage::hitFx(int sx, int sy) const {
    if (sy < OR_BTN_Y || sy >= OR_BTN_Y + OR_BTN_H) return -1;
    for (int i = 0; i < OR_FX_N; i++) {
        int x = orFxX(i);
        if (sx >= x && sx < x + OR_FX_W) return i;
    }
    return -1;
}

bool OrganPage::hitHome(int sx, int sy) const {
    return (sx >= OR_HOME_X && sx < OR_HOME_X + OR_HOME_W &&
            sy >= OR_BTN_Y  && sy < OR_BTN_Y  + OR_BTN_H);
}

void OrganPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}
