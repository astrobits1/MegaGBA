#include <gba/gba.h>
#include <gba/renderer.h>
#include <SDL2/SDL.h>

static inline void latchDISPCNT(GBA* gba) {
	uint16_t DISPCNT = readIO(gba, DISPCNT, WIDTH_16);
    gba->latchedDISPCNT = DISPCNT;
}

static inline uint16_t readPaletteRAM(GBA* gba, uint8_t index) {
    /* Returns rgb555 - bit15 is always 0 */
    return gba->PaletteRAM[2*index] | ((gba->PaletteRAM[2*index+1] & ~(1<<15)) << 8);
}

static void repeatLoadFramebuffer(GBA* gba, uint16_t rgb, uint16_t* start, uint32_t size) {
    /* Repeat an RG555 colour across framebuffer from a given start point for size no. of pixels */
    for (uint32_t i=0; i<size; i++) {
        start[i] = rgb;
    }
}

/* ---------------------------------------------------------------------- */

static uint16_t getAffinePalette_M1_M2(GBA* gba, uint32_t mapDataBase, uint32_t tileDataBase, int32_t x_i, int32_t y_i, uint16_t noTilesPerRow) {
    uint8_t pixelRow = y_i&0b111;                  /* 0-7 within tile */
    uint32_t tileRowsBefore = (y_i&(~0b111))/8;    /* No. of tile rows before the tile we are on */
    uint32_t byteOffset = tileRowsBefore*noTilesPerRow;     /* Offset to be applied on mapData base to reach current tile row */

    uint8_t tile = (x_i&(~0b111))/8;              /* Current tile in row */
    uint8_t pixel = x_i&0b111;                    /* Current pixel in tile */
    /* Read tile index (0-255) */
    uint8_t tileIndex = busRead(gba, mapDataBase + byteOffset + tile, WIDTH_8);
    
    /* Obtain pixel row base address, where each tile is 64 bytes (8 bit depth),
     * each pixel row is 8 bytes and each pixel has 1 byte 256/1 palette index */
    uint32_t pixelRowBase = tileDataBase + tileIndex*64 + pixelRow*8;
    uint8_t pixelPalette = busRead(gba, pixelRowBase + pixel, WIDTH_8);
    uint16_t rgb = readPaletteRAM(gba, pixelPalette);

    if (pixelPalette == 0) rgb |= 1 << 15;
    return rgb;
}

/* Bitmap modes dont have such a thing as transparent palettes */
static uint16_t getAffinePalette_M3(GBA* gba, uint32_t _, uint32_t __, int32_t x_i, int32_t y_i, uint16_t noTilesPerRow) {
    uint32_t address = 480*y_i+2*x_i;
	uint16_t rgb = gba->VRAM[address] | (gba->VRAM[address+1] << 8);

    return rgb;
}

static uint16_t getAffinePalette_M4(GBA* gba, uint32_t mapDataBase, uint32_t _, int32_t x_i, int32_t y_i, uint16_t noTilesPerRow) {

	uint8_t index = gba->VRAM[mapDataBase + 240*y_i + x_i];
    uint16_t rgb = readPaletteRAM(gba, index);

    return rgb;
}

static uint16_t getAffinePalette_M5(GBA* gba, uint32_t mapDataBase, uint32_t _, int32_t x_i, int32_t y_i, uint16_t noTilesPerRow) {
    uint32_t address = mapDataBase+320*y_i+2*x_i;
    uint16_t rgb = gba->VRAM[address] | (gba->VRAM[address+1] << 8);	

    return rgb;
}

