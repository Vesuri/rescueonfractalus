// terrain OCS palette — 4 entries, 12-bit 0x0RGB, for GTIA mode-10 nibble zones.
// Nibble mapping (see StandbyScene::render): 0→col0, 1-2→col1, 3-4→col2, 5-8+→col3.
// Calibrated against SDL reference (atari000.png): terrain area = solid (82,132,13) ≈ $C8.
static const uint16_t kTerrainPalette[4] = {
    0x013,  // col0: COLBK sky — dark teal placeholder (will be set from mem[$0071] in R2)
    0x570,  // col1: COLPF0-1 range — lighter green
    0x580,  // col2: COLPF2-3 range — (82,132,13) Atari $C8 green
    0x580,  // col3: COLPM0-3 range — (82,132,13) dominant door fill (nibble 8 = $88 terrain)
};
