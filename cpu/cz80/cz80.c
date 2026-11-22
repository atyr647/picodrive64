/******************************************************************************
 *
 * CZ80 (Z80 CPU emulator) version 0.9
 * Compiled with Dev-C++
 * Copyright 2004-2005 St駱hane Dallongeville
 *
 * (Modified by NJ)
 *
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cz80.h"

#if PICODRIVE_HACKS
#include <pico/pico_int.h>
#include <pico/memory.h>
#endif

#ifndef ALIGN_DATA
#ifdef _MSC_VER
#define ALIGN_DATA
#define inline
#undef  CZ80_USE_JUMPTABLE
#define CZ80_USE_JUMPTABLE 0
#else
#define ALIGN_DATA      __attribute__((aligned(4)))
#endif
#endif

#define CF					0x01
#define NF					0x02
#define PF					0x04
#define VF					PF
#define XF					0x08
#define HF					0x10
#define YF					0x20
#define ZF					0x40
#define SF					0x80


/******************************************************************************
	マクロ
******************************************************************************/

#include "cz80macro.h"


/******************************************************************************
	グローバル構造体
******************************************************************************/

cz80_struc ALIGN_DATA CZ80;


/******************************************************************************
	ローカル変数
******************************************************************************/

static UINT8 ALIGN_DATA cz80_bad_address[1 << CZ80_FETCH_SFT];

static UINT8 ALIGN_DATA SZ[256];
static UINT8 ALIGN_DATA SZP[256];
static UINT8 ALIGN_DATA SZ_BIT[256];
static UINT8 ALIGN_DATA SZHV_inc[256];
static UINT8 ALIGN_DATA SZHV_dec[256];
#if CZ80_BIG_FLAGS_ARRAY
static UINT8 ALIGN_DATA SZHVC_add[2*256*256];
static UINT8 ALIGN_DATA SZHVC_sub[2*256*256];
#endif


/******************************************************************************
	ローカル関数
******************************************************************************/

/*--------------------------------------------------------
	割り込みコールバック
--------------------------------------------------------*/

static INT32 Cz80_Interrupt_Callback(INT32 line)
{
	return 0xff;
}


/******************************************************************************
	CZ80インタフェース関数
******************************************************************************/

/*--------------------------------------------------------
	CPU初期化
--------------------------------------------------------*/

