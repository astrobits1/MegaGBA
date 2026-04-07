#include <gba/gba.h>
#include <gba/renderer.h>
#include <SDL2/SDL.h>
#include <unistd.h>


#define TILE_DATA_BASE_TEXT     0x10000
#define TILE_DATA_BASE_BITMAP   0x14000

static inline void latchDISPCNT(GBA* gba) {
	uint16_t DISPCNT = readIO_internal(gba, DISPCNT, WIDTH_16);
    gba->latchedDISPCNT = DISPCNT;
}

static inline uint16_t readBGPaletteRAM(GBA* gba, uint8_t index) {
    /* Returns rgb555 - bit15 is always 0 */
    return gba->PaletteRAM[2*index] | ((gba->PaletteRAM[2*index+1] & ~(1<<7)) << 8);
}

static inline  uint16_t readSpritePaletteRAM(GBA* gba, uint8_t index) {
    /* Returns rgb555 - bit15 is always 0 */
    return gba->PaletteRAM[0x200+2*index] | ((gba->PaletteRAM[0x200+2*index+1] & ~(1<<7)) << 8);
}


static inline uint8_t readVRAM_8(GBA* gba, uint32_t address) {
    /* VRAM open bus */
    if (address > VRAM_96KB_END-VRAM_96KB) return 0;
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
    if (address > OAM_1KB_END-OAM_1KB) return 0;
    return gba->OAM[address];
}

static inline uint16_t readOAM_16(GBA* gba, uint32_t address) {
    return readOAM_8(gba, address) | (readOAM_8(gba, address+1) << 8);
}

static void repeatLoadFramebuffer(uint16_t rgb, uint16_t* start, uint32_t size) {
    /* Repeat an RG555 colour across framebuffer from a given start point for size no. of pixels */
    for (uint32_t i=0; i<size; i++) {
        start[i] = rgb;
    }
}

static uint16_t alphaBlend(uint16_t BGR1, uint16_t BGR2, uint8_t EVA, uint8_t EVB) {
    uint8_t R1 = BGR1 & 0x1F; uint8_t G1 = BGR1 >> 5 & 0x1F; uint8_t B1 = BGR1 >> 10 & 0x1F;
    uint8_t R2 = BGR2 & 0x1F; uint8_t G2 = BGR2 >> 5 & 0x1F; uint8_t B2 = BGR2 >> 10 & 0x1F;

    uint8_t R3 = (R1*EVA+R2*EVB)/16; uint8_t G3 = (G1*EVA+G2*EVB)/16; uint8_t B3 = (B1*EVA+B2*EVB)/16;

    if (R3>31) R3=31;
    if (G3>31) G3=31;
    if (B3>31) B3=31;

    uint16_t BGR3 = (B3 << 10) | (G3 << 5) | R3;
    return BGR3;
}

static uint16_t brightnessIncrease(uint16_t BGR1, uint8_t EVY) {
    uint8_t R1 = BGR1 & 0x1F; uint8_t G1 = BGR1 >> 5 & 0x1F; uint8_t B1 = BGR1 >> 10 & 0x1F;
    uint8_t R2 = R1+((31-R1)*EVY)/16;
    uint8_t G2 = G1+((31-G1)*EVY)/16;
    uint8_t B2 = B1+((31-B1)*EVY)/16;

    uint16_t BGR2 = (B2 << 10) | (G2 << 5) | R2;
    return BGR2;
}

static uint16_t brightnessDecrease(uint16_t BGR1, uint8_t EVY) {
    uint8_t R1 = BGR1 & 0x1F; uint8_t G1 = BGR1 >> 5 & 0x1F; uint8_t B1 = BGR1 >> 10 & 0x1F;
    uint8_t R2 = R1-(R1*EVY)/16;
    uint8_t G2 = G1-(G1*EVY)/16;
    uint8_t B2 = B1-(B1*EVY)/16;

    uint16_t BGR2 = (B2 << 10) | (G2 << 5) | R2;
    return BGR2;
}

/* --------------- Compositor --------------- */
static Compositor compositorNew() {
    Compositor c;
    c.layerCount = 0;
    c.headPointer = 0;
    c.basePointer = 0;
    
    return c;
}
static Layer compositorNewLayer(LAYER_TYPE type) {
    Layer layer;
    layer.type = type;
   
    /* Initialise buffer to transparent */
    repeatLoadFramebuffer((uint16_t)(1 << 15), layer.linebuffer, 240);

    return layer;
}

static void compositorPushTopLayer(Compositor* comp, Layer layer) {
    /* Push layer to compositor top
     * Head pointer points above the highest occupied cell */
    comp->layerStack[comp->headPointer++] = layer;
    comp->headPointer &= COMPOSITOR_STACK_SIZE-1;
    comp->layerCount++;
}

static void compositorPushBackLayer(Compositor* comp, Layer layer) {
    /* Push layer to compositor bottom
     * Base pointer points to the lowest unoccupied cell */
    comp->basePointer--;
    comp->basePointer &= COMPOSITOR_STACK_SIZE-1;
    comp->layerStack[comp->basePointer] = layer;
    comp->layerCount++;
}

