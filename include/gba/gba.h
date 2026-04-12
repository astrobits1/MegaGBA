#ifndef gba_include_h
#define gba_include_h

#include <stdint.h>
#include <stdlib.h> 
#include <gba/arm7tdmi.h>
#include <gba/renderer.h>
#include <gba/gamepak.h>


enum {
	BIOS_ROM_16KB 		= 0x00000000,
	BIOS_ROM_16KB_END 	= 0x00003FFF,

	EXT_WRAM_256KB		= 0x02000000,
	EXT_WRAM_256KB_END  = 0x0203FFFF,

	INT_WRAM_32KB 		= 0x03000000,
	INT_WRAM_32KB_END  	= 0x03007FFF,

	IO_REG_1KB 			= 0x04000000,
	IO_REG_1KB_END 		= 0x040003FE,

	PALETTE_RAM_1KB 	= 0x05000000,
	PALETTE_RAM_1KB_END = 0x050003FF,

	VRAM_96KB 			= 0x06000000,
	VRAM_96KB_END 		= 0x06017FFF,

	OAM_1KB 			= 0x07000000,
	OAM_1KB_END 		= 0x070003FF,

	EXT_ROM0_32MB 		= 0x08000000, 				/* ROM from gamepak/flash with waitstate 0*/
	EXT_ROM0_32MB_END 	= 0x09FFFFFF,

	EXT_ROM1_32MB 		= 0x0A000000, 				/* Mirror of ROM with waitstate 1 */
	EXT_ROM1_32MB_END 	= 0x0BFFFFFF,

	EXT_ROM2_32MB		= 0x0C000000, 				/* Mirror of ROM with waitstate 2 */
	EXT_ROM2_32MB_END 	= 0x0DFFFFFF,

	EXT_SRAM_64KB		= 0x0E000000,
	EXT_SRAM_64KB_END 	= 0x0E00FFFF

	/* 0x10000000-0xFFFFFFFF unused (upper 4 bits of address bus) */
};

enum {
	WIDTH_8,
	WIDTH_16,
	WIDTH_32
};

typedef enum {
	DISPCNT 	= 0x00,
	GREENSWP 	= 0x2,
	DISPSTAT 	= 0x4,
	VCOUNT 		= 0x6,
	BG0CNT 		= 0x8,
	BG1CNT 		= 0xA,
	BG2CNT 		= 0xC,
	BG3CNT 		= 0xE,

    BG0HOFS     = 0x10,
    BG0VOFS     = 0x12,
    BG1HOFS     = 0x14,
    BG1VOFS     = 0x16,
    BG2HOFS     = 0x18,
    BG2VOFS     = 0x1A,
    BG3HOFS     = 0x1C,
    BG3VOFS     = 0x1E,

    BG2PA       = 0x20,
    BG2PB       = 0x22,
    BG2PC       = 0x24,
    BG2PD       = 0x26,

    BG2X_L      = 0x28,
    BG2X_H      = 0x2A,
    BG2Y_L      = 0x2C,
    BG2Y_H      = 0x2E,

    BG3PA       = 0x30,
    BG3PB       = 0x32,
    BG3PC       = 0x34,
    BG3PD       = 0x36,

    BG3X_L      = 0x38,
    BG3X_H      = 0x3A,
    BG3Y_L      = 0x3C,
    BG3Y_H      = 0x3E,

    WIN0H       = 0x40,
    WIN1H       = 0x42,
    WIN0V       = 0x44,
    WIN1V       = 0x46,
    WININ       = 0x48,
    WINOUT      = 0x4A,
    
    MOSAIC      = 0x4C,

    BLDCNT      = 0x50,
    BLDALPHA    = 0x52,
    BLDY        = 0x54,

    DMA0SAD     = 0xB0,
    DMA1SAD     = 0xBC,
    DMA2SAD     = 0xC8,
    DMA3SAD     = 0xD4,

    DMA0DAD     = 0xB4,
    DMA1DAD     = 0xC0,
    DMA2DAD     = 0xCC,
    DMA3DAD     = 0xD8,

    DMA0CNT_L   = 0xB8,
    DMA0CNT_H   = 0xBA,
    DMA1CNT_L   = 0xC4,
    DMA1CNT_H   = 0xC6,
    DMA2CNT_L   = 0xD0,
    DMA2CNT_H   = 0xD2,
    DMA3CNT_L   = 0xDC,
    DMA3CNT_H   = 0xDE,

    KEYINPUT    = 0x130,

    IE          = 0x200,
    IF          = 0x202,
    IME         = 0x208,

    HALTCNT     = 0x301
} IO_REG;

/* Mapped to KEYINPUT bits */
typedef enum {
    KEYINPUT_A,
    KEYINPUT_B,
    KEYINPUT_SELECT,
    KEYINPUT_START,
    KEYINPUT_DPRIGHT,
    KEYINPUT_DPLEFT,
    KEYINPUT_DPUP,
    KEYINPUT_DPDOWN,
    KEYINPUT_R,
    KEYINPUT_L
} KEYINPUT_CODE;

typedef enum {
    EVENT_PPU
} GBAEventType;

typedef struct {
    uint64_t scheduledFor;
    GBAEventType type;
} GBAEvent;