void Cz80_Init(cz80_struc *CPU)
{
	UINT32 i, j, p;
#if CZ80_BIG_FLAGS_ARRAY
	int oldval, newval, val;
	UINT8 *padd, *padc, *psub, *psbc;
#endif

	memset(CPU, 0, sizeof(cz80_struc));

	memset(cz80_bad_address, 0xff, sizeof(cz80_bad_address));

	for (i = 0; i < CZ80_FETCH_BANK; i++)
	{
		CPU->Fetch[i] = (FPTR)cz80_bad_address - (i << CZ80_FETCH_SFT);
#if CZ80_ENCRYPTED_ROM
		CPU->OPFetch[i] = 0;
#endif
	}

	// flags tables initialisation
	for (i = 0; i < 256; i++)
	{
		SZ[i] = i & (SF | YF | XF);
		if (!i) SZ[i] |= ZF;

		SZ_BIT[i] = i & (SF | YF | XF);
		if (!i) SZ_BIT[i] |= ZF | PF;

		for (j = 0, p = 0; j < 8; j++) if (i & (1 << j)) p++;
		SZP[i] = SZ[i];
		if (!(p & 1)) SZP[i] |= PF;

		SZHV_inc[i] = SZ[i];
		if(i == 0x80) SZHV_inc[i] |= VF;
		if((i & 0x0f) == 0x00) SZHV_inc[i] |= HF;

		SZHV_dec[i] = SZ[i] | NF;
		if (i == 0x7f) SZHV_dec[i] |= VF;
		if ((i & 0x0f) == 0x0f) SZHV_dec[i] |= HF;
	}

#if CZ80_BIG_FLAGS_ARRAY
	padd = &SZHVC_add[  0*256];
	padc = &SZHVC_add[256*256];
	psub = &SZHVC_sub[  0*256];
	psbc = &SZHVC_sub[256*256];

	for (oldval = 0; oldval < 256; oldval++)
	{
		for (newval = 0; newval < 256; newval++)
		{
			/* add or adc w/o carry set */
			val = newval - oldval;
			*padd = (newval) ? ((newval & 0x80) ? SF : 0) : ZF;
			*padd |= (newval & (YF | XF));	/* undocumented flag bits 5+3 */
			if ((newval & 0x0f) < (oldval & 0x0f)) *padd |= HF;
			if (newval < oldval ) *padd |= CF;
			if ((val ^ oldval ^ 0x80) & (val ^ newval) & 0x80) *padd |= VF;
			padd++;

			/* adc with carry set */
			val = newval - oldval - 1;
			*padc = (newval) ? ((newval & 0x80) ? SF : 0) : ZF;
			*padc |= (newval & (YF | XF));	/* undocumented flag bits 5+3 */
			if ((newval & 0x0f) <= (oldval & 0x0f)) *padc |= HF;
			if (newval <= oldval) *padc |= CF;
			if ((val ^ oldval ^ 0x80) & (val ^ newval) & 0x80) *padc |= VF;
			padc++;

			/* cp, sub or sbc w/o carry set */
			val = oldval - newval;
			*psub = NF | ((newval) ? ((newval & 0x80) ? SF : 0) : ZF);
			*psub |= (newval & (YF | XF));	/* undocumented flag bits 5+3 */
			if ((newval & 0x0f) > (oldval & 0x0f)) *psub |= HF;
			if (newval > oldval) *psub |= CF;
			if ((val^oldval) & (oldval^newval) & 0x80) *psub |= VF;
			psub++;

			/* sbc with carry set */
			val = oldval - newval - 1;
			*psbc = NF | ((newval) ? ((newval & 0x80) ? SF : 0) : ZF);
			*psbc |= (newval & (YF | XF));	/* undocumented flag bits 5+3 */
			if ((newval & 0x0f) >= (oldval & 0x0f)) *psbc |= HF;
			if (newval >= oldval) *psbc |= CF;
			if ((val ^ oldval) & (oldval^newval) & 0x80) *psbc |= VF;
			psbc++;
		}
	}
#endif

	CPU->pzR8[0] = &zB;
	CPU->pzR8[1] = &zC;
	CPU->pzR8[2] = &zD;
	CPU->pzR8[3] = &zE;
	CPU->pzR8[4] = &zH;
	CPU->pzR8[5] = &zL;
	CPU->pzR8[6] = &zF;	// 処理の都合上、Aと入れ替え
	CPU->pzR8[7] = &zA;	// 処理の都合上、Fと入れ替え

	CPU->pzR16[0] = pzBC;
	CPU->pzR16[1] = pzDE;
	CPU->pzR16[2] = pzHL;
	CPU->pzR16[3] = pzFA;

	zIX = zIY = 0xffff;

	CPU->Interrupt_Callback = Cz80_Interrupt_Callback;
}


/*--------------------------------------------------------
	CPUリセット
--------------------------------------------------------*/

void Cz80_Reset(cz80_struc *CPU)
{
	// I, R, CPU and interrupts logic is reset, registers are untouched
	memset(&CPU->R, 0, (FPTR)&CPU->BasePC - (FPTR)&CPU->R);
	Cz80_Set_Reg(CPU, CZ80_PC, 0);
}

/* */
#if PICODRIVE_HACKS
static inline unsigned char picodrive_read(unsigned short a)
{
	uptr v = z80_read_map[a >> Z80_MEM_SHIFT];
	if (map_flag_set(v))
		return ((z80_read_f *)(v << 1))(a);
	return *(unsigned char *)((v << 1) + a);
}
#endif

#if PICODRIVE_HACKS && defined(Z80_DRIVER_ACCEL)
/* Native fast-forward of a known sound-driver main loop (Gley Lancer's Z80
 * driver, 0x0023-0x005c), which takes ~80% of all Z80 instructions: per
 * pass it polls YM timer B and two command bytes from the 68k, then, while
 * DE != 0, streams one DAC sample from (HL) to YM register 0x2a.
 *
 * This executes exactly what Cz80 would, instruction by instruction: same
 * cycle counts (taken from cz80_op.c) applied at the same points, the same
 * memory handlers called at the same ICount (so YM timer/busy status, DAC
 * timestamps and 68k bus delays come out identical), the same flags (SZP),
 * the same R increments, and the same per-instruction "ICount > 0" stop
 * rule.  It hands back to Cz80, at the exact instruction, wherever the code
 * would leave the loop (timer B tick, 68k command, interrupt-enabled CPU) or
 * the timeslice ends.  Only used when the bytes in Z80 RAM match, except
 * the delay count at 0x0031 (ld b,N), which the driver rewrites to set the
 * DAC sample rate.
 *
 * Returns the Z80 PC to continue at, or -1 if not applicable (nothing done).
 */