static bool computeBGRotScalScanline(GBA* gba, uint8_t N, uint16_t linebuffer[], uint32_t mapDataBase, uint32_t tileDataBase, uint16_t noTilesPerRow, uint16_t noTilesPerCol, uint8_t displayOverflow, uint16_t (*getAffinePalette)(GBA*, uint32_t, uint32_t, int32_t, int32_t, uint16_t)){
    /* Extract BG registers from layer no. (N) */
 
    int32_t BGX = N&1 ? gba->internalBG3X : gba->internalBG2X;
    int32_t BGY = N&1 ? gba->internalBG3Y : gba->internalBG2Y;

    /* BGPx are already exact 16 bit signed fixed point (16.8) */
    uint8_t base = (N&1) ? BG3PA : BG2PA;
    int16_t BGPA = (int16_t)(readIO(gba, base, WIDTH_16));
    int16_t BGPB = (int16_t)(readIO(gba, base+2, WIDTH_16));
    int16_t BGPC = (int16_t)(readIO(gba, base+4, WIDTH_16));
    int16_t BGPD = (int16_t)(readIO(gba, base+6, WIDTH_16));

    //printf("x: %d|y: %d|dx: %d|dmx: %d|dy: %d|dmy: %d|nR: %d\n", BGX>>8, BGY>>8, BGPA>>8, BGPB>>8, BGPC>>8, BGPD>>8, noTilesPerRow);

    bool transparentPixelExists = false;
    bool transparentPixel = false;
    int32_t y = BGY;
    int32_t x = BGX;

    for (int xReal=0; xReal<WIDTH_PX; xReal++) {
        /* If x is out of bounds
         * 1. displayOverflow is set to transparent and so we render transparent pixel 
         *    x is left in overflowed state but is still transformed for every pixel
         *    so future pixels can continue reading it as transparent until it is in bounds again
         * 2. displayoOverflow is set to wrap and we modulo x such that it wraps back around 
         *    to the start/end of the map if its out of bounds */

        int32_t x_i = x >> 8;
        int32_t y_i = y >> 8;

        if (x_i >= noTilesPerRow*8 || x_i < 0) {
            if (displayOverflow == 0) transparentPixel = true;
                /* Wrap it, in both + and - case it gets set to start/end */
            else x_i&=noTilesPerRow*8 - 1;
        } else transparentPixel = false;

        /* Do the same thing for y */
        if (y_i >= noTilesPerCol*8 || y_i < 0) {
            if (displayOverflow == 0) transparentPixel = true;
            else y_i&=noTilesPerCol*8 - 1;
        }

        if (!transparentPixel) {
            uint16_t rgb = getAffinePalette(gba, mapDataBase, tileDataBase, x_i, y_i, noTilesPerRow);

            /* Transparent from palette */
            if (rgb >> 15 & 1) transparentPixelExists = true;
            linebuffer[xReal] = rgb;
        } else {
            /* Transparent pixel */
            linebuffer[xReal] = 0xFFFF;
            transparentPixelExists = true;
        }
   
        // dx
        x = (x_i << 8) | (x&0xFF);
        x += BGPA;

        // dmx
        y = (y_i << 8) | (y&0xFF);
        y += BGPB;
    }

    /* Reset to initial coordinates */
    x = BGX;
    y = BGY;

    // dy
    x += BGPC;
    // dmy
    y += BGPD;

    /* Update internal scroll registers with new dy and dmy increments, for next scanline
     * Hardware register value is still unchanged and will overwrite the internal register
     * at every write to it and at the end of VBLANK */
    if (N&1) {
        gba->internalBG3X = x;
        gba->internalBG3Y = y;
    } else {
        gba->internalBG2X = x;
        gba->internalBG2Y = y;
    }

    return transparentPixelExists;
}

static bool computeBGRotScalScanline_M1_M2(GBA* gba, uint8_t N, uint16_t linebuffer[]) {
    uint16_t BGCNT = readIO(gba, BG0CNT+2*N, WIDTH_16);
    /* Extract character data and screen data base addresses 
     * as well as screen size */
    uint32_t mapDataBase = VRAM_96KB + (BGCNT >> 8 & 0x1F) * 0x800;
    uint32_t tileDataBase = VRAM_96KB + (BGCNT >> 2 & 0b11) * 0x4000;
    uint8_t screenSize = BGCNT >> 14 & 0b11;
    uint8_t displayOverflow = BGCNT >> 13 & 1;

    /* 16x16, 32x32, 64x64, 128x128 tile screen size options specified from BGCNT */
    uint16_t noTilesPerRow = 1 << (4+screenSize);
    uint16_t noTilesPerCol = noTilesPerRow;

    return computeBGRotScalScanline(gba, N, linebuffer, mapDataBase, tileDataBase, noTilesPerRow, noTilesPerCol, displayOverflow, getAffinePalette_M1_M2);
}



