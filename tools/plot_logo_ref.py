#!/usr/bin/env python3
"""Re-run the Atari's $5111 stroke plotter and prove src/rof_logo_field.h is what it produces.

The logo is NOT stored as a bitmap.  $5000 expands ~130 bytes of run-length stroke data at
$525F, through a 7-entry pattern table, into the 2480-byte GTIA-9 field at $60A3 -- see
docs/logo-station-plan.md 1.2 for the encoding and the three-pass emboss.  The port bakes the
RESULT (the drawing runs with playfield DMA off, so it is never visible), and this script is
what makes that bake reproducible rather than a magic blob: it reads segment 5 straight out of
rof.xex, plots both phases, and diffs them against the committed header.

    python3 tools/plot_logo_ref.py        # expect: 0 mismatches on both phases

Two fall-throughs in the 6502 decide the whole shape and are easy to misread as RTS-terminated:
$51BB falls into $51C0 (so it plots rows Y-1 AND Y), and $51A8's DEX falls into $51BB (so the
"fat" brush is a 2-column x 4-row block).  Read either as ending in RTS and every other scanline
comes out blank.
"""
import re
import sys


def segment5():
    """Segment 5 ($5000-$536F) out of rof.xex -- it is absent from the final RAM image."""
    data = open('rof.xex', 'rb').read()
    u16 = lambda i: data[i] | (data[i + 1] << 8)
    p = 2
    while p + 4 <= len(data):
        if u16(p) == 0xFFFF:
            p += 2
            if p + 4 > len(data):
                break
        s, e = u16(p), u16(p + 2)
        p += 4
        body = data[p:p + e - s + 1]
        p += len(body)
        if s == 0x5000:
            return body
    raise SystemExit('rof.xex: no segment at $5000')


seg = segment5()
def m(a): return seg[a-0x5000]
mem=bytearray(0x10000)
tbl=[0x6053+40*n for n in range(0x40)]          # $5037-$5048
def poke(col,row,sh):                            # $51C0-$51EE
    a=tbl[row]+(col>>1)
    if col&1: mem[a]=(mem[a]&0xF0)|sh
    else:     mem[a]=(mem[a]&0x0F)|(sh<<4)
def pair(col,row,sh):                            # $51BB: DEY,plot,INY, fall through to plot
    poke(col,row-1,sh); poke(col,row,sh)
def brush(col,row,sh,fat):                       # $51A8
    if not fat: pair(col,row,sh)
    else:
        pair(col,   row-2, sh)                   # DEY DEY, JSR $51BB
        pair(col+1, row-2, sh)                   # INX,     JSR $51BB
        pair(col+1, row,   sh)                   # INY INY, JSR $51BB
        pair(col,   row,   sh)                   # DEX,     fall through
def cell(pat,col,row,pd,fat):                    # $5169
    if pd:                                       # $517A
        sh = 2 if pd==2 else (7 if (row>>2)==8 else row>>2)
    else:
        sh = (((row-2)&0xFF)>>2)^0x0F            # $516F
    x=(col+8)&0xFF; p=pat
    for _ in range(8):                           # $5195 loop, LSB = rightmost pixel
        x-=1
        if p&1: brush(x,row,sh,fat)
        p>>=1
    return row+2                                 # $51A3
def strip(y,col,row,pd,fat):                     # $5134
    pat=m(0x525F+y); y+=1; run=1                 # first byte: raw pattern, run 1
    while True:
        for _ in range(run): row=cell(pat,col,row,pd,fat)
        b=m(0x525F+y)
        if b==0: return y+1                      # $5167 terminator
        run=b&0x1F; hi=b>>5
        if hi==0: pat=m(0x525F+y+1); y+=2        # PPP=0 -> literal byte follows
        else:     pat=m(0x52DF+hi);  y+=1        # PPP=1..7 -> pattern table
def word(cnt,y0,col,row,pd,fat):                 # $5111 / $5117
    y=y0
    for _ in range(cnt): y=strip(y,col,row,pd,fat); col+=8
    return y
h=open('src/rof_logo_field.h').read()
def arr(n):
    b=h.split('kLogo%s['%n,1)[1].split('{',1)[1].split('};',1)[0]
    return bytes(int(v,16) for v in re.findall(r'0x([0-9A-Fa-f]{2})',b))
def snap(): return bytes(mem[0x60A3:0x60A3+62*40])
yend=word(9,0x00,5,4,2,True)                     # $5052: X=5, $7B=4, $7D=2, fat  -> shadow
word(9,0x00,4,3,1,True)                          # $5058: X=4, $7B=3, $7D=1, fat  -> dark grad
word(9,0x00,4,3,0,False)                         # $505F: X=4, $7B=3, $7D=0, thin -> bright grad
A=snap(); ref=arr('Field')
bad = sum(a!=b for a,b in zip(A,ref))
print('LUCASFILM consumed %d stroke bytes (GAMES starts at $%02X) ; mismatches = %d/%d'
      % (yend, 0x66, bad, len(ref)))
yg=word(4,0x66,20,0x36,2,False)                  # $50B2: X=$14, $7B=$36, $7D=2, thin
word(4,0x66,19,0x35,1,False)                     # $50B9: X=$13, $7B=$35, $7D=1, thin
B=snap(); g=arr('Games'); GR,GC,GRW,GCL=50,13,11,13
sub=b''.join(B[r*40+GC:r*40+GC+GCL] for r in range(GR,GR+GRW))
print('GAMES consumed through $%02X (table starts $52DF = index $80) ; overlay mismatches = %d/%d'
      % (yg, sum(a!=b for a,b in zip(sub,g)), len(g)))
bad2 = sum(a!=b for a,b in zip(sub,g))
bad3 = sum(1 for i in range(2480)
           if B[i] != (g[(i//40-GR)*GCL + (i%40-GC)]
                       if GR <= i//40 < GR+GRW and GC <= i%40 < GC+GCL else ref[i]))
print('full-field phase2 mismatches vs (kLogoField+overlay) = %d' % bad3)
sys.exit(1 if (bad or bad2 or bad3) else 0)
