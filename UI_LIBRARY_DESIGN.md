# magitrac_ui — UI primitive library (design, for review)

Status: **proposal**. No code yet. Build starts after the setlist/backup work is
flashed and confirmed live. This doc is the thing to review/redline first.

## 1. Why

Drag-scroll has been hand-built four times: the note grid (`TouchHandler`), the
drum picker (`DrumEditorPage`), `FileBrowser` list mode (no inertia), and the
setlist server picker (`SetlistPage`). Plus there are de-facto widgets already —
`FileBrowser`, `KeyboardPopup`, `ConfirmDialog`, `HexpadPopup`, `HoldRepeat`. The
toolkit basically exists; it's just scattered, inconsistent, and re-implemented.

Goal: one focused, reusable library so a scroll list, button, dialog, etc. is
*used*, not *rebuilt*. Swing-like in **ergonomics** (components, events,
composition), e-paper-native in **mechanics**.

## 2. Non-goals (deliberately NOT Swing)

- **No retained-mode repaint loop.** Swing assumes cheap, frequent repaints +
  invalidation. E-paper is the opposite: ~17 ms repaints, ghosting, partial
  update. A widget tree that repaints on every invalidate looks bad and runs slow.
- **No layout managers (initially).** Absolute pixel bounds, like today. A simple
  vertical-stack helper can come later if it earns its place.
- **No heap-heavy scene graph.** Components are stack/owner-allocated value types,
  consistent with the project's "lean roll-your-own over wrappers" stance.

## 3. Model: immediate-mode components with events

Keep the idiom the codebase already uses (`open()` / `draw()` / `poll()`), but
make it a uniform, composable contract.

```cpp
struct UIRect { int x, y, w, h; };

enum class UIEventType : uint8_t {
    NONE, TAP, BACK, ITEM_TAP, ITEM_LONG, VALUE_CHANGED, SUBMIT, CANCEL, ...
};
struct UIEvent {
    UIEventType type = UIEventType::NONE;
    int  index = -1;     // ITEM_TAP/ITEM_LONG row index
    int  value = 0;      // VALUE_CHANGED payload
};

class UIComponent {
public:
    virtual void    layout(const UIRect& bounds) { _bounds = bounds; }
    virtual void    draw()  = 0;            // render into the shared painter
    virtual UIEvent poll()  = 0;            // consume touch, return one event
    bool            dirty() const { return _dirty; }   // wants a repaint
    void            clearDirty()  { _dirty = false; }
protected:
    UIRect _bounds{};
    bool   _dirty = false;                  // set when state changed visibly
    EPD_PainterAdafruit& _d;  GT911_Lite& _touch;   // injected, as today
};
```

- `poll()` returns **one event per call** (matches `FileBrowserResult` today).
- Optional Swing-style sugar later: `onItemTap = [](int i){...}` callbacks layered
  over the event return — but the event return is the load-bearing API (no
  capture/lifetime traps, easy to reason about on embedded).

## 4. E-paper repaint strategy (the important part)

- Components **draw into the shared `EPD_PainterAdafruit`**; they do **not** own
  framebuffers.
- A component only redraws the region it changed and sets `_dirty`. The **owner**
  (page or container) coalesces a single `paintLater()` per frame after polling
  all children — one deferred flush, not one per widget.
- Scroll/animation snaps to **whole rows**: redraw only when the integer top-row
  changes (the trick already used in the note grid + new setlist picker). Fractional
  scroll lives in state; the screen never sub-row-scrolls.
- `clear()` for anti-ghost stays the owner's call at page transitions, per the
  existing convention.

## 5. Flagship component: `UIScrollList`

The one that pays for the whole library on day one.

```cpp
// Data source: either a simple string list, or a custom row renderer for rich
// rows (e.g. the setlist's Move/After buttons).
class UIScrollListModel {
public:
    virtual int  count() const = 0;
    virtual void drawRow(int index, const UIRect& row, bool selected) = 0;
    // default text impl provided for the common case
};

class UIScrollList : public UIComponent {
public:
    void setModel(UIScrollListModel* m);
    void setRowHeight(int px);
    void scrollTo(int index);                 // ensure visible
    int  topRow() const;

    void    draw() override;
    UIEvent poll() override;   // emits ITEM_TAP(index) / drag / fling
    // built in: drag-scroll, inertia (shared INERTIA_* constants), tap vs drag
    //           disambiguation, edge clamp (no wrap), scrollbar/position hint.
};
```

Inertia/threshold tuning reuses `Constants.h` `INERTIA_DECAY / MIN_VEL / MAX_VEL /
STOP_VEL` so the feel matches the note grid everywhere.

## 6. Component catalogue (extract from existing, in priority order)

| Component        | Replaces / consolidates                          |
|------------------|--------------------------------------------------|
| `UIScrollList`   | setlist picker, FileBrowser list mode, drum picker, note-grid scroll |
| `UIButton`       | the ad-hoc `uiButton()` calls everywhere          |
| `UILabel`        | centred/truncated text drawing repeated by hand   |
| `UIConfirmDialog`| `ConfirmDialog`                                   |
| `UIKeyboard`     | `KeyboardPopup`                                    |
| `UIHexPad`       | `HexpadPopup`                                      |
| `UIRepeatButton` | `HoldRepeat`                                       |
| `UIGrid`         | FileBrowser grid (SD), perform pads                |
| `UIPanel`        | container: owns children, dispatches draw/poll, coalesces paint |

`UIPanel` is the "Swing JPanel" — a container that lays children out by absolute
bounds, forwards `poll()` and merges their events, and does the single
`paintLater()`. Layout helpers (vstack/hstack) optional, later.

## 7. Packaging

- Sibling Arduino library `magitrac_ui/` (mirrors `magitrac_lib/`), symlinked into
  `~/Documents/Arduino/libraries/`. Pulled in via `#include <magitrac_ui.h>`.
- Depends only on `EPD_Painter_Adafruit`, `gt911_lite`, `Constants.h` — no
  TrackerData/song coupling, so it's reusable by the **server's** CoreS3 touch UI
  too (which has its own bezel/list screens).
- Naming: `UI*` prefix, no `magitrac` in type names (it's generic).

## 8. Migration path (incremental, no big-bang)

1. Land `UIComponent` + `UIScrollList` + `UIPanel`.
2. Migrate the **setlist server picker** first (smallest, freshest, already
   isolated) → proves the API end-to-end.
3. Migrate `FileBrowser` list mode → the Songs page gets inertia for free.
4. Migrate the drum picker, then the note-grid scroll.
5. Extract `UIButton`/`UILabel` opportunistically as screens are touched.
6. Old classes stay until their last caller is migrated — nothing breaks at once.

## 9. Decisions to confirm before coding

1. **Event return vs callbacks** as the primary API. (Proposed: event return is
   primary; callbacks are optional sugar.)
2. **Library name** `magitrac_ui` and `UI*` type prefix — ok?
3. **Server reuse**: design it dependency-free so the CoreS3 server UI can adopt
   it too, or keep it client-only for now?
4. **Scope of v1**: just `UIScrollList` (+ the base + `UIPanel`), or also pull
   `UIButton`/`UILabel` in the first cut?
5. **Custom rich rows**: renderer-interface (`drawRow`) vs a small child-widget
   list per row. (Proposed: renderer callback — far cheaper on e-paper/RAM.)
