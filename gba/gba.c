#include <gba/gba.h>
#include <gba/arm7tdmi.h>
#include <gba/gamepak.h>
#include <gba/debugGBA.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

bool initialiseSDL(GBA* gba) {
    SDL_Init(SDL_INIT_EVERYTHING);
    SDL_CreateWindowAndRenderer(WIDTH_PX * DISPLAY_SCALING, HEIGHT_PX * DISPLAY_SCALING, SDL_WINDOW_SHOWN,
            &gba->SDL_Window, &gba->SDL_Renderer);

    if (!gba->SDL_Window) return false;          /* Failed to create screen */

    SDL_SetWindowTitle(gba->SDL_Window, "MegaGBA");
    SDL_RenderSetScale(gba->SDL_Renderer, DISPLAY_SCALING, DISPLAY_SCALING);
    SDL_RenderClear(gba->SDL_Renderer);
    return true;
}

void SDLEvents(GBA* gba) {
    /* We listen for events like keystrokes and window closing */
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT || event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) {
            gba->run = false;
        }
    }
}

void cleanSDL(GBA* gba) {
	SDL_DestroyRenderer(gba->SDL_Renderer);
    SDL_DestroyWindow(gba->SDL_Window);
    SDL_Quit();

	gba->SDL_Renderer = NULL;
	gba->SDL_Window = NULL;
}

/* ----------------------------------------------------- */

void initialiseGBA(GBA* gba, GamePak* gamepak, uint8_t* biosBuffer, size_t biosSize) {
	gba->gamepak = gamepak;
	gba->run = false;
	gba->SDL_Renderer = NULL;
	gba->SDL_Window = NULL;

	/* Initial Latched DISPCNT values */
	gba->BG0_Flag = 0;
	gba->BG1_Flag = 0;
	gba->BG2_Flag = 0;
	gba->BG3_Flag = 0;
	gba->forcedBlank = 0;
	gba->ppuHState = PPU_HDRAW;
	gba->ppuVState = PPU_VDRAW;
	gba->videoMode = VMODE_0;

	/* Allocate memory for components */
	uint8_t* IWRAM 		= (uint8_t*)malloc(0x8000);			// 32 KB
	uint8_t* EWRAM 		= (uint8_t*)malloc(0x40000); 		// 256 KB
	uint8_t* IO    		= (uint8_t*)malloc(0x3FF);
	uint8_t* PaletteRAM = (uint8_t*)malloc(0x400);
	uint8_t* VRAM 		= (uint8_t*)malloc(0x18000);
	uint8_t* OAM 		= (uint8_t*)malloc(0x400);

	if (!IWRAM || !EWRAM || !IO || !PaletteRAM || !VRAM || !OAM) {
		printf("[FATAL] Error allocating memory for GBA components\n");
		exit(89);
	}

    gba->biosROM    = biosBuffer;
    gba->biosSize   = biosSize;

	gba->IWRAM 		= IWRAM;
	gba->EWRAM 		= EWRAM;
	gba->IO    		= IO;
	gba->PaletteRAM = PaletteRAM;
	gba->VRAM 		= VRAM;
	gba->OAM 		= OAM;

	/* Initialise all IO memory to 0 */
	memset(gba->IO, 0, 0x3FF);

	/* Initialising functions */
	initialiseCPU(gba);
	bool initSDL = initialiseSDL(gba);

	if (!initSDL) {
		printf("[FATAL] GBA cannot start without SDL2\n");
		exit(120);
	}

#ifdef DEBUG_ENABLED
	initDissembler();
#endif
}

void freeGBA(GBA* gba) {
	cleanSDL(gba);

	free(gba->IWRAM);
	free(gba->EWRAM);
	free(gba->IO);
	free(gba->PaletteRAM);
	free(gba->VRAM);
	free(gba->OAM);

    /* BIOS ROM is cleaned by main */
    gba->biosROM = NULL;
    gba->biosSize = 0;

	gba->IWRAM = NULL;
	gba->EWRAM = NULL;
	gba->IO = NULL;
	gba->PaletteRAM = NULL;
	gba->VRAM = NULL;
	gba->OAM = NULL;
}

void startGBAEmulator(GamePak* gamepak, uint8_t* biosBuffer, size_t biosSize) {
	GBA gba;
	initialiseGBA(&gba, gamepak, biosBuffer, biosSize);

	gba.run = true;

	latchDISPCNT(&gba);
	/* For now, we take each instruction as 1 cycle consumed */

	while (gba.run) {
		for (int i = 0; i < 960; i++) {
			stepCPU(&gba);
		}

		/* HDRAW is over, run the PPU to catch up
		 * without ticking the clock */
		stepPPU(&gba);

		for (int i = 0; i < 272; i++) {
			stepCPU(&gba);
		}

		/* HBLANK is over, run the PPU to catch up */
		stepPPU(&gba);
	}
}

