#include <gba/gba.h>
#include <gba/arm7tdmi.h>
#include <gba/gamepak.h>
#include <gba/debugGBA.h>
#include <gba/renderer.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

bool initialiseSDL(GBA* gba) {
    SDL_Init(SDL_INIT_EVERYTHING);

    gba->SDL_Window = SDL_CreateWindow("MegaGBA", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIDTH_PX * DISPLAY_SCALING, HEIGHT_PX * DISPLAY_SCALING, SDL_WINDOW_SHOWN);
    //SDL_CreateWindowAndRenderer(WIDTH_PX * DISPLAY_SCALING, HEIGHT_PX * DISPLAY_SCALING, SDL_WINDOW_SHOWN,&gba->SDL_Window, &gba->SDL_Renderer);

    if (gba->SDL_Window == NULL) return false;          /* Failed to create screen */

    gba->SDL_Renderer = SDL_CreateRenderer(gba->SDL_Window, -1, SDL_RENDERER_ACCELERATED);

    if (gba->SDL_Renderer == NULL) return false; 

    gba->SDL_Texture = SDL_CreateTexture(gba->SDL_Renderer, SDL_PIXELFORMAT_BGR555, SDL_TEXTUREACCESS_STREAMING, WIDTH_PX, HEIGHT_PX);

    if (gba->SDL_Texture == NULL) return false;

    SDL_RenderSetScale(gba->SDL_Renderer, DISPLAY_SCALING, DISPLAY_SCALING);
    SDL_RenderClear(gba->SDL_Renderer);
    return true;
}

void SDLEvents(GBA* gba) {
    /* We listen for events like keystrokes and window closing */
    SDL_Event event;

#define KEYINPUT_SET(b)     writeIO(gba, KEYINPUT, readIO(gba, KEYINPUT, WIDTH_16) | (1 << b), WIDTH_16)
#define KEYINPUT_RESET(b)   writeIO(gba, KEYINPUT, readIO(gba, KEYINPUT, WIDTH_16) & ~(1 << b), WIDTH_16)

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            gba->run = false;
        } else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
            /* Handle keydown by updating KEYINPUT */
            switch (event.key.keysym.scancode) {
                case SDL_SCANCODE_S: KEYINPUT_RESET(7); break;
                case SDL_SCANCODE_W: KEYINPUT_RESET(6); break;
                case SDL_SCANCODE_A: KEYINPUT_RESET(5); break;
                case SDL_SCANCODE_D: KEYINPUT_RESET(4); break;

                case SDL_SCANCODE_Z: KEYINPUT_RESET(0); break;
                case SDL_SCANCODE_X: KEYINPUT_RESET(1); break;
                case SDL_SCANCODE_RETURN: KEYINPUT_RESET(3); break;
                case SDL_SCANCODE_TAB: KEYINPUT_RESET(2); break;
                case SDL_SCANCODE_I: KEYINPUT_RESET(9); break;
                case SDL_SCANCODE_O: KEYINPUT_RESET(8); break;

                default: break;
            }
        } else if (event.type == SDL_KEYUP && event.key.repeat == 0) {
            /* Handle keyup by updating KEYINPUT */
            switch (event.key.keysym.scancode) {
                case SDL_SCANCODE_S: KEYINPUT_SET(7); break;
                case SDL_SCANCODE_W: KEYINPUT_SET(6); break;
                case SDL_SCANCODE_A: KEYINPUT_SET(5); break;
                case SDL_SCANCODE_D: KEYINPUT_SET(4); break;

                case SDL_SCANCODE_Z: KEYINPUT_SET(0); break;
                case SDL_SCANCODE_X: KEYINPUT_SET(1); break;
                case SDL_SCANCODE_RETURN: KEYINPUT_SET(3); break;
                case SDL_SCANCODE_TAB: KEYINPUT_SET(2); break;
                case SDL_SCANCODE_I: KEYINPUT_SET(9); break;
                case SDL_SCANCODE_O: KEYINPUT_SET(8); break;

                default: break;
            }
        }
    }
}

