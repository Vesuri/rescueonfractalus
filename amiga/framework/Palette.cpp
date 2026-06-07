#include "Palette.h"

uint8_t Palette::fadeTable[17][16];

void Palette::initialize()
{
    for (int fade = 0; fade < 17; fade++) {
        for (int value = 0; value < 16; value++) {
            fadeTable[fade][value] = (uint8_t)((fade * value) >> 4);
        }
    }
}

Palette::Palette(const uint16_t* palette, uint16_t colorCount, uint16_t fade, uint16_t fadeBaseColor) :
    sourcePalette(0),
    currentPalette(0),
    fade_(fade),
    fadeBaseColor(fadeBaseColor)
{
    setPalette(palette, colorCount);
}

Palette::~Palette()
{
    delete[] sourcePalette;
    delete[] currentPalette;
}

void Palette::setPalette(const uint16_t* palette, uint16_t colorCount)
{
    delete[] sourcePalette;
    delete[] currentPalette;

    sourcePalette = new uint16_t[colorCount];
    currentPalette = new uint16_t[colorCount];
    colorCount_ = colorCount;

    for (int i = 0; i < colorCount; i++) {
        sourcePalette[i] = palette[i];
        currentPalette[i] = palette[i];
    }
}

uint16_t Palette::colorCount() const
{
    return colorCount_;
}

uint16_t Palette::fade() const
{
    return fade_;
}

void Palette::setFade(uint16_t fade)
{
    this->fade_ = fade;
    update();
}

void Palette::setFadeBaseColor(uint16_t fadeBaseColor)
{
    this->fadeBaseColor = fadeBaseColor;
}

void Palette::update()
{
    uint16_t baseR = fadeBaseColor >> 8;
    uint16_t baseG = (fadeBaseColor & 0x0f0) >> 4;
    uint16_t baseB = fadeBaseColor & 0x00f;
    for (uint16_t i = 0; i < colorCount_; i++) {
        uint16_t color = sourcePalette[i];
        uint16_t r = color >> 8;
        uint16_t g = (color & 0x0f0) >> 4;
        uint16_t b = color & 0x00f;
        int16_t rDelta = r - baseR;
        int16_t gDelta = g - baseG;
        int16_t bDelta = b - baseB;
        uint16_t fadedR = baseR + (rDelta >= 0 ? fadeTable[fade_][rDelta] : -fadeTable[fade_][-rDelta]);
        uint16_t fadedG = baseG + (gDelta >= 0 ? fadeTable[fade_][gDelta] : -fadeTable[fade_][-gDelta]);
        uint16_t fadedB = baseB + (bDelta >= 0 ? fadeTable[fade_][bDelta] : -fadeTable[fade_][-bDelta]);
        currentPalette[i] = (uint16_t)((fadedR << 8) | (fadedG << 4) | fadedB);
    }
}
