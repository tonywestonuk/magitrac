#include "OrganPage.h"
#include "ServerPairing.h"
#include "MagiMsg.h"   // OrganOp constants

// Footage labels, low harmonic → high (matches the Hammond panel order).
static const char* const OR_FOOTAGE[OR_N] =
    { "16", "5 1/3", "8", "4", "2 2/3", "2", "1 3/5", "1 1/3", "1" };

// Organ voice models — must line up with the server's ORGAN_TYPE_NAMES order.
static const char* const OR_TYPE_NAMES[OR_TYPE_N] =
    { "DRAWBAR", "TONEWHEEL", "CLAUDE", "NEBULA", "SAMPLE" };
static const char* const OR_VC_NAMES[OR_VC_N]   = { "OFF", "V1", "V2", "V3", "C1", "C2", "C3" };
static const char* const OR_LES_NAMES[OR_LES_N] = { "STOP", "SLOW", "FAST" };
static const char* const OR_DRV_NAMES[OR_DRV_N] = { "OFF", "ON" };

static int orFxX(int i) { return OR_FX_X0 + i * (OR_FX_W + OR_FX_GAP); }

// Per-type knob column.  Order/meaning must match the server's s_param mapping.
struct OrKnobCfg { int n; const char* names[OR_NKNOB]; };
static const OrKnobCfg OR_KNOBCFG[OR_TYPE_N] = {
    { 0, { "", "", "" } },                          // DRAWBAR  — drawbars only
    { 1, { "CLICK", "", "" } },                     // TONEWHEEL
    { 0, { "", "", "" } },                          // CLAUDE   — knob-less on purpose
    { 3, { "DETUNE", "GLIDE", "BRIGHT" } },         // NEBULA
    { 3, { "SAMPLE", "MORPH", "BRIGHT" } },         // SAMPLE
};
static int orKnobX(int k) { return OR_KCOL_X0 + k * (OR_KCOL_W + OR_KCOL_GAP); }

// Classic Hammond registrations (drawbar values 0..8, low → high footage).
struct OrganPreset { const char* name; uint8_t bars[OR_N]; };
static const OrganPreset OR_PRESETS[OR_PRESET_N] = {
    { "FULL",   { 8, 8, 8, 8, 8, 8, 8, 8, 8 } },   // everything out — big and bright
    { "JAZZ",   { 8, 8, 8, 0, 0, 0, 0, 0, 0 } },   // Jimmy Smith comping
    { "GOSPEL", { 8, 8, 8, 8, 0, 0, 0, 8, 8 } },   // foundation + sparkle on top
    { "FLUTE",  { 0, 0, 8, 8, 0, 6, 0, 0, 0 } },   // mellow flutes
    { "ROCK",   { 8, 8, 8, 8, 0, 0, 0, 0, 0 } },   // Booker T / Green Onions
};

OrganPage::OrganPage(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display)
    , _touch(touch)
    , _open(false)
    , _wasDown(false)
    , _dragCol(-1)
    , _dragKnob(-1)
    , _activePreset(1)   // JAZZ — matches the default registration below
    , _type(0)           // DRAWBAR
    , _vibChorus(0)      // effects default OFF so the page opens dry
    , _leslie(0)
    , _drive(0)
{
    // Classic full-drawbar default: 16' 5 1/3' 8' pulled, rest in.
    const uint8_t init[OR_N] = { 8, 8, 8, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < OR_N; i++) _bars[i] = init[i];

    for (int t = 0; t < OR_TYPE_N; t++)
        for (int k = 0; k < OR_NKNOB; k++) _knob[t][k] = 0;
    _knob[1][0] = 4;                         // TONEWHEEL click
    _knob[3][0] = 4; _knob[3][1] = 5; _knob[3][2] = 6;   // NEBULA detune/glide/bright
    _knob[4][0] = 0; _knob[4][1] = 4; _knob[4][2] = 8;   // SAMPLE sample/morph/bright
}

void OrganPage::open() {
    _open    = true;
    _wasDown = _touch.isTouched;
    _dragCol = -1;
    _d.clear();
    draw();
    _d.paint();

    // Flip the server into organ mode and sync type, effects + registration.
    gServerPairing.sendOrgan(ORGAN_OP_ENTER);
    gServerPairing.sendOrgan(ORGAN_OP_TYPE,      0, (uint8_t)_type);
    gServerPairing.sendOrgan(ORGAN_OP_VIBCHORUS, 0, (uint8_t)_vibChorus);
    gServerPairing.sendOrgan(ORGAN_OP_LESLIE,    0, (uint8_t)_leslie);
    gServerPairing.sendOrgan(ORGAN_OP_DRIVE,     0, (uint8_t)_drive);
    syncKnobs();
    for (int i = 0; i < OR_N; i++) gServerPairing.sendOrgan(ORGAN_OP_SET, i, _bars[i]);
}

void OrganPage::draw() {
    _d.fillRect(0, 0, 960, 540, COL_WHITE);
    drawHeader();
    drawPresets();
    for (int i = 0; i < OR_N; i++) drawBar(i);
    drawKnobs();
    drawFooter();
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
}

