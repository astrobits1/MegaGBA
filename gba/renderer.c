#include <gba/gba.h>
#include <gba/renderer.h>
#include <SDL2/SDL.h>

void latchDISPCNT(GBA* gba) {
	uint16_t DISPCNT = readIO(gba, DISPCNT, WIDTH_16);

	/* Note: Values 6 and 7 are prohibited and dont mean anything */
	gba->videoMode = DISPCNT & 0b111;
	gba->forcedBlank = DISPCNT >> 7 & 1;
	gba->BG0_Flag = DISPCNT >> 8 & 1;
	gba->BG1_Flag = DISPCNT >> 9 & 1;
	gba->BG2_Flag = DISPCNT >> 10 & 1;
	gba->BG3_Flag = DISPCNT >> 11 & 1;
}

static inline uint8_t toRGB888(uint8_t rgb555) {
    /* Input can be red, green or blue value of rgb 555 */
    return (rgb555 << 3) | (rgb555 >> 2);
}

static void renderWhiteScanline(GBA* gba) {
	SDL_SetRenderDrawColor(gba->SDL_Renderer, 255, 255, 255, 255);
	SDL_RenderDrawLine(gba->SDL_Renderer, 0, gba->IO[VCOUNT], 239, gba->IO[VCOUNT]);
}

static void renderMode3Scanline(GBA* gba) {
	/* Mode 3 is a simple bitmap mode with only 1 frame/screen and the pixel data 
	 * along with colors are stored directly in VRAM from 0x06000000-0x06012BFF 
	 * Each pixel occupies 2 bytes of data (16 bit color), thus first 480 bytes define 
	 * the first scanline, and so on for every scanline */
	uint8_t y = gba->IO[VCOUNT];

    printf("Y: %d |", y);
	for (int x = 0; x < 240; x++) { 			/* 240 Pixels */
		uint32_t address = 480*y+2*x;
		uint16_t rgb = gba->VRAM[address] | (gba->VRAM[address+1] << 8);
		uint8_t r = rgb & 0x1F;
		uint8_t g = rgb >> 5 & 0x1F;
		uint8_t b = rgb >> 10 & 0x1F;

        printf("%d %d %d |", toRGB888(r), toRGB888(g), toRGB888(b));
		SDL_SetRenderDrawColor(gba->SDL_Renderer, toRGB888(r), toRGB888(g), toRGB888(b), 255);
		SDL_RenderDrawPoint(gba->SDL_Renderer, x, y);
	}

    printf("\n");
}

static void renderMode4Scanline(GBA* gba) {
	/* Mode 4 is a bitmap mode similar to mode 3, with the exception of having 2 frames
	 * 
	 * Display frame is selected using bit 4 of DISPCNT
	 * frame 0 -> 0x06000000-0x060095FF 
	 * frame 1 -> 0x0600A000-0x060135FF 
	 * Each pixel is 1 byte, first scanline is 0-240, and so on
	 * The byte represents the BG Palette RAM index, color 0 being transparent 
	 * Note: Transparent color is the color 0 of BG Palette, currently sprites are not supported */

	uint8_t frame = gba->IO[DISPCNT] >> 4 & 1;
	uint32_t base = frame ? 0xA000 : 0x0000;
	uint8_t y = gba->IO[VCOUNT];

	for (int x = 0; x < 240; x++) {
		/* BG Palette RAM */
		uint8_t index = gba->VRAM[base + 240*y + x];
		uint16_t rgb = gba->PaletteRAM[2*index] | (gba->PaletteRAM[2*index+1] << 8);

        //if (rgb != 0xFFFF) printf("c %x\n", rgb);
		uint8_t r = rgb & 0x1F;
		uint8_t g = rgb >> 5 & 0x1F;
		uint8_t b = rgb >> 10 & 0x1F;

		SDL_SetRenderDrawColor(gba->SDL_Renderer, toRGB888(r), toRGB888(g), toRGB888(b), 255);
		SDL_RenderDrawPoint(gba->SDL_Renderer, x, y);
	}
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
 
	switch (gba->ppuVState) {
		case PPU_VDRAW: {
			if (gba->ppuHState == PPU_HDRAW) {
				/* Check for V-Count match in DISPSTAT */
				uint8_t vmatch = gba->IO[DISPSTAT + 1];
				if (vmatch == gba->IO[VCOUNT]) {
					gba->IO[DISPSTAT] |= 0b100;
				} else gba->IO[DISPSTAT] &= ~0b100;

				/* CPU has finished running through HDRAW, now render the entire scanline
				 * using latched DISPCNT values */
				if (gba->forcedBlank) {
					renderWhiteScanline(gba);
				} else {
					switch (gba->videoMode) {
						case VMODE_3: {
							/* Video/BG mode 3 -> Bitmap */
							if (gba->BG2_Flag) {
								renderMode3Scanline(gba);
							} else {
								/* BG2 not enabled, render white scanline */
								renderWhiteScanline(gba);
							}
							break;
						}

						case VMODE_4: {
							if (gba->BG2_Flag) renderMode4Scanline(gba);
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
				uint8_t vmatch = gba->IO[DISPSTAT + 1];
				if (vmatch == gba->IO[VCOUNT]) {
					gba->IO[DISPSTAT] |= 0b100;
				} else gba->IO[DISPSTAT] &= ~0b100;

				/* Set HBLANK DISPSTAT flag (should be done later) */
				gba->IO[DISPSTAT] |= 0b10;
				gba->ppuHState = PPU_HBLANK;
			} else if (gba->ppuHState == PPU_HBLANK) {
				gba->IO[VCOUNT]++;
				/* Set HDRAW, Clear HBLANK DISPSTAT flag */
				gba->IO[DISPSTAT] &= ~0b10;
				gba->ppuHState = PPU_HDRAW;

				if (gba->IO[VCOUNT] == 228) {
					/* End of VBLANK */
					gba->IO[VCOUNT] = 0;
					gba->ppuVState = PPU_VDRAW;

					latchDISPCNT(gba);
				} else if (gba->IO[VCOUNT] == 227) {
					/* Last line of VBLANK, unset VBLANK flag in DISPSTAT */
					gba->IO[DISPSTAT] &= ~1;
				}
			}
			break;
		}
	}
}
