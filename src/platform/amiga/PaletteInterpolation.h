#pragma once

// $08A2 advances once per wrap of the descending 8-bit $08A1 timer, while
// (phase >> 2) selects one atmosphere-table state.  Return sixteen evenly
// spaced positions through that four-wrap interval.  Countdown zero is the
// boundary frame and is followed by 255, so both represent the new segment's
// start rather than a one-frame backwards jump.
static inline uint8_t atmosphereBlendStep(uint8_t phase, uint8_t countdown)
{
    const uint16_t elapsed = countdown == 0u ? 0u : (uint16_t)(255u - countdown);
    const uint16_t interval = (uint16_t)(((uint16_t)(phase & 3u) << 8) + elapsed);
    return (uint8_t)(interval >> 6);       // 0..15, denominator 16
}

static inline uint8_t lerpOCSNibble(uint8_t from, uint8_t to, uint8_t step)
{
    if (to >= from)
        return (uint8_t)(from + (((uint16_t)(to - from) * step + 8u) >> 4));
    return (uint8_t)(from - (((uint16_t)(from - to) * step + 8u) >> 4));
}

// Interpolate the three independent OCS RGB nibbles.  step is a sixteenth:
// step 0 is the current table colour and the original boundary supplies the
// exact next endpoint after step 15, so the day/night period does not grow.
static inline uint16_t lerpOCS(uint16_t from, uint16_t to, uint8_t step)
{
    const uint16_t r = (uint16_t)lerpOCSNibble((uint8_t)((from >> 8) & 0x0Fu),
                                               (uint8_t)((to >> 8) & 0x0Fu), step);
    const uint16_t g = (uint16_t)lerpOCSNibble((uint8_t)((from >> 4) & 0x0Fu),
                                               (uint8_t)((to >> 4) & 0x0Fu), step);
    const uint16_t b = (uint16_t)lerpOCSNibble((uint8_t)(from & 0x0Fu),
                                               (uint8_t)(to & 0x0Fu), step);
    return (uint16_t)((r << 8) | (g << 4) | b);
}
