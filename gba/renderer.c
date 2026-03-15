#include <gba/gba.h>
#include <gba/renderer.h>
#include <SDL2/SDL.h>

static inline void latchDISPCNT(GBA* gba) {
	uint16_t DISPCNT = readIO(gba, DISPCNT, WIDTH_16);
    gba->latchedDISPCNT = DISPCNT;
}

static inline uint16_t readPaletteRAM(GBA* gba, uint8_t index) {
    /* Returns rgb555 */
    return gba->PaletteRAM[2*index] | (gba->PaletteRAM[2*index+1] << 8);
}

static void repeatLoadFramebuffer(GBA* gba, uint16_t rgb, uint16_t* start, uint32_t size) {
    /* Repeat an RG555 colour across framebuffer from a given start point for size no. of pixels */
    for (uint32_t i=0; i<size; i++) {
        start[i] = rgb;
    }
}

/* ---------------------------------------------------------------------- */


static void renderWhiteScanline(GBA* gba) {
    /* Load a white scanline at current y in framebuffer */
    uint16_t* start = &gba->framebuffer[gba->IO[VCOUNT]*WIDTH_PX];
    memset(start, 0xFF, BYTES_PER_Y);
}

static void renderBGMode0Scanline(GBA* gba) {
    /* BG Mode 0 - Text mode only */

    /* Use one of BG0-3 based on priority */
    uint16_t BGCNT = 0;
    /* Priority goes from 0-3, 3 being the lowest */
    uint8_t lowestPrio = 4;

    for (int i=8; i<12; i++) {
        if (gba->latchedDISPCNT >> i & 1) {
            uint16_t CNT = readIO(gba, BG0CNT+2*(i-8), WIDTH_16);
            uint8_t prio = CNT & 0b11;
            if (prio < lowestPrio) {
                lowestPrio = prio;
                BGCNT = CNT;
            }

            /* If priority comes out to be equal to the previous lowest,
             * since lower BG(N) corresponds to higher priority in that case
             * we do nothing as lower must have been iterated before */
        }
    }

    if (lowestPrio == 4) {
        /* No BG layer enabled, render white scanline */
        renderWhiteScanline(gba);
        return;
    }

    /* Identify Character(Tile) data base block and Screen (BG Map) base block 
     * and extract rest of the parameters from BGCNT */
    uint32_t tileDataBase = VRAM_96KB + (BGCNT >> 2 & 0b11)*0x4000;
    uint32_t mapDataBase  = VRAM_96KB + (BGCNT >> 8 & 0x1F)*0x800;
    uint8_t colorDepth = BGCNT >> 7 & 1;
    uint8_t screenSize = BGCNT >> 14 & 0b11;

    /* A BG Map is a 32x32 tile (256x256 pixel) sequentially loaded array of 2 byte/tile index data 
     * There can be anywhere from 1 to 4 BG Maps loaded in VRAM based on screen size */

    uint8_t y = gba->IO[VCOUNT];

    switch (screenSize) {
        case 0: {
            /* Text mode - screen size 0 - 32x32 tile (256x256 pixel) singular map specified at base */
            uint8_t pixelRow = y&0b111;                  /* 0-7 within tile */
            uint32_t tileRowsBefore = (y&(~0b111))/8;    /* No. of tile rows before the tile we are on */

            uint32_t byteOffset = tileRowsBefore*32*2;   /* Offset to be applied on mapData base to reach current tile row */

            /* For now we dont consider scrolling or any other elements 
             * Process from tile 0 to tile 29, which completes 240 horizontal pixels */
            for (int i=0; i<30; i++) {
                uint32_t tileEntryAddress = mapDataBase + byteOffset + 2*i;
                uint16_t tileEntry = busRead(gba, tileEntryAddress, WIDTH_16);

                /* Tile entry in tileMap has been calculated */
                uint16_t tileNumber = tileEntry & 0x3FF;            /* Tile index in tileData */
                uint8_t hFlip = tileEntry >> 10 & 1;                /* Horizontal Flip */
                uint8_t vFlip = tileEntry >> 11 & 1;                /* Vertical Flip */
                uint8_t paletteNum = tileEntry >> 12 & 0xF;         /* for 4 bit color depth mode only */

                if (colorDepth == 1) {
                    /* 8 bit color depth - 256/1 
                     * 64 bytes / tile */
                    uint32_t tileStartAddress = tileDataBase + tileNumber*64;
                    uint32_t rowStartAddress = tileStartAddress + (vFlip ? (7-pixelRow) : pixelRow)*8;

                    /* 8 bytes per row, 1 byte per pixel */
                    for (int j=0; j<8; j++) {
                        uint8_t pixelPalette = busRead(gba, rowStartAddress+j, WIDTH_8);
                        uint16_t rgb = readPaletteRAM(gba, pixelPalette);
                        uint8_t x = i*8 + (hFlip ? 7-j : j);

                        gba->framebuffer[y*WIDTH_PX+x] = rgb;
                    }
                } else {
                    /* 4 bit colour depth - 16/16 
                     * 32 bytes / tile */
                    uint32_t tileStartAddress = tileDataBase + tileNumber*32;
                    uint32_t rowStartAddress = tileStartAddress + (vFlip ? (7-pixelRow) : pixelRow)*4;

                    for (int j=0; j<4; j++) {
                        uint8_t paletteData = busRead(gba, rowStartAddress+j, WIDTH_8);
                        uint8_t paletteLeft = paletteData & 0xF;
                        uint8_t paletteRight = paletteData >> 4;

                        uint8_t xL = i*8 + (hFlip ? 7-j*2 : j-2);
                        uint8_t xR = xL+(hFlip ? -1 : +1);
                        uint16_t rgbL = readPaletteRAM(gba, paletteNum*16+paletteLeft);
                        uint16_t rgbR = readPaletteRAM(gba, paletteNum*16+paletteRight);

                        gba->framebuffer[y*WIDTH_PX+xL] = rgbL;
                        gba->framebuffer[y*WIDTH_PX+xR] = rgbR;
                    }
                }
            }

            break;
        }

        default: {
            renderWhiteScanline(gba);
            break;
        }
    }
}

