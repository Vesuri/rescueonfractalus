#ifndef ROF_DATA_H
#define ROF_DATA_H

#include "rof_data_layout.h"

#define ROF_DATA_VERSION       1u
#define ROF_DATA_READY         0x52444621ul  /* "RDF!" */

typedef struct RofDataDescriptor {
    unsigned long  magic0;       /* 'RoF!' */
    unsigned long  magic1;       /* 'DATA' */
    unsigned short version;
    unsigned short reserved;
    unsigned long  ready;
    unsigned char *data;
    unsigned long  size;
} RofDataDescriptor;

#ifdef __cplusplus
extern "C" {
#endif
extern RofDataDescriptor rof_data_descriptor;
extern unsigned char rof_game_data[];
extern unsigned char rof_game_data_end[];
extern volatile unsigned short rof_data_error;
int rof_data_available(void);
const unsigned char *rof_data_at_rom_offset(unsigned long offset, unsigned long length);
void rof_data_reset_mem(void (*write)(unsigned short, const unsigned char*, unsigned long),
                        const unsigned char *charset, unsigned long charset_len);
unsigned long rof_data_load_stage(unsigned long from,
                                  void (*write)(unsigned short, const unsigned char*, unsigned long));
#ifdef __cplusplus
}
#endif

#endif
