#ifndef gba_debug_h
#define gba_debug_h
#include <gba/gba.h>
#include <stdio.h>

/* -------- Options -------- */
//#define DEBUG_ENABLED
    //#define DEBUG_TRACE_STATE
        #define DEBUG_PRINT_REGS
            #define DEBUG_LIMIT_REGS
    //#define DEBUG_LOG_MEM

#define DEBUG_SET_BREAKPOINT(s) puts(s); getchar()

/* Contains useful dissembler for debugging and tracing the emulator */


#ifdef DEBUG_ENABLED


void printStateARM(GBA* gba, uint32_t opcode);
void printStateTHUMB(GBA* gba, uint16_t opcode);
void initDissembler();
#endif
#endif