/* -------- Bus Functions --------- */

static inline uint32_t littleEndian32Decode(uint8_t* ptr) {
	return (uint32_t)((ptr[3] << 24) | (ptr[2] << 16) | (ptr[1] << 8) | ptr[0]);
}

static inline uint16_t littleEndian16Decode(uint8_t* ptr) {
	return (uint16_t)((ptr[1] << 8) | ptr[0]);
}

static inline void littleEndian32Encode(uint8_t* ptr, uint32_t value) {
	ptr[0] = value & 0xFF;
	ptr[1] = (value >> 8) & 0xFF;
	ptr[2] = (value >> 16) & 0xFF;
	ptr[3] = (value >> 24) & 0xFF;
}

static inline void littleEndian16Encode(uint8_t* ptr, uint16_t value) {
	ptr[0] = value & 0xFF;
	ptr[1] = (value >> 8) & 0xFF;
}

uint32_t busRead(GBA* gba, uint32_t address, uint8_t size) {
	/* We're reading a 32/16/8 bit value from the given address */
    if (address >= BIOS_ROM_16KB && address <= BIOS_ROM_16KB_END) {
        /* Read from BIOS ROM that may or may not be available */
        if (gba->biosROM == NULL) return 0;
        if (address > gba->biosSize-1) return 0;

        uint8_t* ptr = &gba->biosROM[address];

        switch (size) {
			case WIDTH_32: return littleEndian32Decode(ptr);
	    	case WIDTH_16: return littleEndian16Decode(ptr);
			case WIDTH_8:  return *ptr;
		}
    } else if (address >= EXT_ROM0_32MB && address <= EXT_ROM2_32MB_END) {
		uint32_t relativeAddress;

		switch ((address >> 24) & 0xF) {
			case 0x8:
				relativeAddress = address - EXT_ROM0_32MB;
				break;
			case 0xA:
				relativeAddress = address - EXT_ROM1_32MB;
				break;
			case 0xC:
				relativeAddress = address - EXT_ROM2_32MB;
				break;
		}

		if (relativeAddress > (gba->gamepak->size - 1)) {
			// printf("[WARNING] Read attempt from gamepak to an invalid address %08x\n", address);
			return 0;
		}

		uint8_t* ptr = &gba->gamepak->allocated[relativeAddress];

		switch (size) {
			case WIDTH_32: return littleEndian32Decode(ptr);
			case WIDTH_16: return littleEndian16Decode(ptr);
			case WIDTH_8 : return *ptr;
		}
	} else if (address >= INT_WRAM_32KB && address <= INT_WRAM_32KB_END) {
		/* Read from internal work RAM */
		uint8_t* ptr = &gba->IWRAM[address - INT_WRAM_32KB];

		switch (size) {
			case WIDTH_32: return littleEndian32Decode(ptr);
			case WIDTH_16: return littleEndian16Decode(ptr);
			case WIDTH_8:  return *ptr;
		}
	} else if (address >= EXT_WRAM_256KB && address <= EXT_WRAM_256KB_END) {
		/* Read from external work RAM - waitstates apply */
		uint8_t* ptr = &gba->EWRAM[address - EXT_WRAM_256KB];

		switch (size) {
			case WIDTH_32: return littleEndian32Decode(ptr);
			case WIDTH_16: return littleEndian16Decode(ptr);
			case WIDTH_8:  return *ptr;
		}
	} else if (address >= VRAM_96KB && address <= VRAM_96KB_END) {
		/* Read from Video RAM */
		uint8_t* ptr = &gba->VRAM[address - VRAM_96KB];

		switch (size) {
			case WIDTH_32: return littleEndian32Decode(ptr);
			case WIDTH_16: return littleEndian16Decode(ptr);
			case WIDTH_8:  return *ptr;
		}
	} else if (address >= IO_REG_1KB && address <= IO_REG_1KB_END) {
		/* Read from IO register */
		uint8_t* ptr = &gba->IO[address - IO_REG_1KB];

		switch (size) {
			case WIDTH_32: return littleEndian32Decode(ptr);
			case WIDTH_16: return littleEndian16Decode(ptr);
			case WIDTH_8: return *ptr;
		}
	} else if (address >= PALETTE_RAM_1KB && address <= PALETTE_RAM_1KB_END) {
		/* Read from Palette RAM */
		uint8_t* ptr = &gba->PaletteRAM[address - PALETTE_RAM_1KB];

		switch (size) {
			case WIDTH_32: return littleEndian32Decode(ptr);
			case WIDTH_16: return littleEndian16Decode(ptr);
			case WIDTH_8:  return *ptr;
		}
	}

	return 0;
}

