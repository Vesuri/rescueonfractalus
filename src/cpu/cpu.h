#ifndef CPU_H
#define CPU_H
#include <stdint.h>

/* 6502 register state.  Flags are stored unpacked (0/1 per flag) for
   readable branch conditions in the transliterated C.  PHP/PLP pack/
   unpack via P_pack/P_unpack. */
typedef struct {
    uint8_t A, X, Y;
    uint8_t S;          /* stack pointer */
    /* status flags */
    uint8_t N, V, Z, C;
    uint8_t I, D;       /* interrupt-disable, decimal (decimal ignored in game) */
} Cpu6502;

extern Cpu6502 cpu;
extern volatile uint8_t mem[65536]; /* shared between main thread and VBI audio thread */

/* ---------- flag helpers ------------------------------------------ */
#define UPD_NZ(v)  do { uint8_t _nzv=(uint8_t)(v); cpu.N=_nzv>>7; cpu.Z=(_nzv==0); } while(0)

static inline uint8_t P_pack(void) {
    return (cpu.N<<7)|(cpu.V<<6)|0x30|(cpu.D<<3)|(cpu.I<<2)|(cpu.Z<<1)|cpu.C;
}
static inline void P_unpack(uint8_t p) {
    cpu.N=(p>>7)&1; cpu.V=(p>>6)&1; cpu.D=(p>>3)&1;
    cpu.I=(p>>2)&1; cpu.Z=(p>>1)&1; cpu.C=p&1;
}

/* ---------- stack -------------------------------------------------- */
#define PUSH(v)  do { mem[0x100|cpu.S]=(uint8_t)(v); cpu.S--; } while(0)
#define PULL(v)  do { cpu.S++; (v)=mem[0x100|cpu.S]; } while(0)
#define PHA()    PUSH(cpu.A)
#define PLA()    do { PULL(cpu.A); UPD_NZ(cpu.A); } while(0)
#define PHP()    PUSH(P_pack())
#define PLP()    do { uint8_t _p; PULL(_p); P_unpack(_p); } while(0)

/* ---------- load / store ------------------------------------------ */
#define LDA(v)  do { cpu.A=(uint8_t)(v); UPD_NZ(cpu.A); } while(0)
#define LDX(v)  do { cpu.X=(uint8_t)(v); UPD_NZ(cpu.X); } while(0)
#define LDY(v)  do { cpu.Y=(uint8_t)(v); UPD_NZ(cpu.Y); } while(0)

/* ---------- transfer ---------------------------------------------- */
#define TAX()   do { cpu.X=cpu.A; UPD_NZ(cpu.X); } while(0)
#define TAY()   do { cpu.Y=cpu.A; UPD_NZ(cpu.Y); } while(0)
#define TXA()   do { cpu.A=cpu.X; UPD_NZ(cpu.A); } while(0)
#define TYA()   do { cpu.A=cpu.Y; UPD_NZ(cpu.A); } while(0)
#define TSX()   do { cpu.X=cpu.S; UPD_NZ(cpu.X); } while(0)
#define TXS()   do { cpu.S=cpu.X; } while(0)  /* TXS: no flag change */

/* ---------- arithmetic -------------------------------------------- */
/* ADC: A = A + v + C.  Honours decimal mode (cpu.D) — the game uses BCD for the
 * score ($497d) and other counters ($75c0/$7b8d), so SED/ADC must produce packed
 * BCD (09+01 -> 10, not 0A) with a decimal carry between digits.
 * NOTE: evaluate the operand EXACTLY ONCE — `v` may have side effects (e.g.
 * bus_read(0xD20A) steps the POKEY RANDOM LFSR).  Double-evaluating it (the old
 * macro referenced (v) in both the sum and the overflow calc) made `ADC <hwreg>`
 * read the register twice, desyncing the RANDOM stream from the real 6502.
 * Decimal flag quirks follow the NMOS 6502: Z from the binary result, V from the
 * binary overflow, N from the high nibble; C is the decimal carry. */
#define ADC(v) do { \
    uint8_t _v = (uint8_t)(v); \
    uint16_t _t = (uint16_t)cpu.A + _v + cpu.C; \
    cpu.V = ((~(cpu.A ^ _v) & (cpu.A ^ (uint8_t)_t)) >> 7) & 1; \
    if (cpu.D) { \
        uint16_t _al = (uint16_t)(cpu.A & 0x0F) + (_v & 0x0F) + cpu.C; \
        uint16_t _ah = (uint16_t)(cpu.A >> 4) + (_v >> 4); \
        if (_al > 9) { _al += 6; _ah += 1; } \
        cpu.Z = ((uint8_t)_t == 0) ? 1 : 0; \
        cpu.N = (_ah & 0x08) ? 1 : 0; \
        if (_ah > 9) _ah += 6; \
        cpu.C = (_ah > 0x0F) ? 1 : 0; \
        cpu.A = (uint8_t)(((_ah << 4) | (_al & 0x0F)) & 0xFF); \
    } else { \
        cpu.C = (_t > 0xFF) ? 1 : 0; \
        cpu.A = (uint8_t)_t; \
        UPD_NZ(cpu.A); \
    } \
} while(0)

/* SBC: A = A - v - (1-C).  In binary mode == ADC(~v).  In decimal mode the NMOS
 * 6502 sets all flags (C/Z/N/V) exactly as the binary subtraction and only the A
 * register gets the decimal correction (low nibble, then high nibble). */
