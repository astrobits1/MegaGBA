#include <gba/gba.h>
#include <gba/arm7tdmi.h>
#include <gba/gamepak.h>
#include <gba/debugGBA.h>
#include <gba/renderer.h>
#include <string.h>
#include <stdio.h>

static void reloadDMAInternal(GBA* gba, uint8_t N);
static void stepDMA(GBA* gba, uint8_t N);

static void handleEvents(GBA* gba);

/* ----------------------------------------------------- */

static void initialiseIO(GBA* gba) {
    /* PPU */
    /* DISCNT, DISPSTAT, VCOUNT, BGNCNT are all 0 */

    /* Initialise affine matrix to identity, so even if they are not used
     * things render normally */
    writeIO_internal(gba, BG2PA, 1 << 8, WIDTH_16);
    writeIO_internal(gba, BG3PA, 1 << 8, WIDTH_16);
    writeIO_internal(gba, BG2PD, 1 << 8, WIDTH_16);
    writeIO_internal(gba, BG3PD, 1 << 8, WIDTH_16);

    /* Keypad */

    /* KEYINPUT - Set all key states to released */
    writeIO_internal(gba, KEYINPUT, 0xFFFF, WIDTH_16);
}


/* ----------------------------------------------------- */

void initialiseGBA(GBA* gba, GamePak gamepak, uint8_t* biosBuffer, size_t biosSize) {
	gba->gamepak = gamepak;
	gba->runningStepFrame = false;
    gba->cycles = 0;

    /* Initialise all DMA related arrays of internal registers to 0 for DMA0-3 */
    gba->dmaInProgressMaster = false;
    memset(&gba->dmaInProgress, 0, 4*(sizeof(bool)+3*sizeof(uint32_t)));

    /* Initialise scheduler */
    gba->eventBasePointer = 0;
    gba->eventHeadPointer = 0;
    gba->eventCount = 0;

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


#ifdef DEBUG_ENABLED
	initDissembler();
#endif
}

void freeGBA(GBA* gba) {
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

    gba->runningStepFrame = false;
}

void stepGBAFrame(GBA* gba) {
	gba->runningStepFrame = true;

	while (gba->runningStepFrame) {
        if (gba->dmaInProgressMaster) {
            uint8_t N = 0;
            for (int i=0; i<4; i++) {
                if (gba->dmaInProgress[i]) {
                    N = i;
                    break;
                }
            }

            stepDMA(gba, N);
        } else {
			stepCPU(gba);
        }

        /* Handle scheduler events at CPU instruction boundary or DMA step boundary */
        handleEvents(gba);	
	}
}

void keyinputSet(GBA* gba, KEYINPUT_CODE code) {
    writeIO_internal(gba, KEYINPUT, readIO_internal(gba, KEYINPUT, WIDTH_16) | (1 << code), WIDTH_16);
}

void keyinputReset(GBA* gba, KEYINPUT_CODE code) {
    writeIO_internal(gba, KEYINPUT, readIO_internal(gba, KEYINPUT, WIDTH_16) & ~(1 << code), WIDTH_16);
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

static uint32_t readMem(GBA* gba, uint8_t* ptr, uint8_t size) {
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
            littleEndian32Encode(ptr, data); 
            return;
        }
		case WIDTH_16: {
            littleEndian16Encode(ptr, data); 
            return;
        }
		case WIDTH_8: {
            *ptr = (uint8_t)data;
            return;
        }
	}

}



/* busRead and busWrite are not completely fullproof, you could for example
 * read from write only memory or write to read only memory if you positioned a 16/32bit read/write
 * at the right place. Only first addresses are checked.To prevent this a more thorough 
 * checking is needed which resolves every byte individually
 *
 * Open bus is yet to be emulated */

uint32_t busRead(GBA* gba, uint32_t address, uint8_t size) {
    uint8_t* ptr = NULL;

    gba->cycles++;

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

		if (relativeAddress > (gba->gamepak.size - 1)) {
			// printf("[WARNING] Read attempt from gamepak to an invalid address %08x\n", address);
			return 0;
		}

		ptr = &gba->gamepak.allocated[relativeAddress];
		
	} else if (address >= INT_WRAM_32KB && address <= INT_WRAM_32KB_END) {
		/* Read from internal work RAM */
		ptr = &gba->IWRAM[address - INT_WRAM_32KB];
    } else if (address >= 0x03FFFF00 && address <= 0x03FFFFFF) {
        /* Mirror of IWRAM 0x03007F00 - 0x03007FFF */
        ptr = &gba->IWRAM[(address & 0xFF)|(0x7F<<8)];
	} else if (address >= EXT_WRAM_256KB && address <= EXT_WRAM_256KB_END) {
		/* Read from external work RAM - waitstates apply */
		ptr = &gba->EWRAM[address - EXT_WRAM_256KB];
	} else if (address >= VRAM_96KB && address <= VRAM_96KB_END) {
		/* Read from Video RAM */
		ptr = &gba->VRAM[address - VRAM_96KB];
	} else if (address >= IO_REG_1KB && address <= IO_REG_1KB_END) {
		/* Read from IO register */
		return readIO(gba, address-IO_REG_1KB, size);
	} else if (address >= PALETTE_RAM_1KB && address <= PALETTE_RAM_1KB_END) {
		/* Read from Palette RAM */
		ptr = &gba->PaletteRAM[address - PALETTE_RAM_1KB];	
	} else if (address >= OAM_1KB && address <= OAM_1KB_END) {
        ptr = &gba->OAM[address - OAM_1KB];
    }

    if (ptr == NULL) return 0;
    return readMem(gba, ptr, size);
}

