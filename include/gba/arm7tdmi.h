/* Implementation header for the primary gameboy advance cpu ARM7TDMI */

#ifndef gba_arm7tdmi_h
#define gba_arm7tdmi_h

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct GBA GBA;

typedef enum {
	CPU_MODE_USER = 0b10000, 				/* User - normal execution mode */
	CPU_MODE_FIQ  = 0b10001, 				/* FIQ Exception */
	CPU_MODE_IRQ  = 0b10010, 				/* IRQ Exception */
	CPU_MODE_SVC  = 0b10011,				/* Supervisor Exception */
	CPU_MODE_ABT  = 0b10111, 				/* Abort Exception */
	CPU_MODE_UND  = 0b11011,				/* Undefined Exception */
	CPU_MODE_SYSTEM = 0b11111 				/* System mode */
} CPU_MODE;

typedef enum {
    CPU_EXCEP_SWI,                          /* Software Interrupt */
    CPU_EXCEP_IRQ,                          /* Interrupt Exception */

    CPU_EXCEP_COUNT
} CPU_EXCEP;                                /* High number = higher priority */

typedef enum {                  /* IRQ in bit order corresponding to IE & IF */
    IRQ_LCD_VBLANK,
    IRQ_LCD_HBLANK,
    IRQ_LCD_VCOUNTER,
    IRQ_TIMER1_OVF,
    IRQ_TIMER2_OVF,
    IRQ_TIMER3_OVF,
    IRQ_SERIAL,
    IRQ_DMA0,
    IRQ_DMA1,
    IRQ_DMA2,
    IRQ_DMA3,
    IRQ_KEYPAD,
    IRQ_EXTERNAL
} IRQ;

typedef enum {
	CPU_STATE_ARM,	 			/* 32 bit ARM */
	CPU_STATE_THUMB 			/* 16 bit THUMB */
} CPU_STATE;

enum { 							/* CPSR bits */
	FLG_N = 31,
	FLG_Z = 30,
	FLG_C = 29,
	FLG_V = 28,
	CPSR_IRQ_DIS = 7,
	CPSR_FIQ_DIS = 6,
	CPSR_T = 5
};

enum { 							/* Helpful register abbreviations */
	R0, R1, R2, R3, R4, R5, R6, R7,
	R8, R9, R10, R11, R12, R13, R14, R15,

	/* Banked Registers */
	R8_FIQ = 0, R9_FIQ, R10_FIQ, R11_FIQ, R12_FIQ, R13_FIQ, R14_FIQ,
	R13_SVC = 0, R14_SVC,
	R13_ABT = 0, R14_ABT,
	R13_IRQ = 0, R14_IRQ,
	R13_UND = 0, R14_UND,
	SPSR_FIQ = 0, SPSR_SVC, SPSR_ABT, SPSR_IRQ, SPSR_UND,

	/* Swap values */
	// R8_SWAP = 0, R9_SWAP, R10_SWAP, R11_SWAP, R12_SWAP, R13_SWAP, R14_SWAP,
};

void initialiseCPU(GBA* gba);
void stepCPU(GBA* gba);

void requestInterrupt(GBA* gba, IRQ irq);

/* Helpers */
uint32_t twosComplementOffset(uint32_t base, uint32_t offset, uint8_t signBit);

#endif