static void renderBGMode3Scanline(GBA* gba) {
	/* Mode 3 is a simple bitmap mode with only 1 frame/screen and the pixel data 
	 * along with colors are stored directly in VRAM from 0x06000000-0x06012BFF 
	 * Each pixel occupies 2 bytes of data (16 bit color), thus first 480 bytes define 
	 * the first scanline, and so on for every scanline */
	uint8_t y = gba->IO[VCOUNT];

	for (int x = 0; x < 240; x++) { 			/* 240 Pixels */
		uint32_t address = 480*y+2*x;
		uint16_t rgb = gba->VRAM[address] | (gba->VRAM[address+1] << 8);

        gba->framebuffer[y*WIDTH_PX+x] = rgb;
	}
}

static void renderBGMode4Scanline(GBA* gba) {
	/* Mode 4 is a bitmap mode similar to mode 3, with the exception of having 2 frames
	 * 
	 * Display frame is selected using bit 4 of DISPCNT
	 * frame 0 -> 0x06000000-0x060095FF 
	 * frame 1 -> 0x0600A000-0x060135FF 
	 * Each pixel is 1 byte, first scanline is 0-240, and so on
	 * The byte represents the BG Palette RAM index, color 0 being transparent 
	 * Note: Transparent color is the color 0 of BG Palette, currently sprites are not supported */

	uint32_t base = (gba->latchedDISPCNT >> 4 & 1) ? 0xA000 : 0x0000;
	uint8_t y = gba->IO[VCOUNT];

	for (int x = 0; x < 240; x++) {
		/* BG Palette RAM */
		uint8_t index = gba->VRAM[base + 240*y + x];
		uint16_t rgb = readPaletteRAM(gba, index);

        gba->framebuffer[y*WIDTH_PX+x] = rgb; 
	}
}

static void renderBGMode5Scanline(GBA* gba) {
    /* 2 byte per colour direct bitmap just like mode 3,
     * but display size is reduced to 160x128. This allows us to have 2 frames
     * that are swapable like mode 4. */
	uint32_t base = (gba->latchedDISPCNT >> 4 & 1) ? 0xA000 : 0x0000;
	uint8_t y = gba->IO[VCOUNT];

    if (y < 128) {
	    for (int x = 0; x < 160; x++) {
		    /* BG Palette RAM */
            uint32_t address = 320*y+2*x;
            uint16_t rgb = gba->VRAM[address] | (gba->VRAM[address+1] << 8);	

		    gba->framebuffer[y*WIDTH_PX+x] = rgb;  
	    }
    }

    /* For the background, use the first colour entry in the palette 
     * (This is not the exact behaviour, which has to do with affine and this 
     * part is subject to change) */

    uint16_t rgb = gba->PaletteRAM[0] | (gba->PaletteRAM[1] << 8);

    for (int i= y>=128 ? 0 : 160; i<240; i++) {
        gba->framebuffer[y*WIDTH_PX+i] = rgb;
    }
}

/* ------------------------------------------------------------------------------- */


void initialisePPU(GBA* gba) {
	gba->ppuHState = PPU_HDRAW;
	gba->ppuVState = PPU_VDRAW;

    /* Initialise framebuffer to full white */
    memset(&gba->framebuffer, 0xFF, WIDTH_PX*HEIGHT_PX*sizeof(uint16_t));
    latchDISPCNT(gba);
}

