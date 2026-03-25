#include <gba/gba.h>
#include <gba/renderer.h>
#include <SDL2/SDL.h>
#include <unistd.h>


static inline void latchDISPCNT(GBA* gba) {
	uint16_t DISPCNT = readIO_internal(gba, DISPCNT, WIDTH_16);
    gba->latchedDISPCNT = DISPCNT;
}

static inline uint16_t readPaletteRAM(GBA* gba, uint8_t index) {
    /* Returns rgb555 - bit15 is always 0 */
    return gba->PaletteRAM[2*index] | ((gba->PaletteRAM[2*index+1] & ~(1<<15)) << 8);
}

static inline uint8_t readVRAM_8(GBA* gba, uint32_t address) {
    /* VRAM open bus */
    if (address > 0x17FFF) return 0;
    return gba->VRAM[address];
}

static inline uint16_t readVRAM_16(GBA* gba, uint32_t address) {
    return readVRAM_8(gba, address) | (readVRAM_8(gba, address+1) << 8);
}

static inline uint32_t readVRAM_32(GBA* gba, uint32_t address) {
    return readVRAM_16(gba, address) | (readVRAM_16(gba, address+2) << 16);
}

static inline uint64_t readVRAM_64(GBA* gba, uint32_t address) {
    return (uint64_t)readVRAM_32(gba, address) | ((uint64_t)readVRAM_32(gba, address+4) << 32);
}

static inline uint8_t readOAM_8(GBA* gba, uint32_t address) {
    return gba->OAM[address];
}

static inline uint16_t readOAM_16(GBA* gba, uint32_t address) {
    return gba->OAM[address] | (gba->OAM[address+1] << 8);
}

static void repeatLoadFramebuffer(GBA* gba, uint16_t rgb, uint16_t* start, uint32_t size) {
    /* Repeat an RG555 colour across framebuffer from a given start point for size no. of pixels */
    for (uint32_t i=0; i<size; i++) {
        start[i] = rgb;
    }
}
/* ---------------------------------------------------------------------- */

static void getSpriteDimensions(uint8_t size, uint8_t shape, uint8_t* _width, uint8_t* _height) {
    /* Converts size and shape to height and width in tiles */
    uint8_t width=0;
    uint8_t height=0;

    if (size == 0) {
        width = 1;
        height = 1;
        if (shape == OBJ_SHAPE_HORIZONTAL) width = 2;
        else if (shape == OBJ_SHAPE_VERTICAL) height = 2;
    } else if (size == 1) {
        width = 2;
        height = 2;
        if (shape == OBJ_SHAPE_HORIZONTAL) {
            width = 4;
            height = 1;
        } else if (shape == OBJ_SHAPE_VERTICAL) {
            width = 1;
            height = 4;
        }
    } else if (size == 2) {
        width = 4;
        height = 4;

        if (shape == OBJ_SHAPE_HORIZONTAL) height = 2;
        else if (shape == OBJ_SHAPE_VERTICAL) width = 2;
    } else if (size == 3) {
        width = 8;
        height = 8;

        if (shape == OBJ_SHAPE_HORIZONTAL) height = 4;
        else if (shape == OBJ_SHAPE_VERTICAL) width = 4;
    }

    *_width = width;
    *_height = height;
}

static bool checkSpriteVisibility(GBA* gba, uint8_t height, uint8_t width, uint16_t x_obj, uint8_t y_obj, uint8_t* y_objInternal) {
    /* Check if it cuts the current scanline */
    uint8_t y_rendering = gba->IO[VCOUNT];
    bool inVerticalBounds = false;
    bool inHorizontalBounds = false;

    if (y_obj >= 160) {
        /* Y out of screen */
        if (height*8 > 256-y_obj) {
            /* Wrapover to top scanline */
            uint8_t noWrapped = height*8 - (256-y_obj);

            if (y_rendering < noWrapped) {
                *y_objInternal = (256-y_obj)+y_rendering;
                inVerticalBounds = true;
            }
        }
    } else {
        /* Y on screen for atleast 1 scanline and has no chance of being wrapped to top */
        if ((y_rendering >= y_obj) && (y_rendering <= (y_obj + height*8-1))){
            *y_objInternal = y_rendering - y_obj;
            inVerticalBounds = true;
        }
    }

    if (x_obj >= 240) {
        /* X out of screen */
        if (width*8 > 512-x_obj) {
            /* Wraps to start of scanline */
            inHorizontalBounds = true;
        }
    } else {
        /* Atleast 1 pixel visible */
        inHorizontalBounds = true;
    }

    return inVerticalBounds && inHorizontalBounds;
}