static const UINT8 drvacc_code[0x5d - 0x23] = {
	0x3a,0x00,0x40, 0xe6,0x02, 0x28,0x06, 0xcd,0x36,0x04, 0xc3,0x34,0x00,
	0x06,0x00, 0x10,0xfe,	/* 0x0031: delay count, not compared */
	0x3a,0xff,0x1f, 0xb7, 0xc4,0xdc,0x01,
	0x3a,0xf8,0x1f, 0xb7, 0xc4,0xaa,0x01, 0x7a, 0xb3, 0x28,0xdd,
	0x06,0x2a, 0x4e, 0x23, 0x3a,0x00,0x40, 0xe6,0x80, 0x20,0xf9, 0x78,
	0x32,0x00,0x40, 0x79, 0x32,0x01,0x40, 0x1b, 0xc3,0x23,0x00
};

static INT32 z80_driver_accel(cz80_struc *CPU)
{
	UINT32 n = 0;	/* opcodes fetched, for R */
	UINT8 *ram;

	/* ICount <= 0 happens after DI/EI (they run the next opcode
	 * unconditionally): leave that one to Cz80 */
	if (CPU->ICount <= 0 || CPU->ExtraCycles)
		return -1;
	/* a pending maskable interrupt with IFF1 clear is a no-op per opcode */
	if (CPU->Status && (CPU->Status != CZ80_HAS_INT || zIFF1))
		return -1;
	ram = (UINT8 *)CPU->Fetch[0];
	if (memcmp(ram + 0x23, drvacc_code, 0x31 - 0x23) ||
	    memcmp(ram + 0x32, drvacc_code + (0x32 - 0x23), 0x5d - 0x32))
		return -1;

#define DA_RET(pc_)  { zR += n; return pc_; }
#define DA_STEP(pc_) if (CPU->ICount <= 0) DA_RET(pc_) n++;
	for (;;)
	{
		DA_STEP(0x23) zA = picodrive_read(0x4000); USE_CYCLES(13)	/* ld a,(4000h) */
		DA_STEP(0x26) zA &= 0x02; zF = SZP[zA] | HF; USE_CYCLES(7)	/* and 02h */
		DA_STEP(0x28)							/* jr z,0030h */
		if (!(zF & ZF)) { USE_CYCLES(7) DA_RET(0x2a) }			/* timer B: call 0436h */
		USE_CYCLES(12)
		DA_STEP(0x30) zB = ram[0x31]; USE_CYCLES(7)			/* ld b,N */
		for (;;)
		{
			DA_STEP(0x32) USE_CYCLES(1)				/* djnz $ */
			if (!--zB) { USE_CYCLES(7) break; }
			USE_CYCLES(12)
		}
		DA_STEP(0x34) zA = picodrive_read(0x1fff); USE_CYCLES(13)	/* ld a,(1fffh) */
		DA_STEP(0x37) zF = SZP[zA]; USE_CYCLES(4)			/* or a */
		if (!(zF & ZF)) DA_RET(0x38)					/* call nz,01dch */
		DA_STEP(0x38) USE_CYCLES(10)
		DA_STEP(0x3b) zA = picodrive_read(0x1ff8); USE_CYCLES(13)	/* ld a,(1ff8h) */
		DA_STEP(0x3e) zF = SZP[zA]; USE_CYCLES(4)			/* or a */
		if (!(zF & ZF)) DA_RET(0x3f)					/* call nz,01aah */
		DA_STEP(0x3f) USE_CYCLES(10)
		DA_STEP(0x42) zA = zD; USE_CYCLES(4)				/* ld a,d */
		DA_STEP(0x43) zA |= zE; zF = SZP[zA]; USE_CYCLES(4)		/* or e */
		DA_STEP(0x44)							/* jr z,0023h */
		if (zF & ZF) { USE_CYCLES(12) continue; }
		USE_CYCLES(7)
		DA_STEP(0x46) zB = 0x2a; USE_CYCLES(7)				/* ld b,2ah */
		DA_STEP(0x48) zC = picodrive_read(zHL); USE_CYCLES(7)		/* ld c,(hl) */
		DA_STEP(0x49) zHL++; USE_CYCLES(6)				/* inc hl */
		for (;;)
		{
			DA_STEP(0x4a) zA = picodrive_read(0x4000); USE_CYCLES(13) /* ld a,(4000h) */
			DA_STEP(0x4d) zA &= 0x80; zF = SZP[zA] | HF; USE_CYCLES(7) /* and 80h */
			DA_STEP(0x4f)						/* jr nz,004ah */
			if (zF & ZF) { USE_CYCLES(7) break; }
			USE_CYCLES(12)
		}
		DA_STEP(0x51) zA = zB; USE_CYCLES(4)				/* ld a,b */
		DA_STEP(0x52) WRITE_MEM8(0x4000, zA); USE_CYCLES(13)		/* ld (4000h),a */
		DA_STEP(0x55) zA = zC; USE_CYCLES(4)				/* ld a,c */
		DA_STEP(0x56) WRITE_MEM8(0x4001, zA); USE_CYCLES(13)		/* ld (4001h),a */
		DA_STEP(0x59) zDE--; USE_CYCLES(6)				/* dec de */
		DA_STEP(0x5a) USE_CYCLES(10)					/* jp 0023h */
	}
#undef DA_STEP
#undef DA_RET
}
#endif

