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
    uint16_t BGHOFS = 0;
    uint16_t BGVOFS = 0;
    /* Priority goes from 0-3, 3 being the lowest */
    uint8_t lowestPrio = 4;

    for (int i=8; i<12; i++) {
        if (gba->latchedDISPCNT >> i & 1) {
            uint16_t CNT = readIO(gba, BG0CNT+2*(i-8), WIDTH_16);
            uint8_t prio = CNT & 0b11;
            if (prio < lowestPrio) {
                lowestPrio = prio;
                BGCNT = CNT;
                BGHOFS = readIO(gba, BG0HOFS+2*(i-8), WIDTH_16);
                BGVOFS = readIO(gba, BG0VOFS+2*(i-8), WIDTH_16);
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
     * and extract rest of the parameters from BGCNT 
     *
     * SC1-3 may or may not be used depending on screen size, but are pre calculated
     * By default map base is set to SC0 */
    uint32_t screen0Base = VRAM_96KB + (BGCNT >> 8 & 0x1F)*0x800;
    uint32_t screen1Base = screen0Base + 0x800;
    uint32_t screen2Base = screen1Base + 0x800;
    uint32_t screen3Base = screen2Base + 0x800;

    uint32_t tileDataBase = VRAM_96KB + (BGCNT >> 2 & 0b11)*0x4000;
    uint32_t mapDataBase  = screen0Base;              
    uint8_t colorDepth = BGCNT >> 7 & 1;
    uint8_t screenSize = BGCNT >> 14 & 0b11;

    /* A BG Map is a 32x32 tile (256x256 pixel) sequentially loaded array of 2 byte/tile index data 
     * There can be anywhere from 1 to 4 BG Maps loaded in VRAM based on screen size */

    uint8_t yReal = gba->IO[VCOUNT];

    switch (screenSize) {
        case 1: case 2: case 3:
        case 0: {
            /* Text mode - screen size 0 - 32x32 tile (256x256 pixel) singular map specified at base */

            /* y is calculated by offseting VCOUNT by veritcal scroll and a modulo with 256 is applied
             * (full y is also modulo'd with 512) */
            uint16_t y = (gba->IO[VCOUNT]+(BGVOFS & 0x1FF)) & 0x1FF;

            if (y > 0xFF) {
                /* Wrap around itself, or extend to SC1/SC2 depending on screen size 
                 * For Y this is done only once, as scanline position does not change
                 * But for X it may change multiple times during rendering */

                y &= 0xFF;

                switch (screenSize) {
                    case BG_TEXT_256_512:
                        mapDataBase = screen1Base;
                        break;
                    case BG_TEXT_512_512:
                        mapDataBase = screen2Base;
                        break;
                        
                }
            }

            uint8_t pixelRow = y&0b111;                  /* 0-7 within tile */
            uint32_t tileRowsBefore = (y&(~0b111))/8;    /* No. of tile rows before the tile we are on */

            uint32_t byteOffset = tileRowsBefore*32*2;   /* Offset to be applied on mapData base to reach current tile row */

            /* Calculate starting tile and starting pixel within tile based on horizontal scrolling 
            */
            uint8_t startTile = ((BGHOFS&(~0b111))/8) & 0x3F;

            if (startTile > 0x1F) {
                /* Wrap around itself or extend to SC1/SC3 depending on screen size
                 * (modulo by 32) */
                startTile &= 0x1F;

                switch (screenSize) {
                    case BG_TEXT_512_256:
                        /* Top Right Quadrant */
                        mapDataBase = screen1Base;
                        break;
                    case BG_TEXT_512_512: {
                        if (mapDataBase == screen2Base) {
                            /* Bottom right quadrant */
                            mapDataBase = screen3Base;
                        } else if (mapDataBase == screen0Base) {
                            /* Top right quadrant */
                            mapDataBase = screen1Base;
                        }
                        break;
                    }
                }
            }

            uint8_t startPixel = BGHOFS&0b111;

            /* We should start from first tile and keep going indefinitely till buffer is full.
             * When we are done filling the scanline framebuffer, the rendering for scanline 
             * would stop. We need to ensure that horizontal wrapping is done properly using mod 32 */
            bool flag=true;
            uint16_t i = startTile;

            while (flag) { 
                uint32_t tileEntryAddress = mapDataBase + byteOffset + 2*i;
                uint16_t tileEntry = busRead(gba, tileEntryAddress, WIDTH_16);

                /* Tile entry in tileMap has been calculated */
                uint16_t tileNumber = tileEntry & 0x3FF;            /* Tile index in tileData */
                uint8_t hFlip = tileEntry >> 10 & 1;                /* Horizontal Flip */
                uint8_t vFlip = tileEntry >> 11 & 1;                /* Vertical Flip */
                uint8_t paletteNum = tileEntry >> 12 & 0xF;         /* for 4 bit color depth mode only */

                /* Indexing based on colour depth */
                uint8_t bytesPerTile = colorDepth == 1 ? 64 : 32;
                uint8_t bytesPerPixelRow  = colorDepth == 1 ? 8 : 4;
 
                uint32_t tileStartAddress = tileDataBase + tileNumber*bytesPerTile;
                uint32_t rowStartAddress = tileStartAddress + (vFlip ? (7-pixelRow) : pixelRow)*bytesPerPixelRow;

                /* 8/4 bytes per row, 1 byte / 1 nibble per pixel based on 8bit/4bit colour depth */
                for (int j=0; j<8; j++) {
                    uint16_t rgb = 0;

                    /* Load pixel colour rgb555 based on colour depth and indexing mode */
                    if (colorDepth == 1) {
                        /* 8 bit colour depth - 256/1 palette - 64 bytes/tile */
                        uint16_t pixelPalette = busRead(gba, rowStartAddress+j, WIDTH_8);
                        rgb = readPaletteRAM(gba, pixelPalette);
                    } else {
                        /* 4 bit colour depth - 16/16 palette - 32 bytes/tile */
                        uint8_t paletteByte = busRead(gba, rowStartAddress+((j&(~1))/2), WIDTH_8);
                        uint16_t pixelPalette = j&1 ? paletteByte >> 4 : paletteByte & 0xF;
                        rgb = readPaletteRAM(gba, paletteNum*16+pixelPalette);
                    }

                    /* Offset by startPixel for every tile for framebuffer loading */
                    uint8_t xReal = (i-startTile)*8 + (hFlip ? (7-j) : j) - startPixel;

                    /* Make sure hFlip is handled correctly with horizontal scrolling
                     * on the first tile. We skip some iterations either at the start or end
                     * depending on whether we do hFlip or not */
                    if (i==startTile && startPixel > 0) {
                        if (hFlip) {
                            if (j< (8-startPixel)) continue;
                        } else {
                            if (j < startPixel) continue;
                        }
                    }

                    gba->framebuffer[yReal*WIDTH_PX+xReal] = rgb;

                    /* No need to render the last tile fully if horizontal scrolling % 8 != 0,
                        * stop when the buffer is full */
                    if (xReal == WIDTH_PX-1) {
                        flag = false;
                        break;
                    }
                }

                i++;
                if (i > 0x1F) {
                    /* Wrap around itself or extend to SC1/SC3 depending on screen size 
                     * (modulo by 32) */
                    i &= 0x1F;

                    switch (screenSize) {
                        case BG_TEXT_512_256:
                            /* Swap adjacent quadrants */
                            if (mapDataBase == screen0Base) mapDataBase = screen1Base;
                            else mapDataBase = screen0Base;
                            break;
                        case BG_TEXT_512_512: {
                            /* Swap adjacent quadrants 
                             * Could be upper 2 or lower 2 depending on Y's scroll state */
                            if (mapDataBase == screen2Base) {
                                mapDataBase = screen3Base;
                            } else if (mapDataBase == screen3Base) {
                                mapDataBase = screen2Base;
                            } else if (mapDataBase == screen0Base) {
                                mapDataBase = screen1Base;
                            } else if (mapDataBase == screen1Base) {
                                mapDataBase = screen0Base;
                            }
                            break;
                        }
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
