#include <gba/debugGBA.h>
#include <gba/gba.h>
#include <stdio.h>
#include <string.h>

#ifdef DEBUG_ENABLED

void (*Dissembler_ARM_LUT[4096])(GBA* gba, uint32_t opcode);
void (*Dissembler_THUMB_LUT[256])(GBA* gba, uint16_t opcode);


/* -------------- ARM Handlers ------------------- */

static char* condition(struct GBA* gba, uint8_t condCode) {
	switch (condCode) {
		case 0xE: return "";
		case 0x0: return "EQ";
		case 0x1: return "NE";
		case 0x2: return "CS";
		case 0x3: return "CC";
		case 0x4: return "MI";
		case 0x5: return "PL";
		case 0x6: return "VS";
		case 0x7: return "VC";
		case 0x8: return "HI";
		case 0x9: return "LS";
		case 0xA: return "GE";
		case 0xB: return "LT";
		case 0xC: return "GT";
		case 0xD: return "LE";

		default: return "";
	}
}

static void BX(struct GBA* gba, uint32_t ins) {
	printf("BX");
}

static void B(struct GBA* gba, uint32_t ins) {
	printf("%-9s %xh", "B", (int32_t)(ins & 0xFFFFFF) << 2);
}

static void BL(struct GBA* gba, uint32_t ins) {
	printf("%-9s %xh", "BL", (int32_t)(ins & 0xFFFFFF) << 2);
}

static void decodeDataProcessing(struct GBA* gba, uint32_t ins) {
	/* Dissembles Data Processing instructions */
	uint8_t opcode = (ins >> 21) & 0xF;
	uint8_t S 	   = (ins >> 20) & 1;
	uint8_t I 	   = (ins >> 25) & 1;

	uint8_t Rn 	   = (ins >> 16) & 0xF;
	uint8_t Rd 	   = (ins >> 12) & 0xF;

	char* name = NULL;
	char* cond = condition(gba, ins >> 28);

	switch (opcode) {
		case 0x0: name = "AND"; break;
		case 0x1: name = "EOR"; break;
		case 0x2: name = "SUB"; break;
		case 0x3: name = "RSB"; break;
		case 0x4: name = "ADD"; break;
		case 0x5: name = "ADC"; break;
		case 0x6: name = "SBC"; break;
		case 0x7: name = "RSC"; break;
		case 0x8: name = "TST"; break;
		case 0x9: name = "TEQ"; break;
		case 0xA: name = "CMP"; break;
		case 0xB: name = "CMN"; break;
		case 0xC: name = "ORR"; break;
		case 0xD: name = "MOV"; break;
		case 0xE: name = "BIC"; break;
		case 0xF: name = "MVN"; break;
	}

	char format[7] = "";
	strcpy(format, name);
	strcat(format, cond);

	char OP2[15] = "";
	
	if (I) {
		sprintf(OP2, "%02xh, {%d}", ins & 0xFF, ins >> 8 & 0xF);
	} else {
		uint8_t Rm = ins & 0xF;
		uint8_t isReg = (ins >> 4) & 1;

		switch (ins >> 5 & 0b11) {
			case 0: sprintf(OP2, "R%d, LSL ", Rm); break;
			case 1: sprintf(OP2, "R%d, LSR ", Rm); break;
			case 2: sprintf(OP2, "R%d, ASR ", Rm); break;
			case 3: sprintf(OP2, "R%d, ROR ", Rm); break;
		}

		char shiftSrc[5] = "";

		if (isReg) {
			sprintf(shiftSrc, "R%d", (ins >> 8) & 0xF);
		} else {
			sprintf(shiftSrc, "{%d}", (ins >> 7) & 0x1F);
		}

		strcat(OP2, shiftSrc);
	}

	switch (opcode) {
		case 0: case 1: case 2: case 3: case 4: case 5:
		case 6: case 7: case 0xC: case 0xE:
			strcat(format, S ? "S" : "");
			printf("%-9s R%d,  R%d,  %s", format, Rd, Rn, OP2);
			break;

		case 8: case 9: case 0xA: case 0xB:
			printf("%-9s R%d,  %s", format, Rn, OP2);
			break;

		case 0xD: case 0xF:
			strcat(format, S ? "S" : "");
			printf("%-9s R%d,  %s", format, Rd, OP2);
			break;
	}
}


