#ifndef gba_renderer_h
#define gba_renderer_h

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

typedef enum {
	PPU_HDRAW, 				// } Horizontal State
	PPU_HBLANK,				// }

	PPU_VDRAW, 				// } Vertical State
	PPU_VBLANK				// }
} PPU_State;

void initialisePPU(GBA* gba);
/* Step the PPU State */
void stepPPU(GBA* gba);

#endif