void stepPPU(GBA* gba) {
	/* Called at the end of every HDRAW and HBLANK to synchronize
	 *
	 * The state machine cycles back and forth between HDRAW and HBLANK throughout the frame
	 * From VCOUNT=0-159, it is part of VDRAW and from 160-226 it is part of VBLANK
	 * The frame is drawn at the end of VDRAW */
	
	/* DISPCNT should be latched at the start of HDRAW and unlatched at start of HBLANK.
	 * The PPU is called to synchornize after the CPU is done for the particular amount of cycles
	 * this means we're doing a post-sync */
    uint16_t STAT = readIO(gba, DISPSTAT, WIDTH_16);

	switch (gba->ppuVState) {
		case PPU_VDRAW: {
			if (gba->ppuHState == PPU_HDRAW) {
				/* Check for V-Count match in DISPSTAT */
				uint8_t vmatch = STAT >> 8;
				if (vmatch == gba->IO[VCOUNT]) {
					writeIO(gba, DISPSTAT, STAT | 0b100, WIDTH_16);
				} else writeIO(gba, DISPSTAT, STAT & ~0b100, WIDTH_16);

				/* CPU has finished running through HDRAW, now render the entire scanline
				 * using latched DISPCNT values */
				if (gba->latchedDISPCNT >> 7 & 1) {
					renderWhiteScanline(gba);
				} else {
                    uint8_t BG2_Flag = gba->latchedDISPCNT >> 10 & 1;
					switch (gba->latchedDISPCNT & 0b111) {
                        case BGMODE_0: {
                            renderBGMode0Scanline(gba);
                            break;
                        }
						case BGMODE_3: {
							/* Video/BG mode 3 -> Bitmap */
							if (BG2_Flag) {
								renderBGMode3Scanline(gba);
							} else {
								/* BG2 not enabled, render white scanline */
								renderWhiteScanline(gba);
							}
							break;
						}

						case BGMODE_4: {
							if (BG2_Flag) renderBGMode4Scanline(gba);
							else renderWhiteScanline(gba);
							break;
						}
                        case BGMODE_5: {
                            if (BG2_Flag) renderBGMode5Scanline(gba);
                            else renderWhiteScanline(gba);
                            break;
                        }

						default: {
							//printf("[WARNING] Invalid Video Mode %d, rendering white line\n", gba->videoMode);
							renderWhiteScanline(gba);
							break;
						}
					}
				}

				/* Switch to HBLANK - TODO HBLANK flag is set late */
				gba->IO[DISPSTAT] |= 0b10;
				gba->ppuHState = PPU_HBLANK;
			} else {
				/* HBLANK
				 * CPU has finished running through HBLANK, now prepare for the next HDRAW,
				 * do latching of DISPCNT or enter VBLANK */
				gba->IO[VCOUNT]++;
				gba->IO[DISPSTAT] &= ~0b10;
				gba->ppuHState = PPU_HDRAW;

				if (gba->IO[VCOUNT] == 160) {
					/* Enter VBLANK and render frame */
					gba->ppuVState = PPU_VBLANK;
					/* Set VBLANK STAT flag */
					gba->IO[DISPSTAT] |= 1;

					SDLEvents(gba);

                    /* Update texture with framebuffer and render it at the end of frame */
                    SDL_UpdateTexture(gba->SDL_Texture, NULL, &gba->framebuffer, WIDTH_PX*sizeof(uint16_t));

                    SDL_RenderClear(gba->SDL_Renderer);
                    SDL_RenderCopy(gba->SDL_Renderer, gba->SDL_Texture, NULL, NULL);
					SDL_RenderPresent(gba->SDL_Renderer);
				} else {
					/* Latch DISPCNT if not entering VBLANK */
					latchDISPCNT(gba);
				}
			}
			break;
		}
		case PPU_VBLANK: {
			/* PPU is not rendering anything, and is in VBLANK */
			if (gba->ppuHState == PPU_HDRAW) {
				/* Check for V-Count match in DISPSTAT */
				uint8_t vmatch = STAT >> 8;
				if (vmatch == gba->IO[VCOUNT]) {
					writeIO(gba, DISPSTAT, STAT | 0b100, WIDTH_16);
				} else writeIO(gba, DISPSTAT, STAT & ~0b100, WIDTH_16);

				/* Set HBLANK DISPSTAT flag (should be done later) */
				writeIO(gba, DISPSTAT, STAT | 0b10, WIDTH_16);
				gba->ppuHState = PPU_HBLANK;
			} else if (gba->ppuHState == PPU_HBLANK) {
				gba->IO[VCOUNT]++;
				/* Set HDRAW, Clear HBLANK DISPSTAT flag */
				writeIO(gba, DISPSTAT, STAT & ~0b10, WIDTH_16);
				gba->ppuHState = PPU_HDRAW;

				if (gba->IO[VCOUNT] == 228) {
					/* End of VBLANK */
					gba->IO[VCOUNT] = 0;
					gba->ppuVState = PPU_VDRAW;

					latchDISPCNT(gba);
				} else if (gba->IO[VCOUNT] == 227) {
					/* Last line of VBLANK, unset VBLANK flag in DISPSTAT */
					writeIO(gba, DISPSTAT, STAT & ~1, WIDTH_16);
				}
			}
			break;
		}
	}
}
