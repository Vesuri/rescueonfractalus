#ifndef _SPRITE_H
#define _SPRITE_H

#include "Util.h"

class Sprite {
public:
    Sprite(uint16_t* data, uint16_t height, bool attached = false, bool takeOwnership = false);
    ~Sprite();

    void setX(uint16_t x);
    void setY(uint16_t y);
    void setAttached(bool attached);
    __inline uint16_t* data() const;

    static Sprite* allocate(uint16_t height);

    // Allocate ONE chip buffer holding two hardware-CHAINED sprites (plus the trailing 0,0
    // terminator) and construct a non-owning Sprite over each half.  A sprite channel re-fetches
    // its control words straight after the VSTOP line, so two sprites laid out back to back in
    // one buffer let a single channel display both — vertical reuse with no copper re-point, and
    // therefore no arming deadline to hit.  ⚠ The second sprite's VSTART must be STRICTLY GREATER
    // than the first's VSTOP: the re-fetch happens ON the VSTOP line, so an exactly-adjacent
    // VSTART races that line's start compare.  Returns the buffer to pass to freeChain, or 0.
    static uint16_t* allocateChain(uint16_t heightA, uint16_t heightB, Sprite*& a, Sprite*& b);
    static void freeChain(uint16_t* buffer, uint16_t heightA, uint16_t heightB);

private:
    uint16_t* data_;
    uint16_t height;
    bool owner;
    bool attached_;
};

#endif
