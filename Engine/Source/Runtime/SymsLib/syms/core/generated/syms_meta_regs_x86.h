// Copyright Epic Games, Inc. All Rights Reserved.
// generated
#ifndef _SYMS_META_REGS_X86_H
#define _SYMS_META_REGS_X86_H
//~ generated from code at syms/metaprogram/syms_metaprogram_regs.c:322
typedef struct SYMS_RegX86{
SYMS_Reg32 eax;
SYMS_Reg32 ecx;
SYMS_Reg32 edx;
SYMS_Reg32 ebx;
SYMS_Reg32 esp;
SYMS_Reg32 ebp;
SYMS_Reg32 esi;
SYMS_Reg32 edi;
SYMS_Reg32 fsbase;
SYMS_Reg32 gsbase;
SYMS_Reg32 eflags;
SYMS_Reg32 eip;
SYMS_Reg256 ymm0;
SYMS_Reg256 ymm1;
SYMS_Reg256 ymm2;
SYMS_Reg256 ymm3;
SYMS_Reg256 ymm4;
SYMS_Reg256 ymm5;
SYMS_Reg256 ymm6;
SYMS_Reg256 ymm7;
SYMS_Reg32 dr0;
SYMS_Reg32 dr1;
SYMS_Reg32 dr2;
SYMS_Reg32 dr3;
SYMS_Reg32 dr4;
SYMS_Reg32 dr5;
SYMS_Reg32 dr6;
SYMS_Reg32 dr7;
SYMS_Reg80 fpr0;
SYMS_Reg80 fpr1;
SYMS_Reg80 fpr2;
SYMS_Reg80 fpr3;
SYMS_Reg80 fpr4;
SYMS_Reg80 fpr5;
SYMS_Reg80 fpr6;
SYMS_Reg80 fpr7;
SYMS_Reg80 st0;
SYMS_Reg80 st1;
SYMS_Reg80 st2;
SYMS_Reg80 st3;
SYMS_Reg80 st4;
SYMS_Reg80 st5;
SYMS_Reg80 st6;
SYMS_Reg80 st7;
SYMS_Reg16 fcw;
SYMS_Reg16 fsw;
SYMS_Reg16 ftw;
SYMS_Reg16 fop;
SYMS_Reg16 fcs;
SYMS_Reg16 fds;
SYMS_Reg32 fip;
SYMS_Reg32 fdp;
SYMS_Reg32 mxcsr;
SYMS_Reg32 mxcsr_mask;
SYMS_Reg16 ss;
SYMS_Reg16 cs;
SYMS_Reg16 ds;
SYMS_Reg16 es;
SYMS_Reg16 fs;
SYMS_Reg16 gs;
} SYMS_RegX86;
//~ generated from code at syms/metaprogram/syms_metaprogram_regs.c:341
typedef enum SYMS_RegX86Code
{
SYMS_RegX86Code_Null,
SYMS_RegX86Code_eax,
SYMS_RegX86Code_ecx,
SYMS_RegX86Code_edx,
SYMS_RegX86Code_ebx,
SYMS_RegX86Code_esp,
SYMS_RegX86Code_ebp,
SYMS_RegX86Code_esi,
SYMS_RegX86Code_edi,
SYMS_RegX86Code_fsbase,
SYMS_RegX86Code_gsbase,
SYMS_RegX86Code_eflags,
SYMS_RegX86Code_eip,
SYMS_RegX86Code_ymm0,
SYMS_RegX86Code_ymm1,
SYMS_RegX86Code_ymm2,
SYMS_RegX86Code_ymm3,
SYMS_RegX86Code_ymm4,
SYMS_RegX86Code_ymm5,
SYMS_RegX86Code_ymm6,
SYMS_RegX86Code_ymm7,
SYMS_RegX86Code_dr0,
SYMS_RegX86Code_dr1,
SYMS_RegX86Code_dr2,
SYMS_RegX86Code_dr3,
SYMS_RegX86Code_dr4,
SYMS_RegX86Code_dr5,
SYMS_RegX86Code_dr6,
SYMS_RegX86Code_dr7,
SYMS_RegX86Code_fpr0,
SYMS_RegX86Code_fpr1,
SYMS_RegX86Code_fpr2,
SYMS_RegX86Code_fpr3,
SYMS_RegX86Code_fpr4,
SYMS_RegX86Code_fpr5,
SYMS_RegX86Code_fpr6,
SYMS_RegX86Code_fpr7,
SYMS_RegX86Code_st0,
SYMS_RegX86Code_st1,
SYMS_RegX86Code_st2,
SYMS_RegX86Code_st3,
SYMS_RegX86Code_st4,
SYMS_RegX86Code_st5,
SYMS_RegX86Code_st6,
SYMS_RegX86Code_st7,
SYMS_RegX86Code_fcw,
SYMS_RegX86Code_fsw,
SYMS_RegX86Code_ftw,
SYMS_RegX86Code_fop,
SYMS_RegX86Code_fcs,
SYMS_RegX86Code_fds,
SYMS_RegX86Code_fip,
SYMS_RegX86Code_fdp,
SYMS_RegX86Code_mxcsr,
SYMS_RegX86Code_mxcsr_mask,
SYMS_RegX86Code_ss,
SYMS_RegX86Code_cs,
SYMS_RegX86Code_ds,
SYMS_RegX86Code_es,
SYMS_RegX86Code_fs,
SYMS_RegX86Code_gs,
// ALIASES BEGIN
SYMS_RegX86Code_ax,
SYMS_RegX86Code_cx,
SYMS_RegX86Code_bx,
SYMS_RegX86Code_dx,
SYMS_RegX86Code_sp,
SYMS_RegX86Code_bp,
SYMS_RegX86Code_si,
SYMS_RegX86Code_di,
SYMS_RegX86Code_ip,
SYMS_RegX86Code_ah,
SYMS_RegX86Code_ch,
SYMS_RegX86Code_dh,
SYMS_RegX86Code_bh,
SYMS_RegX86Code_al,
SYMS_RegX86Code_cl,
SYMS_RegX86Code_dl,
SYMS_RegX86Code_bl,
SYMS_RegX86Code_bpl,
SYMS_RegX86Code_spl,
SYMS_RegX86Code_xmm0,
SYMS_RegX86Code_xmm1,
SYMS_RegX86Code_xmm2,
SYMS_RegX86Code_xmm3,
SYMS_RegX86Code_xmm4,
SYMS_RegX86Code_xmm5,
SYMS_RegX86Code_xmm6,
SYMS_RegX86Code_xmm7,
SYMS_RegX86Code_mm0,
SYMS_RegX86Code_mm1,
SYMS_RegX86Code_mm2,
SYMS_RegX86Code_mm3,
SYMS_RegX86Code_mm4,
SYMS_RegX86Code_mm5,
SYMS_RegX86Code_mm6,
SYMS_RegX86Code_mm7,
SYMS_RegX86Code_COUNT
} SYMS_RegX86Code;
//~ generated from code at syms/metaprogram/syms_metaprogram_regs.c:367
static SYMS_RegMetadata reg_metadata_X86[SYMS_RegX86Code_COUNT] = {
{0, 0, {(SYMS_U8*)"<nil>", 5}, 0, 0},
{0, 4, {(SYMS_U8*)"eax", 3}, REG_ClassX86X64_GPR, 0},
{0, 4, {(SYMS_U8*)"ecx", 3}, REG_ClassX86X64_GPR, 0},
{0, 4, {(SYMS_U8*)"edx", 3}, REG_ClassX86X64_GPR, 0},
{0, 4, {(SYMS_U8*)"ebx", 3}, REG_ClassX86X64_GPR, 0},
{0, 4, {(SYMS_U8*)"esp", 3}, REG_ClassX86X64_GPR, 0},
{0, 4, {(SYMS_U8*)"ebp", 3}, REG_ClassX86X64_GPR, 0},
{0, 4, {(SYMS_U8*)"esi", 3}, REG_ClassX86X64_GPR, 0},
{0, 4, {(SYMS_U8*)"edi", 3}, REG_ClassX86X64_GPR, 0},
{0, 4, {(SYMS_U8*)"fsbase", 6}, REG_ClassX86X64_GPR, 0},
{0, 4, {(SYMS_U8*)"gsbase", 6}, REG_ClassX86X64_GPR, 0},
{0, 4, {(SYMS_U8*)"eflags", 6}, REG_ClassX86X64_STATE, 0},
{0, 4, {(SYMS_U8*)"eip", 3}, REG_ClassX86X64_STATE, 0},
{0, 32, {(SYMS_U8*)"ymm0", 4}, REG_ClassX86X64_VEC, 0},
{0, 32, {(SYMS_U8*)"ymm1", 4}, REG_ClassX86X64_VEC, 0},
{0, 32, {(SYMS_U8*)"ymm2", 4}, REG_ClassX86X64_VEC, 0},
{0, 32, {(SYMS_U8*)"ymm3", 4}, REG_ClassX86X64_VEC, 0},
{0, 32, {(SYMS_U8*)"ymm4", 4}, REG_ClassX86X64_VEC, 0},
{0, 32, {(SYMS_U8*)"ymm5", 4}, REG_ClassX86X64_VEC, 0},
{0, 32, {(SYMS_U8*)"ymm6", 4}, REG_ClassX86X64_VEC, 0},
{0, 32, {(SYMS_U8*)"ymm7", 4}, REG_ClassX86X64_VEC, 0},
{0, 4, {(SYMS_U8*)"dr0", 3}, REG_ClassX86X64_CTRL, 0},
{0, 4, {(SYMS_U8*)"dr1", 3}, REG_ClassX86X64_CTRL, 0},
{0, 4, {(SYMS_U8*)"dr2", 3}, REG_ClassX86X64_CTRL, 0},
{0, 4, {(SYMS_U8*)"dr3", 3}, REG_ClassX86X64_CTRL, 0},
{0, 4, {(SYMS_U8*)"dr4", 3}, REG_ClassX86X64_CTRL, 0},
{0, 4, {(SYMS_U8*)"dr5", 3}, REG_ClassX86X64_CTRL, 0},
{0, 4, {(SYMS_U8*)"dr6", 3}, REG_ClassX86X64_CTRL, 0},
{0, 4, {(SYMS_U8*)"dr7", 3}, REG_ClassX86X64_CTRL, 0},
{0, 10, {(SYMS_U8*)"fpr0", 4}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"fpr1", 4}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"fpr2", 4}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"fpr3", 4}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"fpr4", 4}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"fpr5", 4}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"fpr6", 4}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"fpr7", 4}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"st0", 3}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"st1", 3}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"st2", 3}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"st3", 3}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"st4", 3}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"st5", 3}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"st6", 3}, REG_ClassX86X64_FP, 0},
{0, 10, {(SYMS_U8*)"st7", 3}, REG_ClassX86X64_FP, 0},
{0, 2, {(SYMS_U8*)"fcw", 3}, REG_ClassX86X64_CTRL, 0},
{0, 2, {(SYMS_U8*)"fsw", 3}, REG_ClassX86X64_CTRL, 0},
{0, 2, {(SYMS_U8*)"ftw", 3}, REG_ClassX86X64_CTRL, 0},
{0, 2, {(SYMS_U8*)"fop", 3}, REG_ClassX86X64_CTRL, 0},
{0, 2, {(SYMS_U8*)"fcs", 3}, REG_ClassX86X64_CTRL, 0},
{0, 2, {(SYMS_U8*)"fds", 3}, REG_ClassX86X64_CTRL, 0},
{0, 4, {(SYMS_U8*)"fip", 3}, REG_ClassX86X64_CTRL, 0},
{0, 4, {(SYMS_U8*)"fdp", 3}, REG_ClassX86X64_CTRL, 0},
{0, 4, {(SYMS_U8*)"mxcsr", 5}, REG_ClassX86X64_CTRL, 0},
{0, 4, {(SYMS_U8*)"mxcsr_mask", 10}, REG_ClassX86X64_CTRL, 0},
{0, 2, {(SYMS_U8*)"ss", 2}, REG_ClassX86X64_GPR, 0},
{0, 2, {(SYMS_U8*)"cs", 2}, REG_ClassX86X64_GPR, 0},
{0, 2, {(SYMS_U8*)"ds", 2}, REG_ClassX86X64_GPR, 0},
{0, 2, {(SYMS_U8*)"es", 2}, REG_ClassX86X64_GPR, 0},
{0, 2, {(SYMS_U8*)"fs", 2}, REG_ClassX86X64_GPR, 0},
{0, 2, {(SYMS_U8*)"gs", 2}, REG_ClassX86X64_GPR, 0},
// ALIASES BEGIN
{0, 2, {(SYMS_U8*)"ax", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_eax},
{0, 2, {(SYMS_U8*)"cx", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_ecx},
{0, 2, {(SYMS_U8*)"bx", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_ebx},
{0, 2, {(SYMS_U8*)"dx", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_edx},
{0, 2, {(SYMS_U8*)"sp", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_esp},
{0, 2, {(SYMS_U8*)"bp", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_ebp},
{0, 2, {(SYMS_U8*)"si", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_esi},
{0, 2, {(SYMS_U8*)"di", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_edi},
{0, 2, {(SYMS_U8*)"ip", 2}, REG_ClassX86X64_STATE, SYMS_RegX86Code_eip},
{1, 1, {(SYMS_U8*)"ah", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_eax},
{1, 1, {(SYMS_U8*)"ch", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_ecx},
{1, 1, {(SYMS_U8*)"dh", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_edx},
{1, 1, {(SYMS_U8*)"bh", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_ebx},
{0, 1, {(SYMS_U8*)"al", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_eax},
{0, 1, {(SYMS_U8*)"cl", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_ecx},
{0, 1, {(SYMS_U8*)"dl", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_edx},
{0, 1, {(SYMS_U8*)"bl", 2}, REG_ClassX86X64_GPR, SYMS_RegX86Code_ebx},
{0, 1, {(SYMS_U8*)"bpl", 3}, REG_ClassX86X64_GPR, SYMS_RegX86Code_ebp},
{0, 1, {(SYMS_U8*)"spl", 3}, REG_ClassX86X64_GPR, SYMS_RegX86Code_esp},
{0, 16, {(SYMS_U8*)"xmm0", 4}, REG_ClassX86X64_VEC, SYMS_RegX86Code_ymm0},
{0, 16, {(SYMS_U8*)"xmm1", 4}, REG_ClassX86X64_VEC, SYMS_RegX86Code_ymm1},
{0, 16, {(SYMS_U8*)"xmm2", 4}, REG_ClassX86X64_VEC, SYMS_RegX86Code_ymm2},
{0, 16, {(SYMS_U8*)"xmm3", 4}, REG_ClassX86X64_VEC, SYMS_RegX86Code_ymm3},
{0, 16, {(SYMS_U8*)"xmm4", 4}, REG_ClassX86X64_VEC, SYMS_RegX86Code_ymm4},
{0, 16, {(SYMS_U8*)"xmm5", 4}, REG_ClassX86X64_VEC, SYMS_RegX86Code_ymm5},
{0, 16, {(SYMS_U8*)"xmm6", 4}, REG_ClassX86X64_VEC, SYMS_RegX86Code_ymm6},
{0, 16, {(SYMS_U8*)"xmm7", 4}, REG_ClassX86X64_VEC, SYMS_RegX86Code_ymm7},
{0, 8, {(SYMS_U8*)"mm0", 3}, REG_ClassX86X64_FP, SYMS_RegX86Code_fpr0},
{0, 8, {(SYMS_U8*)"mm1", 3}, REG_ClassX86X64_FP, SYMS_RegX86Code_fpr1},
{0, 8, {(SYMS_U8*)"mm2", 3}, REG_ClassX86X64_FP, SYMS_RegX86Code_fpr2},
{0, 8, {(SYMS_U8*)"mm3", 3}, REG_ClassX86X64_FP, SYMS_RegX86Code_fpr3},
{0, 8, {(SYMS_U8*)"mm4", 3}, REG_ClassX86X64_FP, SYMS_RegX86Code_fpr4},
{0, 8, {(SYMS_U8*)"mm5", 3}, REG_ClassX86X64_FP, SYMS_RegX86Code_fpr5},
{0, 8, {(SYMS_U8*)"mm6", 3}, REG_ClassX86X64_FP, SYMS_RegX86Code_fpr6},
{0, 8, {(SYMS_U8*)"mm7", 3}, REG_ClassX86X64_FP, SYMS_RegX86Code_fpr7},
};

#endif
