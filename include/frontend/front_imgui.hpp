#ifndef FRONT_IMGUI_H
#define FRONT_IMGUI_H

#include <vector>
#include <stdint.h>

int frontendImguiMain(std::vector<uint8_t>& buffer, size_t size, std::vector<uint8_t>& biosBuffer, size_t biosSize);

#endif