static bool computeBGTextScanline(GBA* gba, uint8_t N, uint16_t linebuffer[]) {
    /* This function computes the entire BG Text Mode scanline (VCOUNT) palette data given layer no.
     * (N) and loads rgb555 16 bit data per pixel (including transparency 
     * as bit 16) into the array specified by linebuffer. 
     * It also returns whether there exists a transparent pixel in the scanline */

    /* Extract BG registers from layer no. (N) */
    uint16_t BGCNT = readIO(gba, BG0CNT+2*N, WIDTH_16);
    uint16_t BGHOFS = readIO(gba, BG0HOFS+2*N, WIDTH_16);
    uint16_t BGVOFS = readIO(gba, BG0VOFS+2*N, WIDTH_16);

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

    /* Calculate starting tile and starting pixel within tile based on horizontal scrolling */
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
                if (mapDataBase == screen2Base) mapDataBase = screen3Base; /* Bottom right quadrant */
                else if (mapDataBase == screen0Base) mapDataBase = screen1Base; /* Top right quadrant */ 
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
    bool transparentPixelExists = false;

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
            uint8_t pixelPalette;
            uint16_t rgb;
            /* Load pixel colour rgb555 based on colour depth and indexing mode */
            if (tileStartAddress <= VRAM_96KB + 0xFFFF) {
                if (colorDepth == 1) {
                    /* 8 bit colour depth - 256/1 palette - 64 bytes/tile */
                    pixelPalette = busRead(gba, rowStartAddress+j, WIDTH_8);
                    rgb = readPaletteRAM(gba, pixelPalette);
                } else {
                    /* 4 bit colour depth - 16/16 palette - 32 bytes/tile */
                    uint8_t paletteByte = busRead(gba, rowStartAddress+((j&(~1))/2), WIDTH_8);
                    pixelPalette = j&1 ? paletteByte >> 4 : paletteByte & 0xF;
                    rgb = readPaletteRAM(gba, 16*paletteNum+pixelPalette);
                }
            } else {
                /* If tile is located in OBJ space for mode 0/1/2 then BG reads fail
                 * completely and we read a transparent tile (quirk) */
                pixelPalette = 0;
                rgb = 0;
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

            if (pixelPalette == 0) {
                /* Transparent pixel is marked by setting bit 15 in the colour */
                rgb |= 1 << 15;
                transparentPixelExists = true;
            }

            linebuffer[xReal] = rgb;

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
                    if (mapDataBase == screen2Base) mapDataBase = screen3Base;
                    else if (mapDataBase == screen3Base) mapDataBase = screen2Base;
                    else if (mapDataBase == screen0Base) mapDataBase = screen1Base;
                    else if (mapDataBase == screen1Base) mapDataBase = screen0Base;
                    break;
                }
            }
        }
    }

    return transparentPixelExists;
}

static bool getHighestPriorityBG(GBA* gba, uint8_t* N, bool exclude[]) {
    /* Return highest priority BG layer that is enabled and not excluded
     * Priority goes from 0-3, 3 being the lowest */
    uint8_t lowestPrio = 4;

    for (int i=8; i<12; i++) {
        if (gba->latchedDISPCNT >> i & 1) {
            uint16_t CNT = readIO(gba, BG0CNT+2*(i-8), WIDTH_16);
            uint8_t prio = CNT & 0b11;
            if (prio < lowestPrio && !exclude[i-8]) {
                lowestPrio = prio;
                *N = i-8;
            }

            /* If priority comes out to be equal to the previous lowest,
             * since lower BG(N) corresponds to higher priority in that case
             * we do nothing as lower must have been iterated before */
        }
    }

    /* No BG layer available or enabled */
    if (lowestPrio == 4) return false;

    return true;
}

/* BG Stacking is done for BG Modes 0-2, for bitmap modes stacking is not needed as only BG2 is used */

static void stackBG(GBA* gba, bool exclude[], uint8_t mode[], uint16_t linebuffer[]) {
    uint8_t N = 0;  /* 0-3 */

    bool found = getHighestPriorityBG(gba, &N, exclude);
    if (!found) {
        /* No BG layer enabled or available 
         * Fill linebuffer with white */
        repeatLoadFramebuffer(gba, (uint16_t)~(1 << 15), linebuffer, 240);
        return;
    } 

    bool transparentPixelExists;

    if (mode[N] == 1) {
        transparentPixelExists = computeBGTextScanline(gba, N, linebuffer);
    } else {
        transparentPixelExists = computeBGRotScalScanline_M1_M2(gba, N, linebuffer);
    }

    /* Transparent pixel exists, resolve by layering till there are no transparent pixels
     * or we have exhausted available layers */
    while (transparentPixelExists) {
        transparentPixelExists = false;
        /* Exclude last BG */
        exclude[N] = true;
        found = getHighestPriorityBG(gba, &N, exclude);

        /* Layers cannot be resolved anymore */
        if (!found) break;

        uint16_t currentlinebuffer[240];
        if (mode[N] == 1) {
            /* Text mode */
            computeBGTextScanline(gba, N, currentlinebuffer);
        } else {
            /* Rotation/Scaling mode */
            computeBGRotScalScanline_M1_M2(gba, N, currentlinebuffer);
        }

        /* Fill transparent pixels left over from previous compositions */
        for (int x=0; x<240; x++) {
            /* If bit 15 is set, pixel is transparent */
            if ((linebuffer[x] >> 15) & 1) {
                linebuffer[x] = currentlinebuffer[x];
                if ((linebuffer[x] >> 15) & 1) transparentPixelExists = true;
            }
        }
    }

}