static void MRS(struct GBA* gba, uint32_t ins) {
	printf("MRS");
}

static void MSR(struct GBA* gba, uint32_t ins) {
	printf("MSR");
}

static void MUL(struct GBA* gba, uint32_t ins) {
	printf("MUL");
}

static void MLA(struct GBA* gba, uint32_t ins) {
	printf("MLA");
}

static void UMULL(struct GBA* gba, uint32_t ins) {
	printf("UMULL");
}

static void UMLAL(struct GBA* gba, uint32_t ins) {
	printf("UMLAL");
}

static void SMULL(struct GBA* gba, uint32_t ins) {
	printf("SMULL");
}

static void SMLAL(struct GBA* gba, uint32_t ins) {
	printf("SMLAL");
}

static void LDR_Rx(struct GBA* gba, uint32_t ins) {
	printf("LDR Rx");
}

static void LDR_Imm(struct GBA* gba, uint32_t ins) {

	printf("LDR Imm");
}

static void STR_Rx(struct GBA* gba, uint32_t ins) {
	printf("STR Rx");
}

static void STR_Imm(struct GBA* gba, uint32_t ins) {
	printf("STR Imm");
}

static void LDRH(struct GBA* gba, uint32_t ins) {
	printf("LDRH");
}

static void LDRSB(struct GBA* gba, uint32_t ins) {
	printf("LDRSB");
}

static void LDRSH(struct GBA* gba, uint32_t ins) {
	printf("LDRSH");
}

static void STRH(struct GBA* gba, uint32_t ins) {
	printf("STRH");
}

static void LDM(struct GBA* gba, uint32_t ins) {
	printf("LDM");
}

static void STM(struct GBA* gba, uint32_t ins) {
	printf("STM");
}

static void SWP(struct GBA* gba, uint32_t ins) {
	printf("SWP");
}

static void SWP_B(struct GBA* gba, uint32_t ins) {
	printf("SWPB");
}

static void SWI(struct GBA* gba, uint32_t ins) {
	printf("SWI");
}

static void Undefined(struct GBA* gba, uint32_t ins) {
	printf("Undefined");
}

static void Unimplemented_ARM(struct GBA* gba, uint32_t ins) {
	printf("Unimplemented %x", ins);
}

/* ------------- THUMB Handlers ----------------- */

static void LSL_LSR_ASR(struct GBA* gba, uint16_t ins) {
	char name[4];

	switch (ins >> 11 & 0b11) {
		case 0: strcpy((char*)&name, "LSL"); break;
		case 1: strcpy((char*)&name, "LSR"); break;
		case 2: strcpy((char*)&name, "ASR"); break;
	}

	name[3] = '\0';

	printf("%-9s R%d, R%d, #%d", name, ins & 0b111, ins >> 3 & 0b111, ins >> 6 & 0b11111);
}

static void ADD_SUB(struct GBA* gba, uint16_t ins) {
	uint8_t I = ins >> 10 & 1;
	uint8_t SUB = ins >> 9 & 1;
	uint8_t Rs = ins >> 3 & 0b111;
	uint8_t Rd = ins & 0b111;

	printf("%-9s R%d, R%d, ", SUB ? "SUB" : "ADD", Rd, Rs);
	if (I) printf("#%d", ins >> 6 & 0b111);
	else printf("R%d", ins >> 6 & 0b111);
}

static void MOV_CMP_ADD_SUB_Imm(struct GBA* gba, uint16_t ins) {
	uint8_t opcode = ins >> 11 & 0b11;
	uint8_t Rd = ins >> 8 & 0b111;
	uint8_t offset = ins & 0xFF;

	switch (opcode) {
		case 0: printf("%-9s ", "MOV"); break;
		case 1: printf("%-9s ", "CMP"); break;
		case 2: printf("%-9s ", "ADD"); break;
		case 3: printf("%-9s ", "SUB"); break;
	}

	printf("R%d, #%d", Rd, offset);
}