static uint64_t getSpriteRowData_8bit(GBA* gba, uint8_t mappingType, uint32_t tileDataBase, uint16_t tileIndex, uint16_t width, uint8_t tileInRow, uint8_t y_objInternal) {
    /* Returns tile data row for given parameters in 8 bit 256/1 palette indexing mode */

    uint8_t noTileRowsBefore = (y_objInternal & ~0b111)/8;
    /* 64 bytes per tile, in incremements of 2 tile index per tile */

    if (mappingType == OBJ_VRAM_MAPPING_1DIM) {
        /* 1 dimensional indexing */
        uint32_t offset = tileIndex*32 + width*noTileRowsBefore*64 + tileInRow*64;
        uint32_t internalYOffset = y_objInternal*8;

        return readVRAM_64(gba, tileDataBase + offset + internalYOffset);
    } else {
        /* 2 dimensional indexing *
         * Bit 0 is ignored */
        tileIndex &= ~1;
        uint32_t offset = tileIndex*32 + noTileRowsBefore*0x20*32 + tileInRow*64;
        uint32_t internalYOffset = y_objInternal*8;

        return readVRAM_64(gba, tileDataBase + offset + internalYOffset); 
    }
}

static uint32_t getSpriteRowData_4bit(GBA* gba, uint8_t mappingType, uint32_t tileDataBase, uint16_t tileIndex, uint8_t height, uint16_t width, uint8_t tileInRow, uint8_t y_objInternal) {
    /* Returns tile data row for given parameters in 4 bit 16/16 palette indexing mode */

    return 0;
}



static bool computeSpriteScanline(GBA* gba, uint16_t linebuffer[], uint8_t minPriority, uint8_t maxPriority, uint32_t tileDataBase) {
    /* Computes and stacks sprites between minPriority and maxPriority that are enabled on a single
     * linebuffer, filling the rest with transparent. Returns whether it found atleast a single
     * sprite that is enabled and visible in that range */
    uint16_t DCNT = readIO_internal(gba, DISPCNT, WIDTH_16);
    uint8_t vramMapping = DCNT >> 6 & 1;

    bool spriteRendered = false;
    /* BG priority and OAM priority conflicts should not happen, that is opposing priorities
     * for BG and OAM for 2 sprites (overlapping or non overlapping). It is handled deterministically
     * here as we scan all OAM entries for every BG priority number, so higher BG priority and lower
     * OAM priority will overlap a low BG priority and high OAM priority. 
     * On real hardware this causes garbage to be rendered */
    for (int p=maxPriority; p>=minPriority; p--) {
        uint16_t currentlinebuffer[240];
        /* Set all palettes to transparent by default */
        memset(currentlinebuffer, 1<<15, 240*sizeof(uint16_t));

        for (int i=0; i<128; i++) {
            /* Scan OAM for sprites of a certain BG Priority */
            uint32_t oamAddress = i*8;

            uint16_t attr0 = readOAM_16(gba, i*8);
            uint16_t attr1 = readOAM_16(gba, i*8+1);
            uint16_t attr2 = readOAM_16(gba, i*8+2);

            /* Not same BG Priority */
            if ((attr2 >> 10 & 0b11) != p) continue;

            /* Other OBJ modes are not supported for now */
            if ((attr0 >> 10 & 0b11) != 0) continue;

            uint8_t y_obj = attr0 & 0xFF;
            uint8_t paletteMode = attr0 >> 13 & 1;          /* 256/1 or 16/16 */
            uint8_t shape = attr0 >> 14 & 0b11;

            uint16_t x_obj = attr1 & 0x1FF;
            uint8_t size = attr1 >> 14 & 0b11;

            uint16_t tileIndex = attr2 & 0x3FF;
            uint8_t paletteNum = attr2 >> 12 & 0xF;         /* For 16/16 palettes only */

            if ((attr0 >> 8 & 1)) {
                /* Rotation and Scaling */
                printf("Rotation/Scaling mode not handled\n");
                continue;
            } else {
                /* Normal Mode */
                /* OBJ disabled */
                if ((attr0 >> 9 & 1)) continue;

                uint8_t height, width, y_objInternal;
                getSpriteDimensions(size, shape, &width, &height);

                bool inBounds = checkSpriteVisibility(gba, height, width, x_obj, y_obj, &y_objInternal);
                if (!inBounds) continue;

                /* This sprite must be rendered, and we know its exact internal Y */
                spriteRendered = true;
                uint8_t hFlip = attr1 >> 12 & 1;
                uint8_t vFlip = attr1 >> 13 & 1;

                uint8_t noTileRowsBefore = (y_objInternal & ~0b111)/8;

                for (int tile=0; tile<width; tile++) {
                    if (paletteMode == PALETTE_256_1_8BIT) {
                        uint8_t yInternal = vFlip ? (height*8-1)-y_objInternal : y_objInternal;
                        uint64_t rowData = getSpriteRowData_8bit(gba, vramMapping, tileDataBase, tileIndex, width, hFlip ? (width-1)-tile : tile, yInternal);

                        for (uint8_t xInternal=0; xInternal<8; xInternal++) {
                            uint8_t xReal = x_obj+tile*8+xInternal;
                            uint8_t paletteIndex = rowData>>((hFlip?(7-xInternal):xInternal)*8)&0xFF;
                            uint16_t rgb = readPaletteRAM(gba, paletteIndex);

                            if (paletteIndex == 0) rgb |= 1<<15;
                            currentlinebuffer[xReal] = rgb;
                        }
                        
                    } else {
                        printf("Cannot render 16/16 sprites\n");
                        break;
                    }
                }
            }
        }

        /* Overlay current layer with the previous composed */
        for (int x=0; x<240; x++) {
            /* If bit 15 is set, pixel is transparent */
            if (!(currentlinebuffer[x] >> 15 & 1)) {
                /* If not transparent then overlay */
                linebuffer[x] = currentlinebuffer[x];
            }
        }


    }
    return spriteRendered;
}

