#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <gba/gba.h>
#include <gba/arm7tdmi.h>
#include <gba/debugGBA.h>


static void flushRefillPipeline(GBA*);
static inline void doInternalPrefetchARM(GBA* gba);
static void switchMode(GBA* gba, CPU_MODE newMode);
static bool checkCondition(GBA*, uint8_t);
static void requestAsyncException(GBA*, CPU_EXCEP);
static void triggerException(GBA* gba, CPU_EXCEP excep);
static void returnException(GBA*);
/* ---------------------- CPSR Functions ---------------------- */

static inline CPU_MODE CPSR_GetMode(GBA* gba) {
	return (CPU_MODE)(gba->CPSR & 0xF);
}

static inline void CPSR_SetMode(GBA* gba, CPU_MODE mode) {
	gba->CPSR &= ~0x1F;
	gba->CPSR |= mode;
}

static inline uint8_t CPSR_GetBit(GBA* gba, uint8_t bit) {
	return (gba->CPSR >> bit) & 1;
}

static inline void CPSR_SetBit(GBA* gba, uint8_t bit) {
	gba->CPSR |= 1 << bit;
}

static inline void CPSR_ClearBit(GBA* gba, uint8_t bit) {
	gba->CPSR &= ~(1 << bit);
}

static inline void CPSR_ModifyBit(GBA* gba, uint8_t bit, uint8_t value) {
	if (value) gba->CPSR |= 1 << bit;
	else gba->CPSR &= ~(1 << bit);
}

/* ------------------------------------------------------- */
static uint32_t barrelShifter(GBA* gba, uint8_t shiftType, uint32_t operand, uint8_t amount, uint8_t* carry) {
	/* Responsible for all barrel shifter operations for Data Processing and THUMB
	 * Calculates operand shifted by amount, with shift types LSL, LSR, ASR and ROR/RRX
	 * and also calculates carry flag
	 *
	 * 0 -> Logical Left Shift  	(LSL)
	 * 1 -> Logical Right Shift 	(LSR)
	 * 2 -> Arithmetic Right Shift 	(ASR)
	 * 3 -> Rotate Right            (ROR)
     * 4 -> Rotate Right Extended   (RRX)*
	 *
	 * Carry should be set to a default value before calling this function
	 * When no shift operation occurs, carry is unchanged. This is normally for LSL and LSR.
	 * For ASR and ROR, amount=0 is treated specially
	 *
	 * Shift Amount can be anything between 0-255
	 * */

	switch (shiftType) {
		/* Type of shift */
		case 0: {	// Logical Left
			if (amount == 0) return operand; 				// No shift yields unchanged carry?
			if (amount > 32) {
				*carry = 0;
				operand = 0;
			} else {
				*carry = (operand >> (32 - amount)) & 1;
				operand = ((uint64_t)operand << amount) & 0xFFFFFFFF;
			}
			break;
		}
		case 1: {	// Logical Right
			if (amount == 0) return operand;
			if (amount > 32) {
				*carry = 0;
				operand = 0;
			} else {
				*carry = (operand >> (amount - 1)) & 1;
				operand = ((uint64_t)operand >> amount) & 0xFFFFFFFF;
			}
			break;
		}
		case 2: { 	// Arithmetic Right
            if (amount == 0) return operand;
			if (amount > 32) amount = 32;
			uint8_t bit31 = operand >> 31;
			*carry = (operand >> (amount - 1)) & 1;
			operand = ((uint64_t)operand >> amount) & 0xFFFFFFFF;

			/* Set shifted in bits at left to 1, if bit 31 is set, otherwise they're 0 */
			if (bit31) operand |= (uint32_t)(0xFFFFFFFF << (32 - amount));
			break;
		}
		case 3: {	// Rotate Right	
			// Normal
            if (amount == 0) return operand;
			if (amount > 32) {
				amount %= 32;
				if (amount == 0) amount = 32;
			}

			*carry = (operand >> (amount - 1)) & 1;
			uint32_t shiftedOut = operand & (0xFFFFFFFF >> (32 - amount));
			operand = ((uint64_t)operand >> amount) & 0xFFFFFFFF;
			operand |= shiftedOut << (32 - amount);
			break;
		}
        case 4: {
			// Rotate Right Extended - RRX
            // Special barrel shifter function encoded by ROR #0 (only immediate)
            // No 'amount' parameter
			*carry = operand & 1;
			operand >>= 1;
			operand |= CPSR_GetBit(gba, FLG_C) << 31;
			break;
        }
	}

	return operand;
}

static uint8_t getVFlag(GBA* gba, uint32_t OP1, uint32_t OP2, uint32_t result) {
	/* Tests V flag for Data Processing and THUMB addition and subtraction */

	/* V flag test */
	if ((OP1 >> 31) == (OP2 >> 31)) {
		/* Change in sign means overflow into bit 31 */
		uint8_t sign = OP1 >> 31;
		if (sign != (result >> 31)) return 1;
	}

	return 0;
}

uint32_t twosComplementOffset(uint32_t base, uint32_t offset, uint8_t signBit) {
    /* Helper function to perform Twos complement signed value offset calculation
     * without causing sign miscalculations. Useful for branching */

	uint8_t sign = offset >> signBit & 1;

	if (sign) {
		// Convert 2s complement to an absolute positive integer (since we already know sign)
		offset = (~offset & ((1 << (signBit+1))-1))+1;
		// Subtract absolute value
		base -= offset;
	} else {
		// If sign is positive, we can directly add
		base += offset;
	}

    return base;
}

static void branchAndExchange(GBA* gba, uint32_t address) {
	/* Utility function to perform branch exchange */
	if (address & 1) {
		/* Switch to THUMB */
		address &= ~((uint32_t)1); 					// Clear lower bit to HW align
		CPSR_SetBit(gba, CPSR_T);
		gba->cpu_state = CPU_STATE_THUMB;
	} else {
		/* Switch to ARM */
		address &= ~((uint32_t)3); 					// Clear lower 2 bits to W align
		CPSR_ClearBit(gba, CPSR_T);
		gba->cpu_state = CPU_STATE_ARM;
	}

	gba->REG[R15] = address;
	flushRefillPipeline(gba);
}

/* ----------------- ARM Instruction Handlers ------------- */

static void BX(struct GBA* gba, uint32_t ins) {
	/* Branch and exchange */
	uint32_t content = gba->REG[ins & 0xF];
	branchAndExchange(gba, content);
}

static void B_BL(struct GBA* gba, uint32_t ins) {
	/* Branch and Branch with link implementation */
	if (ins >> 24 & 1) {
		/* Load R14 with return instruction address if L bit is set
		 * Note: We adjust for prefetch */
		gba->REG[R14] = gba->REG[R15] - 4;
	}

    /* 24 bit 2s complement becomes 26 bit when shifted by 2, this means sign bit is bit 25 */
	uint32_t offset = (ins & 0xFFFFFF) << 2;

    uint32_t jump = twosComplementOffset(gba->REG[R15], offset, 25);
    gba->REG[R15] = jump;

	flushRefillPipeline(gba);
}

static uint32_t getDataProcessing_RxOP2(GBA* gba, uint16_t OP2, uint8_t* carry) {
	/* Decodes Operand 2 for register operand data processing functions
	 * We calculate the shift */

	uint16_t reg = OP2 & 0xF;
	uint32_t data = gba->REG[reg];
    uint8_t shiftType = (OP2 >> 5) & 0b11;

	// Default carry - Unchanged

	uint8_t amount; 								// Amount to be shifted by

	if ((OP2 >> 4) & 1) {
		/* Shift amount stored in register */
		amount = gba->REG[OP2 >> 8] & 0xFF;
        if (reg == R15) {
		    /* If we're dealing with register specified operand 2 shift with PC as operand 1 or 2, 
             * then an extra internal cycle is consumed by the CPU, so we read PC+12*/

		    data += 4;
        }
	} else {
		/* Shift amount in immediate value */
		amount = OP2 >> 7;


        if ((shiftType == 1 || shiftType == 2) && amount == 0) {
            /* LSR/ASR with immediate shift amount = 0 is a very special case
             * Rather, an encoding in itself that sets shift amount to #32
             * Feeding 0 into LSR/ASR through a register DOES NOT create the same behaviour
             * and would treated like actual LSR/ASR #0
             *
             * LSL #0 does what its expected, i.e nothing but additionally preserves carry 
             */
            amount = 32;
        } else if (shiftType == 3 && amount == 0) {
            /* Special encoding for RRX, that is ROR #0 (immediate only) */
            return barrelShifter(gba, 4, data, 0, carry);
        }
	}

	uint32_t shifted = barrelShifter(gba, shiftType, data, amount, carry);
	return shifted;
}

static uint32_t getDataProcessing_ImmOP2(GBA* gba, uint16_t OP2, uint8_t* carry) {
	uint32_t Imm = (uint32_t)(OP2 & 0xFF);
	uint8_t rotate = ((OP2 >> 8) & 0xF) * 2;

    uint32_t result = barrelShifter(gba, 3, Imm, rotate, carry);

    /* This instruction uses the same barrel shifter, hence carry will be affected accordingly */
    /*
	uint32_t shiftedOut = Imm & (0xFFFFFFFF >> (32 - rotate));
	Imm >>= rotate;
	Imm |= shiftedOut << (32 - rotate);
    */

	return result;
}

static void dataProcessingLogical(struct GBA* gba, uint32_t ins) {
	/* Handles all logical instructions of data processing */

	uint8_t carry = CPSR_GetBit(gba, FLG_C);
	uint8_t S = (ins >> 20) & 1;
	uint8_t I = (ins >> 25) & 1;

    uint8_t OP1_REG = (ins >> 16) & 0xF;
	uint32_t OP1  = gba->REG[OP1_REG];
	uint32_t OP2  = I ? getDataProcessing_ImmOP2(gba, ins & 0xFFF, &carry) :
						getDataProcessing_RxOP2(gba, ins & 0xFFF, &carry);
	uint8_t DestReg = (ins >> 12) & 0xF;

    if (!I && (OP1_REG == R15) && (ins >> 4) & 1) {
        /* A special case when register specified shift is being used for operand 2
         * and operand 1 is R15. This causes an extra internal cycle before ALU processing
         * and hence PC+12 is read in OP1 instead of PC+8.
         *
         * This happens for R15 as operand 2 aswell which is handled in RxOP2 */
        OP1 += 4;
    }

	/* ---------------------------------------------------------- */
	uint8_t opcode = ins >> 21 & 0xF;
	uint32_t result = 0;
	bool testInstruction = false;

	switch (opcode) {
		case 0x0: {
			/* AND */
			result = OP1 & OP2;
			break;
		}
		case 0x1: {
			/* EOR */
			result = OP1 ^ OP2;
			break;
		}
		case 0x8: {
			/* TST */
			result = OP1 & OP2;
			testInstruction = true;
			break;
		}
		case 0x9: {
			/* TEQ */
			result = OP1 ^ OP2;
			testInstruction = true;
			break;
		}
		case 0xC: {
			/* ORR */
			result = OP1 | OP2;
			break;
		}
		case 0xD: {
			/* MOV */
			result = OP2;
			break;
		}
		case 0xE: {
			/* BIC */
			result = OP1 & ~OP2;
			break;
		}
		case 0xF: {
			/* MVN */
			result = ~OP2;
			break;
		}
	}

	/* ---------------------------------------------------------- */
	if (DestReg == R15) {
	 	/* If S flag is set, exception return is triggered
		 * If S flag is not set, result is written normally but CPSR is not changed
		 * Writing to R15 also flushes the pipeline */

		if (S && (gba->cpu_mode != CPU_MODE_USER && gba->cpu_mode != CPU_MODE_SYSTEM)) {
			returnException(gba);
		}

		if (!testInstruction) {
			gba->REG[R15] = result;
			flushRefillPipeline(gba);
		}
	} else {
		if (S) {
			CPSR_ModifyBit(gba, FLG_C, carry);
			CPSR_ModifyBit(gba, FLG_Z, result == 0);
			CPSR_ModifyBit(gba, FLG_N, result >> 31);
		}

		if (!testInstruction) gba->REG[DestReg] = result;
	}
}

