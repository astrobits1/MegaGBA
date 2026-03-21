#ifndef gba_renderer_h
#define gba_renderer_h
#include <stdint.h>

#define CYCLES_PER_FRAME 280896 

#define HEIGHT_PX 160
#define WIDTH_PX 240
#define DISPLAY_SCALING 4

#define BYTES_PER_Y WIDTH_PX*sizeof(uint16_t)

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

void initialisePPU(GBA* gba);
/* Step the PPU State */
void stepPPU(GBA* gba);

void updateInternalBGNXY(GBA* gba, uint8_t N);
#endif