void busWrite(GBA* gba, uint32_t address, uint32_t data, uint8_t size) {
	if (address >= INT_WRAM_32KB && address <= INT_WRAM_32KB_END) {
		/* Write to internal workram with current size and little endian formatting */
		uint8_t* ptr = &gba->IWRAM[address - INT_WRAM_32KB];

		switch (size) {
			case WIDTH_32: littleEndian32Encode(ptr, data); return;
			case WIDTH_16: littleEndian16Encode(ptr, data); return;
			case WIDTH_8:  *ptr = (uint8_t)data; return;
		}
	} else if (address >= EXT_WRAM_256KB && address <= EXT_WRAM_256KB_END) {
		uint8_t* ptr = &gba->EWRAM[address - EXT_WRAM_256KB];

		switch (size) {
			case WIDTH_32: littleEndian32Encode(ptr, data); return;
			case WIDTH_16: littleEndian16Encode(ptr, data); return;
			case WIDTH_8:  *ptr = (uint8_t)data; return;
		}
	} else if (address >= VRAM_96KB && address <= VRAM_96KB_END) {
		uint8_t* ptr = &gba->VRAM[address - VRAM_96KB];

		/* VRAM only supports 16 and 32 bit writes, writing a byte to the addressed
		 * halfword is going to mirror it to both upper and lower byte */
		switch (size) {
			case WIDTH_32: littleEndian32Encode(ptr, data); return;
			case WIDTH_16: littleEndian16Encode(ptr, data); return;
			case WIDTH_8:  {
				/* Halfword aligned */
				ptr = &gba->VRAM[(address & ~1) - VRAM_96KB];
				ptr[0] = (uint8_t)data;
				ptr[1] = (uint8_t)data;
				return;
			}
		}
	} else if (address >= IO_REG_1KB && address <= IO_REG_1KB_END) {
		uint8_t* ptr = &gba->IO[address - IO_REG_1KB];

		/* Check for read-only registers, and prevent a write */
		switch (address - IO_REG_1KB) {
			case VCOUNT: return;
			case DISPSTAT: {
				/* Handle read only bits */
				uint8_t current = *ptr;
				/* V-Blank, H-Blank and V-Counter flags are read only */
				data &= ~0b111;
				data |= current & 0b111;
				break;
			}
		}

		switch (size) {
			case WIDTH_32: littleEndian32Encode(ptr, data);
			case WIDTH_16: littleEndian16Encode(ptr, data);
			case WIDTH_8: *ptr = data;
		}
	} else if (address >= PALETTE_RAM_1KB && address <= PALETTE_RAM_1KB_END) {
		uint8_t* ptr = &gba->PaletteRAM[address - PALETTE_RAM_1KB];

		/* Palette RAM only supports 16 and 32 bit writes, writing a byte to the addressed
		 * halfword is going to mirror it to both upper and lower byte */
		switch (size) {
			case WIDTH_32: littleEndian32Encode(ptr, data); return;
			case WIDTH_16: littleEndian16Encode(ptr, data); return;
			case WIDTH_8:  {
				/* Halfword aligned */
				ptr = &gba->PaletteRAM[(address & ~1) - PALETTE_RAM_1KB];
				ptr[0] = (uint8_t)data;
				ptr[1] = (uint8_t)data;
				return;
			}
		}
	}
}

/* ------------- IO Read/Write -------------- */

uint32_t readIO(GBA* gba, uint32_t address, uint8_t size) {
	switch (size) {
		case WIDTH_32: return littleEndian32Decode(&gba->IO[address]);
		case WIDTH_16: return littleEndian16Decode(&gba->IO[address]);
		case WIDTH_8:  return gba->IO[address];

		default: return 0;
	}
}

void writeIO(GBA* gba, uint32_t address, uint32_t data, uint8_t size) {
	switch (size) {
		case WIDTH_32: littleEndian32Encode(&gba->IO[address], data); return;
		case WIDTH_16: littleEndian16Encode(&gba->IO[address], data); return;
		case WIDTH_8:  gba->IO[address] = (uint8_t)data; return;
	}
}

/* -------------------------------- */