static void dataProcessingArithmetic(struct GBA* gba, uint32_t ins) {
	/* Handles all arithmetic instructions of data processing */

	uint8_t x; 							/* Carry from barrel shifter is discarded */
	uint8_t S = (ins >> 20) & 1;
	uint8_t I = (ins >> 25) & 1;

    uint8_t OP1_REG = (ins >> 16) & 0xF;
	uint32_t OP1  = gba->REG[OP1_REG];
	uint32_t OP2  = I ? getDataProcessing_ImmOP2(gba, ins & 0xFFF, &x) :
						getDataProcessing_RxOP2(gba, ins & 0xFFF, &x);
	uint8_t DestReg = (ins >> 12) & 0xF;

    if (!I && (OP1_REG == R15) && (ins >> 4) & 1) {
        /* A special case when register specified shift is being used for operand 2
         * and operand 1 is R15. This causes an extra internal cycle before ALU processing
         * and hence PC+12 is read in OP1 instead of PC+8.
         *
         * This happens for R15 as operand 2 aswell which is handled in RxOP2 */
        OP1 += 4;
    }
	/* ---------------------------------------------------------- */
	uint8_t opcode = ins >> 21 & 0xF;
	uint32_t result = 0;
	bool testInstruction = false;

	uint8_t C = 0;

	switch (opcode) {
		case 0x2: {
			/* SUB */
			/* SUB can be treated as ADD with OP2 inverted with 1 added to it (in other words ADC
			 * with carry in forced to 1) */
			OP2 = ~OP2;
			result = OP1 + OP2 + 1;

			C = ((uint64_t)OP1 + (uint64_t)OP2 + 1) >> 32;
			break;
		}
		case 0x3: {
			/* RSB - Reverse SUB */
			OP1 = ~OP1;
			result = OP2 + OP1 + 1;

			C = ((uint64_t)OP1 + (uint64_t)OP2 + 1) >> 32;
			break;
		}
		case 0x4: {
			/* ADD */
			result = OP1 + OP2;

			C = ((uint64_t)OP1 + (uint64_t)OP2) >> 32;
			break;
		}
		case 0x5: {
			/* ADC */
			uint8_t carryInput = CPSR_GetBit(gba, FLG_C);

			result = OP1 + OP2 + carryInput;
			/* We dont account for carry Input when calculating signed overflow */

			C = ((uint64_t)OP1 + (uint64_t)OP2 + carryInput) >> 32;
			break;
		}
		case 0x6: {
			/* SBC */
			uint8_t carryInput = CPSR_GetBit(gba, FLG_C);

			OP2 = ~OP2;
			result = OP1 + OP2 + carryInput;

			C = ((uint64_t)OP1 + (uint64_t)OP2 + carryInput) >> 32;
			break;
		}
		case 0x7: {
			/* RSC - Reverse SBC */
			uint8_t carryInput = CPSR_GetBit(gba, FLG_C);

			OP1 = ~OP1;
			result = OP2 + OP1 + carryInput;

			C = ((uint64_t)OP1 + (uint64_t)OP2 + carryInput) >> 32;
			break;
		}
		case 0xA: {
			/* CMP - compare same as SUB without result stored */
			OP2 = ~OP2;
			result = OP1 + OP2 + 1;

			C = ((uint64_t)OP1 + (uint64_t)OP2 + 1) >> 32;
			testInstruction = true;
			break;
		}
		case 0xB: {
			/* CMN - compare same as ADD without result stored */
			result = OP1 + OP2;

			C = ((uint64_t)OP1 + (uint64_t)OP2) >> 32;
			testInstruction = true;
			break;
		}
	}

	/* ---------------------------------------------------------- */
	if (DestReg == R15) {
		/* If S flag is set, exception return is triggered.
		 * If S flag is not set, result is written normally but CPSR is not changed
		 * Writing to R15 also flushes the pipeline */
		if (S && (gba->cpu_mode != CPU_MODE_USER && gba->cpu_mode != CPU_MODE_SYSTEM)) {
			returnException(gba);
		}

		if (!testInstruction) {
			gba->REG[R15] = result;
			flushRefillPipeline(gba);
		}
	} else {
		if (S) {
			/* C flag is handled differently by accounting carry input for ADC/SBC/RSC
			 * V flag is handled the same for all operations (carry input is not accounted for) */
			CPSR_ModifyBit(gba, FLG_V, getVFlag(gba, OP1, OP2, result));
			CPSR_ModifyBit(gba, FLG_C, C);
			CPSR_ModifyBit(gba, FLG_Z, result == 0);
			CPSR_ModifyBit(gba, FLG_N, result >> 31);
		}

		if (!testInstruction) gba->REG[DestReg] = result;
	}
}

static void MRS(struct GBA* gba, uint32_t ins) {
	/* Move xPSR to Register */
	uint8_t Rd = ins >> 12 & 0xF;
	uint8_t source = ins >> 22 & 1;

	// R15 as source is not allowed
	if (Rd == R15) return;
	if (source == 0) {
		/* CPSR to Register */
		gba->REG[Rd] = gba->CPSR;
	} else {
		/* SPSR <mode> to Register
		 * If SPSR doesnt exist for current mode then ignore */
		if (gba->cpu_mode == CPU_MODE_USER || gba->cpu_mode == CPU_MODE_SYSTEM) return;
		gba->REG[Rd] = gba->SPSR;
	}
}

static void MSR(struct GBA* gba, uint32_t ins) {
	/* Move Register/Imm to xPSR */
	uint8_t C = (ins >> 16) & 1;
	uint8_t I = ins >> 25 & 1;
	uint8_t toSPSR  = ins >> 22 & 1;
	uint32_t data = 0;

    /* Write to SPSR only makes sense in correct mode */
	if (toSPSR && (gba->cpu_mode == CPU_MODE_USER || gba->cpu_mode == CPU_MODE_SYSTEM)) return;
    /* Control bit writing is disabled for USER mode */
    if (gba->cpu_mode == CPU_MODE_USER) {C = 0;}

	if (I) {
        /* Rotation happens in immediate read, which could modify carry */ 
        uint8_t carry = CPSR_GetBit(gba, FLG_C);
		data = getDataProcessing_ImmOP2(gba, ins & 0xFFF, &carry);
        CPSR_ModifyBit(gba, FLG_C, carry);
	} else {
		/* Register
		* Destination R15 not allowed
        */
		if ((ins & 0xF) == R15) return;
		data = gba->REG[ins & 0xF];
	}

    if (C) {
        /* Flags and Control Enabled */
        data &= 0xFF0000FF;
        /* Write to CPSR/SPSR T bit is allowed here, but doesnt do any mode switch */
        if (toSPSR) {
            gba->SPSR = data;
        } else {
            gba->CPSR = data;
            /* Since control bits can be written, mode swap is possible */
            switchMode(gba, data & 0x1F);
        }
    } else {
        /* Only Flags enabled */
        data &= 0xFF000000;
        if (toSPSR) {
            gba->SPSR = data | (gba->SPSR & 0xFF);
        } else {
            gba->CPSR = data | (gba->CPSR & 0xFF);
        }
    }	
}

static void MUL_MLA(struct GBA* gba, uint32_t ins) {
	/* MUL and MLA */
	uint8_t S = ins >> 20 & 1;
	uint8_t A = ins >> 21 & 1;

	uint8_t Rd = ins >> 16 & 0xF;
	uint8_t Rn = ins >> 12 & 0xF;
	uint8_t Rs = ins >> 8 & 0xF;
	uint8_t Rm = ins & 0xF;

	if ((Rd+1|Rn+1|Rs+1|Rm+1) >> 4) return; 		/* R15 is not allowed as operand or dest*/

	uint32_t result;
	if (A) {
		/* Multiply Accumulate (MLA) */
		result = gba->REG[Rm] * gba->REG[Rs] + gba->REG[Rn];
	} else {
		/* Multiply only (MUL) */
		result = gba->REG[Rm] * gba->REG[Rs];
	}

	gba->REG[Rd] = result;

	if (S) {
		CPSR_ModifyBit(gba, FLG_Z, result == 0);
		CPSR_ModifyBit(gba, FLG_N, result >> 31);
		/* C Flag is set to a meaningless value (we preserve it for now) */
	}
}

static void MULL_MLAL(struct GBA* gba, uint32_t ins) {
	/* Multiply Long and Multiply Accumulate Long */
	uint8_t U = ins >> 22 & 1;
	uint8_t A = ins >> 21 & 1;
	uint8_t S = ins >> 20 & 1;

	uint8_t RdHi = ins >> 16 & 0xF;
	uint8_t RdLow = ins >> 12 & 0xF;
	uint8_t Rs = ins >> 8 & 0xF;
	uint8_t Rm = ins & 0xF;

	if (RdLow == RdHi || RdHi == Rm || RdLow == Rm) return;
	if ((RdLow+1|RdHi+1|Rs+1|Rm+1) >> 4) return; 		/* R15 is not allowed as operand or dest*/

	uint64_t result;

	if (U) {
		/* Signed   - SMULL, SMLAL */
		int32_t m = (int32_t)gba->REG[Rm];
		int32_t s = (int32_t)gba->REG[Rs];

		/* We convert U32 values to S32, then sign extend that to S64, and at last convert S64 to U64 */
		if (A) {
			result = (uint64_t)((int64_t)m * (int64_t)s + (int64_t)(gba->REG[RdLow] | (uint64_t)gba->REG[RdHi] << 32));
		} else {
			result = (uint64_t)((int64_t)m * (int64_t)s);
		}
	} else {
		/* Unsigned - UMULL, UMLAL */
		if (A) {
			result = (uint64_t)gba->REG[Rm] * (uint64_t)gba->REG[Rs] + (gba->REG[RdLow] | ((uint64_t)gba->REG[RdHi] << 32));
		} else {
			result = (uint64_t)gba->REG[Rm] * (uint64_t)gba->REG[Rs];
		}
	}

	gba->REG[RdLow] = result & 0xFFFFFFFF;
	gba->REG[RdHi]  = result >> 32;

	if (S) {
		CPSR_ModifyBit(gba, FLG_Z, result == 0);
		CPSR_ModifyBit(gba, FLG_N, result >> 63);
		/* C and V are preserved */
	}
}