static void compositorPushBackBackdrop(Compositor* comp, uint16_t rgb) {
    Layer backdrop = compositorNewLayer(LAYER_BACKDROP);
    repeatLoadFramebuffer(rgb, backdrop.linebuffer, 240);

    compositorPushBackLayer(comp, backdrop);
}

static Layer compositorGetLayer(Compositor* comp, uint8_t index) {
    /* 0 being the bottom and layerCount-1 being the top */
    return comp->layerStack[(comp->basePointer+index)&0xF];
}

static bool checkDoBrightness(Layer topLayer, uint16_t BCNT) {
    bool doBrightness = false;

    if (topLayer.type == LAYER_BACKDROP) {
        if (BCNT >> 5 & 1) doBrightness = true;
    } else if (topLayer.type == LAYER_SPRITE || topLayer.type == LAYER_SEMI_TRANSPARENT_SPRITE) {
        if (BCNT >> 4 & 1) doBrightness = true;
    } else {
        if (BCNT >> topLayer.type & 1) doBrightness = true;
    }

    return doBrightness;
}

static bool checkDoAlphaBlend(Compositor* comp, uint8_t topLayerIndex, uint8_t x, uint16_t BCNT, Layer* firstTargetLayer, Layer* secondTargetLayer) {
    /* Checks for a given top layer and X whether alpha blending can be done
     * by checking first and second target from BLDCNT. */
    Layer topLayer = compositorGetLayer(comp, topLayerIndex);
    bool firstTarget = false;
    bool secondTarget = false;

    if (topLayer.type == LAYER_SEMI_TRANSPARENT_SPRITE) {
        /* Force first target for semi transparent sprites regardless of BLDCNT */
        firstTarget = true;
    } else if (topLayer.type == LAYER_SPRITE) {
        if (BCNT >> 4 & 1) firstTarget = true;
    } else {
        if (BCNT >> topLayer.type & 1) firstTarget = true;
    }

    if (firstTarget) {
        /* First target confirmed, search for 2nd target */
        *firstTargetLayer = topLayer;

        for (uint8_t index = topLayerIndex-1; index >= 0; index--) {
            Layer layer = compositorGetLayer(comp, index);
            uint16_t pixel = layer.linebuffer[x];

            /* Non transparent pixel found (always will be) */
            if (!(pixel >> 15 & 1)) {
                if (layer.type == LAYER_SPRITE || layer.type == LAYER_SEMI_TRANSPARENT_SPRITE) {
                    /* Sprite-Sprite blending is forbidden */
                    if (topLayer.type != LAYER_SPRITE && topLayer.type != LAYER_SEMI_TRANSPARENT_SPRITE) {
                        if (BCNT >> 12 & 1) secondTarget = true;
                    }
                } else if (layer.type == LAYER_BACKDROP) {
                    if (BCNT >> 13 & 1) secondTarget = true;
                } else {
                    if (BCNT >> (8+layer.type) & 1) secondTarget = true;
                }

                *secondTargetLayer = layer;
                break;
            }
        }

        if (secondTarget) {
            return true;
        }
    }

    return false;
}