/* ---------------------------------------------------------------------- */

static uint16_t getBGAffinePalette_M1_M2(GBA* gba, uint32_t mapDataBase, uint32_t tileDataBase, int32_t x_i, int32_t y_i, uint16_t noTilesPerRow) {
    uint8_t pixelRow = y_i&0b111;                  /* 0-7 within tile */
    uint32_t tileRowsBefore = (y_i&(~0b111))/8;    /* No. of tile rows before the tile we are on */
    uint32_t byteOffset = tileRowsBefore*noTilesPerRow;     /* Offset to be applied on mapData base to reach current tile row */

    uint8_t tile = (x_i&(~0b111))/8;              /* Current tile in row */
    uint8_t pixel = x_i&0b111;                    /* Current pixel in tile */
    /* Read tile index (0-255) */
    uint8_t tileIndex = readVRAM_8(gba, mapDataBase + byteOffset + tile);
    
    /* Obtain pixel row base address, where each tile is 64 bytes (8 bit depth),
     * each pixel row is 8 bytes and each pixel has 1 byte 256/1 palette index */
    uint32_t pixelRowBase = tileDataBase + tileIndex*64 + pixelRow*8;
    uint8_t pixelPalette = readVRAM_8(gba, pixelRowBase + pixel);
    uint16_t rgb = readPaletteRAM(gba, pixelPalette);

    rgb &= ~(1<<15);
    if (pixelPalette == 0) rgb |= 1 << 15;
    return rgb;
}

/* Bitmap modes dont have such a thing as transparent palettes */
static uint16_t getBGAffinePalette_M3(GBA* gba, uint32_t _, uint32_t __, int32_t x_i, int32_t y_i, uint16_t noTilesPerRow) {
    uint32_t address = 480*y_i+2*x_i;
	uint16_t rgb = gba->VRAM[address] | (gba->VRAM[address+1] << 8);

    rgb &= ~(1<<15);
    return rgb;
}

static uint16_t getBGAffinePalette_M4(GBA* gba, uint32_t mapDataBase, uint32_t _, int32_t x_i, int32_t y_i, uint16_t noTilesPerRow) {

	uint8_t index = gba->VRAM[mapDataBase + 240*y_i + x_i];
    uint16_t rgb = readPaletteRAM(gba, index);

    rgb &= ~(1<<15);
    return rgb;
}

static uint16_t getBGAffinePalette_M5(GBA* gba, uint32_t mapDataBase, uint32_t _, int32_t x_i, int32_t y_i, uint16_t noTilesPerRow) {
    uint32_t address = mapDataBase+320*y_i+2*x_i;
    uint16_t rgb = gba->VRAM[address] | (gba->VRAM[address+1] << 8);	

    rgb &= ~(1<<15);
    return rgb;
}

