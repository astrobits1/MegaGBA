#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <gba/gamepak.h>

static bool checkString(uint8_t* c, const char* code, int length) {
    for (int i=0; i<length; i++) {
        if (c[i] != code[i]) return false;
    }

    return true;
}

static void scanAndAllocateBackup(GamePak* gamepak) {
    /* Scan for SRAM/FRAM, EEPROM, FLASH and allocate buffer accordingly */
    BACKUP_ID id;
    bool found = false;

    for (int i=0; i<0x1000; i+=4) {
        if (i > gamepak->size-1) break; 

        char* checkCode = NULL;
        int checkLength = 0;

        switch (gamepak->allocated[i]) {
            case 'E':
                checkLength = 8;
                id = BACKUP_EEPROM;
                checkCode = "EEPROM_V";
                break;
            case 'S':
                checkLength = 6;
                id = BACKUP_SRAM_32KB;
                checkCode = "SRAM_V";
                break;
            case 'F':
                if (i+7 <= gamepak->size && checkString(&gamepak->allocated[i], "FLASH_V", 7)) {
                    id = BACKUP_FLASH_64KB;
                    found = true;
                } else if (i+10 <= gamepak->size && checkString(&gamepak->allocated[i], "FLASH512_V", 10)) {
                    id = BACKUP_FLASH_64KB;
                    found = true;
                } else if (i+9 <= gamepak->size && checkString(&gamepak->allocated[i], "FLASH1M_V", 9)) {
                    id = BACKUP_FLASH_128KB;
                    found = true;
                }
                break;
        }

        if (!found && checkCode != NULL && i+checkLength <= gamepak->size) {
            if (checkString(&gamepak->allocated[i], checkCode, checkLength)) {
                found = true;
            }
        }
        if (found) break;
    }


    if (found) {
        gamepak->backupId = id;

        /*
        switch (id) {
            case BACKUP_SRAM_32KB: {
                uint8_t* sram = (uint8_t*)malloc(0x8000);
                if (sram == NULL) {
                    printf("Could not allocate SRAM\n");
                    return;
                }

                printf("Found and allocated SRAM\n");
                gamepak->sram = sram;
            }

            default: break;
        }
        */
    }
    else gamepak->backupId = BACKUP_SRAM_32KB;

    uint8_t* sram = (uint8_t*)malloc(0x8000);
    if (sram == NULL) {
        printf("Could not allocate SRAM\n");
        return;
    }

    gamepak->sram = sram;
}

bool initGamePak(GamePak* gamepak, uint8_t* allocated, size_t size) {
	gamepak->allocated = allocated;
	gamepak->inserted = false;
	gamepak->size = (size_t)size;

    gamepak->backupId = BACKUP_SRAM_32KB;
    gamepak->sram = NULL;

    scanAndAllocateBackup(gamepak);

	return true;
}

void freeGamePak(GamePak* gamepak) {
    if (gamepak->sram != NULL) free(gamepak->sram);
}
