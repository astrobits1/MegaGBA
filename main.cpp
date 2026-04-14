#include <frontend/context.hpp>

#include <fstream>
#include <iostream>
#include <vector>
#include <stdint.h>

std::string defaultBIOSPath = "gbabios.bin";

static int readBin(std::string filePath, size_t* _size, std::vector<uint8_t>& _buffer, size_t maxSize) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    size_t size;

    if (!file.is_open()) {
        std::cout << "Error opening file: " << filePath << std::endl;
        std::cout << "Please check your file path\n";
        return 1;
    }

    size = file.tellg();
    file.seekg(0, std::ios::beg);

    /* Truncate it */
    if (size > maxSize) size = maxSize;

    _buffer.resize(size);
    if (!(file.read((char*)_buffer.data(), size))) {
        printf("An error occured while reading file\n");
        file.close();
        return 3;
    }

    file.close();
    *_size = size;

    return 0;
}

int main(int argc, char** argv) {
    std::vector<uint8_t> biosBuffer;
    size_t biosSize = 0;

    /* Check for flags and check file type */
    std::string filepath;

    for (int i=1; i<argc; i++) {
        std::string flag(argv[i]);

        if (flag == "-bios") {
            if (argc < 4) {
                std::cout << "Please specify filepath for '-bios'\n";
                return 4;
            }
            if (!biosBuffer.empty()) {
                std::cout << "BIOS file already specified\n";
                return 5;
            }
            /* Load BIOS file */
            std::string biosPath(argv[i+1]);
            int err = readBin(biosPath, &biosSize, biosBuffer, 0x4000);
            if (err != 0) {
                std::cout << "Could not load BIOS file: " << biosPath << std::endl;
                return err;
            }
                
            /* Filepath has been read */
            i++;
        } else if (flag == "-help") {
            /* Ignore help if other instructions are specified */
            if (argc != 2) continue;

            std::cout << "Welcome to MegaGBA!\n";
            std::cout << "Usage:\n";
            std::cout << "   megagba [ROM FILEPATH]    (Specifies .gba ROM file)\n\n";
            std::cout << "Add-Ons:\n";
            std::cout << "   -bios [BIOS FILEPATH]    (Specifies bios ROM file)\n";

            std::cout << "\n";
            std::cout << "Help:\n";
            std::cout << "   megagba -help\n";
            return 0;
        } else {
            if (!filepath.empty()) {
                std::cout << "Invalid argument sequence\n";
                return 6;
            }
            filepath = flag;
        }
    }

    if (filepath.empty()) {
        std::cout << "Please specify filepath\n";
        std::cout << "\nRun:\n   megagba -help\nFor usage information\n";
        return 7;
    }

    if (biosBuffer.empty()) {
        int err = readBin(defaultBIOSPath, &biosSize, biosBuffer, 0x4000);
        if (err != 0) {
            std::cout << "Could not load BIOS file from default path: " << defaultBIOSPath << std::endl;;
            std::cout << "Please add BIOS file in calling directory or specify path using -bios\n";
            return err;
        }
    }

    std::vector<uint8_t> buffer;
    size_t size;
    int err = readBin(filepath, &size, buffer, 0x02000000);

    if (err != 0) {
        std::cout << "Could not load ROM file\n";
        return err;
    }

    /* At this stage, file reading and copying to memory is complete */
    
    int err2 = frontendImguiMain(buffer, size, biosBuffer, biosSize);
    return err2;
}
