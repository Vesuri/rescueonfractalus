#ifndef _UTIL_H
#define _UTIL_H

// ASSEMBLER selects the hand-written *Assembler.s routines over the portable C++
// bodies. Default: on for both SAS/C and GCC (the hand-tuned 68k is faster on
// real hardware). Override on the command line: -DNO_ASSEMBLER forces the C++
// bodies.
//   - SAS/C + ASSEMBLER: the __asm declarations link the *Assembler.s directly.
//   - GCC   + ASSEMBLER: reaches the same asm via register-marshalling wrappers
//                        (see Util::sqrt et al. in the *.cpp).
#if !defined(ASSEMBLER) && !defined(NO_ASSEMBLER)
#define ASSEMBLER
#endif

#if __cplusplus < 201103L
typedef char int8_t;
typedef short int16_t;
typedef long int32_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned long uint32_t;
typedef unsigned char bool;
#define true 1
#define false 0
#endif

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

class Util {
public:
    static int16_t sin[1024 + 256];
    static int16_t* cos;
    static uint8_t bayerMatrix[256];   // 16x16 ordered-dither threshold table
    // Opt-in CHIP-RAM arena: call allocateMemoryPool() once, then carve
    // 8-byte-aligned segments with getFreeMemoryPoolSegment() (no per-allocation
    // AllocMem overhead, no fragmentation). The framework's own allocate()s still
    // use AllocMem; a production opts in for its own data. (From DanceDiverse3.)
    static uint8_t* memoryPool;
    static uint32_t memoryPoolSize;
    static uint8_t* memoryPoolFreeSegment;
    static uint8_t* allocateMemoryPool(uint32_t bytes);
    static void freeMemoryPool();
    static uint8_t* getFreeMemoryPoolSegment(uint32_t bytes);
#if defined(ASSEMBLER) && defined(__SASC)
    __asm static uint32_t sqrt(register __d1 uint32_t x);
    __asm static int32_t ungzip(register __a0 void* input, register __a1 void* output);
#else
    static uint32_t sqrt(uint32_t x);
    static int32_t ungzip(void* input, void* output);
#endif
};

struct Point2D {
    Point2D();
    Point2D(int16_t x, int16_t y);
    int16_t x, y;
};

struct Rect {
    Rect();
    Rect(const Point2D& topLeft, const Point2D& bottomRight);
#if defined(ASSEMBLER) && defined(__SASC)
    __asm void update();
    __asm void unite(register __a1 const Rect& rectangle);
#else
    void update();
    void unite(const Rect& rectangle);
#endif
    Point2D topLeft;
    Point2D bottomRight;
    uint16_t width;
    uint16_t height;
    Point2D center;
    bool isEmpty;
    bool isNull;
};

struct Polygon {
    Polygon();
    Polygon(uint16_t size);
    ~Polygon();
    void setPoint(uint16_t index, const Point2D& point);
    Rect boundingRect() const;
    Point2D* points;
    uint16_t size;
};

#endif