static void LDR_STR(struct GBA* gba, uint32_t ins) {
	/* Load/Store single byte/word from/to memory */
	uint8_t I = ins >> 25 & 1;
	uint8_t P = ins >> 24 & 1;
	uint8_t U = ins >> 23 & 1;
	uint8_t B = ins >> 22 & 1;
	uint8_t W = ins >> 21 & 1;
	uint8_t L = ins >> 20 & 1;

	uint8_t Rn = ins >> 16 & 0xF;
	uint8_t Rd = ins >> 12 & 0xF;
	uint8_t carry = CPSR_GetBit(gba, FLG_C);

	/* R15 Restriction checking */
	if (Rn == R15 && W) return;
	if (I && ((ins & 0xF) == R15)) return;

	/* We clear out bit 4 of offset as it specifies shift value through a register which
	 * is not supported */
	uint32_t offset = I ? getDataProcessing_RxOP2(gba, ins & 0xFEF, &carry) : ins & 0xFFF;
	uint32_t base   = gba->REG[Rn];

	if (P) {
		/* Pre - Add offset before transfer */
		base += U ? offset : -offset;	
	}

#define WRITEBACK() if (!P) gba->REG[Rn] = base + (U ? offset : -offset); \
	                else if (W) gba->REG[Rn] = base;

	if (L) {
		/* LDR */

        /* Writeback happens before data transfer to register for LDR */
        WRITEBACK();

		if (B) {
			/* LDR BYTE */
			gba->REG[Rd] = busRead(gba, base, WIDTH_8);
		} else {
			/* LDR WORD
			 * If address is not word aligned, the data is rotated such that
			 * the addressed byte always occupies first byte on the register */
			uint8_t alignOffset = base & 0x3; 			/* Can be 0-3 */
			uint32_t word = busRead(gba, base & ~0x3, WIDTH_32);
            uint32_t trailing = word & ((1 << (alignOffset*8))-1);

            word >>= alignOffset * 8;
            word |= trailing << (32-(alignOffset*8));

			gba->REG[Rd] = word;
		}

        if (Rd == R15) {
            /* Preserve half word alignment always */
            //if (gba->cpu_state == CPU_STATE_ARM) gba->REG[R15] &= ~0b11;
            gba->REG[R15] &= ~1;

            flushRefillPipeline(gba);
        }
	} else {
		/* STR */
		uint32_t source = gba->REG[Rd];
		if (Rd == R15) {source += gba->cpu_state == CPU_STATE_ARM ? 4 : 2;} 		/* PC+12 */
		if (B) {
			/* STR BYTE */
			busWrite(gba, base, source & 0xFF, WIDTH_8);
		} else {
			/* STR WORD
			 * Address is always treated as word aligned */
			busWrite(gba, base & ~0x3, source, WIDTH_32);
		}


        /* Writeback happens after data write for STR */
        WRITEBACK();
	}
#undef WRITEBACK
}

static void LDR_STR_H_SB_SH(struct GBA* gba, uint32_t ins) {
	/* Handle LDR/STR of Unsigned Halfwords, Signed bytes and Signed Halwords (sign extension) */

	uint8_t P = ins >> 24 & 1;
	uint8_t U = ins >> 23 & 1;
	uint8_t I = ins >> 22 & 1;
	uint8_t W = ins >> 21 & 1;
	uint8_t L = ins >> 20 & 1;

	uint8_t Rn = ins >> 16 & 0xF;
	uint8_t Rd = ins >> 12 & 0xF;

	/* R15 Restriction checking */
	if (Rn == R15 && W) return;
	if (!I && ((ins & 0xF) == R15)) return;

	uint32_t offset = I ? ((ins >> 4 & 0xF0) | (ins & 0xF)) : gba->REG[ins & 0xF];
	uint32_t base = gba->REG[Rn];

	if (P) {
		/* Pre - Add offset before transfer */
		base += U ? offset : -offset;
	}

    /* Writeback is done before memory read for LD and after memory write for ST */
#define WRITEBACK() if (!P) gba->REG[Rn] = base + (U ? offset : -offset); \
	                else if (W) gba->REG[Rn] = base;


	switch (ins >> 5 & 0x3)	{
		/* 0 will never occur as its decoded as an SWP */
		case 1: {
			/* Operate with unsigned halfwords (LDRH/STRH)
			 * The addresses should always be halfword aligned or otherwise it causes
			 * unpredictable reads/writes on the GBA, we tackle that issue by force clearing b0 */
			if (L) {
				/* LDRH - if bit 0 is set, we rotate the half word such that the indexed byte gets
                 * loaded at the least signifant byte of the register
                 * Following from how word misalignment rotation works in LDR */ 

                WRITEBACK();

				uint32_t data = busRead(gba, base & ~1, WIDTH_16);

                if (base & 1) {
                    uint8_t temp = data & 0xFF;
                    data >>= 8;
                    data |= temp << 24;
                }
                
                gba->REG[Rd] = data;
                if (Rd == R15) flushRefillPipeline(gba);
			} else {
				/* STRH - Bit 0 is cleared for halfword alignment */ 
				/* PC+12 if Rd == R15 */
                uint8_t pcOffset = gba->cpu_state == CPU_STATE_ARM ? 4 : 2;
				if (Rd == R15) busWrite(gba, base & ~1, (gba->REG[Rd] + pcOffset) & 0xFFFF, WIDTH_16);
				else busWrite(gba, base & ~1, gba->REG[Rd] & 0xFFFF, WIDTH_16);

                WRITEBACK(); 
			}
			break;
		}
		case 2: {
			/* Operate with signed bytes (LDRSB)
			 * Bit 7 is repeated across bits 31-8 of register to preserve sign */
            WRITEBACK(); 

			uint32_t value = busRead(gba, base, WIDTH_8);
			if (value >> 7 & 1) value |= 0xFFFFFF00;
			gba->REG[Rd] = value;
            if (Rd == R15) flushRefillPipeline(gba);
			break;
		}
		case 3: {
			/* Signed Halfwords (LDRSH)
			 * Bit 15 is repeated across bits 31-16 of register to preserve sign */
            WRITEBACK(); 

			uint32_t data = busRead(gba, base & ~1, WIDTH_16);
            uint8_t b15 = data >> 15 & 1;

            if (base & 1) {
                /* If address is not halfword aligned, do LDR style rotate 
                 * but then set the preceding bits to bit 15 */
                data >>= 8;

                if (b15) data |= 0xFFFFFF00;
                else data &= 0xFF;
            } else {
                /* If address is halfword aligned, we can just repeat bit 15 on the rest of
                 * the bits to the left */
                if (b15) data |= 0xFFFF0000;
                else data &= 0xFFFF;
            }

			gba->REG[Rd] = data;
            if (Rd == R15) flushRefillPipeline(gba);
			break;
		}
	}
#undef WRITEBACK
}

static void LDM_STM(struct GBA* gba, uint32_t ins) {
	/* LDM/STM - Block data transfer
	 *
	 * Write back is done at the very end, or not done if base is in register list */
	uint8_t P = ins >> 24 & 1;
	uint8_t U = ins >> 23 & 1;
	uint8_t S = ins >> 22 & 1;
	uint8_t W = ins >> 21 & 1;
	uint8_t L = ins >> 20 & 1;

	uint8_t Rn = ins >> 16 & 0xF;
	uint16_t regList = ins & 0xFFFF;
	uint32_t base = gba->REG[Rn];
    uint32_t baseInitial = base;

	if (Rn == R15) return;
	if (S && (gba->cpu_mode == CPU_MODE_USER || gba->cpu_mode == CPU_MODE_SYSTEM)) {
		printf("WARNING LDM/STM (S) in USR/SYS mode\n");
		return;
	}

	/* Find out number of registers involved and also store them in an array */
	uint8_t REG_COUNT = 0;
	uint8_t REGS[16];

	memset(&REGS, 0, 16);

	for (int i = 0; i < 16; i++) {
		if (regList >> i & 1) { 
            REGS[REG_COUNT] = i;
			REG_COUNT++;
	    }
    }

	/* Check the reglist from lowest to highest, and transfer the ones which are enabled */
	if (L) {
		/* LDM */
		bool UBT = S && !(regList >> 15 & 1); 					// User Bank Transfer
        bool abortWriteBack = false;

		for (int i = 0; i < REG_COUNT; i++) {
			uint8_t REG = REGS[i];
			uint32_t* readIn = &gba->REG[REG];

			if (UBT) {
				/* User bank transfer with LDM */
				if (gba->cpu_mode == CPU_MODE_FIQ && (REG >= 8 || REG <= 14)) {
					/* Transfer from USR register */
					readIn = &gba->REG_SWAP[REG-8];
				} else if (REG == 13 || REG == 14) {
					/* Transfer from USR register */
					readIn = &gba->REG_SWAP[5 + (REG-13)];
				}
			}

			/* In case REG is part of UBT, the writeback doesnt happen anyway */
			if (REG == Rn) abortWriteBack = true;

			if (U) {
				/* Add offset */
				uint32_t address = P ? base + 4 : base; 			// Pre/Post
				address &= ~3;

				*readIn = busRead(gba, address, WIDTH_32);
				base += 4;
			} else {
				/* Subtract offset */
				uint32_t reference = P ? ((base-4)+i*4)-(REG_COUNT-1)*4 : (base+i*4)-(REG_COUNT-1)*4;
				uint32_t address = reference + i*4;

				address &= ~3;		// Pre/Post

				*readIn = busRead(gba, address, WIDTH_32);
				base -= 4;
			}

			/* Flush pipeline if R15 was loaded into // Handle S bit */
			if (REG == R15) {
				if (S && gba->cpu_mode != CPU_MODE_USER && gba->cpu_mode != CPU_MODE_SYSTEM) {
					/* CPSR = SPSR_mode */
                    returnException(gba);
                }

                /* Preserve half word alignment */
                //if (gba->cpu_state == CPU_STATE_ARM) gba->REG[R15] &= ~0b11;
                gba->REG[R15] &= ~1;

				flushRefillPipeline(gba);
			}
		}

		/* Write back - In the end, do write back to Rn if it was not overwritten by LDR
		 * and W bit is set, and only if User Bankk Transfer is not taking place
		 * Note: Lower 2 bits of base are preserved, they arent interpreted by cpu differently
		 * but dont get cleared either */
		if (!UBT && W && !abortWriteBack) gba->REG[Rn] = base;

	} else {
		/* STM */
		bool UBT = S; 				// User Bank Transfer

		for (int i = 0; i < REG_COUNT; i++) {
			uint8_t REG = REGS[i];
			uint32_t data = gba->REG[REG];
			bool transferringBanked = false;

			if (UBT) {
				/* User Bank Transfer i.e transfer registers from user mode bank */
				if (gba->cpu_mode == CPU_MODE_FIQ && (REG >= 8 || REG <= 14)) {
					/* Transfer from USR register */
					data = gba->REG_SWAP[REG-8];
					transferringBanked = true;
				} else if (REG == 13 || REG == 14) {
					/* Transfer from USR register */
					data = gba->REG_SWAP[5 + (REG-13)];
					transferringBanked = true;
				}
			}

			if (REG == R15) data += gba->cpu_state == CPU_STATE_ARM ? 4 : 2; 			// PC+12 

            /* If Rn is being transferred in STM and is not a banked register, and
			 * is the first in order, the value stored will be the initial Rn value (or base value),
			 * otherwise the value stored becomes the updated address (writeback value)
             * that would be the final value */

			if (!transferringBanked && REG == Rn && i != 0) {
                data = U ? baseInitial+REG_COUNT*4 : baseInitial-REG_COUNT*4;
            }

			if (U) {
				/* Add offset */
				uint32_t address = P ? base + 4 : base; 			// Pre/Post
				address &= ~3;	

				busWrite(gba, address, data, WIDTH_32);
				base += 4;
			} else {
				/* Subtract offset */
				uint32_t reference = P ? ((base-4)+i*4)-(REG_COUNT-1)*4 : (base+i*4)-(REG_COUNT-1)*4;
				uint32_t address = reference + i*4;
				address &= ~3;		// Pre/Post

				busWrite(gba, address, data, WIDTH_32);
				base -= 4;
			}
		}

        /* Writeback at the end of STM if required */
        if (!UBT && W) gba->REG[Rn] = base;
	}

    /* ARMv4 weird behaviour for empty reglist 
     * Is R15 stored throughout base to base +- 0x40? for STM?
     * Yes, also exactly following pre and post behaviour for increment
     * 
     * R15 also seems to be loaded to [base] for LDM
     */
   
    if (REG_COUNT == 0) {
        /* Load/Store R15 from/to base address */
        if (L) {
            gba->REG[R15] = busRead(gba, base, WIDTH_32);
            flushRefillPipeline(gba);
        } else {
            /* PC+12 seems to be read here according to arm.gba */
            uint8_t pcOffset = gba->cpu_state == CPU_STATE_ARM ? 4 : 2;
            for (int i=0; i<16; i++) {
                uint32_t address;
                if (P) address = U ? base+4*(i+1) : base-4*(i+1);
                else address = U ? base+4*i : base-4*i;
                busWrite(gba, address, gba->REG[R15]+pcOffset, WIDTH_32);
            }
        }

        /* When writeback is enabled or forced, we do +-0x40 on the base register
         * as if the reglist was full and all registers were iterated over */
        if (!P || W) {
            gba->REG[Rn] += (U ? 0x40 : -0x40);
        }
    }
}

