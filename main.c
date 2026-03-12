#include <SDL2/SDL.h>
#include <gba/gba.h>
#include <gba/gamepak.h>
#include <stdio.h>

static int readBin(char* filePath, size_t* _size, uint8_t** _buffer, size_t maxSize) {
    FILE* file;
    size_t size;

    file = fopen(filePath, "r");
    if (file == NULL) {
        printf("Error opening file: %s\nPlease check your file path\n", filePath);
        return 1;
    }

    fseek(file, 0L, SEEK_END);
    size = ftell(file);
    rewind(file);

    /* Truncate it */
    if (size > maxSize) size = maxSize;

    uint8_t* buffer = malloc(size);
    if (buffer == NULL) {
        printf("An error occured with buffer allocation\n");
        fclose(file);
        return 2;
    }

    size_t readCount = fread(buffer, 1, size, file);
    if (readCount != size) {
        printf("An error occured while reading file\n");
        fclose(file);
        free(buffer);
        return 3;
    }

    fclose(file);
    *_buffer = buffer;
    *_size = size;

    return 0;
}

int main(int argc, char** argv) {
    uint8_t* biosBuffer = NULL;
    size_t biosSize = 0;

    /* Check for flags and check file type */
    char* filepath = NULL;

    for (int i=1; i<argc; i++) {
        char* flag = argv[i];
        if (strcmp(flag, "-bios") == 0) {
            if (argc < 4) {
                printf("Please specify filepath for '-bios'\n");
                return 4;
            }
            if (biosBuffer != NULL) {
                printf("BIOS file already specified\n");
                free(biosBuffer);
                return 5;
            }
            /* Load BIOS file */
            int err = readBin(argv[i+1], &biosSize, &biosBuffer, 0x4000);
            if (err != 0) {
                printf("Could not load BIOS file: %s\n", argv[i+1]);
                return err;
            }
                
            /* Filepath has been read */
            i++;
        } else if (strcmp(flag, "-help") == 0) {
            /* Ignore help if other instructions are specified */
            if (argc != 2) continue;

            printf("Welcome to MegaGBA!\n");
            printf("Usage:\n");
            printf("   megagba [ROM FILEPATH]    (Specifies .gba ROM file)\n\n");
            printf("Add-Ons:\n");
            printf("   -bios [BIOS FILEPATH]    (Specifies bios ROM file)\n");

            printf("\n");
            printf("Help:\n");
            printf("   megagba -help\n");
            return 0;
        } else {
            if (filepath != NULL) {
                printf("Invalid argument sequence\n");
                return 6;
            }
            filepath = flag;
        }
    }

    if (filepath == NULL) {
        printf("Please specify filepath\n");
        printf("\nRun:\n   megagba -help\nFor usage information\n");
        return 7;
    }

    uint8_t* buffer;
    size_t size;
    int err = readBin(filepath, &size, &buffer, 0x02000000);
    if (err != 0) {
        printf("Could not load ROM file\n");
        return err;
    }

    /* At this stage, file reading and copying to memory is complete */
    
    GamePak gamepak;
    initGamePak(&gamepak, buffer, size); 

    /* Mainloop */
    startGBAEmulator(&gamepak, biosBuffer, biosSize);
    /* --------------- */
    free(biosBuffer);
    freeGamePak(&gamepak);
    return 0;
}
