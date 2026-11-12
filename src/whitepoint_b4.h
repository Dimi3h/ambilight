// White of the TV's histogram per colour temperature (LG's setting: -50 warm ... 50 cool),
// measured on the OLED55B4 on 2026-11-12 with a white field in the top UI layer over HDR
// video: {setting, R, G, B} in raw histogram levels. The picture mode makes no difference,
// greys stay proportional and the primaries do not mix (docs/phase-0-1-findings.md).
#pragma once
#define WB_N 10
static const float WB_TABLE[WB_N][4] = {
    { -50, 253.8f, 237.0f, 205.3f },
    { -40, 254.0f, 238.1f, 213.3f },
    { -30, 253.8f, 239.9f, 221.2f },
    { -20, 253.6f, 239.7f, 227.7f },
    { -10, 253.8f, 240.9f, 236.0f },
    { 0, 253.7f, 241.8f, 243.9f },
    { 10, 252.0f, 241.1f, 246.0f },
    { 20, 250.9f, 239.9f, 247.9f },
    { 25, 249.8f, 239.9f, 248.8f },
    { 50, 246.7f, 239.7f, 253.6f },
};
// the setting the level profile (profile_b4.h) was recorded with
#define WB_PROFILE_TEMP -50.0