void cleanSDL(GBA* gba) {
    SDL_DestroyTexture(gba->SDL_Texture);
	SDL_DestroyRenderer(gba->SDL_Renderer);
    SDL_DestroyWindow(gba->SDL_Window);
    SDL_Quit();

	gba->SDL_Renderer = NULL;
	gba->SDL_Window = NULL;
    gba->SDL_Renderer = NULL;
}

/* ----------------------------------------------------- */

static void initialiseIO(GBA* gba) {
    /* PPU */
    /* DISCNT, DISPSTAT, VCOUNT, BGNCNT are all 0 */

    /* Initialise affine matrix to identity, so even if they are not used
     * things render normally */
    writeIO(gba, BG2PA, 1 << 8, WIDTH_16);
    writeIO(gba, BG3PA, 1 << 8, WIDTH_16);
    writeIO(gba, BG2PD, 1 << 8, WIDTH_16);
    writeIO(gba, BG3PD, 1 << 8, WIDTH_16);

    /* Keypad */

    /* KEYINPUT - Set all key states to released */
    writeIO(gba, KEYINPUT, 0xFFFF, WIDTH_16);
}


/* ----------------------------------------------------- */

void initialiseGBA(GBA* gba, GamePak* gamepak, uint8_t* biosBuffer, size_t biosSize) {
	gba->gamepak = gamepak;
	gba->run = false;
    gba->SDL_Texture = NULL;
	gba->SDL_Renderer = NULL;
	gba->SDL_Window = NULL;

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

    /* Initialise all memory */
    memset(gba->IWRAM, 0, 0x8000);
    memset(gba->EWRAM, 0, 0x40000);
    memset(gba->IO, 0, 0x3FF);
    memset(gba->PaletteRAM, 0, 0x400);
    memset(gba->VRAM, 0, 0x18000);
    memset(gba->OAM, 0, 0x400);

	/* Initialising functions */
	initialiseCPU(gba);
    initialisePPU(gba);
    initialiseIO(gba);

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

/* busRead and busWrite are not completely fullproof, you could for example
 * read from write only memory or write to read only memory if you positioned a 16/32bit read/write
 * at the right place. Only first addresses are checked.To prevent this a more thorough 
 * checking is needed which resolves every byte individually
 *
 * Open bus is yet to be emulated */

uint32_t busRead(GBA* gba, uint32_t address, uint8_t size) {
    uint8_t* ptr = NULL;

	/* We're reading a 32/16/8 bit value from the given address */
    if (address >= BIOS_ROM_16KB && address <= BIOS_ROM_16KB_END) {
        /* Read from BIOS ROM that may or may not be available */
        if (gba->biosROM == NULL) return 0;
        if (address > gba->biosSize-1) return 0;

        ptr = &gba->biosROM[address];
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

		ptr = &gba->gamepak->allocated[relativeAddress];
		
	} else if (address >= INT_WRAM_32KB && address <= INT_WRAM_32KB_END) {
		/* Read from internal work RAM */
		ptr = &gba->IWRAM[address - INT_WRAM_32KB];
	} else if (address >= EXT_WRAM_256KB && address <= EXT_WRAM_256KB_END) {
		/* Read from external work RAM - waitstates apply */
		ptr = &gba->EWRAM[address - EXT_WRAM_256KB];
	} else if (address >= VRAM_96KB && address <= VRAM_96KB_END) {
		/* Read from Video RAM */
		ptr = &gba->VRAM[address - VRAM_96KB];
	} else if (address >= IO_REG_1KB && address <= IO_REG_1KB_END) {
		/* Read from IO register */
		ptr = &gba->IO[address - IO_REG_1KB];

        /* Handle read only */
        if ((address - IO_REG_1KB) >= BG0HOFS && (address - IO_REG_1KB) <= BG3VOFS) {
            return 0;
        } else if ((address - IO_REG_1KB) >= BG2PA && (address - IO_REG_1KB) <= BG3Y_H+1) {
            return 0;
        }
	} else if (address >= PALETTE_RAM_1KB && address <= PALETTE_RAM_1KB_END) {
		/* Read from Palette RAM */
		ptr = &gba->PaletteRAM[address - PALETTE_RAM_1KB];	
	}

    if (ptr == NULL) return 0;

    switch (size) {
		case WIDTH_32: return littleEndian32Decode(ptr);
		case WIDTH_16: return littleEndian16Decode(ptr);
		case WIDTH_8 : return *ptr;
        default: return 0;
	}
}

static void writeMem(GBA* gba, uint8_t* ptr, uint32_t data, uint8_t size) {
    switch (size) {
		case WIDTH_32: {
#if defined(DEBUG_ENABLED) && defined(DEBUG_LOG_MEM)
            printf("Written (W) %08x to %08x\n", data, address);
#endif
            littleEndian32Encode(ptr, data); 
            return;
        }
		case WIDTH_16: {
#if defined(DEBUG_ENABLED) && defined(DEBUG_LOG_MEM)
            printf("Written (HW) %04x to %08x\n", data, address);
#endif
            littleEndian16Encode(ptr, data); 
            return;
        }
		case WIDTH_8: {
#if defined(DEBUG_ENABLED) && defined(DEBUG_LOG_MEM)
            printf("Written (B) %02x to %08x\n", data, address);
#endif
            *ptr = (uint8_t)data;
            return;
        }
	}

}

void busWrite(GBA* gba, uint32_t address, uint32_t data, uint8_t size) {
    uint8_t* ptr = NULL;

	if (address >= INT_WRAM_32KB && address <= INT_WRAM_32KB_END) {
		/* Write to internal workram with current size and little endian formatting */
		ptr = &gba->IWRAM[address - INT_WRAM_32KB];
	} else if (address >= EXT_WRAM_256KB && address <= EXT_WRAM_256KB_END) {
		ptr = &gba->EWRAM[address - EXT_WRAM_256KB];
	} else if (address >= VRAM_96KB && address <= VRAM_96KB_END) {
		ptr = &gba->VRAM[address - VRAM_96KB];

		/* VRAM only supports 16 and 32 bit writes, writing a byte to the addressed
		 * halfword is going to mirror it to both upper and lower byte */
		if (size == WIDTH_8) {
			/* Halfword aligned */
			ptr = &gba->VRAM[(address & ~1) - VRAM_96KB];
            data = (data << 8) | data;
            size = WIDTH_16;
		}
	} else if (address >= IO_REG_1KB && address <= IO_REG_1KB_END) {
        uint32_t ioaddr = address - IO_REG_1KB;
		ptr = &gba->IO[ioaddr];
		/* Check for read-only registers, and prevent a write */
		switch (ioaddr) {
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

        /* Update internal registers on every write for BGNXY */
        if (ioaddr >= BG2X_L && ioaddr <= BG2Y_H) {
            writeMem(gba, ptr, data, size);
            updateInternalBGNXY(gba, 2);
            return;
        } else if (ioaddr >= BG3X_L && ioaddr <= BG3Y_H) {
            writeMem(gba, ptr, data, size);
            updateInternalBGNXY(gba, 3);
            return;
        } else if (ioaddr == IF || ioaddr == IF+1) {
            /* Intercept write to IF, whatever bits are high in the data for byte (for byte/hw)
             * will be 'acknowledged' and cleared in IF if they were set */
            writeIO(gba, ioaddr, readIO(gba, ioaddr, size)&(~data), size);
            return;
        }
	} else if (address >= PALETTE_RAM_1KB && address <= PALETTE_RAM_1KB_END) {
		ptr = &gba->PaletteRAM[address - PALETTE_RAM_1KB];

		/* Palette RAM only supports 16 and 32 bit writes, writing a byte to the addressed
		 * halfword is going to mirror it to both upper and lower byte */
		if (size == WIDTH_8) {
			/* Halfword aligned */
			ptr = &gba->PaletteRAM[(address & ~1) - PALETTE_RAM_1KB];
            data = (data << 8) | data;
            size = WIDTH_16;
		}
	}

    if (ptr == NULL) return;
    writeMem(gba, ptr, data, size);
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