void OrganPage::drawHeader() {
    _d.fillRect(0, 0, 960, OR_HDR_H, COL_BLACK);

    // Four tap-to-cycle controls.  Effect buttons invert (black) when not OFF,
    // so an active effect reads at a glance.
    char buf[20];
    snprintf(buf, sizeof(buf), "%s", OR_TYPE_NAMES[_type]);
    uiButton(_d, orFxX(0), OR_BTN_Y, OR_FX_W, OR_BTN_H, buf, COL_WHITE, COL_BLACK, 2);

    bool vcOn = (_vibChorus != 0), lesOn = (_leslie != 0), drvOn = (_drive != 0);
    snprintf(buf, sizeof(buf), "VIB %s", OR_VC_NAMES[_vibChorus]);
    uiButton(_d, orFxX(1), OR_BTN_Y, OR_FX_W, OR_BTN_H, buf,
             vcOn ? COL_BLACK : COL_WHITE, vcOn ? COL_WHITE : COL_BLACK, 2);
    snprintf(buf, sizeof(buf), "LESLIE %s", OR_LES_NAMES[_leslie]);
    uiButton(_d, orFxX(2), OR_BTN_Y, OR_FX_W, OR_BTN_H, buf,
             lesOn ? COL_BLACK : COL_WHITE, lesOn ? COL_WHITE : COL_BLACK, 2);
    snprintf(buf, sizeof(buf), "DRIVE %s", OR_DRV_NAMES[_drive]);
    uiButton(_d, orFxX(3), OR_BTN_Y, OR_FX_W, OR_BTN_H, buf,
             drvOn ? COL_BLACK : COL_WHITE, drvOn ? COL_WHITE : COL_BLACK, 2);

    uiButton(_d, OR_HOME_X, OR_BTN_Y, OR_HOME_W, OR_BTN_H, "HOME", COL_WHITE, COL_BLACK, 3);
}

void OrganPage::drawKnobs() {
    // Clear the column, then draw the active type's knobs (if any).
    _d.fillRect(OR_KCOL_X0 - 6, OR_HDR_H + 2, 960 - (OR_KCOL_X0 - 6),
                (OR_LABEL_Y + 16) - (OR_HDR_H + 2), COL_WHITE);
    for (int k = 0; k < OR_KNOBCFG[_type].n; k++) drawKnob(k);
}

void OrganPage::drawKnob(int k) {
    int x  = orKnobX(k);
    int cx = x + OR_KCOL_W / 2;
    int v  = _knob[_type][k];
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

    const char* lab = OR_KNOBCFG[_type].names[k];
    _d.setTextSize(1); _d.setTextColor(COL_BLACK);
    int lw = (int)strlen(lab) * 6;
    _d.setCursor(cx - lw / 2, OR_LABEL_Y + 2); _d.print(lab);
}

void OrganPage::syncKnobs() {
    for (int k = 0; k < OR_KNOBCFG[_type].n; k++)
        gServerPairing.sendOrgan(ORGAN_OP_PARAM, (uint8_t)k, _knob[_type][k]);
}

int OrganPage::hitKnob(int sx, int sy) const {
    if (sy < OR_HDR_H || sy >= OR_LABEL_Y) return -1;
    for (int k = 0; k < OR_KNOBCFG[_type].n; k++) {
        int x = orKnobX(k);
        if (sx >= x && sx < x + OR_KCOL_W) return k;
    }
    return -1;
}

void OrganPage::drawFooter() {
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    _d.setCursor(20, 510);
    _d.print("Drag the drawbars - play your MIDI keyboard to sound the organ");
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
            _dragCol = (sy >= OR_HDR_H && sx >= OR_MARGIN &&
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
                drawBar(_dragCol);
                if (_activePreset != -1) { _activePreset = -1; drawPresets(); }  // hand-edited
                _d.paintLater();
            }
        } else if (_dragKnob >= 0) {
            if (v != _knob[_type][_dragKnob]) {
                _knob[_type][_dragKnob] = (uint8_t)v;
                gServerPairing.sendOrgan(ORGAN_OP_PARAM, (uint8_t)_dragKnob, (uint8_t)v);
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
            int p = hitPreset(sx, sy);
            if (p >= 0) { applyPreset(p); return Result::NONE; }

            int fx = hitFx(sx, sy);
            if (fx >= 0) {
                switch (fx) {
                    case 0: _type      = (_type + 1) % OR_TYPE_N;
                            gServerPairing.sendOrgan(ORGAN_OP_TYPE,      0, (uint8_t)_type);
                            syncKnobs();          // push the new type's knob values
                            drawKnobs();          // its knob column differs
                            break;
                    case 1: _vibChorus = (_vibChorus + 1) % OR_VC_N;
                            gServerPairing.sendOrgan(ORGAN_OP_VIBCHORUS, 0, (uint8_t)_vibChorus); break;
                    case 2: _leslie    = (_leslie + 1) % OR_LES_N;
                            gServerPairing.sendOrgan(ORGAN_OP_LESLIE,    0, (uint8_t)_leslie);    break;
                    case 3: _drive     = (_drive + 1) % OR_DRV_N;
                            gServerPairing.sendOrgan(ORGAN_OP_DRIVE,     0, (uint8_t)_drive);     break;
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