static void compositorMerge(Compositor* comp, uint16_t linebuffer[], uint16_t BCNT, uint16_t BALPHA, uint8_t BY) {
    /* Merges all layers in compositor and stores it in linebuffer,
     * Applies blending and brightness effects as implied in BLDCNT 
     * Backdrop is expected to be present in compositor */

    uint8_t sfxType = BCNT >> 6 & 0b11;
    /*
    printf("Compositor: BLDCNT: %04x\n", BCNT);
    for (int i=comp->layerCount-1; i>=0; i--) {
        Layer l = compositorGetLayer(comp, i);
        
        switch (l.type) {
            case LAYER_BG0: printf("BG 0\n"); break;
            case LAYER_BG1: printf("BG 1\n"); break;
            case LAYER_BG2: printf("BG 2\n"); break;
            case LAYER_BG3: printf("BG 3\n"); break;
            case LAYER_SPRITE: printf("SPRITE\n"); break;
            case LAYER_BACKDROP: printf("BACKDROP\n"); break;
        }
    }
    printf("\n");
    */

    for (int i=0; i<240; i++) {
        uint8_t topLayerIndex = -1;
        Layer topLayer;
        uint16_t topPixel;

        /* Find topmost non transparent pixel for certain x */
        for (uint8_t index = comp->layerCount-1; index>=0; index--) {
            Layer layer = compositorGetLayer(comp, index);
            uint16_t pixel = layer.linebuffer[i];

            /* Pixel not transparent, break */
            if (!(pixel >> 15 & 1)) {
                topLayerIndex = index;
                topPixel = pixel;
                topLayer = layer;
                break;
            }
        }


        /* Check and apply special effects */
        if (sfxType == 2) {
            /* Brightness Increase */
            bool doBrightness = checkDoBrightness(topLayer, BCNT);
            if (doBrightness) {
                uint8_t EVY = BY & 0x1F;

                if (EVY > 16) EVY = 16;

                uint16_t BGR1 = topPixel;
                uint16_t BGR2 = brightnessIncrease(BGR1, EVY);

                topPixel = BGR2;
            }
        } else if (sfxType == 3) {
            /* Brightness Decrease */
            bool doBrightness = checkDoBrightness(topLayer, BCNT);
            if (doBrightness) {
                uint8_t EVY = BY & 0x1F;

                if (EVY > 16) EVY = 16;

                uint16_t BGR1 = topPixel;
                uint16_t BGR2 = brightnessDecrease(BGR1, EVY);

                topPixel = BGR2;
            }

        }

        /* Alpha blending is applied over brightness for semi transparent sprites, 
         * and only the original pixel is used for first target */
        if (topLayerIndex > 0 && (sfxType == 1 || topLayer.type == LAYER_SEMI_TRANSPARENT_SPRITE)) {
            /* Alpha Blending */
            Layer firstTargetLayer, secondTargetLayer;
            bool doAlphaBlending = checkDoAlphaBlend(comp, topLayerIndex, i, BCNT, &firstTargetLayer, &secondTargetLayer);

            if (doAlphaBlending) {
                /* Blend confirmed, blend first target and second target
                 * and store in topPixel */
                uint8_t EVA = BALPHA & 0x1F;
                uint8_t EVB = BALPHA >> 8 & 0x1F;

                if (EVA > 16) EVA = 16;
                if (EVB > 16) EVB = 16;

                uint16_t BGR1 = firstTargetLayer.linebuffer[i];
                uint16_t BGR2 = secondTargetLayer.linebuffer[i];
                uint16_t BGR3 = alphaBlend(BGR1, BGR2, EVA, EVB);

                topPixel = BGR3;
            }
        }

        /* Finally write pixel to buffer */
        linebuffer[i] = topPixel;
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

    //printf("y rendering: %d\n", y_rendering);
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
    uint8_t y_tile = y_objInternal & 0b111;

    if (mappingType == OBJ_VRAM_MAPPING_1DIM) {
        /* 1 dimensional indexing */
        uint32_t offset = ((tileIndex+2*width*noTileRowsBefore+2*tileInRow)&0x3FF)*32;
        uint32_t internalYOffset = y_tile*8;

        return readVRAM_64(gba, tileDataBase + offset + internalYOffset);
    } else {
        /* 2 dimensional indexing *
         * Bit 0 is ignored */
        tileIndex &= ~1;
        uint32_t offset = ((tileIndex+noTileRowsBefore*0x20+2*tileInRow)&0x3FF)*32;
        uint32_t internalYOffset = y_tile*8;

        return readVRAM_64(gba, tileDataBase + offset + internalYOffset); 
    }
}

static uint32_t getSpriteRowData_4bit(GBA* gba, uint8_t mappingType, uint32_t tileDataBase, uint16_t tileIndex, uint16_t width, uint8_t tileInRow, uint8_t y_objInternal) {
    /* Returns tile data row for given parameters in 4 bit 16/16 palette indexing mode */
    uint8_t noTileRowsBefore = (y_objInternal & ~0b111)/8;
    uint8_t y_tile = y_objInternal & 0b111;
    /* 32 bytes per tile */

    if (mappingType == OBJ_VRAM_MAPPING_1DIM) {
        /* 1 dimensional indexing */
        uint32_t offset = ((tileIndex+width*noTileRowsBefore+tileInRow)&0x3FF)*32;
        uint32_t internalYOffset = y_tile*4;

        return readVRAM_32(gba, tileDataBase + offset + internalYOffset);
    } else {
        /* 2 dimensional indexing */
        uint32_t offset = ((tileIndex+noTileRowsBefore*0x20+tileInRow)&0x3FF)*32;
        uint32_t internalYOffset = y_tile*4;

        return readVRAM_32(gba, tileDataBase + offset + internalYOffset); 
    }
}


static void computeSpriteNormalScanline(GBA* gba, uint16_t linebuffer[], uint16_t semiTransparentLayerMask[], uint8_t height, uint8_t width, uint8_t y_objInternal, uint16_t x_obj, uint16_t attr0, uint16_t attr1, uint16_t attr2, uint32_t tileDataBase, uint8_t vramMapping) {
    /* Compute a normal non-affine sprite */

    uint8_t mode = attr0 >> 10 & 0b11;
    uint8_t hFlip = attr1 >> 12 & 1;
    uint8_t vFlip = attr1 >> 13 & 1;
    uint16_t tileIndex = attr2 & 0x3FF;
    uint8_t paletteNum = attr2 >> 12 & 0xF;         /* For 16/16 palettes only */
    uint8_t paletteMode = attr0 >> 13 & 1;          /* 256/1 or 16/16 */
    bool completed = false;

    uint8_t noTileRowsBefore = (y_objInternal & ~0b111)/8;
    uint8_t startTile = 0;
    uint8_t startPixel = 0;

    if (x_obj >= 240) {
        /* Some wrap around is happening as we are still in horizontal bounds */
        uint16_t noPixelsToSkip = 512-x_obj;
        startTile = (noPixelsToSkip & ~0b111)/8;
        startPixel = noPixelsToSkip & 0b111;
    }

    for (int tile=startTile; tile<width; tile++) {
        uint64_t rowData;
        uint8_t yInternal = vFlip ? (height*8-1)-y_objInternal : y_objInternal;

        if (paletteMode == PALETTE_256_1_8BIT) {
            rowData = getSpriteRowData_8bit(gba, vramMapping, tileDataBase, tileIndex, width, hFlip ? (width-1)-tile : tile, yInternal);                        
        } else {
            rowData = getSpriteRowData_4bit(gba, vramMapping, tileDataBase, tileIndex, width, hFlip ? (width-1)-tile : tile, yInternal); 
        }

        for (int xInternal = tile==startTile ? startPixel:0; xInternal<8; xInternal++) {
            uint8_t xReal = (x_obj>=240?0:x_obj)+(tile-startTile)*8+xInternal-startPixel;
            uint8_t paletteIndex;
            uint16_t rgb;

            if (paletteMode == PALETTE_256_1_8BIT) {
                paletteIndex = rowData>>((hFlip?(7-xInternal):xInternal)*8)&0xFF;
                rgb = readSpritePaletteRAM(gba, paletteIndex);
            } else {
                paletteIndex = rowData>>((hFlip?(7-xInternal):xInternal)*4)&0xF;
                rgb = readSpritePaletteRAM(gba, 16*paletteNum+paletteIndex);
           }

            //printf("xReal: %d|p: %d||pn: %d|c: %04x\n", xReal, paletteIndex, paletteNum, rgb);

            if (paletteIndex != 0) {
                /* If palette is not transparent then overlay,
                 * otherwise previous palette remains 
                 *
                 * Handle semi transparent case by filling out mask
                 * as this will be used for layer separation */
                if (mode == 1) {
                    semiTransparentLayerMask[xReal] = rgb;
                } else {
                    linebuffer[xReal] = rgb;
                    semiTransparentLayerMask[xReal] = 1 << 15;
                }
            }

            if (xReal == WIDTH_PX-1) {completed = true; break;}
        }
        
        if (completed) break;
    }
}

static void computeSpriteRotScalScanline(GBA* gba, uint16_t linebuffer[], uint16_t semiTransparentLayerMask[], uint8_t height, uint8_t width, uint8_t y_objInternal, uint16_t x_obj, uint16_t attr0, uint16_t attr1, uint16_t attr2, uint32_t tileDataBase, uint8_t vramMapping) {
    uint8_t mode = attr0 >> 10 & 0b11;
    uint16_t tileIndex = attr2 & 0x3FF;
    uint8_t paletteNum = attr2 >> 12 & 0xF;         /* For 16/16 palettes only */
    uint8_t paletteMode = attr0 >> 13 & 1;          /* 256/1 or 16/16 */

    uint8_t doubleSize = attr0 >> 9 & 1;
    uint8_t affineParameterGroup = attr1 >> 9 & 0x1F;

    /* Load affine parameters */
    int16_t PA = (int16_t)readOAM_16(gba, affineParameterGroup*0x20+0x6);
    int16_t PB = (int16_t)readOAM_16(gba, affineParameterGroup*0x20+0xE);
    int16_t PC = (int16_t)readOAM_16(gba, affineParameterGroup*0x20+0x16);
    int16_t PD = (int16_t)readOAM_16(gba, affineParameterGroup*0x20+0x1E);

    /* Affine map origin is top left corner and we instantaneously calculate effect of past scanlines
     * for Y jump (PB and PD) as well as relative affine transformation. 
     * The center of rotation is the center of sprite */

    int32_t x, y;

    if (doubleSize) {
        x = PA*(-width*4) + PB*(y_objInternal-height*4) + (width*2<<8);
        y = PC*(-width*4) + PD*(y_objInternal-height*4) + (height*2<<8);
    } else {
        x = PA*(-width*4) + PB*(y_objInternal-height*4) + (width*4<<8);
        y = PC*(-width*4) + PD*(y_objInternal-height*4) + (height*4<<8);
    }
  
    /* Render whole sprite to a buffer first, then sort out visible parts before
     * copying to final linebuffer */
    uint16_t spritebuffer[width*8];
    repeatLoadFramebuffer((uint16_t)(1<<15), spritebuffer, width*8);

    for (uint16_t xReal=0; xReal<width*8; xReal++) {
        /* x and y represent internal sprite coordinates with origin at top left */
        int32_t x_i = x >> 8;
        int32_t y_i = y >> 8;
        bool transparentPixel = false;

        /* Handle x or y overflow taking double size into consideration
         * as sprite map size doesnt really change */
        if (x_i >= width*(doubleSize?4:8) || x_i < 0) {
            transparentPixel = true;
        }

        if (y_i >= height*(doubleSize?4:8) || y_i < 0) {
            transparentPixel = true;
        }

        if (!transparentPixel) {
            uint8_t paletteIndex = 0;
            uint16_t rgb = 0;
            if (paletteMode == 1) {
                /* 8 bit - 256/1 */
                uint64_t row = getSpriteRowData_8bit(gba, vramMapping, tileDataBase, tileIndex, doubleSize?width/2:width, x_i>>3, y_i);
                paletteIndex = row >> (8*(x_i & 0b111)) & 0xFF;
                rgb = readSpritePaletteRAM(gba, paletteIndex);
            } else {
                /* 4 bit - 16/16 */
                uint32_t row = getSpriteRowData_4bit(gba, vramMapping, tileDataBase, tileIndex, doubleSize?width/2:width, x_i>>3, y_i);
                paletteIndex = row >> (4*(x_i & 0b111)) & 0xF;
                rgb = readSpritePaletteRAM(gba, paletteNum*16+paletteIndex);
            }

            /* Pixel has been read */
            if (paletteIndex != 0) {
                spritebuffer[xReal] = rgb;
            }
        }

        x = (x_i<<8) | (x&0xFF);
        x += PA;

        y = (y_i<<8) | (y&0xFF);
        y += PC;
    }

    /* Sprite rendering complete, now copy visible parts to main buffer */
    if (x_obj >= 240) {
        uint16_t noPixelsToSkip = 512-x_obj;

        for (uint16_t i=0; i<width*8-noPixelsToSkip; i++) {
            if (!(spritebuffer[noPixelsToSkip+i] >> 15 & 1)) {
                if (mode == 1) {
                    /* Semi-Transparent */
                    semiTransparentLayerMask[i] = spritebuffer[noPixelsToSkip+i];
                } else {
                    /* Normal */
                    linebuffer[i] = spritebuffer[noPixelsToSkip+i];
                    semiTransparentLayerMask[i] = 1 << 15;
                }
            }
        }
        //memcpy(linebuffer, &spritebuffer[noPixelsToSkip], sizeof(uint16_t)*(width*8-noPixelsToSkip));
    } else {
        uint16_t noPixelsToCopy = width*8;

        if (x_obj + width*8 > 240) {
            /* Cutoff near the end */
            noPixelsToCopy -= x_obj + width*8 - 240;
        }

        for (uint16_t i=0; i<noPixelsToCopy; i++) {
            if (!(spritebuffer[i] >> 15 & 1)) {
                if (mode == 1) {
                    /* Semi-Transparent */
                    semiTransparentLayerMask[x_obj+i] = spritebuffer[i];
                } else {
                    /* Normal */
                    linebuffer[x_obj+i] = spritebuffer[i];
                    semiTransparentLayerMask[x_obj+i] = 1 << 15;
                }
            }
        }

        //memcpy(&linebuffer[x_obj], spritebuffer, sizeof(uint16_t)*noPixelsToCopy);
    }
}

static bool computeSpriteScanline(GBA* gba, uint16_t linebuffer[], uint16_t semiTransparentLayerMask[], bool* foundSemiTransparent, uint8_t minPriority, uint8_t maxPriority, uint32_t tileDataBase) {
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

    /* Semi-Transparent sprite pixels must be separated out into their own layer,
     * and the primary sprite layer must be made transparent in those zones.
     * We use this mask buffer to keep track of those pixels */

    for (int p=minPriority; p>=maxPriority; p--) {
        //printf("BG Priority: %d\n", p);
        uint16_t currentlinebuffer[240];
        /* Set all palettes to transparent by default */
        repeatLoadFramebuffer(1<<15, currentlinebuffer, 240);
        
        for (int i=127; i>=0; i--) {
            //printf("OAM index: %d\n", i);
            /* Scan OAM for sprites of a certain BG Priority */
            uint32_t oamAddress = i*8;

            uint16_t attr0 = readOAM_16(gba, i*8);
            uint16_t attr1 = readOAM_16(gba, i*8+2);
            uint16_t attr2 = readOAM_16(gba, i*8+4);

            /* Not same BG Priority */
            if ((attr2 >> 10 & 0b11) != p) continue;

            /* Other OBJ modes are not supported for now */
            uint8_t mode = attr0 >> 10 & 0b11;
            if (mode >= 2) continue;

            uint8_t y_obj = attr0 & 0xFF;
            uint8_t shape = attr0 >> 14 & 0b11;
            uint8_t rotationAndScaling = attr0 >> 8 & 1;
            uint8_t doubleSize = attr0 >> 9 & 1;

            uint16_t x_obj = attr1 & 0x1FF;
            uint8_t size = attr1 >> 14 & 0b11;

            uint8_t height, width, y_objInternal;
            getSpriteDimensions(size, shape, &width, &height); 

            if (rotationAndScaling) {
                /* Rotation and Scaling */
                if (doubleSize) {
                    height *= 2;
                    width *= 2;
                }
                bool inBounds = checkSpriteVisibility(gba, height, width, x_obj, y_obj, &y_objInternal);
                if (!inBounds) continue;

                spriteRendered = true;
                computeSpriteRotScalScanline(gba, currentlinebuffer, semiTransparentLayerMask, height, width, y_objInternal, x_obj, attr0, attr1, attr2, tileDataBase, vramMapping);
            } else {
                /* Normal Mode */
                /* OBJ disabled */
                if ((attr0 >> 9 & 1)) continue;

                bool inBounds = checkSpriteVisibility(gba, height, width, x_obj, y_obj, &y_objInternal);
                if (!inBounds) continue;

                /* This sprite must be rendered, and we know its exact internal Y */
                spriteRendered = true;
                computeSpriteNormalScanline(gba, currentlinebuffer, semiTransparentLayerMask, height, width, y_objInternal, x_obj, attr0, attr1, attr2, tileDataBase, vramMapping);
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

    /* 'Hollow' out main linebuffer in places where semi transparent layer separates out */
    *foundSemiTransparent = false;
    for (int i=0; i<240; i++) {
        uint16_t semi = semiTransparentLayerMask[i];

        if (!(semi >> 15 & 1)) {
            /* Atleast 1 non transparent pixel in semi transparent layer */
            *foundSemiTransparent = true;
            linebuffer[i] = 1 << 15;
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
    uint16_t rgb = readBGPaletteRAM(gba, pixelPalette);

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
    uint16_t rgb = readBGPaletteRAM(gba, index);

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
    uint16_t BGHOFS = readIO_internal(gba, BG0HOFS+4*N, WIDTH_16);
    uint16_t BGVOFS = readIO_internal(gba, BG0VOFS+4*N, WIDTH_16);


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

    //printf("yR: %d | y: %d | HOFS: %d | VOFS: %d\n", yReal, y, BGHOFS, BGVOFS);
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
        //printf("tN:%d|", tileNumber);
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
                    rgb = readBGPaletteRAM(gba, pixelPalette);
                } else {
                    /* 4 bit colour depth - 16/16 palette - 32 bytes/tile */
                    uint8_t paletteByte = readVRAM_8(gba, rowStartAddress+((j&(~1))/2));
                    pixelPalette = j&1 ? paletteByte >> 4 : paletteByte & 0xF;
                    rgb = readBGPaletteRAM(gba, 16*paletteNum+pixelPalette);
                }
            } else {
                /* If tile is located in OBJ space for mode 0/1/2 then BG reads fail
                 * completely and we read a transparent tile (quirk) */
                pixelPalette = 0;
                rgb = 0;
            }

            /* Offset by startPixel for every tile for framebuffer loading */
            uint8_t xReal = (i-startTile)*8 + (hFlip ? (7-j) : j) - startPixel;

            //printf("xR: %d | i: %d | j: %d | sT: %d | sP: %d | f: %d | HOFS: %d | VOFS: %d\n", xReal, i, j, startTile, startPixel, hFlip, BGHOFS, BGVOFS);
            /* Make sure hFlip is handled correctly with horizontal scrolling
             * on the first tile. We skip some iterations either at the start or end
             * depending on whether we do hFlip or not */
            if (i==startTile && startPixel > 0) {
                if (hFlip) {
                    if (j > (7-startPixel)) continue;
                } else {
                    if (j < startPixel) continue;
                }
                /* When rendering is being done in reverse for flipped last tile
                 * skip xReal >= 240 */
            } else if (xReal >= 240) {
                continue;
            }

            if (pixelPalette == 0) {
                /* Transparent pixel is marked by setting bit 15 in the colour */
                rgb |= 1 << 15;
                transparentPixelExists = true;
            }

            linebuffer[xReal] = rgb;

            /* No need to render the last tile fully if horizontal scrolling % 8 != 0,
                * mark stop when the buffer is full */
            if (xReal == WIDTH_PX-1) {
                flag = false;
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

    //printf("\n");
    return transparentPixelExists;
}

static bool getHighestPriorityBG(GBA* gba, uint8_t* N, bool exclude[], uint8_t* priority) {
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

    *priority = lowestPrio;
    return true;
}

/* Stacking is done for BG Modes 0-2 in this function
 * OBJ and BG layers are interleaved based on priority and pushed to compositor
 * Then compositor merges and applies SFX */

static void stackLayers(GBA* gba, bool exclude[], uint8_t mode[], uint16_t linebuffer[]) {
    uint8_t N = 0;  /* 0-3 */
    uint8_t priority = 3;   /* 0-3 */
    bool spritesEnabled = (bool)(gba->latchedDISPCNT >> 12 & 1);
    //spritesEnabled = false;
    int8_t spritePriorityRendered = -1;     /* Not rendered yet */

    /* For mode 0-2 */
    Compositor comp = compositorNew();

    bool found = getHighestPriorityBG(gba, &N, exclude, &priority);
    if (found) { 
        do {
            Layer bgLayer = compositorNewLayer(N);

            if (mode[N] == 1) {
                /* Text mode */
                computeBGTextScanline(gba, N, bgLayer.linebuffer);
            } else {
                /* Rotation/Scaling mode */
                computeBGRotScalScanline_M1_M2(gba, N, bgLayer.linebuffer);
            } 

            /* Exclude BG we just computed */
            exclude[N] = true;
            uint8_t oldPriority = priority;
            /* Find next BG */
            found = getHighestPriorityBG(gba, &N, exclude, &priority);

            /* BG Layers cannot be resolved anymore */
            if (spritesEnabled && (oldPriority != priority || !found)) {
                /* Shift to a lower priority occured or BG layers were exhausted, 
                 * stack sprite layers equal to or higher than
                 * the old priority and merge it with final BG before starting next priority
                 * composition */
                /* Every layer higher than spritePriorityRendered has been rendered 
                 * max and min can be same */
                uint8_t max = spritePriorityRendered+1;
                uint8_t min = oldPriority;

                /* Overwrite currentlinebuffer */
                Layer spriteLayer = compositorNewLayer(LAYER_SPRITE);
                Layer semiSpriteLayer = compositorNewLayer(LAYER_SEMI_TRANSPARENT_SPRITE);
                bool foundSemiTransparent = false;

                bool found = computeSpriteScanline(gba, spriteLayer.linebuffer, semiSpriteLayer.linebuffer, &foundSemiTransparent, min, max, TILE_DATA_BASE_TEXT);

                if (found) {
                    /* BG hasnt been pushed yet */
                    if (foundSemiTransparent) {
                        compositorPushBackLayer(&comp, semiSpriteLayer);
                    }
                    compositorPushBackLayer(&comp, spriteLayer);
                }

                spritePriorityRendered = oldPriority;
            }

            compositorPushBackLayer(&comp, bgLayer);
        } while (found);

        if (spritesEnabled && spritePriorityRendered < 3) {
            /* BG layers have been exhausted, loop will quit at the end of this 
             * iteration. Resolve any underlying sprite layers only if they are available */
            Layer spriteLayer = compositorNewLayer(LAYER_SPRITE);
            Layer semiSpriteLayer = compositorNewLayer(LAYER_SEMI_TRANSPARENT_SPRITE);
            bool foundSemiTransparent = false;

            bool found = computeSpriteScanline(gba, spriteLayer.linebuffer, semiSpriteLayer.linebuffer, &foundSemiTransparent, 3, spritePriorityRendered+1, TILE_DATA_BASE_TEXT);

            if (found) {
                if (foundSemiTransparent) {
                    compositorPushBackLayer(&comp, semiSpriteLayer);
                }
                compositorPushBackLayer(&comp, spriteLayer);
            }
        } 
    } else {
        /* No BG layer enabled, stack all sprites */
        if (spritesEnabled) {
            Layer spriteLayer = compositorNewLayer(LAYER_SPRITE);
            Layer semiSpriteLayer = compositorNewLayer(LAYER_SEMI_TRANSPARENT_SPRITE);
            bool foundSemiTransparent = false;

            bool found = computeSpriteScanline(gba, spriteLayer.linebuffer, semiSpriteLayer.linebuffer, &foundSemiTransparent, 3, 0, TILE_DATA_BASE_TEXT);

            if (found) {
                if (foundSemiTransparent) {
                    compositorPushBackLayer(&comp, semiSpriteLayer);
                }
                compositorPushBackLayer(&comp, spriteLayer);
            }
        }
    }

    /* At this point, either all background and sprite layers have been resolved
     * Add a final backdrop layer at the very bottom */
    compositorPushBackBackdrop(&comp, readBGPaletteRAM(gba, 0));

    /* Compositor is ready to be merged, with alpha blending and brightness effects */
    uint16_t BCNT = readIO_internal(gba, BLDCNT, WIDTH_16);
    uint16_t BALPHA = readIO_internal(gba, BLDALPHA, WIDTH_16);
    uint8_t BY = readIO_internal(gba, BLDY, WIDTH_8);

    compositorMerge(&comp, linebuffer, BCNT, BALPHA, BY);

    /* Stacked linebuffer is ready */
}

/* Stacking is done for bitmap modes 3-5 in this function, only BG2 is used along with sprites.
 * Layers are filled into compositor starting from mode specific properties and then merged
 * along with SFX */

static void stackLayersBitmap(GBA* gba, uint16_t linebuffer[], uint32_t mapDataBase, uint16_t noTilesPerRow, uint16_t noTilesPerCol, uint16_t (*getBGAffinePalette)(GBA*, uint32_t, uint32_t, int32_t, int32_t, uint16_t)) {
    bool BGEnabled = gba->latchedDISPCNT >> 10 & 1;
    bool spriteEnabled = gba->latchedDISPCNT >> 12 & 1;
    uint8_t spritePriorityMin = 3;

    Compositor comp = compositorNew();

    if (BGEnabled) {
        Layer bgLayer = compositorNewLayer(LAYER_BG2);
        computeBGRotScalScanline(gba, 2, bgLayer.linebuffer, mapDataBase, 0, noTilesPerRow, noTilesPerCol, 0, getBGAffinePalette);
        compositorPushBackLayer(&comp, bgLayer);

        uint16_t BGCNT = readIO_internal(gba, BG2CNT, WIDTH_16);
        spritePriorityMin = BGCNT & 0b11;
    }

    if (spriteEnabled) {
        Layer spriteLayer = compositorNewLayer(LAYER_SPRITE);
        Layer semiSpriteLayer = compositorNewLayer(LAYER_SEMI_TRANSPARENT_SPRITE);
        bool foundSemiTransparent = false;

        bool found = computeSpriteScanline(gba, spriteLayer.linebuffer, semiSpriteLayer.linebuffer, &foundSemiTransparent, spritePriorityMin, 0, TILE_DATA_BASE_BITMAP);

        if (found) {
            if (foundSemiTransparent) {
                compositorPushTopLayer(&comp, semiSpriteLayer);
            }
            compositorPushTopLayer(&comp, spriteLayer);
        }
    }

    compositorPushBackBackdrop(&comp, readBGPaletteRAM(gba, 0));

    /* Compositor is ready to be merged, with alpha blending and brightness effects */
    uint16_t BCNT = readIO_internal(gba, BLDCNT, WIDTH_16);
    uint16_t BALPHA = readIO_internal(gba, BLDALPHA, WIDTH_16);
    uint8_t BY = readIO_internal(gba, BLDY, WIDTH_8);

    compositorMerge(&comp, linebuffer, BCNT, BALPHA, BY);
}

static void renderTransparentScanline(GBA* gba) {
    /* Load a transparent scanline at current y in framebuffer */
    uint16_t* start = &gba->framebuffer[gba->IO[VCOUNT]*WIDTH_PX];
    repeatLoadFramebuffer((uint16_t)(1 << 15), start, 240);
}

static void renderLinebuffer(GBA* gba, uint16_t linebuffer[]) {
    /* Proceed to rendering the final composite BG linebuffer */

    for (int x=0; x<240; x++) {
        uint16_t rgb = linebuffer[x];

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
    uint16_t linebuffer[240];

    stackLayersBitmap(gba, linebuffer, 0x0000, 30, 20, getBGAffinePalette_M3);
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


    uint16_t linebuffer[240];
	uint32_t base = (gba->latchedDISPCNT >> 4 & 1) ? 0xA000 : 0x0000;

    stackLayersBitmap(gba, linebuffer, base, 30, 20, getBGAffinePalette_M4);
    renderLinebuffer(gba, linebuffer);
}

static void renderBGMode5Scanline(GBA* gba) {
    /* 2 byte per colour direct bitmap just like mode 3,
     * but display size is reduced to 160x128. This allows us to have 2 frames
     * that are swapable like mode 4. */

    /* BG2 should be enabled */

    uint16_t linebuffer[240];
	uint32_t base = (gba->latchedDISPCNT >> 4 & 1) ? 0xA000 : 0x0000;

    stackLayersBitmap(gba, linebuffer, base, 20, 16, getBGAffinePalette_M5);
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

    /* Schedule the first PPU sync */
    GBAEvent e;
    e.scheduledFor = 960;
    e.type = EVENT_PPU;

    pushEvent(gba, e);
}

static void schedulePPU(GBA* gba, uint64_t scheduledFor) {
    /* Schedule next PPU call at HBLANK/HDRAW end from now */
    GBAEvent e;
    e.scheduledFor = gba->cycles+scheduledFor;
    e.type = EVENT_PPU;

    pushEvent(gba, e);
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

                /* Check if one or more enabled DMA is waiting to be started at HBLANK */
                for (int i=0; i<4; i++) {
                    uint16_t CNT_H = readIO_internal(gba, DMA0CNT_H+i*0xC, WIDTH_16);
                    if ((CNT_H >> 15 & 1) && (CNT_H >> 12 & 0b11) == 2) {
                        startDMA(gba, i);
                    }
                }
                /* Schedule next PPU call at HBLANK end 272 cycles from now */
                schedulePPU(gba, 272);
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

                    /* Check if one or more enabled DMA is waiting to be started at VBLANK */
                    for (int i=0; i<4; i++) {
                        uint16_t CNT_H = readIO_internal(gba, DMA0CNT_H+i*0xC, WIDTH_16);
                        if ((CNT_H >> 15 & 1) && (CNT_H >> 12 & 0b11) == 1) {
                            startDMA(gba, i);
                        }
                    }

					SDLEvents(gba);

                    /* Update texture with framebuffer and render it at the end of frame */
                    SDL_UpdateTexture(gba->SDL_Texture, NULL, &gba->framebuffer, WIDTH_PX*sizeof(uint16_t));

                    SDL_RenderClear(gba->SDL_Renderer);
                    SDL_RenderCopy(gba->SDL_Renderer, gba->SDL_Texture, NULL, NULL);
					SDL_RenderPresent(gba->SDL_Renderer);


                   
                    uint64_t ticksCurrent = clock_u();
                    double diff = (1e6/60)-(ticksCurrent-gba->ticksAtLastFrame);
                    //gba->ticksAtLastFrame = ticksCurrent;
                    if (diff > 0) usleep(diff);

                    //printf("fps: %g|cycles: %lu\n", 1/((clock_u()-gba->ticksAtLastFrame)/1e6), gba->cycles);
                    gba->ticksAtLastFrame = ticksCurrent;
                    
				} else {
					/* Latch DISPCNT if not entering VBLANK */
					latchDISPCNT(gba);
				}

                schedulePPU(gba, 960);
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

                /* Request HBLANK interrupt if enabled */
                if (STAT >> 4 & 1) requestInterrupt(gba, IRQ_LCD_HBLANK);

                /* HBLANK DMA not run during VBLANK */
                schedulePPU(gba, 272);
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
                
                schedulePPU(gba, 960);
			}
			break;
		}
	}
}