void busWrite(GBA* gba, uint32_t address, uint32_t data, uint8_t size) {
    uint8_t* ptr = NULL;
  

#if defined(DEBUG_ENABLED) && defined(DEBUG_LOG_MEM)
    switch (size) {
		case WIDTH_32: {
            printf("Written (W) %08x to %08x\n", data, address);
            break;
        }
		case WIDTH_16: {
            printf("Written (HW) %04x to %08x\n", data, address);
            break;
        }
		case WIDTH_8: {
            printf("Written (B) %02x to %08x\n", data, address);
            break;
        }
	}
#endif


    /* Write cycle */
    gba->cycles++;

	if (address >= INT_WRAM_32KB && address <= INT_WRAM_32KB_END) {
		/* Write to internal workram with current size and little endian formatting */
		ptr = &gba->IWRAM[address - INT_WRAM_32KB];
    } else if (address >= 0x03FFFF00 && address <= 0x03FFFFFF) {
        /* Mirror of IWRAM 0x03007F00 - 0x03007FFF */
        ptr = &gba->IWRAM[(address & 0xFF)|(0x7F<<8)];
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
        writeIO(gba, address-IO_REG_1KB, data, size);
        return;
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
	} else if (address >= OAM_1KB && address <= OAM_1KB_END) {
        ptr = &gba->OAM[address - OAM_1KB];

		/* OAM only supports 16 and 32 bit writes, writing a byte to the addressed
		 * halfword is going to mirror it to both upper and lower byte */
		if (size == WIDTH_8) {
			/* Halfword aligned */
			ptr = &gba->OAM[(address & ~1) - OAM_1KB];
            data = (data << 8) | data;
            size = WIDTH_16;
		}

    }

    if (ptr == NULL) return;
    writeMem(gba, ptr, data, size);
}

/* ------------- IO Read/Write -------------- */

inline uint32_t readIO_internal(GBA* gba, uint32_t ioaddr, uint8_t size) {
    return readMem(gba, &gba->IO[ioaddr], size);
}

inline void writeIO_internal(GBA* gba, uint32_t ioaddr, uint32_t data, uint8_t size) {
    writeMem(gba, &gba->IO[ioaddr], data, size);
}

static uint32_t readIO_byte(GBA* gba, uint32_t ioaddr) {
    /* Handle read only */
    if ((ioaddr) >= BG0HOFS && (ioaddr) <= BG3VOFS) {
        return 0;
    } else if ((ioaddr) >= BG2PA && (ioaddr) <= BG3Y_H+1) {
        return 0;
    /* TO ADD: DMA registers */
    }

    return gba->IO[ioaddr];
}

uint32_t readIO(GBA* gba, uint32_t ioaddr, uint8_t size) {
    switch (size) {
        case WIDTH_32: {
            uint32_t value = readIO_byte(gba, ioaddr);
            value |= readIO_byte(gba, ioaddr+1) << 8;
            value |= readIO_byte(gba, ioaddr+2) << 16;
            value |= readIO_byte(gba, ioaddr+3) << 24;

            return value;
        }
        case WIDTH_16: {
            uint16_t value = readIO_byte(gba, ioaddr);
            value |= readIO_byte(gba, ioaddr+1) << 8;

            return value;
        }
        case WIDTH_8: {
            return readIO_byte(gba, ioaddr);
        }
    }

    return 0;
}

