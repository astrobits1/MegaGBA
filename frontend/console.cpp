#include <frontend/console.hpp>
#include <frontend/context.hpp>

#include <gba/gba.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <vector>

void Console::output(std::string out) {
    this->outputBuffer += out + "\n";
}

std::string Console::error(std::string err) {
    return std::string("[Error] ") + err + "\n";
}

std::string Console::runCommand(std::string inp) {
    this->outputBuffer.clear();
    std::vector<std::string> tokens;

    int tokStart = 0;
    for (int i=0; i<inp.length(); i++) {
        if (inp[i] == ' ') {
            if (i-tokStart > 0) tokens.push_back(inp.substr(tokStart, (i-tokStart)));
            if (i+1 < inp.length()) {
                tokStart = i+1;
            } else break;
        }
    }
    if (inp.length()-tokStart > 0 && inp[tokStart] != ' ') tokens.push_back(inp.substr(tokStart, inp.length()-tokStart));

    if (tokens.empty()) return error("Please enter a command");

    std::string cmd = tokens.at(0);

    if (cmd == "pause") {
        if (this->ctx->paused) output("Already Paused");
        else {
            this->ctx->paused = true;
            output("Paused");
        }
    } else if (cmd == "unpause") {
        if (!this->ctx->paused) output("Not paused");
        else {
            this->ctx->paused = false;
            output("Unpaused");
        }
    } else if (cmd == "dump") {
        if (tokens.size() < 2) return error("Please enter address");
        uint32_t address;
        try {
            address = std::stoi(tokens.at(1), 0, 16);
            std::stringstream stream = this->dumpMemory(address);
            output(stream.str());
        } catch (int ex) {
            return error("Please enter valid address");
        }
    } else if (cmd == "step") {
        if (this->ctx->stepsLeft != -1) return error("Emulator is already stepping: " + std::to_string(this->ctx->stepsLeft) + " left");
        if (!this->ctx->paused) return error("Emulator must be paused for manual stepping");

        bool frame = false;
        int n = 1;

        if (tokens.size() > 1) {
            if (tokens.at(1) == "frame") {
                if (tokens.size() > 2) {
                    try {
                        n = std::stoi(tokens.at(2));
                    } catch (int ex) {
                        return error("Please enter an integer step count");
                    }
                }

                frame = true;
            } else {
                try {
                    n = std::stoi(tokens.at(1));
                } catch (int ex) {
                    return error("Please enter an integer step count");
                }
            }
        }
        
        if (frame) {
            this->ctx->frameSteps = true;
        } else {
            this->ctx->frameSteps = false;
        }

        this->ctx->stepsLeft = n;
        this->ctx->paused = false;
    } else if (cmd == "quit") {
        this->ctx->quit = true;
        output("Quitting Emulator");
    } else return error("Unrecognised command '" + tokens.at(0) + "'");

   
    return outputBuffer;
}

/* Functions */

/* Generalised function for dumping memory into a readable block of multiline text
 * colorised for readability with ANSI colour codes. This can be parsed to separate into
 * strings based on colour for IMGUI rendering */
std::stringstream Console::dumpMemory(uint32_t address) {
    uint8_t rowsBefore = 3;
    uint8_t rowsAfter = 3;
    std::string topHeaderNormal = "\033[96m";       // White
    std::string topHeaderColoured = "\033[94m";    // Blue
    std::string sideHeaderNormal = "\033[96m";
    std::string sideHeaderColoured = "\033[94m";
    std::string byteNormal = "\033[37m";
    std::string byteColoured = "\033[92m";
    std::string normalColour = "\033[0m";
    std::string addressColour = "\033[45m";

    uint32_t start = (address & ~0xF) - rowsBefore*0x10;

    std::stringstream output;

    /* Header initial space for address */
    output << addressColour;
    output << "[ADDRESS] ";
    output << normalColour;
    //std::fill_n(std::ostreambuf_iterator<char>(output), 10, ' ');

    /* Header top byte annotations - colour the the address corresponding */
    output << topHeaderNormal;
    for (int i=0; i<0x10; i++) {
        if (i == (address & 0xF)){ output << topHeaderColoured; }
        output << ' ' << std::setfill('0') << std::setw(2) << std::right << std::hex << i;
        if (i == (address & 0xF)){ output << topHeaderNormal; }
    }
    output << '\n';

    /* Start with side header + byte data */

    for (int i = 0; i < rowsBefore+rowsAfter+1; i++) {
        uint32_t base = start+i*0x10;
        output << sideHeaderNormal;
        if (base == (address & ~0xF)) output << sideHeaderColoured;
        output << "0x" << std::setfill('0') << std::setw(8) << std::right << std::hex << base;
        if (base == (address & ~0xF)) output << sideHeaderNormal;

        output << byteNormal;
        for (int j=0; j<0x10; j++) {
            uint32_t byte;
            uint32_t addr = base+j;
            
            bool success = rawRead(this->ctx->gba, base+j, WIDTH_8, &byte);

            if (addr >= address && addr <= address+3) output << byteColoured;
            output << std::noshowbase;
            if (success) {
                output << ' ' << std::setfill('0') << std::setw(2) << std::right << std::hex << byte;
            } else output << " --";
            if (addr >= address && addr <= address+3) output << byteNormal;
        }

        output << '\n';
    }

    output << normalColour;
    return output;
}
