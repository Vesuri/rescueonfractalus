#include "cpu/cpu.h"
#include "rof_data.h"
#include "rof_data_recipe.h"

volatile unsigned short rof_data_error;

/* Translate a cartridge-file offset to the compact package assembled by
 * tools/extract_rom_data.py.  Return null for a range the port did not request. */
const unsigned char *rof_data_at_rom_offset(unsigned long off, unsigned long len)
{
    unsigned long p;
    if (off >= ROF_DATA_ROM0_OFFSET &&
        off - ROF_DATA_ROM0_OFFSET <= ROF_DATA_ROM0_SIZE &&
        len <= ROF_DATA_ROM0_SIZE - (off - ROF_DATA_ROM0_OFFSET))
        p = ROF_DATA_PACK0_OFFSET + off - ROF_DATA_ROM0_OFFSET;
    else if (off >= ROF_DATA_ROM1_OFFSET &&
             off - ROF_DATA_ROM1_OFFSET <= ROF_DATA_ROM1_SIZE &&
             len <= ROF_DATA_ROM1_SIZE - (off - ROF_DATA_ROM1_OFFSET))
        p = ROF_DATA_PACK1_OFFSET + off - ROF_DATA_ROM1_OFFSET;
    else if (off >= ROF_DATA_ROM2_OFFSET &&
             off - ROF_DATA_ROM2_OFFSET <= ROF_DATA_ROM2_SIZE &&
             len <= ROF_DATA_ROM2_SIZE - (off - ROF_DATA_ROM2_OFFSET))
        p = ROF_DATA_PACK2_OFFSET + off - ROF_DATA_ROM2_OFFSET;
    else
        return 0;
    return rof_game_data + p;
}

int rof_data_available(void)
{
    static const unsigned long nibble_crc[16] = {
        0x00000000ul, 0x1DB71064ul, 0x3B6E20C8ul, 0x26D930ACul,
        0x76DC4190ul, 0x6B6B51F4ul, 0x4DB26158ul, 0x5005713Cul,
        0xEDB88320ul, 0xF00F9344ul, 0xD6D6A3E8ul, 0xCB61B38Cul,
        0x9B64C2B0ul, 0x86D3D2D4ul, 0xA00AE278ul, 0xBDBDF21Cul
    };
    const unsigned char *hs;
    unsigned long crc = 0xFFFFFFFFul;
    unsigned long i;
    rof_data_error = 1;
    if (rof_data_descriptor.version != ROF_DATA_VERSION ||
        rof_data_descriptor.ready != ROF_DATA_READY ||
        rof_data_descriptor.data != rof_game_data ||
        rof_data_descriptor.size != ROF_DATA_PACKAGE_SIZE)
        return 0;
    for (i = 0; i < ROF_DATA_PACKAGE_SIZE; i++) {
        crc ^= rof_game_data[i];
        crc = (crc >> 4) ^ nibble_crc[crc & 15u];
        crc = (crc >> 4) ^ nibble_crc[crc & 15u];
    }
    rof_data_error = 2;
    if ((crc ^ 0xFFFFFFFFul) != ROF_DATA_PACKAGE_CRC32) return 0;
    /* Readable sentinels make a debugger failure self-explanatory too. */
    hs = rof_data_at_rom_offset(0x6200ul, 4);
    if (!hs || hs[0] != 0x28 || hs[1] != 0x29 || hs[2] != 0x27 || hs[3] != 0x28)
        return 0;
    rof_data_error = 3;
    hs = rof_data_at_rom_offset(0x8274ul, 4);
    if (!hs || hs[0] != 0xF9 || hs[1] != 0x16 || hs[2] != 0x70 || hs[3] != 0x04)
        return 0; /* Logo stroke stream */
    rof_data_error = 0;
    return 1;
}

/* The Amiga staged loader calls these through rof_boot.c.  Its resume token is now a
 * recipe index rather than an XEX byte offset; rof_boot_chain only requires monotonicity. */
void rof_data_reset_mem(void (*write)(uint16_t, const uint8_t*, uint32_t),
                        const uint8_t *charset, uint32_t charset_len)
{
    static const uint8_t zero[64] = {0};
    uint32_t a;
    for (a = 0; a < 65536u; a += sizeof zero)
        write((uint16_t)a, zero, sizeof zero);
    write(0xE000u, charset, charset_len > 1024u ? 1024u : charset_len);
}

uint32_t rof_data_load_stage(uint32_t from,
                             void (*write)(uint16_t, const uint8_t*, uint32_t))
{
    uint32_t stage;
    uint32_t end;
    if (from >= ROF_DATA_COPY_COUNT) return from;
    for (stage = 0; stage < 4u && from >= kRofDataStageEnds[stage]; stage++) {}
    if (stage == 4u) return from;
    end = kRofDataStageEnds[stage];
    while (from < end) {
        const RofDataCopy *op = &kRofDataCopies[from++];
        write(op->dst, rof_game_data + op->src, op->len);
    }
    return from;
}