static void SWP(struct GBA* gba, uint32_t ins) {
	/* SWAP Byte/Word */
	uint8_t B = ins >> 22 & 1;
	uint8_t Rn = ins >> 16 & 0xF;
	uint8_t Rd = ins >> 12 & 0xF;
	uint8_t Rm = ins & 0xF;

	if (Rn == R15 || Rd == R15 || Rm == R15) return;

	uint32_t base = gba->REG[Rn];

	if (B) {
		/* Swap byte */
		uint8_t old = busRead(gba, base, WIDTH_8);
		busWrite(gba, base, gba->REG[Rm] & 0xFF, WIDTH_8);
		gba->REG[Rd] = old;
	} else {
		/* Swap word */
		uint32_t old = busRead(gba, base & ~3, WIDTH_32);
		busWrite(gba, base & ~3, gba->REG[Rm], WIDTH_32);

		/* Rotate Word for unaligned address just like LDR */
		uint8_t alignOffset = base & 3; 			/* Can be 0-3 */
        uint32_t shifted = old & ((1 << (alignOffset * 8)) - 1);

        old >>= alignOffset * 8;
        old |= shifted << (32 - (alignOffset * 8));

		gba->REG[Rd] = old;
	}
}


static void SWI(struct GBA* gba, uint32_t ins) {
	/* Triggers the SWI exception */
    triggerException(gba, CPU_EXCEP_SWI);
}

static void Undefined_ARM(struct GBA* gba, uint32_t ins) {
    /* Doesnt trigger the UNDEFINED exception (for now) */
    printf("Instruction: %08x is an undefined ARM instruction\n", ins);
}

static void Unimplemented_ARM(struct GBA* gba, uint32_t ins) {
	printf("Instruction: %08x is unimplemented or an unsupported co-processor ARM instruction\n", ins);
}

/* -------------- THUMB Instruction Handlers -------------- */

static void LSL_LSR_ASR(struct GBA* gba, uint16_t ins) {
	/* Move shifted lo-register (Logical Left/Right Shift and Arithmetic Right Shift) */

	uint8_t opcode = ins >> 11 & 0b11;
	uint8_t offset = ins >> 6 & 0b11111;
	uint8_t Rs = ins >> 3 & 0b111;
	uint8_t Rd = ins & 0b111;

	uint8_t carry = CPSR_GetBit(gba, FLG_C);
	uint32_t data = gba->REG[Rs];

	if (opcode == 3) return; 				// Invalid Encoding

    if ((opcode == 1 || opcode == 2) && offset == 0) {
        /* LSR/ASR #0 special case */
        offset = 32;
    }
	uint32_t shifted = barrelShifter(gba, opcode, data, offset, &carry);
	gba->REG[Rd] = shifted;

	/* Set CPSR */
	CPSR_ModifyBit(gba, FLG_C, carry);
	CPSR_ModifyBit(gba, FLG_N, shifted >> 31);
	CPSR_ModifyBit(gba, FLG_Z, shifted == 0);
}

static void ADD_SUB(struct GBA* gba, uint16_t ins) {
	uint8_t I = ins >> 10 & 1;
	uint8_t SUB = ins >> 9 & 1;
	uint8_t Rs = ins >> 3 & 0b111;
	uint8_t Rd = ins & 0b111;

	uint32_t OP1 = gba->REG[Rs];
	uint32_t OP2 = I ? ins >> 6 & 0b111 : gba->REG[ins >> 6 & 0b111];

	uint8_t C = 0;
	uint32_t result = 0;

	if (SUB) {
		/* SUB */
		/* SUB can be treated as ADD with OP2 inverted with 1 added to it (in other words ADC
		 * with carry in forced to 1) */
		OP2 = ~OP2;
		result = OP1 + OP2 + 1;

		C = ((uint64_t)OP1 + (uint64_t)OP2 + 1) >> 32;
	} else {
		/* ADD */
		result = OP1 + OP2;

		C = ((uint64_t)OP1 + (uint64_t)OP2) >> 32;
	}

	gba->REG[Rd] = result;

	CPSR_ModifyBit(gba, FLG_V, getVFlag(gba, OP1, OP2, result));
	CPSR_ModifyBit(gba, FLG_C, C);
	CPSR_ModifyBit(gba, FLG_Z, result == 0);
	CPSR_ModifyBit(gba, FLG_N, result >> 31);
}

static void MOV_CMP_ADD_SUB_Imm(struct GBA* gba, uint16_t ins) {
	uint8_t opcode = ins >> 11 & 0b11;
	uint8_t Rd = ins >> 8 & 0b111;
	uint32_t offset = ins & 0xFF;

	uint32_t result = 0;
	uint32_t OP1 = gba->REG[Rd];
	uint8_t C = CPSR_GetBit(gba, FLG_C);

	switch (opcode) {
		case 0: {
			/* MOV */
			result = offset;
			gba->REG[Rd] = result;
			break;
		}
		case 1: {
			/* CMP - compare same as SUB without result stored */
			offset = ~offset;
			result = OP1 + offset + 1;

			C = ((uint64_t)OP1 + (uint64_t)offset + 1) >> 32;
			break;
		}
		case 2: {
			/* ADD */
			result = OP1 + offset;

			C = ((uint64_t)OP1 + (uint64_t)offset) >> 32;
			gba->REG[Rd] = result;
			break;
		}
		case 3: {
			/* SUB */
			offset = ~offset;
			result = OP1 + offset + 1;

			C = ((uint64_t)OP1 + (uint64_t)offset + 1) >> 32;
			gba->REG[Rd] = result;
			break;
		}
	}

	if (opcode != 0) {
		/* Set flags for Data Processing Arithmetic specifically
		 * (these flags dont get set for data processing logical) */

		CPSR_ModifyBit(gba, FLG_V, getVFlag(gba, OP1, offset, result));
		CPSR_ModifyBit(gba, FLG_C, C);
	}

	CPSR_ModifyBit(gba, FLG_Z, result == 0);
	CPSR_ModifyBit(gba, FLG_N, result >> 31);
}