static void writeIO_byte(GBA* gba, uint32_t ioaddr, uint8_t data) {
    /* Check for read-only registers, and prevent a write */
    bool DMAxCNT_H_Write = false;
    uint8_t DMAx;

    switch (ioaddr) {
        case VCOUNT: return;
        case DISPSTAT: {
            /* Handle read only bits */
            uint8_t current = gba->IO[ioaddr];
            /* V-Blank, H-Blank and V-Counter flags are read only */
            data &= ~0b111;
            data |= current & 0b111;

            goto skipIf;
        }
        case HALTCNT: {
            uint8_t type = data >> 7 & 1;
            if (type == 0) {
                /* Halt mode */
                gba->halted = true;
            } else {
                /* Stop mode */
            }

            goto skipIf;
        }
        case DMA0CNT_H+1:
            DMAxCNT_H_Write = true;
            DMAx = 0;
            break;
        case DMA1CNT_H+1:
            DMAxCNT_H_Write = true;
            DMAx = 1;
            break;
        case DMA2CNT_H+1:
            DMAxCNT_H_Write = true;
            DMAx = 2;
            break;
        case DMA3CNT_H+1:
            DMAxCNT_H_Write = true;
            DMAx = 3;
            break;
    }

    if (DMAxCNT_H_Write) {
        /* Check for enable bit */
        if (data >> 7 & 1 && !(gba->IO[DMA0CNT_H+DMAx*0xC+1] >> 7 & 1)) {
            /* Enable is being set high when it was previously low */
            gba->IO[ioaddr] = data;
            reloadDMAInternal(gba, DMAx);
               
            uint16_t CNT_H = readIO_internal(gba, DMA0CNT_H+DMAx*0xC, WIDTH_16);
            if ((CNT_H >> 12 & 0b11) == 0) {
                /* If it must start immediately, start it */
                startDMA(gba, DMAx);
            }
            return;
        }
    }

    /* Update internal registers on every write for BGNXY */
    if (ioaddr >= BG2X_L && ioaddr <= BG2Y_H) {
        gba->IO[ioaddr] = data;
        updateInternalBGNXY(gba, 2);
        return;
    } else if (ioaddr >= BG3X_L && ioaddr <= BG3Y_H) {
        gba->IO[ioaddr] = data;
        updateInternalBGNXY(gba, 3);
        return;
    } else if (ioaddr == IF || ioaddr == IF+1) {
        /* Intercept write to IF, whatever bits are high in the data for byte (for byte/hw)
         * will be 'acknowledged' and cleared in IF if they were set */
        gba->IO[ioaddr] &= ~data;
        return;
    } else if (ioaddr >= KEYINPUT && ioaddr <= KEYINPUT+1) {
        /* Read only */
        return;
    }


skipIf:
    gba->IO[ioaddr] = data;
}

/* Writes to IO by doing strict bytewise address checking, by writing every byte manually 
 * through aligned. This version is used for software IO accesses */

void writeIO(GBA* gba, uint32_t ioaddr, uint32_t data, uint8_t size) {
	switch (size) {
		case WIDTH_32: {
            writeIO_byte(gba, ioaddr, data & 0xFF);
            writeIO_byte(gba, ioaddr+1, data >> 8 & 0xFF);
            writeIO_byte(gba, ioaddr+2, data >> 16 & 0xFF);
            writeIO_byte(gba, ioaddr+3, data >> 24 & 0xFF);
            return;
        }
		case WIDTH_16: {
            writeIO_byte(gba, ioaddr, data & 0xFF);
            writeIO_byte(gba, ioaddr+1, data >> 8 & 0xFF);
            return;
        }
		case WIDTH_8: {
            writeIO_byte(gba, ioaddr, data);
            return;
        }
	}
}

/* --------------- Scheduler ----------------- */

void pushEvent(GBA* gba, GBAEvent event) {
    if (gba->eventCount == 16) {
        printf("Error: Cannot push event, at max capacity (16)\n");
        return;
    }
    gba->eventStack[gba->eventHeadPointer++] = event;

    gba->eventHeadPointer &= 0xF;
    gba->eventCount++;
}

GBAEvent popEvent(GBA* gba) {
    if (gba->eventCount == 0) {
        printf("Error: Cannot pop event, at 0 capacity\n");
        GBAEvent e;
        return e;
    }

    GBAEvent event = gba->eventStack[gba->eventBasePointer++];

    gba->eventBasePointer &= 0xF;
    gba->eventCount--;

    return event;
}

GBAEvent peekEvent(GBA* gba, uint8_t index) {
    uint8_t i = (gba->eventBasePointer+index) & 0xF;
    return gba->eventStack[i];
}

static void handleEvent(GBA* gba, GBAEvent event) {
    switch (event.type) {
        case EVENT_PPU: 
            stepPPU(gba);
            break;
    }
}

static void handleEvents(GBA* gba) {
    while (gba->eventCount > 0 && peekEvent(gba, 0).scheduledFor <= gba->cycles) {
        handleEvent(gba, popEvent(gba));
    }
}


/* ---------------- DMA ---------------- */

