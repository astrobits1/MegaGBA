#ifndef gba_gamepak_h
#define gba_gamepak_h

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* Structure which contains game pak information for GBA */

typedef struct {
	uint8_t* allocated;
	size_t size;
	bool inserted;
} GamePak;

#ifdef __cplusplus
extern "C" {
#endif

bool initGamePak(GamePak* gamepak, uint8_t* allocated, size_t size);

#ifdef __cplusplus
}
#endif

#endif