static void ALU(struct GBA* gba, uint16_t ins) {
	uint8_t opcode = ins >> 6 & 0xF;
	uint8_t Rs = ins >> 3 & 0b111;
	uint8_t Rd = ins & 0b111;

	switch (opcode) {
		case 0x0: printf("%-9s ", "AND"); break;
		case 0x1: printf("%-9s ", "EOR"); break;
		case 0x2: printf("%-9s ", "LSL"); break;
		case 0x3: printf("%-9s ", "LSR"); break;
		case 0x4: printf("%-9s ", "ASR"); break;
		case 0x5: printf("%-9s ", "ADC"); break;
		case 0x6: printf("%-9s ", "SBC"); break;
		case 0x7: printf("%-9s ", "ROR"); break;
		case 0x8: printf("%-9s ", "TST"); break;
		case 0x9: printf("%-9s ", "NEG"); break;
		case 0xA: printf("%-9s ", "CMP"); break;
		case 0xB: printf("%-9s ", "CMN"); break;
		case 0xC: printf("%-9s ", "ORR"); break;
		case 0xD: printf("%-9s ", "MUL"); break;
		case 0xE: printf("%-9s ", "BIC"); break;
		case 0xF: printf("%-9s ", "MVN"); break;
	}

	printf("R%d, R%d", Rd, Rs);
}

static void HIREG_OPS_BX(struct GBA* gba, uint16_t ins) {
	uint8_t opcode = ins >> 8 & 0b11;
	uint8_t H1 = ins >> 7 & 1;
	uint8_t H2 = ins >> 6 & 1;
	uint8_t Rs = (H2 << 3) | (ins >> 3 & 0b111);
	uint8_t Rd = (H1 << 3) | (ins & 0b111);

	switch (opcode) {
		case 0b00: printf("%-9s ", "ADD"); break;
		case 0b01: printf("%-9s ", "CMP"); break;
		case 0b10: printf("%-9s ", "MOV"); break;
		case 0b11: {
			printf("%-9s R%d", "BX", Rs);
			return;
		}
	}

	printf("R%d, R%d", Rd, Rs);
}

static void Unimplemented_THUMB(struct GBA* gba, uint16_t ins) {
	printf("Unimplemented %x", ins);
}


/* -------------------------------------------- */

