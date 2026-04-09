#ifndef gba_renderer_h
#define gba_renderer_h
#include <stdint.h>

#define CYCLES_PER_FRAME 280896 

#define HEIGHT_PX 160
#define WIDTH_PX 240
#define DISPLAY_SCALING 5

#define BYTES_PER_Y WIDTH_PX*sizeof(uint16_t)

/* Keep multiple of 2 only for easy modulo */
#define COMPOSITOR_STACK_SIZE 16

typedef struct GBA GBA;

typedef enum {
	BGMODE_0, 				// } -------------------
	BGMODE_1, 				// }    Tilemap Modes
	BGMODE_2,				// } -------------------
	BGMODE_3, 				// } -------------------
	BGMODE_4, 				// } 	Bitmap Modes
	BGMODE_5 				// } -------------------
} PPU_BGMode;

/* Screen Size parameter enums for BGCNT Text Mode and Rotation/Scaling mode */

typedef enum {
    BG_TEXT_256_256 = 0,
    BG_TEXT_512_256,
    BG_TEXT_256_512,
    BG_TEXT_512_512
} PPU_BG_TEXT_SIZE;

typedef enum {
    BG_ROT_SCAL_128_128 = 0,
    BG_ROT_SCAL_256_256,
    BG_ROT_SCAL_512_512,
    BG_ROT_SCAL_1024_1024
} PPU_BG_ROT_SCAL_SIZE;


typedef enum {
	PPU_HDRAW, 				// } Horizontal State
	PPU_HBLANK,				// }

	PPU_VDRAW, 				// } Vertical State
	PPU_VBLANK				// }
} PPU_State;

typedef enum {
    PALETTE_16_16_4BIT,
    PALETTE_256_1_8BIT
} PALETTE_INDEX_MODE;

typedef enum {
    OBJ_SHAPE_SQUARE,
    OBJ_SHAPE_HORIZONTAL,
    OBJ_SHAPE_VERTICAL
} OBJ_SHAPE;

typedef enum {
    OBJ_VRAM_MAPPING_2DIM,
    OBJ_VRAM_MAPPING_1DIM
} OBJ_VRAM_MAPPING;

typedef enum {
    LAYER_BG0,
    LAYER_BG1,
    LAYER_BG2,
    LAYER_BG3,
    LAYER_SPRITE,
    LAYER_SEMI_TRANSPARENT_SPRITE,
    LAYER_BACKDROP
} LAYER_TYPE;

typedef struct {
    LAYER_TYPE type;
    uint16_t linebuffer[240];
} Layer;

typedef struct {
    Layer layerStack[COMPOSITOR_STACK_SIZE];
    uint8_t layerCount;
    uint8_t headPointer;
    uint8_t basePointer;
} Compositor;

void initialisePPU(GBA* gba);
/* Step the PPU State */
void stepPPU(GBA* gba);

void updateInternalBGNXY(GBA* gba, uint8_t N);
#endif