static void ALU(struct GBA* gba, uint16_t ins) {
	/* Performs basic Logical/Arithmetic ALU operations on Lo register pairs
	 *
	 * AND, EOR, LSL, LSR, ASR, ROR, TST, ORR, BIC, MVN
	 * ADC, SBC, NEG, CMP, CMN, MUL */
	uint8_t opcode = ins >> 6 & 0xF;
	uint8_t Rs = ins >> 3 & 0b111;
	uint8_t Rd = ins & 0b111;

	uint32_t OP1 = gba->REG[Rd];
	uint32_t OP2 = gba->REG[Rs];
	uint32_t result = 0;
	uint8_t carry = CPSR_GetBit(gba, FLG_C);
	bool testInstruction = false;
	bool setV = false;

	/* In normal cases when operand 2 is a register, the barrel shifter can cause a carry
	 * to occur and thus the C flag gets set. However in the THUMB subset, the barrel shifter
	 * is only used for LSL, LSR, ASR and ROR. So in all other logical cases the carry flag is
	 * unchanged.
	 *
	 * For arithmetic cases, the carry flag is always manually set and is never left unchanged */
	switch (opcode) {
		case 0: {
			/* AND */
			result = OP1 & OP2;
			break;
		}
		case 1: {
			/* EOR */
			result = OP1 ^ OP2;
			break;
		}
		case 2: {
			/* MOV Rd, LSL Rs
			 * Logical Left or LSL
			 *
			 * Since lower byte of register is being used as shift count,
			 * the number can be greater than 32 */
			result = barrelShifter(gba, 0, OP1, OP2 & 0xFF, &carry);
			break;
		}
		case 3: {
			/* MOV Rd, LSR Rs
			 * Logical Right or LSR */
			result = barrelShifter(gba, 1, OP1, OP2 & 0xFF, &carry);
			break;
		}
		case 4: {
			/* MOV Rd, ASR Rs
			 * Arithmetic Right or ASR */
			result = barrelShifter(gba, 2, OP1, OP2 & 0xFF, &carry);
			break;
		}
		case 7: {
			/* MOV Rd, ROR Rs
			 * Rotate Right or ROR */
			result = barrelShifter(gba, 3, OP1, OP2 & 0xFF, &carry);
			break;
		}
		case 8: {
			/* TST - AND but no result set */
			result = OP1 & OP2;
			testInstruction = true;
			break;
		}
		case 12: {
			/* ORR */
			result = OP1 | OP2;
			break;
		}
		case 14: {
			/* BIC */
			result = OP1 & ~OP2;
			break;
		}
		case 15: {
			/* MVN */
			result = ~OP2;
			break;
		}

		case 5: {
			/* ADC */
			uint8_t carryInput = CPSR_GetBit(gba, FLG_C);

			result = OP1 + OP2 + carryInput;
			/* We dont account for carry Input when calculating signed overflow */

			carry = ((uint64_t)OP1 + (uint64_t)OP2 + carryInput) >> 32;
			setV = true;
			break;
		}
		case 6: {
			/* SBC */
			uint8_t carryInput = CPSR_GetBit(gba, FLG_C);

			OP2 = ~OP2;
			result = OP1 + OP2 + carryInput;

			carry = ((uint64_t)OP1 + (uint64_t)OP2 + carryInput) >> 32;
			setV = true;
			break;
		}
		case 9: {
			/* NEG - Same as RSB Rd, Rs, #0 or Rd = -Rs */
			OP2 = ~OP2;
            OP1 = 0;

			result = OP2 + 1;

			carry = ((uint64_t)OP2 + 1) >> 32;
			setV = true;
			break;
		}
		case 10: {
			/* CMP -> SUB test for Lo Registers */
			OP2 = ~OP2;
			result = OP1 + OP2 + 1;

			carry = ((uint64_t)OP1 + (uint64_t)OP2 + 1) >> 32;
			testInstruction = true;
			setV = true;
			break;
		}
		case 11: {
			/* CMN -> ADD test for Hi Registers */
			result = OP1 + OP2;

			carry = ((uint64_t)OP1 + (uint64_t)OP2) >> 32;
			testInstruction = true;
			setV = true;
			break;
		}
		case 13: {
			/* MUL - Technically not part of ALU but is part of this encoding and suits this
			 * category best
			 * Note: Also, operand restrictions should apply but we arent checking for now */
			result = OP1 * OP2;
			break;
		}
	}

	if (!testInstruction) gba->REG[Rd] = result;

	if (setV) CPSR_ModifyBit(gba, FLG_V, getVFlag(gba, OP1, OP2, result));
	CPSR_ModifyBit(gba, FLG_C, carry);
	CPSR_ModifyBit(gba, FLG_Z, result == 0);
	CPSR_ModifyBit(gba, FLG_N, result >> 31);
}

static void HIREG_OPS_BX(struct GBA* gba, uint16_t ins) {
	/* This encoding is used to perform operations on the Hi registers in thumb mode
	 * Possible configurations are Hi-Lo and Hi-Hi source and destination. Lo-Lo is undefined
	 * for 3 of the 4 opcodes.
	 *
	 * The H1 and H2 bits are just used to extend the destination and source respectively
	 * to support Hi Indexing */

	uint8_t opcode = ins >> 8 & 0b11;
	uint8_t H1 = ins >> 7 & 1;
	uint8_t H2 = ins >> 6 & 1;
	uint8_t Rs = (H2 << 3) | (ins >> 3 & 0b111);
	uint8_t Rd = (H1 << 3) | (ins & 0b111);

	/* Undefined behaviour for H1 = H2 = 0 when using with opcodes CMP, ADD and MOV */
	if ((H1 + H2 == 0) && opcode != 3) return;

	uint32_t OP1 = gba->REG[Rd];
	uint32_t OP2 = gba->REG[Rs];
	bool testInstruction = false;

	switch (opcode) {
		case 0: {
			/* ADD */
			gba->REG[Rd] = OP1 + OP2;
			break;
		}
		case 1: {
			/* CMP - Sets CPSR */
			OP2 = ~OP2;
			uint32_t result = OP1 + OP2 + 1;
			uint8_t C = ((uint64_t)OP1 + (uint64_t)OP2 + 1) >> 32;

			CPSR_ModifyBit(gba, FLG_V, getVFlag(gba, OP1, OP2, result));
			CPSR_ModifyBit(gba, FLG_C, C);
			CPSR_ModifyBit(gba, FLG_Z, result == 0);
			CPSR_ModifyBit(gba, FLG_N, result >> 31);

			testInstruction = true;
			break;
		}
		case 2: {
			/* MOV */
			gba->REG[Rd] = OP2;
			break;
		}
		case 3: {
			/* Branch And Exchange
			 * H2 = 0 for Lo Register Branch
			 * H2 = 1 for Hi Register Branch
			 * H1 does not affect the result */
			branchAndExchange(gba, OP2);
			return; 									/* No need for another flush */
		}
	}

	if (Rd == R15 && !testInstruction) {
        /* PC should always be halfword aligned */
        gba->REG[R15] &= ~1;
        flushRefillPipeline(gba);
    }
}

static void PC_Relative_Load(struct GBA* gba, uint16_t ins) {
    uint8_t Rd = (ins >> 8) & 0b111;
    uint16_t Imm = (ins & 0xFF) << 2;   /* Construct 10 bit word aligned address from 8 bit Imm */

    /* The read from PC yields address of this instruction + 4, naturally via the pipeline */
    /* Internal cycles (if any) dont seem to be causing a PC+6 read instead of PC+4, need to understand if there is a definite pattern to when this happens or is it arbitrary */
    /* We force bit 1 to 0 for word alignment of the final address before dereferencing */
    uint32_t address = (gba->REG[R15] + Imm) & ~0b10;
    uint32_t word = busRead(gba, address, WIDTH_32);
    gba->REG[Rd] = word;

}

static void LDR_STR_REG_OFFSET(struct GBA* gba, uint16_t ins) {
    /* Transcode to equivalent ARM instruction */
    uint8_t Rd = ins & 0b111;
    uint8_t Rb = (ins >> 3) & 0b111;
    uint8_t Ro = (ins >> 6) & 0b111;
    
    uint8_t B = (ins >> 10) & 1;
    uint8_t L = (ins >> 11) & 1;

    uint32_t arm = 0b111001111000 << 20;
    
    if (L) arm |= 1 << 20;
    if (B) arm |= 1 << 22;

    arm |= (Rb << 16) | (Rd << 12);
    arm |= Ro;

    LDR_STR(gba, arm);
}

static void LDR_STR_H_SB_SH_THUMB(struct GBA* gba, uint16_t ins) {
    uint8_t Rd = ins & 0b111;
    uint8_t Rb = (ins >> 3) & 0b111;
    uint8_t Ro = (ins >> 6) & 0b111;

    uint8_t S = (ins >> 10) & 1;
    uint8_t H = (ins >> 11) & 1;


    uint32_t arm = 0b111000011000 << 20;
    arm |= (Rb << 16) | (Rd << 12);
    arm |= 0b1001 << 4;
    arm |= Ro;

    if (S) {
        arm |= (1 << 6);
        if (H) arm |= (1 << 5);
    } else {
        arm |= (1 << 5);
        if (H) arm |= (1 << 20);
    }

    LDR_STR_H_SB_SH(gba, arm);
}

static void LDR_STR_Imm_THUMB(struct GBA* gba, uint16_t ins) {
    uint8_t Rd = ins & 0b111;
    uint8_t Rb = (ins >> 3) & 0b111;
    uint8_t offset5 = (ins >> 6) & 0b11111;
    
    uint8_t L = (ins >> 11) & 1;
    uint8_t B = (ins >> 12) & 1;


    uint32_t arm = 0b111001011000 << 20;
    arm |= (Rb << 16) | (Rd << 12);

    if (B) {
        arm |= offset5;
        arm |= 1 << 22;
    } else {
        arm |= offset5 << 2;
    }

    if (L) arm |= 1 << 20;

    LDR_STR(gba, arm);
}

static void LDR_STR_HW_THUMB(struct GBA* gba, uint16_t ins) {
    /* Load/Store halfwords from and to memory, using Lo registers and a specified 
     * 6 bit offset that is halfword aligned */

    uint8_t Rd = ins & 0b111;
    uint8_t Rb = (ins >> 3) & 0b111;
    uint8_t offset5 = (ins >> 6) & 0b11111;

    uint8_t L = (ins >> 11) & 1;

    uint32_t arm = 0b111000011100 << 20;

    if (L) arm |= 1 << 20;
    arm |= 0b1011 << 4;

    arm |= (Rb << 16) | (Rd << 12);

    uint8_t high = ((offset5 << 1) & 0xF0) >> 4;
    uint8_t low = (offset5 << 1) & 0x0F;

    arm |= low;
    arm |= high << 8;

    LDR_STR_H_SB_SH(gba, arm);
}

static void LDR_STR_SP_Relative_THUMB(struct GBA* gba, uint16_t ins) {
    /* Load/Store words from an to memory, using 10 bit offset and SP register to address */
    uint32_t offset8 = ins & 0xFF;
    uint8_t Rd = (ins >> 8) & 0b111;

    uint8_t L = (ins >> 11) & 1;

    uint32_t arm = 0b111001011000 << 20;

    arm |= offset8 << 2;
    arm |= (R13 << 16) | (Rd << 12);

    if (L) arm |= 1 << 20;

    LDR_STR(gba, arm);
}

static void LOAD_ADDRESS_THUMB(struct GBA* gba, uint16_t ins) {
    /* Load address from PC/SP to some register with 10 bit offset 
     * CPSR is not set */
    uint8_t offset8 = ins & 0xFF;
    uint8_t Rd = (ins >> 8) & 0b111;
    uint8_t SP = (ins >> 11) & 1;

    
    if (SP) {
        /* Read from SP */
        uint32_t address = gba->REG[R13] + (offset8 << 2);
        gba->REG[Rd] = address;
    } else {
        /* Read from PC 
         * Bit 1 of PC is forced to 0 */
        uint32_t address = (gba->REG[R15] & (~0b10)) + (offset8 << 2);
        gba->REG[Rd] = address;
    }
}

static void ADD_OFFSET_SP(struct GBA* gba, uint16_t ins) {
    /* Add signed 9 bit offset to SP 
     * CPSR Condition codes are not set */ 
    int16_t soffset = (ins & 0x7F) << 2;
    uint8_t S = (ins >> 7) & 1;

    if (S) soffset = -soffset;

    uint32_t result = (uint32_t)((int64_t)gba->REG[R13] + soffset);
    gba->REG[R13] = result;
}

