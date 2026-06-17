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
