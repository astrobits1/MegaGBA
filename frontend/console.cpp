#include <frontend/console.hpp>
#include <frontend/context.hpp>

#include <gba/gba.h>
#include <iostream>
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
    } else if (cmd == "step") {
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
            for (int i=0; i<n; i++) stepGBAFrame(this->ctx->gba);
            output("Stepped " + std::to_string(n) + " frames");
        } else {
            for (int i=0; i<n; i++) stepGBAStep(this->ctx->gba);
            output("Stepped " + std::to_string(n) + " times");
        }
    } else if (cmd == "quit") {
        this->ctx->quit = true;
        output("Quitting Emulator");
    } else return error("Unrecognised command '" + tokens.at(0) + "'");

   
    return outputBuffer;
}

/* Debug functions */