struct GBA {
	/* ------------------ CPU -------------------- */
	CPU_STATE cpu_state; 		/* THUMB/ARM state */
	CPU_MODE cpu_mode; 			/* Mode of the CPU */
    bool halted;                /* Is CPU halted? */
	uint32_t pipeline[3]; 		/* 3 Stage pipeline (Queue for fetched opcodes) */
	uint8_t pipelineInsertPoint;/* Point where prefetched opcode is inserted */
	uint8_t pipelineReadPoint;  /* Point where opcode to be executed is read from */
	bool skipFetch; 			/* Skip the pipeline fetch in the current pipeline cycle
								   either because the pipeline was flushed or the fetch was already
								   done internally during execution stage to emulate PC+12 */
    /* Exceptions */
    uint8_t exceptionState;     /* When asynchronous exceptions are requested, the corresponding bit
                                    is set high*/
	/* Main Regs */
	uint32_t REG[16]; 			/* Main 16 registers */
	uint32_t CPSR; 				/* Main CPSR Register */
	uint32_t SPSR; 				/* SPSR accessible in only exception modes */
	
	/* Swap-In for main registers */
	uint32_t REG_SWAP[7]; 		/* Swaps in anything from R8-R14 of the main registers
								 * when switching modes */

	/* Banked Registers */	
	uint32_t BANK_FIQ[7]; 		/* R8_FIQ, R9_FIQ, R10_FIQ, R11_FIQ, R12_FIQ, R13_FIQ, R14_FIQ */
	uint32_t BANK_SVC[2]; 		/* R13_SVC, R14_SVC */
	uint32_t BANK_ABT[2]; 		/* R13_ABT, R14_ABT */
	uint32_t BANK_IRQ[2]; 		/* R13_IRQ, R14_IRQ */
	uint32_t BANK_UND[2]; 		/* R13_UND, R14_UND */
	uint32_t BANK_SPSR[5]; 		/* SPSR_FIQ, SPSR_SVC, SPSR_ABT, SPSR_IRQ, SPSR_UND */

	void (*ARM_LUT[4096])(struct GBA* gba, uint32_t ins);		/* Lookup table with 12 bit indices
																   for ARM instructions */
	void (*THUMB_LUT[256])(struct GBA* gba, uint16_t ins);		/* Lookup table with 8 bit indices
																   for THUMB instructions */
	/* ----------------- Renderer ---------------- */
    uint16_t framebuffer[WIDTH_PX*HEIGHT_PX];       /* Framebuffer used with sdl textures for rendering */
    uint16_t bgLayerLinebufferCache[4][240];        /* Linebuffer cache for BG0-3 */
    bool bgLayerLinebufferCacheUsed[4];             /* Mark cache slots used/free */
    uint16_t latchedDISPCNT;
    int32_t internalBG2X;
    int32_t internalBG2Y;
    int32_t internalBG3X;
    int32_t internalBG3Y;
    int32_t latchedInternalBG2Y;                /* For vertical mosaic in affine BGs */
    int32_t latchedInternalBG3Y;
	
	uint8_t ppuVState; 					// Current Vertical State of PPU
	uint8_t ppuHState; 					// Current Horizontal State of PPU

	/* ----------------- Emulator ---------------- */
	GamePak gamepak; 					/* Cartridge containing allocated code and important info
										   about the game */
	bool runningStepFrame; 			    /* Flag used to keep track of when its running the frame
                                           and when to stop when calling stepGBAFrame */
    uint64_t cycles;                    /* Cycle counter */

	/* Allocations */
    uint8_t* biosROM;
    size_t biosSize;

	uint8_t* IWRAM;
	uint8_t* EWRAM;
	uint8_t* IO;
	
	uint8_t* PaletteRAM;
	uint8_t* VRAM;
	uint8_t* OAM;

    /* Scheduler */
    GBAEvent eventStack[16];
    uint8_t eventCount;
    uint8_t eventHeadPointer;
    uint8_t eventBasePointer;

    /* DMA */
    bool dmaInProgressMaster;
    bool dmaInProgress[4];
    uint32_t dmaSAD[4];
    uint32_t dmaDAD[4];
    uint32_t dmaWordCount[4];
	/* ------------------------------------------- */

};

typedef struct GBA GBA;

/* Read/Write IO registers - internal versions for internal hardware emulation to access/write 
 * registers and normal for stricter bytewise address checking (for software access) */

uint32_t readIO_internal(GBA* gba, uint32_t ioaddr, uint8_t size);
uint32_t readIO(GBA* gba, uint32_t ioaddr, uint8_t size);
void writeIO_internal(GBA* gba, uint32_t address, uint32_t data, uint8_t size);
void writeIO(GBA* gba, uint32_t address, uint32_t data, uint8_t size);

/* Bus functions */

uint32_t busRead(GBA* gba, uint32_t address, uint8_t size);
void busWrite(GBA* gba, uint32_t address, uint32_t data, uint8_t size);

/* --------------- */

/* DMA */
void startDMA(GBA* gba, uint8_t N);

/* Scheduler */
void pushEvent(GBA* gba, GBAEvent event);
GBAEvent peekEvent(GBA* gba, uint8_t index);
GBAEvent popEvent(GBA* gba);

/* ------- API ------- */
#ifdef __cplusplus
extern "C" {
#endif

void stepGBAFrame(GBA* gba);

void initialiseGBA(GBA* gba, GamePak gamepak, uint8_t* biosROM, size_t biosSize);
void freeGBA(GBA* gba);
void keyinputSet(GBA* gba, KEYINPUT_CODE code);
void keyinputReset(GBA* gba, KEYINPUT_CODE code);

#ifdef __cplusplus
}
#endif
#endif