static bool computeBGRotScalScanline(GBA* gba, uint8_t N, uint16_t linebuffer[], uint32_t mapDataBase, uint32_t tileDataBase, uint16_t noTilesPerRow, uint16_t noTilesPerCol, uint8_t displayOverflow, uint16_t (*getBGAffinePalette)(GBA*, uint32_t, uint32_t, int32_t, int32_t, uint16_t)){
    /* Extract BG registers from layer no. (N) */
 
    int32_t BGX = N&1 ? gba->internalBG3X : gba->internalBG2X;
    int32_t BGY = N&1 ? gba->internalBG3Y : gba->internalBG2Y;

    /* BGPx are already exact 16 bit signed fixed point (16.8) */
    uint8_t base = (N&1) ? BG3PA : BG2PA;
    int16_t BGPA = (int16_t)(readIO_internal(gba, base, WIDTH_16));
    int16_t BGPB = (int16_t)(readIO_internal(gba, base+2, WIDTH_16));
    int16_t BGPC = (int16_t)(readIO_internal(gba, base+4, WIDTH_16));
    int16_t BGPD = (int16_t)(readIO_internal(gba, base+6, WIDTH_16));

    //printf("y: %d|BX: %d|BY: %d|PA: %04x|PB: %04x|PC: %04x|PD: %04x|npx: %d\n", gba->IO[VCOUNT], (int)BGX>>8, (int)BGY>>8, BGPA, BGPB, BGPC, BGPD, noTilesPerRow*8);

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
                /* Wrap it, in both + and - case it gets set to start/end 
                 * bitANDing only works as mod for powers of 2, bitmap modes have forced display
                 * overflow = 0 so the else shouldnt run for them (they dont have sides in powers of 2) */
            else x_i&=noTilesPerRow*8 - 1;
        } else transparentPixel = false;

        /* Do the same thing for y */
        if (y_i >= noTilesPerCol*8 || y_i < 0) {
            if (displayOverflow == 0) transparentPixel = true;
            else y_i&=noTilesPerCol*8 - 1;
        }

        if (!transparentPixel) {
            uint16_t rgb = getBGAffinePalette(gba, mapDataBase, tileDataBase, x_i, y_i, noTilesPerRow);

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
        y += BGPC;
    }

    /* Reset to initial coordinates */
    x = BGX;
    y = BGY;

    // dy
    x += BGPB;
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
    uint16_t BGCNT = readIO_internal(gba, BG0CNT+2*N, WIDTH_16);
    /* Extract character data and screen data base addresses 
     * as well as screen size */
    uint32_t mapDataBase = (BGCNT >> 8 & 0x1F) * 0x800;
    uint32_t tileDataBase = (BGCNT >> 2 & 0b11) * 0x4000;
    uint8_t screenSize = BGCNT >> 14 & 0b11;
    uint8_t displayOverflow = BGCNT >> 13 & 1;

    /* 16x16, 32x32, 64x64, 128x128 tile screen size options specified from BGCNT */
    uint16_t noTilesPerRow = 1 << (4+screenSize);
    uint16_t noTilesPerCol = noTilesPerRow;

    return computeBGRotScalScanline(gba, N, linebuffer, mapDataBase, tileDataBase, noTilesPerRow, noTilesPerCol, displayOverflow, getBGAffinePalette_M1_M2);
}



