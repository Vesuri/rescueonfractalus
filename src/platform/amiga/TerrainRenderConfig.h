#pragma once

// Retained startup configuration in version.s.  Loaders may patch this word before
// entering the executable; the scene copies it once during initialize().
extern "C" unsigned short rof_enhanced_terrain_value;

enum FlightTerrainRenderer {
    kFlightTerrainOriginal = 0,
    kFlightTerrainEnhanced = 1
};

static inline FlightTerrainRenderer configuredFlightTerrainRenderer()
{
    return rof_enhanced_terrain_value
        ? kFlightTerrainEnhanced
        : kFlightTerrainOriginal;
}