void initDissembler() {
	/* Initialising ARM Opcodes */
	for (int index = 0; index < 4096; index++) {
		/* index corresponds to 12 bit index formed by combining bits 27-20 and 7-4 of OPCODE
		 *
		 * The order in which we check matters as there are encoding collisions, however 
		 * a simple way to understand it is to check which encoding has the most hardcoded 
		 * bits in 27-20 and 7-4, as these guarantee some instructions. So we check using 
		 * bit masks which are full of 1s (as we need to check more hardcoded bits) and slowly 
		 * reduce to more zeroes as bits become less significant 
		 *
		 * Basically we're extracting bits which are useful then comparing it to see if they match
		 * for the particular encoding */

		if ((index & 0b111111111111) == 0b000100100001) {
			/* Checking for Branch And Exchange */
			Dissembler_ARM_LUT[index] = &BX;
		} else if ((index & 0b111110111111) == 0b000100001001) {
			/* Single Data Swap (SWP) 
			 *
			 * Byte/Word is the only option and we interpret them in the LUT */
			uint8_t B = (index >> 6) & 1;

			Dissembler_ARM_LUT[index] = B ? &SWP_B : &SWP;
		} else if ((index & 0b111111001111) == 0b000000001001) {
			/* Checking for multiply and multiply accumulate
			 * bit 21 tells us whether its accumulate or not, while 20 is the S bit
			 * We check for S at runtime as usual but use different handlers for MUL and MLA */
			uint8_t A = (index >> 5) & 1;
			Dissembler_ARM_LUT[index] = A ? &MLA : &MUL;
		} else if ((index & 0b111110001111) == 0b000010001001) {
			/* Checking for multiply long and multiply accumulate long with variations
			 * (UMULL, UMLAL, SMULL, SMLAL), S bit is checked at runtime */
			uint8_t U = (index >> 6) & 1;
			uint8_t A = (index >> 5) & 1;

			if (U) Dissembler_ARM_LUT[index] = A ? &SMLAL : &SMULL;
			else Dissembler_ARM_LUT[index] = A ? &UMLAL : &UMULL;
		} else if ((index & 0b111110110000) == 0b000100000000) {
			/* Checking for MRS (transfer PSR->Reg)
			 * bit 22 can be 1 or 0 depending on whether SPSR/CPSR has to be used,
			 * which will be determined at runtime */
			Dissembler_ARM_LUT[index] = &MRS;
		} else if ((index & 0b110110110000) == 0b000100100000) {
			/* Checking for MSR (transfer Reg/Imm->PSR)
			 * This instruction has 2 encodings, one for flag only transfer which can be Imm/Reg
			 * or full register transfer which is only Reg. This however introduces complicated 
			 * bit collisions which we can avoid by doing runtime checks instead */
			Dissembler_ARM_LUT[index] = &MSR;
		} else if ((index & 0b111000001001) == 0b000000001001) {
			/* Checking for Halfword and Signed Data transfer
			 * (LDRH, STRH, LDRSB, LDRSH) -> Signed Byte and Signed halfword is
			 * only available for LDR.
			 * We check for each of these options and leave the rest for runtime 
			 * Note: We dont differtiate between Rx and Imm for this one as interpretation
			 * is pretty similar for both of them (no shifting) and we already have many variations */

			uint8_t L = (index >> 4) & 1; 				// LOAD/STORE
			uint8_t SH = (index >> 1) & 0b11;			// Sign and Halfword

			if (L) {
				switch (SH) {
					// 00 never occurs as it is interpreted as a SWP instruction checked earlier
					case 0b01: Dissembler_ARM_LUT[index] = &LDRH; break;
					case 0b10: Dissembler_ARM_LUT[index] = &LDRSB; break;
					case 0b11: Dissembler_ARM_LUT[index] = &LDRSH; break;
				}
			} else {
				// STORE only allows unsigned halfwords
				Dissembler_ARM_LUT[index] = &STRH;
			}
		} else if ((index & 0b111100000000) == 0b111100000000) {
			/* Software Interrupt - SWI */
			Dissembler_ARM_LUT[index] = &SWI;
		} else if ((index & 0b111000000001) == 0b011000000001) {
			/* Undefined Instruction */
			Dissembler_ARM_LUT[index] = &Undefined;
		} else if ((index & 0b111000000000) == 0b101000000000) {
			/* Checking for Branch and Branch with Link */
			uint8_t Link = (index >> 8) & 1;
			Dissembler_ARM_LUT[index] = Link ? &BL : &B;
		} else if ((index & 0b111000000000) == 0b100000000000) {
			/* Block Data Transfer - LDM/STM */
			uint8_t L = (index >> 4) & 1;

			Dissembler_ARM_LUT[index] = L ? &LDM : &STM;
		} else if ((index & 0b110000000000) == 0b010000000000) {
			/* Checking for Single Data Transfer (LDR/STR)
			 * with options for immediate or register with shift */
			uint8_t L = (index >> 4) & 1; 				// LOAD/STORE
			uint8_t I = (index >> 9) & 1; 				// IMM/Reg
			
			if (L) {
				Dissembler_ARM_LUT[index] = I ? &LDR_Rx : &LDR_Imm;
			} else {
				Dissembler_ARM_LUT[index] = I ? &STR_Rx : &STR_Imm;
			}
		} else if ((index & 0b110000000000) == 0) {
			/* Checking for Data Processing Instructions
		 	 * Only bit 27-26 are fixed, rest are variable showing various opcodes and other info */

			Dissembler_ARM_LUT[index] = &decodeDataProcessing;
		} else {
			Dissembler_ARM_LUT[index] = &Unimplemented_ARM;
		}
	}

	/* Initialising THUMB Opcode */
	for (int index = 0; index < 256; index++) {

		if ((index & 0b11111100) == 0b01000100) {
			/* Hi Register Operations / BX */
			Dissembler_THUMB_LUT[index] = &HIREG_OPS_BX; 
		} else if ((index & 0b11111100) == 0b01000000) {
			/* ALU Operations */
			Dissembler_THUMB_LUT[index] = &ALU;
		} else if ((index & 0b11111000) == 0b00011000) {
			/* ADD/SUB with immediate or register operand */
			Dissembler_THUMB_LUT[index] = &ADD_SUB;
		} else if ((index & 0b11100000) == 0b00100000) {
			/* MOV/CMP/ADD/SUB Immediate */
			Dissembler_THUMB_LUT[index] = &MOV_CMP_ADD_SUB_Imm;
		} else if ((index & 0b11100000) == 0b00000000) {
			/* Move Shifted Register */
			Dissembler_THUMB_LUT[index] = &LSL_LSR_ASR;
		} else {
			Dissembler_THUMB_LUT[index] = &Unimplemented_THUMB;
		}
	}
}

