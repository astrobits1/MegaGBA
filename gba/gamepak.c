#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <gba/gamepak.h>

bool initGamePak(GamePak* gamepak, uint8_t* allocated, size_t size) {
	gamepak->allocated = allocated;
	gamepak->inserted = false;
	gamepak->size = (size_t)size;
	return true;
}