static void reloadDMAInternal(GBA* gba, uint8_t N) {
     /* Reload SAD, DAD, CNT_L to internal registers */
    uint32_t source = readIO_internal(gba, DMA0SAD+N*0xC, WIDTH_32);
    uint32_t dest = readIO_internal(gba, DMA0DAD+N*0xC, WIDTH_32);
    uint32_t wordCount = readIO_internal(gba, DMA0CNT_L+N*0xC, WIDTH_16);

    if (N != 3) {
        wordCount &= 0x3FFF;
        if (wordCount == 0) wordCount = 0x4000;

        dest &= 0x0FFFFFFF;

        if (N == 0) {
            source &= 0x07FFFFFF;
        } else {
            source &= 0x0FFFFFFF;
        }
    } else {
        if (wordCount == 0) wordCount = 0x10000;

        dest &= 0x07FFFFFF;
        source &= 0x0FFFFFFF;
    }

    gba->dmaSAD[N] = source;
    gba->dmaDAD[N] = dest;
    gba->dmaWordCount[N] = wordCount;
}

void startDMA(GBA* gba, uint8_t N) {
    /* DMAN has to be started, internal registers are expected to be loaded in 
     * Enable bit remains set till the end of the transfer.
     */

    gba->dmaInProgressMaster = true;
    gba->dmaInProgress[N] = true;

}

static void stepDMA(GBA* gba, uint8_t N) {
    /* Step forward DMAN */
    uint16_t CNT_H = readIO_internal(gba, DMA0CNT_H+N*0xC, WIDTH_16);

    uint8_t destControl = CNT_H >> 5 & 0b11;
    uint8_t sourceControl = CNT_H >> 7 & 0b11;
    uint8_t repeat = CNT_H >> 9 & 1;
    uint8_t wordSize = CNT_H >> 10 & 1 ? 4 : 2;
    uint8_t triggerIRQ = CNT_H >> 14 & 1;

    uint32_t dest = gba->dmaDAD[N];
    uint32_t source = gba->dmaSAD[N];
    uint32_t wordCount = gba->dmaWordCount[N];

    if (wordSize == 4) {
        /* 32 bit chunk size */
        uint32_t data = busRead(gba, source & ~0b11, WIDTH_32);
        busWrite(gba, dest & ~0b11, data, WIDTH_32);
    } else {
        /* 16 bit chunk size */
        uint16_t data = busRead(gba, source & ~1, WIDTH_16);
        busWrite(gba, dest & ~1, data, WIDTH_16);
    }

    switch (destControl) {
        case 3: /* Increment and Reload */
        case 0: {
            /* Increment */
            gba->dmaDAD[N] += wordSize;
            break;
        }
        case 1: {
            /* Decrement */
            gba->dmaDAD[N] -= wordSize;
            break;
        }
    }

    switch (sourceControl) {
        case 0: {
            /* Increment */
            gba->dmaSAD[N] += wordSize;
            break;
        }
        case 1: {
            /* Decrement */
            gba->dmaSAD[N] -= wordSize;
            break;
        }
    }

    gba->dmaWordCount[N]--;

    if (gba->dmaWordCount[N] == 0) {
        /* End of DMA */
        gba->dmaInProgress[N] = false;
        if (destControl == 3) {
            /* Increment and Reload
             * Reload DAD */
            uint32_t dest = readIO_internal(gba, DMA0DAD+N*0xC, WIDTH_32);
            if (N != 3) {
                dest &= 0x0FFFFFFF;
            } else {
                dest &= 0x07FFFFFF;
            }

            gba->dmaDAD[N] = dest;
        }

        /* Repeat bit */
        if (repeat) {
            /* If repeat is enabled, reload CNT_L internal (wordcount) 
             * Enable bit is left set */
            uint32_t wordCount = readIO_internal(gba, DMA0CNT_L+N*0xC, WIDTH_16);
            if (N != 3) {
                wordCount &= 0x3FFF;
                if (wordCount == 0) wordCount = 0x4000;
            } else if (wordCount == 0) wordCount = 0x10000;

            gba->dmaWordCount[N] = wordCount;
        } else {
            /* Reset enable bit */
            uint32_t ioaddr = DMA0CNT_H+N*0xC;
            uint16_t CNT_H = readIO_internal(gba, ioaddr, WIDTH_16);
            writeIO_internal(gba, ioaddr, CNT_H &= ~(1<<15), WIDTH_16);
        }

        /* Trigger DMA IRQ if enabled */
        if (triggerIRQ) {
            requestInterrupt(gba, IRQ_DMA0+N);
        }

        /* Set the dmaInProgressMaster flag */
        bool dmaRunning = false;
        for (int i=0; i<4; i++) {
            if (gba->dmaInProgress[i]) {
                dmaRunning = true;
                break;
            }
        }

        if (!dmaRunning) gba->dmaInProgressMaster = false;
    }
}