/*--------------------------------------------------------
	CPU実行
--------------------------------------------------------*/

INT32 Cz80_Exec(cz80_struc *CPU, INT32 cycles)
{
#if CZ80_USE_JUMPTABLE
#include "cz80jmp.c"
#endif

	FPTR PC;
#if CZ80_ENCRYPTED_ROM
	FPTR OPBase;
#endif
	UINT32 Opcode;
	UINT32 adr = 0;
	UINT32 res;
	UINT32 val;
	union16 *data;

	PC = CPU->PC;
#if CZ80_ENCRYPTED_ROM
	OPBase = CPU->OPBase;
#endif
	CPU->ICount = cycles - CPU->ExtraCycles;
	CPU->ExtraCycles = 0;

Cz80_Exec:
	if (CPU->Status)
	{
		if (CPU->Status & CZ80_HAS_NMI)
		{
			zIFF1 = 0;
			CPU->Status &= ~(CZ80_HALTED | CZ80_HAS_NMI);
			CPU->ExtraCycles += 11;
			PUSH_16(zRealPC);
			SET_PC(0x66);
		} else if (CPU->Status & CZ80_HAS_INT)
		{
			CHECK_INT
		} else if (CPU->Status & CZ80_HALTED)
		{
			goto Cz80_Exec_End;
		}
		CPU->ICount -= CPU->ExtraCycles;
		CPU->ExtraCycles = 0;
	}

	if (CPU->ICount > 0)
	{
Cz80_Exec_nocheck:
#if PICODRIVE_HACKS && defined(Z80_DRIVER_ACCEL)
		if (PC == CPU->Fetch[0] + 0x23)
		{
			INT32 npc = z80_driver_accel(CPU);
			if (npc >= 0)
			{
				SET_PC(npc);
				goto Cz80_Exec;
			}
		}
#endif
		data = pzHL;
#ifdef Z80_PCHIST
		{ extern unsigned int z80_pchist[0x2000]; z80_pchist[zRealPC & 0x1fff]++; }
#endif
		Opcode = READ_OP();
#if CZ80_EMULATE_R_EXACTLY
		zR++;
#endif
		#include "cz80_op.c"
	}

Cz80_Exec_End:
	CPU->PC = PC;
#if CZ80_ENCRYPTED_ROM
	CPU->OPBase = OPBase;
#endif
	if (!((CPU->Status & CZ80_HALTED) && CPU->ICount > 0))
		cycles -= CPU->ICount;
	CPU->ICount = 0;
#if !CZ80_EMULATE_R_EXACTLY
	zR = (zR + (cycles >> 2)) & 0x7f;
#endif

	return cycles;
}


/*--------------------------------------------------------
	割り込み処理
--------------------------------------------------------*/

void Cz80_Set_IRQ(cz80_struc *CPU, INT32 line, INT32 state)
{
	if (line == IRQ_LINE_NMI)
	{
		if (state)
			CPU->Status |= CZ80_HAS_NMI;
		else
			CPU->Status &= ~CZ80_HAS_NMI;
	} else
	{
		CPU->IRQLine = line;
		CPU->IRQState = state;
		if (state)
		{
			if (zIFF1)
				CPU->Status |= CZ80_HAS_INT;
		} else
		{
			CPU->Status &= ~CZ80_HAS_INT;
		}
	}
}


/*--------------------------------------------------------
	レジスタ取得
--------------------------------------------------------*/

UINT32 Cz80_Get_Reg(cz80_struc *CPU, INT32 regnum)
{
	switch (regnum)
	{
	case CZ80_PC:   return (CPU->PC - CPU->BasePC);
	case CZ80_SP:   return zSP;
	case CZ80_FA:   return zFA;
	case CZ80_BC:   return zBC;
	case CZ80_DE:   return zDE;
	case CZ80_HL:   return zHL;
	case CZ80_IX:   return zIX;
	case CZ80_IY:   return zIY;
	case CZ80_FA2:  return zFA2;
	case CZ80_BC2:  return zBC2;
	case CZ80_DE2:  return zDE2;
	case CZ80_HL2:  return zHL2;
	case CZ80_R:    return zR;
	case CZ80_I:    return zI;
	case CZ80_IM:   return zIM;
	case CZ80_IFF1: return zIFF1;
	case CZ80_IFF2: return zIFF2;
	case CZ80_HALT: return CPU->Status & CZ80_HALTED;
	case CZ80_IRQ:  return CPU->IRQState;
	default: return 0;
	}
}