static void PUSH_POP_REGS(struct GBA* gba, uint16_t ins) {
    /* Push/Pop RList (registers from R0-R7) to stack
     * Optional: Pushing LR (R14) to stack along with RList
     *           Popping PC (R15) from stack along with RList
     *
     *
     * These work the same as STMDB and LDMIA with writeback
     * so the best option here is to transcode this into its equivalent ARM instruction
     * and execute. 
     */
    uint8_t RList = ins & 0xFF;
    uint8_t R = (ins >> 8) & 1;
    uint8_t L = (ins >> 11) & 1;


    uint32_t arm = 0b111010000010 << 20;
    arm |= L << 20;

    if (L) {
        /* Increase After P = 0 U = 1 */
        arm |= 1 << 23;
    } else {
        /* Decrease Before P = 1 U = 0*/
        arm |= 1 << 24;
    }

    /* Add base register (R13 */
    arm |= 0xD << 16;
    /* Add Rlist */
    arm |= RList;

    /* Put R15/R14 on Rlist if needed */
    if (R) {
        if (L) arm |= 1 << 15;
        else arm |= 1 << 14; 
    }

    /* Call our ARM implementation with transcoded opcode */
    LDM_STM(gba, arm);
}

static void MULTIPLE_LOAD_STORE(struct GBA* gba, uint16_t ins) {
    /* LMDIA Rb!, {RList} / STMIA Rb!, {RList}
     *
     * Easiest way is to just transcode to ARM LDM/STM equivalent and it automatically 
     * handles all edge cases */

    uint8_t RList = ins & 0xFF;
    uint8_t Rb = (ins >> 8) & 0b111;
    uint8_t L = (ins >> 11) & 1;

    uint32_t arm = 0b111010001010 << 20;
    arm |= L << 20;

    /* Add base register  */
    arm |= Rb << 16;
    /* Add Rlist */
    arm |= RList;

    /* Call our arm implementation with transcoded opcode */
    LDM_STM(gba, arm);

}

static void CONDITIONAL_BRANCH(struct GBA* gba, uint16_t ins) {
    /* 9 bit Twos complement signed offset (halfword aligned) */
    uint32_t offset = (ins & 0xFF) << 1;
    uint8_t cond = (ins >> 8) & 0xF;

    if (checkCondition(gba, cond)) {
        /* Do condition check and then write to PC */
        gba->REG[R15] = twosComplementOffset(gba->REG[R15], offset, 8);   
        flushRefillPipeline(gba);
    }
}

static void UNCONDITIONAL_BRANCH(struct GBA* gba, uint16_t ins) {
    /* 12 bit Twos complement signed offset (halfword aligned) */
    uint32_t offset = (ins & 0x7FF) << 1;

    gba->REG[R15] = twosComplementOffset(gba->REG[R15], offset, 11);   
    flushRefillPipeline(gba);
}

static void LONG_BRANCH_W_LINK(struct GBA* gba, uint16_t ins) {
    /* Long branch with 23 bit twos complement signed offset thats halfword aligned
     *
     * This requires 2 consecutive thumb instructions, each of which specifies high or low part
     * They are linked by using LR register as intermediate 
     */

    uint32_t offset = ins & 0x7FF;
    uint8_t H = (ins >> 11) & 1;

    if (!H) {
        /* Offset Low behaviour */
        gba->REG[R14] = gba->REG[R15] + (offset << 12);
    } else {
        /* Offset high behaviour 
         *
         * Return value is set in LR, which is the address of the instruction following the final
         * BL. PC is also written here combining both halves */
        uint32_t ret = gba->REG[R15]-2;     /* PC is 2 instructions ahead, we want to point to 1 instruction ahead */

        /* Rather than doing this part by part 2s complement addition, we just extract the previous
         * offset and calculate full offset, then write value accordingly */
        gba->REG[R15]=twosComplementOffset(gba->REG[R15]-2, gba->REG[R14]-(gba->REG[R15]-2)+(offset<<1), 22);
        gba->REG[R14] = ret | 1;

        flushRefillPipeline(gba);
    }
}

static void SWI_THUMB(struct GBA* gba, uint16_t ins) {
    /* THUMB Software interrupt */
    triggerException(gba, CPU_EXCEP_SWI);
}


static void Undefined_THUMB(struct GBA* gba, uint16_t ins) {
	printf("Instruction: %04x is an undefined THUMB Instruction\n", ins);
}

/* ---------------------------------------------------- */

static void switchMode(GBA* gba, CPU_MODE newMode) {
	/* Switches CPU to given mode, sets up banked registers
	 * also updates CPSR */

	CPU_MODE currentMode = gba->cpu_mode;

	if (currentMode == newMode) return;
	else if ((currentMode == CPU_MODE_SYSTEM && newMode == CPU_MODE_USER)
			 || (currentMode == CPU_MODE_USER && newMode == CPU_MODE_SYSTEM)) {

		/* No register bank switching required but we still switch modes */
		gba->cpu_mode = newMode;
		CPSR_SetMode(gba, newMode);
		return;
	}

	/* Save the registers of the current mode (only the ones which will be swapped out) */
	switch (currentMode) {
		case CPU_MODE_SYSTEM:
		case CPU_MODE_USER:
			memcpy(&gba->REG_SWAP[5], &gba->REG[R13], 2*sizeof(uint32_t));
			break;
		case CPU_MODE_FIQ:
			memcpy(&gba->BANK_FIQ, &gba->REG[R8], 7*sizeof(uint32_t));

			/* Load in R8-R12 normal registers as we're about to leave FIQ */
			memcpy(&gba->REG[R8], &gba->REG_SWAP, 5*sizeof(uint32_t));
			break;
		case CPU_MODE_IRQ:
			memcpy(&gba->BANK_IRQ, &gba->REG[R13], 2*sizeof(uint32_t));
			break;
		case CPU_MODE_SVC:
			memcpy(&gba->BANK_SVC, &gba->REG[R13], 2*sizeof(uint32_t));
			break;
		case CPU_MODE_ABT:
			memcpy(&gba->BANK_ABT, &gba->REG[R13], 2*sizeof(uint32_t));
			break;
		case CPU_MODE_UND:
			memcpy(&gba->BANK_UND, &gba->REG[R13], 2*sizeof(uint32_t));
			break;
	}

	/* Save SPSR of the current mode if the mode isnt USER/SYSTEM */
	if (currentMode != CPU_MODE_USER && currentMode != CPU_MODE_SYSTEM) {
        uint8_t index = 0;
        switch (currentMode) {
            case CPU_MODE_FIQ: index = 0; break;
            case CPU_MODE_IRQ: index = 1; break;
            case CPU_MODE_UND: index = 2; break;
            case CPU_MODE_ABT: index = 3; break;
            case CPU_MODE_SVC: index = 4; break;
            default: break;
        }
		gba->BANK_SPSR[index] = gba->SPSR;
	}

	/* Load the registers of the new mode (only the ones for the mode) */
	switch (newMode) {
		case CPU_MODE_SYSTEM:
		case CPU_MODE_USER:
			/* Swap in R13-R14 only, if the previous mode was FIQ it has already reverted the rest */
			memcpy(&gba->REG[R13], &gba->REG_SWAP[5], 2*sizeof(uint32_t));
			break;
		case CPU_MODE_FIQ:
			/* Save R8-R12 in REG_SWAP as we're entering FIQ
			 * R13-R14 should be saved by whichever mode was present before this */
			memcpy(&gba->REG_SWAP, &gba->REG[R8], 5*sizeof(uint32_t));
			/* Swap in R8-R14 */
			memcpy(&gba->REG[R8], &gba->BANK_FIQ, 7*sizeof(uint32_t));
			break;
		case CPU_MODE_IRQ:
			/* Swap in R13-R14 */
			memcpy(&gba->REG[R13], &gba->BANK_IRQ, 2*sizeof(uint32_t));
			break;
		case CPU_MODE_SVC:
			memcpy(&gba->REG[R13], &gba->BANK_SVC, 2*sizeof(uint32_t));
			break;
		case CPU_MODE_ABT:
			memcpy(&gba->REG[R13], &gba->BANK_ABT, 2*sizeof(uint32_t));
			break;
		case CPU_MODE_UND:
			memcpy(&gba->REG[R13], &gba->BANK_UND, 2*sizeof(uint32_t));
			break;
	}

	/* Load in the SPSR if the new mode isnt USER/SYSTEM */
	if (newMode != CPU_MODE_USER && newMode != CPU_MODE_SYSTEM) {
        uint8_t index = 0;
        switch (newMode) {
            case CPU_MODE_FIQ: index = 0; break;
            case CPU_MODE_IRQ: index = 1; break;
            case CPU_MODE_UND: index = 2; break;
            case CPU_MODE_ABT: index = 3; break;
            case CPU_MODE_SVC: index = 4; break;
            default: break;
        }
		gba->SPSR = gba->BANK_SPSR[index];
	} else {
		/* SPSR cannot be read in USER/SYSTEM mode, so we initialise it to a garbage value */
		gba->SPSR = 0xFFFFFFFF;
	}

	CPSR_SetMode(gba, newMode);
	gba->cpu_mode = newMode;
}

static bool checkCondition(GBA* gba, uint8_t condCode) {
	/* Check if an ARM condition code is valid and if the instruction can be executed */
	switch (condCode) {
		case 0b1110: return true;
		case 0b0000: return CPSR_GetBit(gba, FLG_Z);
		case 0b0001: return !CPSR_GetBit(gba, FLG_Z);
		case 0b0010: return CPSR_GetBit(gba, FLG_C);
		case 0b0011: return !CPSR_GetBit(gba, FLG_C);
		case 0b0100: return CPSR_GetBit(gba, FLG_N);
		case 0b0101: return !CPSR_GetBit(gba, FLG_N);
		case 0b0110: return CPSR_GetBit(gba, FLG_V);
		case 0b0111: return !CPSR_GetBit(gba, FLG_V);
		case 0b1000: return CPSR_GetBit(gba, FLG_C) && !CPSR_GetBit(gba, FLG_Z);
		case 0b1001: return !CPSR_GetBit(gba, FLG_C) || CPSR_GetBit(gba, FLG_Z);
		case 0b1010: return CPSR_GetBit(gba, FLG_N) == CPSR_GetBit(gba, FLG_V);
		case 0b1011: return CPSR_GetBit(gba, FLG_N) != CPSR_GetBit(gba, FLG_V);
		case 0b1100: return !CPSR_GetBit(gba, FLG_Z) && (CPSR_GetBit(gba, FLG_N)
							 									== CPSR_GetBit(gba, FLG_V));
		case 0b1101: return CPSR_GetBit(gba, FLG_Z) || (CPSR_GetBit(gba, FLG_N)
							 									!= CPSR_GetBit(gba, FLG_V));
		default: printf("[WARNING] Unknown Condition Code %x\n", condCode); return false;
	}
}

static inline uint32_t readARMOpcode(GBA* gba) {
	/* Increment PC then read, this is done to handle pipeline behaviour in combination
     * with the first read being with peek */

    gba->REG[R15] += 4;
	uint32_t opcode = busRead(gba, gba->REG[R15], WIDTH_32);
	return opcode;
}

static inline uint32_t peekARMOpcode(GBA* gba) {
    return busRead(gba, gba->REG[R15], WIDTH_32);
}

