#pragma once
#include <stdint.h>

// CoreS3 battery meter — thin wrapper over M5.Power (AXP2101 on the
// internal I²C bus).  Works with the built-in cell AND with Battery
// Module 13.2 stacked under the device, since both feed the same PMU
// terminals.
//
// Usage:
//   setup():  batteryInit();
//   loop():   if (batteryPoll()) redrawHeader();   // poll rate-limited internally
//   draw:     batteryDrawIcon(x, y, headerBgColor);

void batteryInit();

// Sample the PMU (rate-limited to ~5 s).  Returns true if the visible
// state (percent bucket or charging flag) changed and a redraw is
// warranted.
bool batteryPoll();

int  batteryPercent();    // 0..100, or -1 if unknown
bool batteryCharging();

// Icon size — exposed so headers can reserve the right amount of space.
constexpr int BATT_ICON_W = 22;   // body + 2 px terminal cap
constexpr int BATT_ICON_H = 12;

void batteryDrawIcon(int x, int y, uint16_t bgColor);