bool flag = false;

void printStateARM(GBA* gba, uint32_t opcode) {
#ifdef DEBUG_TRACE_STATE	
#ifdef DEBUG_PRINT_REGS
	uint32_t R[16];
	memcpy(&R, &gba->REG, sizeof(uint32_t)*16);
#ifdef DEBUG_LIMIT_REGS	
	printf("[R0:%08x|R1:%08x|R11:%08x|R12:%08x]", R[0],R[1],R[11],R[12]); 
#else
	printf("%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X cpsr: %08X | %08X: ", R[0],R[1],R[2],R[3],R[4],R[5],R[6],R[7],R[8],R[9],R[10],R[11],R[12],R[13],R[14],R[15]-4,gba->CPSR, opcode);
#endif
#endif
    printf("[A][%08x][N%dS%dC%dV%d] ", gba->REG[R15] - 8, 
				gba->CPSR>>31, (gba->CPSR>>30)&1, (gba->CPSR>>29)&1, (gba->CPSR>>28)&1);
	Dissembler_ARM_LUT[((opcode & 0x0FF00000) >> 16) | ((opcode >> 4) & 0xF)](gba, opcode);
	printf("\n");

    if (R[12] == 0x199) {
       flag = true;
    }
    if (flag) {
       DEBUG_SET_BREAKPOINT("");
    }
#endif
}

void printStateTHUMB(GBA* gba, uint16_t opcode) {
#ifdef DEBUG_TRACE_STATE
printf("[T][%08x][N%dS%dC%dV%d] ", gba->REG[R15] - 4, 
				gba->CPSR>>31, (gba->CPSR>>30)&1, (gba->CPSR>>29)&1, (gba->CPSR>>28)&1);
	Dissembler_THUMB_LUT[opcode >> 8](gba, opcode);
	printf("\n");

#ifdef DEBUG_PRINT_REGS
	uint32_t R[16];
	memcpy(&R, &gba->REG, sizeof(uint32_t)*16);
#ifdef DEBUG_LIMIT_REGS	
	printf("[R0:%08x|R1:%08x|R2:%08x|R12:%08x|R13:%08x]\n", R[0],R[1],R[2],R[12],R[13]);
#else
	uint32_t R[16];
	memcpy(&R, &gba->REG, sizeof(uint32_t)*16);

	printf("%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X cpsr: %08X | %08X: ", R[0],R[1],R[2],R[3],R[4],R[5],R[6],R[7],R[8],R[9],R[10],R[11],R[12],R[13],R[14],R[15]-2,gba->CPSR, opcode);

	Dissembler_THUMB_LUT[opcode >> 8](gba, opcode);
	printf("\n");
#endif	
#endif
#endif
}

#endif