static bool computeBGTextScanline(GBA* gba, uint8_t N, uint16_t linebuffer[]) {
    /* This function computes the entire BG Text Mode scanline (VCOUNT) palette data given layer no.
     * (N) and loads rgb555 16 bit data per pixel (including transparency 
     * as bit 16) into the array specified by linebuffer. 
     * It also returns whether there exists a transparent pixel in the scanline */

    /* Extract BG registers from layer no. (N) */
    uint16_t BGCNT = readIO_internal(gba, BG0CNT+2*N, WIDTH_16);
    uint16_t BGHOFS = readIO_internal(gba, BG0HOFS+2*N, WIDTH_16);
    uint16_t BGVOFS = readIO_internal(gba, BG0VOFS+2*N, WIDTH_16);

    /* Identify Character(Tile) data base block and Screen (BG Map) base block 
     * and extract rest of the parameters from BGCNT 
     *
     * SC1-3 may or may not be used depending on screen size, but are pre calculated
     * By default map base is set to SC0 */ 

    uint32_t screen0Base = (BGCNT >> 8 & 0x1F)*0x800;
    uint32_t screen1Base = screen0Base + 0x800;
    uint32_t screen2Base = screen1Base + 0x800;
    uint32_t screen3Base = screen2Base + 0x800;

    uint32_t tileDataBase = (BGCNT >> 2 & 0b11)*0x4000;
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
        uint16_t tileEntry = readVRAM_16(gba, tileEntryAddress);

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
            if (tileStartAddress <= 0xFFFF) {
                if (colorDepth == PALETTE_256_1_8BIT) {
                    /* 8 bit colour depth - 256/1 palette - 64 bytes/tile */
                    pixelPalette = readVRAM_8(gba, rowStartAddress+j);
                    rgb = readPaletteRAM(gba, pixelPalette);
                } else {
                    /* 4 bit colour depth - 16/16 palette - 32 bytes/tile */
                    uint8_t paletteByte = readVRAM_8(gba, rowStartAddress+((j&(~1))/2));
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
            uint16_t CNT = readIO_internal(gba, BG0CNT+2*(i-8), WIDTH_16);
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

/* Stacking is done for BG Modes 0-2, for bitmap modes stacking is not needed as only BG2 is used
 * OBJ and BG layers are interleaved based on priority */

static void stackLayers(GBA* gba, bool exclude[], uint8_t mode[], uint16_t linebuffer[]) {
    uint8_t N = 0;  /* 0-3 */
    /* Fill linebuffer with transparent */
    repeatLoadFramebuffer(gba, (uint16_t)(1 << 15), linebuffer, 240);

    bool found = getHighestPriorityBG(gba, &N, exclude);
    if (!found) {
        /* No BG layer enabled or available */
        return;
    } 

    bool transparentPixelExists = false; 
    
    do {
        transparentPixelExists = false;

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

        /* Exclude last BG */
        exclude[N] = true;
        found = getHighestPriorityBG(gba, &N, exclude);

        /* Layers cannot be resolved anymore */
        if (!found) break;

    } while (transparentPixelExists);
    /* Transparent pixel exists, resolve by layering till there are no transparent pixels
     * or we have exhausted available layers */
}

static void renderTransparentScanline(GBA* gba) {
    /* Load a transparent scanline at current y in framebuffer */
    uint16_t* start = &gba->framebuffer[gba->IO[VCOUNT]*WIDTH_PX];
    repeatLoadFramebuffer(gba, (uint16_t)(1 << 15), start, 240);
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

    stackLayers(gba, exclude, mode, linebuffer);
    renderLinebuffer(gba, linebuffer);
}

static void renderBGMode1Scanline(GBA* gba) {
    /* BG Mode 1 - Hybrid - BG0-1 Text Mode and BG2 Rot/Scaling mode, BG3 not supported */

    bool exclude[] = {false, false, false, true};
    uint8_t mode[] = {1, 1, 0, 0};
    uint16_t linebuffer[240];

    stackLayers(gba, exclude, mode, linebuffer);
    renderLinebuffer(gba, linebuffer);
}

static void renderBGMode2Scanline(GBA* gba) {
    /* BG Mode 2 - Rot/Scaling only - BG2-3 Rot/Scaling and BG0-1 not supported */

    bool exclude[] = {true, true, false, false};
    uint8_t mode[] = {0, 0, 0, 0};
    uint16_t linebuffer[240];

    stackLayers(gba, exclude, mode, linebuffer);
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
        renderTransparentScanline(gba);
        return;
    }

    uint16_t linebuffer[240];

    computeBGRotScalScanline(gba, 2, linebuffer, 0, 0, 30, 20, 0, getBGAffinePalette_M3); 
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
        renderTransparentScanline(gba);
        return;
    }

	uint32_t base = (gba->latchedDISPCNT >> 4 & 1) ? 0xA000 : 0x0000;
    uint16_t linebuffer[240];

    computeBGRotScalScanline(gba, 2, linebuffer, base, 0, 30, 20, 0, getBGAffinePalette_M4);
    renderLinebuffer(gba, linebuffer);
}

static void renderBGMode5Scanline(GBA* gba) {
    /* 2 byte per colour direct bitmap just like mode 3,
     * but display size is reduced to 160x128. This allows us to have 2 frames
     * that are swapable like mode 4. */

    /* BG2 should be enabled */
    if ((gba->latchedDISPCNT >> 10 & 1) == 0) {
        renderTransparentScanline(gba);
        return;
    }

	uint32_t base = (gba->latchedDISPCNT >> 4 & 1) ? 0xA000 : 0x0000;
    uint16_t linebuffer[240];


    //printf("yR: %d|BGX: %d|BGY: %d\n", gba->IO[VCOUNT], gba->internalBG2X >> 8, gba->internalBG2Y >> 8);
    computeBGRotScalScanline(gba, 2, linebuffer, base, 0, 20, 16, 0, getBGAffinePalette_M5);
    renderLinebuffer(gba, linebuffer);
}

/* ------------------------------------------------------------------------------- */

void updateInternalBGNXY(GBA* gba, uint8_t N) {
    /* Update internal BGX and BGY affine counters for BG2 or BG3 from IO */

    uint32_t uBGX = readIO_internal(gba, (N&1) ? BG3X_L : BG2X_L, WIDTH_32);
    uint32_t uBGY = readIO_internal(gba, (N&1) ? BG3Y_L : BG2Y_L, WIDTH_32);

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
    uint16_t STAT = readIO_internal(gba, DISPSTAT, WIDTH_16);

	switch (gba->ppuVState) {
		case PPU_VDRAW: {
			if (gba->ppuHState == PPU_HDRAW) {	
				/* CPU has finished running through HDRAW, now render the entire scanline
				 * using latched DISPCNT values */
				if (gba->latchedDISPCNT >> 7 & 1) {
					renderTransparentScanline(gba);
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
							//printf("[WARNING] Invalid Video Mode %d, rendering transparent line\n", gba->videoMode);
							renderTransparentScanline(gba);
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
					writeIO_internal(gba, DISPSTAT, STAT | 0b100, WIDTH_16);
                    /* If vcounter IRQ enabled, then request interrupt */
                    if (STAT >> 5 & 1) requestInterrupt(gba, IRQ_LCD_VCOUNTER);
				} else writeIO_internal(gba, DISPSTAT, STAT & ~0b100, WIDTH_16);

				if (gba->IO[VCOUNT] == 160) {
					/* Enter VBLANK and render frame */
					gba->ppuVState = PPU_VBLANK;
					/* Set VBLANK STAT flag */
					writeIO_internal(gba, DISPSTAT, STAT | 1, WIDTH_16);
                    /* If VBLANK IRQ is enabled then request interrupt */
                    if (STAT >> 3 & 1) {
                        requestInterrupt(gba, IRQ_LCD_VBLANK);
                    }

					SDLEvents(gba);

                    /* Update texture with framebuffer and render it at the end of frame */
                    SDL_UpdateTexture(gba->SDL_Texture, NULL, &gba->framebuffer, WIDTH_PX*sizeof(uint16_t));

                    SDL_RenderClear(gba->SDL_Renderer);
                    SDL_RenderCopy(gba->SDL_Renderer, gba->SDL_Texture, NULL, NULL);
					SDL_RenderPresent(gba->SDL_Renderer);


                   
                    /*
                    uint64_t ticksCurrent = clock_u();
                    double diff = (1e6/60)-(ticksCurrent-gba->ticksAtLastFrame);
                    //gba->ticksAtLastFrame = ticksCurrent;
                    if (diff > 0) usleep(diff);

                    //printf("fps: %g|cycles: %lu\n", 1/((clock_u()-gba->ticksAtLastFrame)/1e6), gba->cycles);
                    gba->ticksAtLastFrame = ticksCurrent;
                    */
                    
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
					writeIO_internal(gba, DISPSTAT, STAT | 0b100, WIDTH_16);
				} else writeIO_internal(gba, DISPSTAT, STAT & ~0b100, WIDTH_16);

				/* Set HBLANK DISPSTAT flag (should be done later) */
				writeIO_internal(gba, DISPSTAT, STAT | 0b10, WIDTH_16);
				gba->ppuHState = PPU_HBLANK;
			} else if (gba->ppuHState == PPU_HBLANK) {
				gba->IO[VCOUNT]++;
				/* Set HDRAW, Clear HBLANK DISPSTAT flag */
				writeIO_internal(gba, DISPSTAT, STAT & ~0b10, WIDTH_16);
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
					writeIO_internal(gba, DISPSTAT, STAT & ~1, WIDTH_16);
				}
			}
			break;
		}
	}
}