static inline uint16_t readTHUMBOpcode(GBA* gba) {
	gba->REG[R15] += 2;

	uint16_t opcode = busRead(gba, gba->REG[R15], WIDTH_16);
	return opcode;
}

static inline uint32_t peekTHUMBOpcode(GBA* gba) {
    return busRead(gba, gba->REG[R15], WIDTH_16);
}

static void dispatchARM(GBA* gba, uint32_t opcode) {
	/* Decodes and Dispatches an ARM gba opcode */
	if (!checkCondition(gba, opcode >> 28)) return;

	/* Execute from Lookup-Table (combine bits 27-20 and 7-4 to form a 12 bit index) */
	gba->ARM_LUT[((opcode & 0x0FF00000) >> 16) | ((opcode >> 4) & 0xF)]((struct GBA*)gba, opcode);
}

static void dispatchTHUMB(GBA* gba, uint16_t opcode) {
	/* No conditional checking here, we directly execute */
	gba->THUMB_LUT[opcode >> 8]((struct GBA*)gba, opcode);
}

/* ----------------------------------------------------------------- */

static void initialiseLUT_ARM(GBA* gba) {
	/* Fills lookup table for ARM instructions with corresponding function pointers
	 * which handle the particular instruction */

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
			gba->ARM_LUT[index] = &BX;
		} else if ((index & 0b111110111111) == 0b000100001001) {
			/* Single Data Swap (SWP) */
			gba->ARM_LUT[index] = &SWP;
		} else if ((index & 0b111111001111) == 0b000000001001) {
			/* Checking for multiply and multiply accumulate
			 * S and A bit are checked at runtime */
			gba->ARM_LUT[index] = &MUL_MLA;
		} else if ((index & 0b111110001111) == 0b000010001001) {
			/* Checking for multiply long and multiply accumulate long with variations
			 * (UMULL, UMLAL, SMULL, SMLAL), S bit is checked at runtime */
			gba->ARM_LUT[index] = &MULL_MLAL; 
        } else if ((index & 0b111000001001) == 0b000000001001) {
			/* Checking for Halfword and Signed Data transfer
			 * (LDRH, STRH, LDRSB, LDRSH) -> Signed Byte and Signed halfword is
			 * only available for LDR.
			 * All options checked at runtime */
			gba->ARM_LUT[index] = &LDR_STR_H_SB_SH;
		} else if ((index & 0b111100000000) == 0b111100000000) {
			/* Software Interrupt - SWI */
			gba->ARM_LUT[index] = &SWI;
		} else if ((index & 0b111000000001) == 0b011000000001) {
			/* Undefined Instruction */
			gba->ARM_LUT[index] = &Undefined_ARM;
		} else if ((index & 0b111000000000) == 0b101000000000) {
			/* Checking for Branch and Branch with Link */
			gba->ARM_LUT[index] = &B_BL;
		} else if ((index & 0b111000000000) == 0b100000000000) {
			/* Block Data Transfer - LDM/STM
			 * All options are interpreted at runtime */
			gba->ARM_LUT[index] = &LDM_STM;
		} else if ((index & 0b110000000000) == 0b010000000000) {
			/* Checking for Single Data Transfer (LDR/STR)
			 * options are checked at runtime */
			gba->ARM_LUT[index] = &LDR_STR;
        } else if ((index & 0b111110110000) == 0b000100000000) {
			/* Checking for MRS (transfer PSR->Reg)
			 * bit 22 can be 1 or 0 depending on whether SPSR/CPSR has to be used,
			 * which will be determined at runtime */
			gba->ARM_LUT[index] = &MRS;
		} else if ((index & 0b110110110000) == 0b000100100000) {
			/* Checking for MSR (transfer Reg/Imm->PSR)
			 * This instruction has 2 encodings, one for flag only transfer which can be Imm/Reg
			 * or full register transfer which is only Reg. This however introduces complicated
			 * bit collisions which we can avoid by doing runtime checks instead */
			gba->ARM_LUT[index] = &MSR;
		}  else if ((index & 0b110000000000) == 0) {
			/* Checking for Data Processing Instructions
		 	 * Only bit 27-26 are fixed, rest are variable showing various opcodes and other info
			 * We setup separate functions for arithmetic and logic, the instructions themselves
			 * share fairly common behaviour */

			switch ((index >> 5) & 0xF) {
				/* Check OP CODE */
				case 0x0: case 0x1: case 0x8: case 0x9: case 0xC: case 0xD: case 0xE: case 0xF:
					gba->ARM_LUT[index] = &dataProcessingLogical;
					break;
				case 0x2: case 0x3: case 0x4: case 0x5: case 0x6: case 0x7: case 0xA: case 0xB:
					gba->ARM_LUT[index] = &dataProcessingArithmetic;
					break;
			}
		} else {
			gba->ARM_LUT[index] = &Unimplemented_ARM;
		}
	}
}

void initialiseLUT_THUMB(GBA* gba) {
	/* For THUMB LUT, we only use the upper byte to decode instructions
	 * so we need 2^8 = 256 entries */

	for (int index = 0; index < 256; index++) {
		/* Bit masking and checking is done the same way as ARM LUT, i.e
		 * we mask the bits we dont need and compare the rest to check for an encoding
		 * The ordering is also important as we prioritise encodings which are the most
		 * deterministic (most bits matching) and leave the unpredictable ones at the bottom */

        if ((index & 0xFF) == 0b10110000) {
            /* Add offset to SP */
            gba->THUMB_LUT[index] = &ADD_OFFSET_SP;
        } else if ((index & 0xFF) == 0b11011111) {
            /* SWI - Software Interrupt */
            gba->THUMB_LUT[index] = &SWI_THUMB;
        } else if ((index & 0b11111100) == 0b01000100) {
			/* Hi Register Operations / BX */
			gba->THUMB_LUT[index] = &HIREG_OPS_BX;
		} else if ((index & 0b11111100) == 0b01000000) {
			/* ALU Operations */
			gba->THUMB_LUT[index] = &ALU;
        } else if ((index & 0b11110110) == 0b10110100) {
            /* Push/Pop registers */
            gba->THUMB_LUT[index] = &PUSH_POP_REGS;
        } else if ((index & 0b11111000) == 0b01001000) {
            /* PC-Relative Load */
            gba->THUMB_LUT[index] = &PC_Relative_Load;
		} else if ((index & 0b11111000) == 0b00011000) {
			/* Add/Sub with immediate or register operand */
			gba->THUMB_LUT[index] = &ADD_SUB;
        } else if ((index & 0b11111000) == 0b11100000) {
            /* Unconditional Branch */
            gba->THUMB_LUT[index] = &UNCONDITIONAL_BRANCH;
        } else if ((index & 0b11110010) == 0b01010000) {
            /* Load/Store with register offset */
            gba->THUMB_LUT[index] = &LDR_STR_REG_OFFSET;
        } else if ((index & 0b11110010) == 0b01010010) {
            /* Load/Store sign extended byte/halfword */
            gba->THUMB_LUT[index] = &LDR_STR_H_SB_SH_THUMB;
        } else if ((index & 0b11110000) == 0b10000000) {
            /* Load/Store halfword */
            gba->THUMB_LUT[index] = &LDR_STR_HW_THUMB;
        } else if ((index & 0b11110000) == 0b10010000) {
            /* SP relative load/store */
            gba->THUMB_LUT[index] = &LDR_STR_SP_Relative_THUMB;
        } else if ((index & 0b11110000) == 0b10100000) {
            /* Load Address */
            gba->THUMB_LUT[index] = &LOAD_ADDRESS_THUMB;
        } else if ((index & 0b11110000) == 0b11000000) {
            /* Multiple Load/Store */
            gba->THUMB_LUT[index] = &MULTIPLE_LOAD_STORE;
        } else if ((index & 0b11110000) == 0b11010000) {
            /* Conditional Branch */
            gba->THUMB_LUT[index] = &CONDITIONAL_BRANCH;
        } else if ((index & 0b11110000) == 0b11110000) {
            /* Long branch with link */
            gba->THUMB_LUT[index] = &LONG_BRANCH_W_LINK;
        } else if ((index & 0b11100000) == 0b01100000) {
            /* Load/Store with immediate offset */
            gba->THUMB_LUT[index] = &LDR_STR_Imm_THUMB;
		} else if ((index & 0b11100000) == 0b00100000) {
			/* MOV/CMP/ADD/SUB Immediate */
			gba->THUMB_LUT[index] = &MOV_CMP_ADD_SUB_Imm;
		} else if ((index & 0b11100000) == 0b00000000) {
			/* Move Shifted Register */
			gba->THUMB_LUT[index] = &LSL_LSR_ASR;
		} else {
			gba->THUMB_LUT[index] = &Undefined_THUMB;
		}
	}
}

void initialiseCPU(GBA* gba) {
	gba->cpu_state = CPU_STATE_ARM;
	gba->cpu_mode = CPU_MODE_SYSTEM;

	/* Preset register values as set by BIOS (we dont use a BIOS file, just emulate
	 * its behaviour, including BIOS functions) */
	memset(&gba->BANK_FIQ, 	0, 7*sizeof(uint32_t));
	memset(&gba->BANK_SVC, 	0, 2*sizeof(uint32_t));
	memset(&gba->BANK_ABT, 	0, 2*sizeof(uint32_t));
	memset(&gba->BANK_IRQ, 	0, 2*sizeof(uint32_t));
	memset(&gba->BANK_UND, 	0, 2*sizeof(uint32_t));
	memset(&gba->BANK_SPSR, 0, 5*sizeof(uint32_t));
	memset(&gba->REG, 	  	0,16*sizeof(uint32_t));
	memset(&gba->REG_SWAP,  0, 7*sizeof(uint32_t));

	/* Loading R0, R1, R13, R15 and CPSR with BIOS initialised values */

	gba->REG[R0]		   = 0x08000000;
	gba->REG[R1] 		   = 0x000000EA;
	gba->BANK_SVC[R13_SVC] = 0x03007FE0;
	gba->BANK_IRQ[R13_IRQ] = 0x03007FA0;
	gba->REG[R13] 		   = 0x03007F00;
	gba->CPSR 			   = 0x6000001F; 	// ARM State, System Mode
	gba->SPSR    		   = 0xFFFFFFFF;    // SPSR cannot be read in USER/SYSTEM mode */ 
	gba->REG[R15] 		   = 0x08000000;

	/* Setup Lookup Tables */
	initialiseLUT_ARM(gba);
	initialiseLUT_THUMB(gba);

    /* Exceptions */
    gba->exceptionState = 0;

	/* Other values */
	memset(&gba->pipeline, 0, 3*sizeof(uint32_t));
	gba->pipelineInsertPoint = 0;
	gba->pipelineReadPoint = 0;
	gba->skipFetch = false;
	gba->cycles = 0;
    gba->halted = false;

	flushRefillPipeline(gba);
    gba->skipFetch = false;
}

