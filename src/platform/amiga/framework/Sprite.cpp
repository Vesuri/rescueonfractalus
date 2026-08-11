#include <proto/exec.h>
#include <exec/memory.h>
#include "Sprite.h"

Sprite::Sprite(uint16_t* data, uint16_t height, bool attached, bool takeOwnership) :
    data_(data),
    height(height),
    owner(takeOwnership),
    attached_(attached)
{
    setAttached(attached);
}

Sprite::~Sprite()
{
    if (owner) {
        uint32_t spriteSize = (height + 2) << 2;
        FreeMem(data_, spriteSize);
    }
}

void Sprite::setX(uint16_t x)
{
    uint8_t* header = (uint8_t*)data_;

    // Horizontal start position SH8-SH1
    header[1] = (uint8_t)(x >> 1);

    // ATT, X, X, X, X, SV8, EV8, SH0
    header[3] &= 0xfe;
    header[3] |= (x & 1);
}

void Sprite::setY(uint16_t y)
{
    uint32_t endY = y + height;
    uint8_t* header = (uint8_t*)data_;

    // Vertical start position SV7-SV0
    header[0] = (uint8_t)y;

    // Vertical stop position EV7-EV0
    header[2] = (uint8_t)endY;

    // ATT, X, X, X, X, SV8, EV8, SH0
    if (attached_) {
        header[3] |= 0x80;
    } else {
        header[3] &= 0x7f;
    }
    if (endY > 255) {
        header[3] |= 0x02;
    } else {
        header[3] &= 0xfd;
    }
}

void Sprite::setAttached(bool attached)
{
    attached_ = attached;

    uint8_t* header = (uint8_t*)data_;
    if (attached) {
        header[3] |= 0x80;
    } else {
        header[3] &= 0x7f;
    }
}

uint16_t* Sprite::data() const
{
    return data_;
}

Sprite* Sprite::allocate(uint16_t height)
{
    uint32_t spriteSize = (height + 2) << 2;
    uint16_t* data = (uint16_t*)AllocMem(spriteSize, MEMF_CHIP | MEMF_CLEAR);
    return data ? new Sprite(data, height, false, true) : 0;
}

// Layout (16-bit words): [ctrlA 2][dataA 2*hA][ctrlB 2][dataB 2*hB][terminator 2].  Both Sprites
// are non-owning views into the one allocation, so ~Sprite frees nothing and the caller releases
// the whole buffer with freeChain.  MEMF_CLEAR leaves the terminator zeroed, which disarms the
// channel after the second sprite exactly as a standalone allocate() does.
static uint32_t chainBytes(uint16_t heightA, uint16_t heightB)
{
    return (uint32_t)(heightA + heightB + 3) << 2;
}

uint16_t* Sprite::allocateChain(uint16_t heightA, uint16_t heightB, Sprite*& a, Sprite*& b)
{
    a = 0; b = 0;
    uint16_t* buffer = (uint16_t*)AllocMem(chainBytes(heightA, heightB), MEMF_CHIP | MEMF_CLEAR);
    if (!buffer) return 0;
    a = new Sprite(buffer, heightA, false, false);
    b = new Sprite(buffer + 2 + (heightA << 1), heightB, false, false);
    if (a && b) return buffer;
    delete a; a = 0;
    delete b; b = 0;
    FreeMem(buffer, chainBytes(heightA, heightB));
    return 0;
}

void Sprite::freeChain(uint16_t* buffer, uint16_t heightA, uint16_t heightB)
{
    if (buffer) FreeMem(buffer, chainBytes(heightA, heightB));
}