#define SBC(v) do { \
    uint8_t _sv = (uint8_t)(v); \
    uint8_t _nv = (uint8_t)~_sv; \
    uint16_t _t = (uint16_t)cpu.A + _nv + cpu.C; \
    cpu.V = ((~(cpu.A ^ _nv) & (cpu.A ^ (uint8_t)_t)) >> 7) & 1; \
    uint8_t _binres = (uint8_t)_t; \
    if (cpu.D) { \
        int _al = (int)(cpu.A & 0x0F) - (int)(_sv & 0x0F) + (int)cpu.C - 1; \
        if (_al < 0) _al = ((_al - 6) & 0x0F) - 0x10; \
        int _ar = (int)(cpu.A & 0xF0) - (int)(_sv & 0xF0) + _al; \
        if (_ar < 0) _ar -= 0x60; \
        cpu.C = (_t > 0xFF) ? 1 : 0; \
        cpu.A = (uint8_t)(_ar & 0xFF); \
        UPD_NZ(_binres); \
    } else { \
        cpu.C = (_t > 0xFF) ? 1 : 0; \
        cpu.A = _binres; \
        UPD_NZ(cpu.A); \
    } \
} while(0)

/* ---------- compare ----------------------------------------------- */
#define CMP(v) do { uint8_t _v=(uint8_t)(v); uint8_t _d=cpu.A-_v; \
    cpu.N=_d>>7; cpu.Z=(cpu.A==_v); cpu.C=(cpu.A>=_v); } while(0)
#define CPX(v) do { uint8_t _v=(uint8_t)(v); uint8_t _d=cpu.X-_v; \
    cpu.N=_d>>7; cpu.Z=(cpu.X==_v); cpu.C=(cpu.X>=_v); } while(0)
#define CPY(v) do { uint8_t _v=(uint8_t)(v); uint8_t _d=cpu.Y-_v; \
    cpu.N=_d>>7; cpu.Z=(cpu.Y==_v); cpu.C=(cpu.Y>=_v); } while(0)

/* ---------- increment / decrement --------------------------------- */
#define INX()      do { cpu.X++; UPD_NZ(cpu.X); } while(0)
#define INY()      do { cpu.Y++; UPD_NZ(cpu.Y); } while(0)
#define DEX()      do { cpu.X--; UPD_NZ(cpu.X); } while(0)
#define DEY()      do { cpu.Y--; UPD_NZ(cpu.Y); } while(0)
#define INC_M(a)   do { uint8_t _v=bus_read(a)+1; bus_write(a,_v); UPD_NZ(_v); } while(0)
#define DEC_M(a)   do { uint8_t _v=bus_read(a)-1; bus_write(a,_v); UPD_NZ(_v); } while(0)

/* ---------- logical ----------------------------------------------- */
#define AND(v)  do { cpu.A &= (uint8_t)(v); UPD_NZ(cpu.A); } while(0)
#define ORA(v)  do { cpu.A |= (uint8_t)(v); UPD_NZ(cpu.A); } while(0)
#define EOR(v)  do { cpu.A ^= (uint8_t)(v); UPD_NZ(cpu.A); } while(0)
#define BIT(v)  do { uint8_t _v=(uint8_t)(v); \
    cpu.N=_v>>7; cpu.V=(_v>>6)&1; cpu.Z=(cpu.A & _v)==0; } while(0)

/* ---------- shift / rotate ---------------------------------------- */
#define ASL_A()  do { cpu.C=cpu.A>>7; cpu.A<<=1; UPD_NZ(cpu.A); } while(0)
#define LSR_A()  do { cpu.C=cpu.A&1;  cpu.A>>=1; UPD_NZ(cpu.A); } while(0)
#define ROL_A()  do { uint8_t _c=cpu.C; cpu.C=cpu.A>>7; cpu.A=(cpu.A<<1)|_c; UPD_NZ(cpu.A); } while(0)
#define ROR_A()  do { uint8_t _c=cpu.C; cpu.C=cpu.A&1; cpu.A=(cpu.A>>1)|(_c<<7); UPD_NZ(cpu.A); } while(0)

#define ASL_M(a) do { uint8_t _v=bus_read(a); cpu.C=_v>>7; _v<<=1; bus_write(a,_v); UPD_NZ(_v); } while(0)
#define LSR_M(a) do { uint8_t _v=bus_read(a); cpu.C=_v&1;  _v>>=1; bus_write(a,_v); UPD_NZ(_v); } while(0)
#define ROL_M(a) do { uint8_t _v=bus_read(a),_c=cpu.C; cpu.C=_v>>7; _v=(_v<<1)|_c; bus_write(a,_v); UPD_NZ(_v); } while(0)
#define ROR_M(a) do { uint8_t _v=bus_read(a),_c=cpu.C; cpu.C=_v&1; _v=(_v>>1)|(_c<<7); bus_write(a,_v); UPD_NZ(_v); } while(0)

/* ---------- flag ops ---------------------------------------------- */
#define CLC() do { cpu.C=0; } while(0)
#define SEC() do { cpu.C=1; } while(0)
#define CLI() do { cpu.I=0; } while(0)
#define SEI() do { cpu.I=1; } while(0)
#define CLD() do { cpu.D=0; } while(0)
#define SED() do { cpu.D=1; } while(0)
#define CLV() do { cpu.V=0; } while(0)

/* ---------- zero-page indirect indexed (post-index) --------------- */
/* LDA (zp),Y → read 16-bit addr from zp/zp+1, add Y */
#define ZP_IND_Y(zp)   ((uint16_t)(mem[(uint8_t)(zp)] | (mem[(uint8_t)((zp)+1)]<<8)) + cpu.Y)
/* LDA (zp,X)  → read 16-bit addr from zp+X (zero-page wrapped) */
#define ZP_IND_X(zp)   (uint16_t)(mem[(uint8_t)((zp)+cpu.X)] | (mem[(uint8_t)((zp)+cpu.X+1)]<<8))

/* ---------- NOP --------------------------------------------------- */
#define NOP() do {} while(0)

#endif /* CPU_H */
