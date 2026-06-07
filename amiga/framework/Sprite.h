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

private:
    uint16_t* data_;
    uint16_t height;
    bool owner;
    bool attached_;
};

#endif