static void renderWhiteScanline(GBA* gba) {
    /* Load a white scanline at current y in framebuffer */
    uint16_t* start = &gba->framebuffer[gba->IO[VCOUNT]*WIDTH_PX];
    repeatLoadFramebuffer(gba, (uint16_t)~(1 << 15), start, 240);
}

static void renderLinebuffer(GBA* gba, uint16_t linebuffer[]) {
    /* Proceed to rendering the final composite BG linebuffer.
     * Transparent pixels are set to first index in first palette.
     * This step will come after sprite layering when its implemented */

    uint16_t backdrop = readPaletteRAM(gba, 0);

    for (int x=0; x<240; x++) {
        uint16_t rgb = linebuffer[x];
        if ((rgb >> 15)&1) rgb = backdrop;

        gba->framebuffer[gba->IO[VCOUNT]*WIDTH_PX+x] = rgb;
    }

}

static void renderBGMode0Scanline(GBA* gba) {
    /* BG Mode 0 - Text mode only */

    /* Use one of BG0-3 based on priority */
    
    bool exclude[] = {false, false, false, false}; /* BG0-3 are supported in mode 0 */
    uint8_t mode[] = {1, 1, 1, 1};              /* 1=Text mode, 0=Rot/Scaling mode */
    uint16_t linebuffer[240];

    stackBG(gba, exclude, mode, linebuffer);
    renderLinebuffer(gba, linebuffer);
}

static void renderBGMode1Scanline(GBA* gba) {
    /* BG Mode 1 - Hybrid - BG0-1 Text Mode and BG2 Rot/Scaling mode, BG3 not supported */

    bool exclude[] = {false, false, false, true};
    uint8_t mode[] = {1, 1, 0, 0};
    uint16_t linebuffer[240];

    stackBG(gba, exclude, mode, linebuffer);
    renderLinebuffer(gba, linebuffer);
}

static void renderBGMode2Scanline(GBA* gba) {
    /* BG Mode 2 - Rot/Scaling only - BG2-3 Rot/Scaling and BG0-1 not supported */

    bool exclude[] = {true, true, false, false};
    uint8_t mode[] = {0, 0, 0, 0};
    uint16_t linebuffer[240];

    stackBG(gba, exclude, mode, linebuffer);
    renderLinebuffer(gba, linebuffer);
}



static void renderBGMode3Scanline(GBA* gba) {
	/* Mode 3 is a simple bitmap mode with only 1 frame/screen and the pixel data 
	 * along with colors are stored directly in VRAM from 0x06000000-0x06012BFF 
	 * Each pixel occupies 2 bytes of data (16 bit color), thus first 480 bytes define 
	 * the first scanline, and so on for every scanline 
     *
     * It also supports Rotation/Scaling mode through BG2
     * displayOverflow = 0 always, i.e when scroll goes beyond map we render transparent
     * pixels */

    /* BG2 should be enabled */
    if ((gba->latchedDISPCNT >> 10 & 1) == 0) {
        renderWhiteScanline(gba);
        return;
    }

    uint16_t linebuffer[240];

    computeBGRotScalScanline(gba, 2, linebuffer, 0, 0, 30, 20, 0, getAffinePalette_M3); 
    renderLinebuffer(gba, linebuffer); 
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

    /* BG2 should be enabled */
    if ((gba->latchedDISPCNT >> 10 & 1) == 0) {
        renderWhiteScanline(gba);
        return;
    }

	uint32_t base = (gba->latchedDISPCNT >> 4 & 1) ? 0xA000 : 0x0000;
    uint16_t linebuffer[240];

    computeBGRotScalScanline(gba, 2, linebuffer, base, 0, 30, 20, 0, getAffinePalette_M4);
    renderLinebuffer(gba, linebuffer);
}