static inline void insertPipeline(GBA* gba, uint32_t opcode) {
	gba->pipeline[gba->pipelineInsertPoint++] = opcode;
	gba->pipelineInsertPoint %= 3;

}

static inline uint32_t peekPipeline(GBA* gba, uint8_t offset) {
    /* 0 -> next in execution (decoded state)
     * 1 -> next to next in execution (fetched state) 
     * Assuming peak is done during dispatch */
    return gba->pipeline[gba->pipelineReadPoint+offset];
}

static inline uint32_t readPipeline(GBA* gba) {
	uint32_t code = gba->pipeline[gba->pipelineReadPoint++];
	gba->pipelineReadPoint %= 3;
	return code;
}

static void flushRefillPipeline(GBA* gba) {
	/* Flushes then refills pipeline relative to PC,
	 * we always ensure that the pipeline is always in execuatable state */

	gba->pipelineInsertPoint = 0;
	gba->pipelineReadPoint = 0;

	if (gba->cpu_state == CPU_STATE_ARM) {
		insertPipeline(gba, peekARMOpcode(gba));
		insertPipeline(gba, readARMOpcode(gba));
        insertPipeline(gba, readARMOpcode(gba));
	} else {
		insertPipeline(gba, peekTHUMBOpcode(gba));
		insertPipeline(gba, readTHUMBOpcode(gba));
        insertPipeline(gba, readTHUMBOpcode(gba));
	}

    /* The pipeline is pushed to full once again ready for execution, and the proceeding
     * fetch is skipped.
     *
     * The PC register (R15) is not actually incremented after fetch, but only during the decode
     * stage, which is why we get a (current address + 8) read when probing PC within the executing
     * instruction instead of +12. If it were incremented at the end of fetch we would get +12.
     * So essentially what must be done to simulate that behaviour is skipping PC increment on the
     * first fetch after every pipeline flush (i.e the first fetch of the fresh pipeline only).
     * And then consequently incrementing PC before read. This allows us to fully capture that
     * behaviour.
     *
     * An exception might also occur after this pipeline flush, in which case these newly
     * fetched values can be used to store in LR */

	gba->skipFetch = true; 							
    /* Set flag in emulator so pipeline operations
	 after the current execution stage are discarded */
}

static inline void doInternalPrefetchARM(GBA* gba) {
	/* If an extra cycle is consumed by the CPU within the intruction, the prefetcher already
	 * fetches the next instruction in the pipeline and hence we read PC as PC+12 instead
	 * of normally reading PC+8.
	 *
	 * This kind of system is more natural than just blatantly returning PC+12 and it also doesnt
	 * create any weird edge cases regarding the prefetcher in terms of self modifying code.
	 * And, this method also doesnt have any performance downsides either
	 *
	 * Though im unsure if there are any possible edge cases in any instructions that really require
	 * the use of this over just simply returning PC+12 so ill not use this for now
	 *
	 * We emulate this by prefetching the instruction right here and scheduling a skip after dispatch
	 * returns */
	insertPipeline(gba, readARMOpcode(gba));
	gba->skipFetch = true;
}

/* ----------------------------------------------------------------- */
/*                       Exception Handling                          */

static void requestAsyncException(GBA* gba, CPU_EXCEP excep) {
    /* Asynchronous exceptions can be requested any time during the execution/prefetching 
     * Handling is done at a fixed time, i.e after execution.
     * For synchronous exceptions like SWI, they are handled immediately within the instruction */
    gba->exceptionState |= 1 << excep;
}

static void clearAsyncExceptionRequest(GBA* gba, CPU_EXCEP excep) {
    gba->exceptionState &= ~(1 << excep);
}

static void triggerException(GBA* gba, CPU_EXCEP excep) {
    /* Given we need to handle a particular exception, this function follows the procedure 
     * It is called at the end of dispatch, before the next execution takes place 
     *
     * Note: If we were to simulate PC+12 behaviour naturally, at the end of instructions
     * the PC might not be where we expect it to be and calculating the next instruction in the
     * pipeline would be more painful.
     * */
    uint32_t retPC;
    uint32_t vector;
    uint32_t CPSR = gba->CPSR;

    switch (excep) {
        case CPU_EXCEP_IRQ: {
            /* ''''''''''''' */ 
            retPC = gba->cpu_state == CPU_STATE_ARM ? gba->REG[R15]-4 : gba->REG[R15];
            vector = 0x18;
            switchMode(gba, CPU_MODE_IRQ);
            break;
        }
        case CPU_EXCEP_SWI: {
            /* Address of instruction that led to exception (the one that just executed) 
             *      + 4 for ARM
             *      + 2 for THUMB */
            retPC = gba->cpu_state == CPU_STATE_ARM ? gba->REG[R15]-4 : gba->REG[R15]-2;
            vector = 0x08;
            switchMode(gba, CPU_MODE_SVC);
            break;
        }
        /* GBA memory does not issue faults, it returns open bus/garbage which means
         * PABORT and DABORT will never naturally occur and its safe to ignore */
        default: break;
    }

    gba->SPSR = CPSR;
    /* Set I after saving CPSR to SPSR */
    CPSR_SetBit(gba, CPSR_IRQ_DIS);
    /* Switches to ARM mode because bit 0 is 0 */
    branchAndExchange(gba, vector);
    gba->REG[R14] = retPC;

}

static void returnException(GBA* gba) {
    /* When PC is written to with S bit set in privileged mode, exception return is
     * triggered, CPSR <- SPSR */
    if (gba->cpu_mode == CPU_MODE_SYSTEM || gba->cpu_mode == CPU_MODE_USER) return;

    /* Restore I to previous value aswell */
    gba->CPSR = gba->SPSR;
    
    /* Do necessary mode and state switch */
    switchMode(gba, gba->CPSR & 0x1F);
    if (CPSR_GetBit(gba, CPSR_T)) {gba->cpu_state = CPU_STATE_THUMB;}
}

static void checkAsyncExceptions(GBA* gba) {
    /* Called at the end of dispatch, check if any asynchronous exceptions have been generated 
     * and handle them accordingly, beginning from next execution */
    uint8_t requested = (gba->exceptionState >> CPU_EXCEP_IRQ) & 1;
    if (requested) {
        /* Skip IRQ if it has been disabled in CPSR 
         * It would be handled after the bits are cleared */
        if (CPSR_GetBit(gba, CPSR_IRQ_DIS)) return;

        /* A suitable exception can be handled now */
        triggerException(gba, CPU_EXCEP_IRQ);
        /* Clear exception request bit */
        gba->exceptionState &= ~(1 << CPU_EXCEP_IRQ);
    }
}

/* ------------------------ Interrupts -------------------------------- */

void requestInterrupt(GBA* gba, IRQ irq) {
    /* Request an interrupt, this sets the request flag in IF */
    uint16_t data = readIO_internal(gba, IF, WIDTH_16);
    data |= 1 << irq;
    writeIO_internal(gba, IF, data, WIDTH_16);
}

static void handleInterrupts(GBA* gba) {
    /* Check for possible interrupts and call for IRQ exception if conditions are satisfied */ 

    uint16_t IE_data = readIO_internal(gba, IE, WIDTH_16);
    uint16_t IF_data = readIO_internal(gba, IF, WIDTH_16);

    /* No interrupts enabled and requested */
    if ((IE_data & IF_data) == 0) {
        clearAsyncExceptionRequest(gba, CPU_EXCEP_IRQ);
        return;
    }

    /* If even a single interrupt is enabled and requested, CPU is resumed if halted
     * IME or CPSR I bit are not checked, but they may prevent IRQ jump anyway */
    gba->halted = false;
    /* Check IME, CPU IRQ enable check is handled by the exception handler */
    if ((readIO_internal(gba, IME, WIDTH_16) & 1) == 0) {
        clearAsyncExceptionRequest(gba, CPU_EXCEP_IRQ);
        return;
    }

    /* Check IF and IE */
    for (int i=0; i<16; i++) {
        if ((IE_data >> i & 1) && (IF_data >> i & 1)) {
            /* Atleast one interrupt is enabled and requested
             * Priorities are handled by the software. 
             * IF bit is not cleared automatically and requires manual acknowledgement */
            requestAsyncException(gba, CPU_EXCEP_IRQ);
            break;
        }
    }
}

/* -------------------------------------------------------------------- */

void stepCPU(GBA* gba) {
	/* CPU Pipeline has 3 stages happening simulataneously, Execute/Decode/Fetch
	 *
	 * C1 -> Execute  - 	Decode  - 		Fetch ins1
	 * C2 -> Execute  - 	Decode ins1 	Fetch ins2
	 * C3 -> Execute ins1   Decode ins2   	Fetch ins3
	 * C4 -> Execute ins2 	Decode ins3 	Fetch ins4
	 * ...
	 *
	 * Any instruction that modifies R15 causes a pipeline flush, causing it clear up
	 * and start fetching again. However, we can optimize by not emulating the first 2 cpu steps where
	 * no execution takes place and keeping the pipeline always in executable state. This is done by
	 * flushRefillPipeline() which is called during initialization and at every R15 modification
	 *
	 * The pipeline is managed by a queue, which is responsible for storing prefetched instructions
	 * The instructions flow in the queue once they are loaded in, independent of what happens at
	 * the actual address. This means if the address of the next instruction is modified,
	 * the instruction will still execute as it was prefetched in the queue */

    /* Handle any requested interrupts */
    handleInterrupts(gba);
    /* Exceptions are checked for, and pipeline may be flushed and refilled before continuing */
    checkAsyncExceptions(gba);
    gba->skipFetch = false;         /* Fetch does not follow but rather a read */

    /* handleInterrupts is responsible for resuming halt 
     * Pipeline is frozen in full state and will resume as is if IRQ jump is not 
     * taken after resuming from halt. Otherwise checkAsyncExceptions will cause IRQ
     * jump and pipeline gets reset to IRQ handler */
    if (gba->halted) {
        /* Fast forward clock to next event if CPU is halted */
        gba->cycles = peekEvent(gba, 0).scheduledFor;
        return;
    }

	if (gba->cpu_state == CPU_STATE_ARM) {
#if defined(DEBUG_ENABLED) && defined(DEBUG_TRACE_STATE)
		uint32_t opcode = readPipeline(gba);
		printStateARM(gba, opcode);
		dispatchARM(gba, opcode);
#else
		dispatchARM(gba, readPipeline(gba));
#endif
		if (gba->skipFetch) {gba->skipFetch = false; return;}
		insertPipeline(gba, readARMOpcode(gba));

	} else {
#if defined(DEBUG_ENABLED) && defined(DEBUG_TRACE_STATE)
		uint16_t opcode = (uint16_t)readPipeline(gba);
		printStateTHUMB(gba, opcode);
		dispatchTHUMB(gba, opcode);
#else
		dispatchTHUMB(gba, readPipeline(gba));
#endif 

		if (gba->skipFetch) {gba->skipFetch = false; return;}
		insertPipeline(gba, readTHUMBOpcode(gba));
	}
}