/*--------------------------------------------------------
	レジスタ設定
--------------------------------------------------------*/

void Cz80_Set_Reg(cz80_struc *CPU, INT32 regnum, UINT32 val)
{
	switch (regnum)
	{
	case CZ80_PC:
		CPU->BasePC = CPU->Fetch[val >> CZ80_FETCH_SFT];
#if CZ80_ENCRYPTED_ROM
		CPU->OPBase = CPU->OPFetch[val >> CZ80_FETCH_SFT];
#endif
		CPU->PC = val + CPU->BasePC;
		break;

	case CZ80_SP:   zSP = val; break;
	case CZ80_FA:   zFA = val; break;
	case CZ80_BC:   zBC = val; break;
	case CZ80_DE:   zDE = val; break;
	case CZ80_HL:   zHL = val; break;
	case CZ80_IX:   zIX = val; break;
	case CZ80_IY:   zIY = val; break;
	case CZ80_FA2:  zFA2 = val; break;
	case CZ80_BC2:  zBC2 = val; break;
	case CZ80_DE2:  zDE2 = val; break;
	case CZ80_HL2:  zHL2 = val; break;
	case CZ80_R:    zR = val; break;
	case CZ80_I:    zI = val; break;
	case CZ80_IM:   zIM = val; break;
	case CZ80_IFF1: zIFF1 = val ? (1 << 2) : 0; break;
	case CZ80_IFF2: zIFF2 = val ? (1 << 2) : 0; break;
	case CZ80_HALT: CPU->Status = !!val * CZ80_HALTED; break;
	case CZ80_IRQ:  CPU->IRQState = val; break;
	default: break;
	}
}


/*--------------------------------------------------------
	フェッチアドレス設定
--------------------------------------------------------*/

void Cz80_Set_Fetch(cz80_struc *CPU, UINT32 low_adr, UINT32 high_adr, FPTR fetch_adr)
{
	int i, j;

	i = low_adr >> CZ80_FETCH_SFT;
	j = high_adr >> CZ80_FETCH_SFT;
	fetch_adr -= i << CZ80_FETCH_SFT;

	while (i <= j)
	{
		CPU->Fetch[i] = fetch_adr;
#if CZ80_ENCRYPTED_ROM
		CPU->OPFetch[i] = 0;
#endif
		i++;
	}
}


/*--------------------------------------------------------
	フェッチアドレス設定 (暗号化ROM対応)
--------------------------------------------------------*/

#if CZ80_ENCRYPTED_ROM
void Cz80_Set_Encrypt_Range(cz80_struc *CPU, UINT32 low_adr, UINT32 high_adr, UINT32 decrypted_rom)
{
	int i, j;

	i = low_adr >> CZ80_FETCH_SFT;
	j = high_adr >> CZ80_FETCH_SFT;
	decrypted_rom -= i << CZ80_FETCH_SFT;

	while (i <= j)
	{
		CPU->OPFetch[i] = (INT32)decrypted_rom - (INT32)CPU->Fetch[i];
		i++;
	}
}
#endif


/*--------------------------------------------------------
	メモリリード/ライト関数設定
--------------------------------------------------------*/

void Cz80_Set_ReadB(cz80_struc *CPU, UINT8 (*Func)(UINT32 address))
{
	CPU->Read_Byte = Func;
}

void Cz80_Set_WriteB(cz80_struc *CPU, void (*Func)(UINT32 address, UINT8 data))
{
	CPU->Write_Byte = Func;
}


/*--------------------------------------------------------
	ポートリード/ライト関数設定
--------------------------------------------------------*/

void Cz80_Set_INPort(cz80_struc *CPU, UINT8 (*Func)(UINT16 port))
{
	CPU->IN_Port = Func;
}

void Cz80_Set_OUTPort(cz80_struc *CPU, void (*Func)(UINT16 port, UINT8 value))
{
	CPU->OUT_Port = Func;
}


/*--------------------------------------------------------
	コールバック関数設定
--------------------------------------------------------*/

void Cz80_Set_IRQ_Callback(cz80_struc *CPU, INT32 (*Func)(INT32 irqline))
{
	CPU->Interrupt_Callback = Func;
}