static void renderBGMode5Scanline(GBA* gba) {
    /* 2 byte per colour direct bitmap just like mode 3,
     * but display size is reduced to 160x128. This allows us to have 2 frames
     * that are swapable like mode 4. */

    /* BG2 should be enabled */
    if ((gba->latchedDISPCNT >> 10 & 1) == 0) {
        renderWhiteScanline(gba);
        return;
    }

	uint32_t base = (gba->latchedDISPCNT >> 4 & 1) ? 0xA000 : 0x0000;
    uint16_t linebuffer[240];


    //printf("yR: %d|BGX: %d|BGY: %d\n", gba->IO[VCOUNT], gba->internalBG2X >> 8, gba->internalBG2Y >> 8);
    computeBGRotScalScanline(gba, 2, linebuffer, base, 0, 20, 16, 0, getAffinePalette_M5);
    renderLinebuffer(gba, linebuffer);
}

/* ------------------------------------------------------------------------------- */

void updateInternalBGNXY(GBA* gba, uint8_t N) {
    /* Update internal BGX and BGY affine counters for BG2 or BG3 from IO */

    uint32_t uBGX = readIO(gba, (N&1) ? BG3X_L : BG2X_L, WIDTH_32);
    uint32_t uBGY = readIO(gba, (N&1) ? BG3Y_L : BG2Y_L, WIDTH_32);

    /* Sign extend */
    if (uBGX >> 27 & 1) uBGX |= (0xF<<28);
    else uBGX &= ~(0xF<<28);

    if (uBGY >> 27 & 1) uBGY |= (0xF<<28);
    else uBGY &= ~(0xF<<28);

    if (N&1) {
        gba->internalBG3X = (int32_t)uBGX;
        gba->internalBG3Y = (int32_t)uBGX;
    } else {
        gba->internalBG2X = (int32_t)uBGX;
        gba->internalBG2Y = (int32_t)uBGY;
    }
}

void initialisePPU(GBA* gba) {
	gba->ppuHState = PPU_HDRAW;
	gba->ppuVState = PPU_VDRAW;

    gba->internalBG2X = 0;
    gba->internalBG2Y = 0;
    gba->internalBG3X = 0;
    gba->internalBG3Y = 0;

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
				/* CPU has finished running through HDRAW, now render the entire scanline
				 * using latched DISPCNT values */
				if (gba->latchedDISPCNT >> 7 & 1) {
					renderWhiteScanline(gba);
				} else {
					switch (gba->latchedDISPCNT & 0b111) {
                        case BGMODE_0:
                            renderBGMode0Scanline(gba);
                            break;
                        case BGMODE_1:
                            renderBGMode1Scanline(gba);
                            break;
                        case BGMODE_2:
                            renderBGMode2Scanline(gba);
                            break;
						case BGMODE_3: 
							/* Video/BG mode 3 -> Bitmap */
                            renderBGMode3Scanline(gba);
							break;

						case BGMODE_4:
							renderBGMode4Scanline(gba);
							break;

                        case BGMODE_5:
                            renderBGMode5Scanline(gba);
                            break;

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

                /* Reques HBLANK interrupt if enabled */
                if (STAT >> 4 & 1) requestInterrupt(gba, IRQ_LCD_HBLANK);
			} else {
				/* HBLANK
				 * CPU has finished running through HBLANK, now prepare for the next HDRAW,
				 * do latching of DISPCNT or enter VBLANK */
				gba->IO[VCOUNT]++;
				gba->IO[DISPSTAT] &= ~0b10;
				gba->ppuHState = PPU_HDRAW;

                /* Check for V-Count match in DISPSTAT */
				uint8_t vmatch = STAT >> 8;
				if (vmatch == gba->IO[VCOUNT]) {
					writeIO(gba, DISPSTAT, STAT | 0b100, WIDTH_16);
                    /* If vcounter IRQ enabled, then request interrupt */
                    if (STAT >> 5 & 1) requestInterrupt(gba, IRQ_LCD_VCOUNTER);
				} else writeIO(gba, DISPSTAT, STAT & ~0b100, WIDTH_16);

				if (gba->IO[VCOUNT] == 160) {
					/* Enter VBLANK and render frame */
					gba->ppuVState = PPU_VBLANK;
					/* Set VBLANK STAT flag */
					gba->IO[DISPSTAT] |= 1;
                    /* If VBLANK IRQ is enabled then request interrupt */
                    if (STAT >> 3 & 1) requestInterrupt(gba, IRQ_LCD_VBLANK);

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

                    /* Update internal BGNXY reference point registers,
                     * which do so at the end of every VBLANK for BG2 and BG3 */
                    updateInternalBGNXY(gba, 2);
                    updateInternalBGNXY(gba, 3);

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
