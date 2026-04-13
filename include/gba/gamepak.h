#ifndef gba_gamepak_h
#define gba_gamepak_h

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

typedef enum {
    BACKUP_NONE,
    BACKUP_EEPROM,
    BACKUP_SRAM_32KB,
    BACKUP_FLASH_64KB,
    BACKUP_FLASH_128KB
} BACKUP_ID;

/* Structure which contains game pak information for GBA */

typedef struct {
	uint8_t* allocated;
	size_t size;
	bool inserted;

    BACKUP_ID backupId;
    uint8_t* sram;
} GamePak;

#ifdef __cplusplus
extern "C" {
#endif

bool initGamePak(GamePak* gamepak, uint8_t* allocated, size_t size);
void freeGamePak(GamePak* gamepak);

#ifdef __cplusplus
}
#endif

#endif
