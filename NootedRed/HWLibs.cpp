// AMDRadeonX5000HWLibs Patches
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include "GoldenSettings.hpp"
#include <ASICCaps.hpp>
#include <GPUDriversAMD/CAIL/ASICCaps.hpp>
#include <GPUDriversAMD/CAIL/DevCaps.hpp>
#include <GPUDriversAMD/CAIL/DeviceType.hpp>
#include <GPUDriversAMD/CAIL/Result.hpp>
#include <GPUDriversAMD/Family.hpp>
#include <GPUDriversAMD/PSP.hpp>
#include <GPUDriversAMD/RavenIPOffset.hpp>
#include <GPUDriversAMD/RenoirPPSMC.hpp>
#include <GPUDriversAMD/PhoenixPPSMC.hpp>
#include <GPUDriversAMD/TTL/Event.hpp>
#include <GPUDriversAMD/TTL/SWIP/DMCU.hpp>
#include <GPUDriversAMD/TTL/SWIP/GC.hpp>
#include <GPUDriversAMD/TTL/SWIP/IPVersion.hpp>
#include <GPUDriversAMD/TTL/SWIP/SDMA.hpp>
#include <GPUDriversAMD/TTL/SWIP/SMU.hpp>
#include <HWLibs.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <PenguinWizardry/PatcherPlus.hpp>
#include <Regs/SDMA0.hpp>
#include <Regs/SMU.hpp>
#include <Regs/VBIOSSMC.hpp>
#include <kern/assert.h>
#include <libkern/OSTypes.h>
#include <libkern/c++/OSBoolean.h>
#include <mach/i386/vm_types.h>
#include <mach/kern_return.h>

static const char ativvaxy_rv_dat[] = {
#embed "Firmware/ativvaxy_rv.dat"
};
static const char ativvaxy_nv_dat[] = {
#embed "Firmware/ativvaxy_nv.dat"
};
static const char atidmcub_rn_dat[] = {
#embed "Firmware/atidmcub_rn.dat"
};

static const char _dmcu_eram_dcn10_abm_2_1[] = {
#embed "Firmware/dmcu_eram_dcn10_abm_2_1.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn10_abm_2_1);
static const char _dmcu_eram_dcn10_abm_2_2[] = {
#embed "Firmware/dmcu_eram_dcn10_abm_2_2.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn10_abm_2_2);
static const char _dmcu_eram_dcn10_abm_2_3[] = {
#embed "Firmware/dmcu_eram_dcn10_abm_2_3.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn10_abm_2_3);
static const char _dmcu_eram_dcn21_abm_2_1[] = {
#embed "Firmware/dmcu_eram_dcn21_abm_2_1.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn21_abm_2_1);
static const char _dmcu_eram_dcn21_abm_2_2[] = {
#embed "Firmware/dmcu_eram_dcn21_abm_2_2.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn21_abm_2_2);
static const char _dmcu_eram_dcn21_abm_2_3[] = {
#embed "Firmware/dmcu_eram_dcn21_abm_2_3.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn21_abm_2_3);
static const char _dmcu_eram_dcn21_abm_2_4[] = {
#embed "Firmware/dmcu_eram_dcn21_abm_2_4.bin"
};
DMCU_FW_CONSTANT(0x100, dmcu_eram_dcn21_abm_2_4);
static const char _dmcu_intvectors_dcn10_abm_2_1[] = {
#embed "Firmware/dmcu_intvectors_dcn10_abm_2_1.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn10_abm_2_1);
static const char _dmcu_intvectors_dcn10_abm_2_2[] = {
#embed "Firmware/dmcu_intvectors_dcn10_abm_2_2.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn10_abm_2_2);
static const char _dmcu_intvectors_dcn10_abm_2_3[] = {
#embed "Firmware/dmcu_intvectors_dcn10_abm_2_3.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn10_abm_2_3);
static const char _dmcu_intvectors_dcn21_abm_2_1[] = {
#embed "Firmware/dmcu_intvectors_dcn21_abm_2_1.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn21_abm_2_1);
static const char _dmcu_intvectors_dcn21_abm_2_2[] = {
#embed "Firmware/dmcu_intvectors_dcn21_abm_2_2.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn21_abm_2_2);
static const char _dmcu_intvectors_dcn21_abm_2_3[] = {
#embed "Firmware/dmcu_intvectors_dcn21_abm_2_3.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn21_abm_2_3);
static const char _dmcu_intvectors_dcn21_abm_2_4[] = {
#embed "Firmware/dmcu_intvectors_dcn21_abm_2_4.bin"
};
DMCU_FW_CONSTANT(0xFFE0, dmcu_intvectors_dcn21_abm_2_4);

static const char _gc_9_1_ce_ucode[] = {
#embed "Firmware/gc_9_1_ce_ucode.bin"
};
GC_FW_CONSTANT("#80", 0x36, 0x800, 0x60, 0x1, 0x0, gc_9_1_ce_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_me_ucode[] = {
#embed "Firmware/gc_9_1_me_ucode.bin"
};
GC_FW_CONSTANT("#167", 0x36, 0x1000, 0x60, 0x1, 0x0, gc_9_1_me_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_mec_jt_ucode[] = {
#embed "Firmware/gc_9_1_mec_jt_ucode.bin"
};
GC_FW_CONSTANT("#480", 0x36, 0x10000, 0x0, 0x1, 0x0, gc_9_1_mec_jt_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_mec_ucode[] = {
#embed "Firmware/gc_9_1_mec_ucode.bin"
};
GC_FW_CONSTANT("#480", 0x36, 0x0, 0x0, 0x0, 0x0, gc_9_1_mec_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_pfp_ucode[] = {
#embed "Firmware/gc_9_1_pfp_ucode.bin"
};
GC_FW_CONSTANT("#196", 0x36, 0x1400, 0x60, 0x1, 0x0, gc_9_1_pfp_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_rlc_srlist_cntl[] = {
#embed "Firmware/gc_9_1_rlc_srlist_cntl.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_1_rlc_srlist_cntl, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_rlc_srlist_gpm_mem[] = {
#embed "Firmware/gc_9_1_rlc_srlist_gpm_mem.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_1_rlc_srlist_gpm_mem, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_rlc_srlist_srm_mem[] = {
#embed "Firmware/gc_9_1_rlc_srlist_srm_mem.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_1_rlc_srlist_srm_mem, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_rlc_ucode[] = {
#embed "Firmware/gc_9_1_rlc_ucode.bin"
};
GC_FW_CONSTANT("#110", 0x1, 0x1000, 0x0, 0x1, 0x0, gc_9_1_rlc_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_rlc_ucode_a0[] = {
#embed "Firmware/gc_9_1_rlc_ucode_a0.bin"
};
GC_FW_CONSTANT("#568", 0x1, 0x1000, 0x0, 0x1, 0x0, gc_9_1_rlc_ucode_a0, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_1_rlcv_ucode[] = {
#embed "Firmware/gc_9_1_rlcv_ucode.bin"
};
GC_FW_CONSTANT("#28", 0x1, 0x800, 0x0, 0x1, 0x0, gc_9_1_rlcv_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_ce_ucode[] = {
#embed "Firmware/gc_9_2_ce_ucode.bin"
};
GC_FW_CONSTANT("#80", 0x35, 0x800, 0x60, 0x1, 0x0, gc_9_2_ce_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_me_ucode[] = {
#embed "Firmware/gc_9_2_me_ucode.bin"
};
GC_FW_CONSTANT("#166", 0x35, 0x1000, 0x60, 0x1, 0x0, gc_9_2_me_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_mec_jt_ucode[] = {
#embed "Firmware/gc_9_2_mec_jt_ucode.bin"
};
GC_FW_CONSTANT("#480", 0x36, 0x0, 0x0, 0x1, 0x0, gc_9_2_mec_jt_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_mec_ucode[] = {
#embed "Firmware/gc_9_2_mec_ucode.bin"
};
GC_FW_CONSTANT("#480", 0x36, 0x0, 0x0, 0x0, 0x0, gc_9_2_mec_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_pfp_ucode[] = {
#embed "Firmware/gc_9_2_pfp_ucode.bin"
};
GC_FW_CONSTANT("#196", 0x36, 0x1400, 0x60, 0x1, 0x0, gc_9_2_pfp_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_rlc_srlist_cntl[] = {
#embed "Firmware/gc_9_2_rlc_srlist_cntl.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_2_rlc_srlist_cntl, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_rlc_srlist_gpm_mem[] = {
#embed "Firmware/gc_9_2_rlc_srlist_gpm_mem.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_2_rlc_srlist_gpm_mem, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_rlc_srlist_srm_mem[] = {
#embed "Firmware/gc_9_2_rlc_srlist_srm_mem.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_2_rlc_srlist_srm_mem, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_rlc_ucode[] = {
#embed "Firmware/gc_9_2_rlc_ucode.bin"
};
GC_FW_CONSTANT("#73", 0x1, 0x1000, 0x0, 0x1, 0x0, gc_9_2_rlc_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_2_rlcv_ucode[] = {
#embed "Firmware/gc_9_2_rlcv_ucode.bin"
};
GC_FW_CONSTANT("#28", 0x1, 0x800, 0x0, 0x1, 0x0, gc_9_2_rlcv_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_ce_ucode[] = {
#embed "Firmware/gc_9_3_ce_ucode.bin"
};
GC_FW_CONSTANT("#80", 0x36, 0x800, 0x60, 0x1, 0x0, gc_9_3_ce_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_me_ucode[] = {
#embed "Firmware/gc_9_3_me_ucode.bin"
};
GC_FW_CONSTANT("#167", 0x36, 0x1000, 0x60, 0x1, 0x0, gc_9_3_me_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_mec_jt_ucode[] = {
#embed "Firmware/gc_9_3_mec_jt_ucode.bin"
};
GC_FW_CONSTANT("#480", 0x36, 0x10000, 0x0, 0x1, 0x0, gc_9_3_mec_jt_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_mec_ucode[] = {
#embed "Firmware/gc_9_3_mec_ucode.bin"
};
GC_FW_CONSTANT("#480", 0x36, 0x0, 0x0, 0x0, 0x0, gc_9_3_mec_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_pfp_ucode[] = {
#embed "Firmware/gc_9_3_pfp_ucode.bin"
};
GC_FW_CONSTANT("#196", 0x36, 0x1400, 0x60, 0x1, 0x0, gc_9_3_pfp_ucode, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_rlc_srlist_cntl[] = {
#embed "Firmware/gc_9_3_rlc_srlist_cntl.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_3_rlc_srlist_cntl, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_rlc_srlist_gpm_mem[] = {
#embed "Firmware/gc_9_3_rlc_srlist_gpm_mem.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_3_rlc_srlist_gpm_mem, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_rlc_srlist_srm_mem[] = {
#embed "Firmware/gc_9_3_rlc_srlist_srm_mem.bin"
};
GC_FW_CONSTANT("#1", 0x1, 0x0, 0x0, 0x1, 0x0, gc_9_3_rlc_srlist_srm_mem, 0x0, 0x0, 0x0, 0x0);
static const char _gc_9_3_rlc_ucode[] = {
#embed "Firmware/gc_9_3_rlc_ucode.bin"
};
GC_FW_CONSTANT("#60", 0x1, 0x1000, 0x0, 0x1, 0x0, gc_9_3_rlc_ucode, 0x0, 0x0, 0x0, 0x0);

static const char psp_asd_bin[] = {
#embed "Firmware/psp_asd.bin"
};
static const char psp_auc_bin[] = {
#embed "Firmware/psp_auc.bin"
};
static const char psp_dtm_bin[] = {
#embed "Firmware/psp_dtm.bin"
};
static const char psp_fp_bin[] = {
#embed "Firmware/psp_fp.bin"
};
static const char psp_hdcp_bin[] = {
#embed "Firmware/psp_hdcp.bin"
};

static const char _sdma_4_1_ucode[] = {
#embed "Firmware/sdma_4_1_ucode.bin"
};
SDMA_FW_CONSTANT("40", sdma_4_1_ucode, 0x29, 0x0, 0x0);

static const UInt8 kDeviceTypeTablePattern[] = {0x60, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61, 0x68, 0x00, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x62, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                0x63, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x68, 0x00, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x67, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kCreateFirmwarePattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54,
                                                   0x53, 0x40, 0x89, 0xC0, 0x41, 0x89, 0xD0, 0x41, 0x89, 0xF0,
                                                   0x40, 0x89, 0xF0, 0xBF, 0x20, 0x00, 0x00, 0x00, 0xE8};
static const UInt8 kCreateFirmwarePatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                   0xFF, 0xF0, 0xFF, 0xF0, 0xFF, 0xFF, 0xF0, 0xFF, 0xFF, 0xF0,
                                                   0xF0, 0xFF, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static const UInt8 kPutFirmwarePattern[] = {0x55, 0x48, 0x89, 0xE5, 0x83, 0xFE, 0x08, 0x7F};

static const UInt8 kCailAsicCapsTableHWLibsPattern[] = {0x6E, 0x00, 0x00, 0x00, 0x98, 0x67, 0x00, 0x00,
                                                        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                                                        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kPspCmdKmSubmitPattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                                   0x54, 0x53, 0x50, 0x49, 0x89, 0xCD, 0x49, 0x89, 0xD7, 0x49, 0x89,
                                                   0xF4, 0x48, 0x89, 0xFB, 0x48, 0x8D, 0x75, 0xD0, 0xC7, 0x06, 0x00,
                                                   0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspCmdKmSubmitPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                   0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kPspCmdKmSubmitPattern1404[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                                   0x54, 0x53, 0x48, 0x83, 0xEC, 0x18, 0x49, 0x89, 0xCD, 0x49, 0x89,
                                                   0xD7, 0x49, 0x89, 0xF4, 0x49, 0x89, 0xFE, 0x48, 0x8D, 0x75, 0xD0,
                                                   0xC7, 0x06, 0x00, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspCmdKmSubmitPatternMask1404[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kCAILAsicCapsInitTablePattern[] = {0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x98, 0x67,
                                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kDeviceCapabilityTblPattern[] = {0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x00,
                                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x00, 0x00,
                                                    0x00, 0x00, 0x00, 0x00, 0xFE, 0xCA, 0xAD, 0xDE, 0x00, 0x00,
                                                    0x00, 0x00, 0xFE, 0xCA, 0xAD, 0xDE, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kPspReset31Pattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x31, 0xC0, 0x48,
                                               0x89, 0x45, 0xF0, 0x48, 0x89, 0x45, 0xE8, 0x48, 0x89, 0x45, 0xE0, 0x48,
                                               0x89, 0x45, 0xD8, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x85, 0xFF};
static const UInt8 kPspReset31Pattern1404[] = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x48, 0x83, 0xEC, 0x28,
                                               0x48, 0xC7, 0x45, 0xE8, 0x00, 0x00, 0x00, 0x00, 0xB8,
                                               0x01, 0x00, 0x00, 0x00, 0x48, 0x85, 0xFF};

static const UInt8 kPspBootloaderLoadSysdrv31Pattern[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x49, 0x89,
    0xFC, 0x31, 0xDB, 0x48, 0x89, 0x5D, 0xD0, 0x48, 0x89, 0x5D, 0xC8, 0x48, 0x89, 0x5D, 0xC0, 0x48, 0x89, 0x5D, 0xB8,
    0x4C, 0x8B, 0xB7, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xBF, 0x00, 0x00, 0x00, 0x00, 0xBE, 0x91, 0x00, 0x00, 0x00};
static const UInt8 kPspBootloaderLoadSysdrv31PatternMask[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const UInt8 kPspBootloaderLoadSysdrv31Pattern1404[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x28,
    0x48, 0x89, 0xFB, 0x48, 0xC7, 0x45, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xB7, 0x38, 0x0B, 0x00,
    0x00, 0x4C, 0x8B, 0xBF, 0x48, 0x0B, 0x00, 0x00, 0x45, 0x31, 0xE4, 0xBE, 0x91, 0x00, 0x00, 0x00};

static const UInt8 kPspBootloaderSetEccMode31Pattern[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x89,
    0xF7, 0x49, 0x89, 0xFE, 0x31, 0xDB, 0x48, 0x89, 0x5D, 0xD8, 0x48, 0x89, 0x5D, 0xD0, 0x48, 0x89, 0x5D,
    0xC8, 0x48, 0x89, 0x5D, 0xC0, 0xBE, 0xA4, 0x00, 0x00, 0x00, 0x31, 0xD2, 0xB9, 0x4B, 0x00, 0x00, 0x00};
static const UInt8 kPspBootloaderSetEccMode31Pattern1404[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x89, 0xF7,
    0x48, 0x89, 0xFB, 0x48, 0xC7, 0x45, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x45, 0x31, 0xF6, 0xBE, 0xA4, 0x00, 0x00,
    0x00, 0x31, 0xD2, 0xB9, 0x4B, 0x00, 0x00, 0x00, 0xE8, 0xA8, 0x5B, 0xFF, 0xFF, 0x3D, 0x00, 0x0C, 0x0B, 0x00};

static const UInt8 kPspBootloaderIsSosRunning31Pattern[] = {0x55, 0x48, 0x89, 0xE5, 0xBE, 0x91, 0x00, 0x00,
                                                            0x00, 0x31, 0xD2, 0xB9, 0x4B, 0x00, 0x00, 0x00};

static const UInt8 kPspBootloaderLoadSos31Pattern[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x89, 0xFF, 0x31,
    0xDB, 0x48, 0x89, 0x5D, 0xD8, 0x48, 0x89, 0x5D, 0xD0, 0x48, 0x89, 0x5D, 0xC8, 0x48, 0x89, 0x5D, 0xC0, 0x4C, 0x8B,
    0xB7, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xA7, 0x00, 0x00, 0x00, 0x00, 0xBE, 0x91, 0x00, 0x00, 0x00};
static const UInt8 kPspBootloaderLoadSos31PatternMask[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const UInt8 kPspBootloaderLoadSos31Pattern1404[] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48,
    0x89, 0xFB, 0x48, 0xC7, 0x45, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xB7, 0x38, 0x0B, 0x00,
    0x00, 0x4C, 0x8B, 0xA7, 0x48, 0x0B, 0x00, 0x00, 0x45, 0x31, 0xFF, 0xBE, 0x91, 0x00, 0x00, 0x00};

static const UInt8 kPspSecurityFeatureCapsSet31Pattern[] = {0x55, 0x48, 0x89, 0xE5, 0x80, 0xA7, 0x20, 0x31, 0x00, 0x00};
static const UInt8 kPspSecurityFeatureCapsSet31Pattern13[] = {0x55, 0x48, 0x89, 0xE5, 0x8B,
                                                              0x87, 0x18, 0x39, 0x00, 0x00};

static const UInt8 kGcSetFwEntryInfoCallPattern[] = {0x14, 0x4C, 0x89, 0xF1, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x80, 0xC0};
static const UInt8 kGcSetFwEntryInfoCallPatternMask[]           = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
                                                                   0x00, 0x00, 0x00, 0xF0, 0xF0};
static constexpr size_t kGcSetFwEntryInfoCallPatternJumpInstOff = 0x4;

static const UInt8      kSdmaInitFuncPtrListCallPattern[]          = {0x44, 0x89, 0x00, 0x44, 0x89, 0x00, 0xE8, 0x00,
                                                                      0x00, 0x00, 0x00, 0x48, 0x8B, 0x00, 0x00, 0x85};
static const UInt8      kSdmaInitFuncPtrListCallPatternMask[]      = {0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0x00, 0xFF, 0x00,
                                                                      0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF};
static constexpr size_t kSdmaInitFuncPtrListCallPatternJumpInstOff = 0x6;

static const UInt8      kDmcuBackdoorLoadFwBranchPattern[]        = {0x8D, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x83, 0xF8,
                                                                     0x02, 0x0F, 0x82, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kDmcuBackdoorLoadFwBranchPatternMask[]    = {0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                     0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kDmcuBackdoorLoadFwDcn1ConstantsBranchOff = 0x9;
static constexpr size_t kDmcuGetDcn1FwConstantsCallOff            = 0x24;
static constexpr size_t kDmcuGetDcn21FwConstantsCallOff           = 0x4C;

static const UInt8      kSmuInitFunctionPointerListCallPattern[]          = {0x49, 0x8B, 0x00, 0x0C, 0x40, 0x8B, 0x00,
                                                                             0x14, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kSmuInitFunctionPointerListCallPatternMask[]      = {0xFF, 0xFF, 0x00, 0xFF, 0xF0, 0xFF, 0x00,
                                                                             0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kSmuInitFunctionPointerListCallPatternJumpInstOff = 0x8;

static const UInt8      kSmu90SendMessageWithParameterCallPattern[]          = {0xBE, 0x45, 0x00, 0x00, 0x00, 0x5D,
                                                                                0xE9, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kSmu90SendMessageWithParameterCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                                0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kSmu90SendMessageWithParameterCallPatternJumpInstOff = 6;

static const UInt8      kSdmaCgsReadRegisterCallPattern[]          = {0xBE, 0x80, 0x00, 0x00, 0x00, 0x31, 0xD2, 0x44,
                                                                      0x89, 0xF0, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kSdmaCgsReadRegisterCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                      0xFF, 0xF0, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kSdmaCgsReadRegisterCallPatternJumpInstOff = 10;

static const UInt8      kSdmaCgsWriteRegisterCallPattern[]     = {0xBE, 0x80, 0x00, 0x00, 0x00, 0x31, 0xD2, 0x89, 0xC1,
                                                                  0x45, 0x89, 0xF0, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kSdmaCgsWriteRegisterCallPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                  0xFF, 0xFF, 0xF0, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kSdmaCgsWriteRegisterCallPatternJumpInstOff = 12;

static const UInt8      kSmuCosWaitForCallPattern[]          = {0xE8, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC0};
static const UInt8      kSmuCosWaitForCallPatternMask[]      = {0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF};
static constexpr size_t kSmuCosWaitForCallPatternJumpInstOff = 0;

static const UInt8      kSmuCgsWriteRegisterCallPattern[]          = {0x41, 0xB8, 0x04, 0x00, 0x00, 0x00, 0x45,
                                                                      0x31, 0xC9, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kSmuCgsWriteRegisterCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                      0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kSmuCgsWriteRegisterCallPatternJumpInstOff = 9;

static const UInt8      kSmuCgsReadRegisterCallPattern[]          = {0xB9, 0x04, 0x00, 0x00, 0x00, 0x45, 0x31,
                                                                     0xC0, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kSmuCgsReadRegisterCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                     0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kSmuCgsReadRegisterCallPatternJumpInstOff = 8;

// Replace call to `_gc_get_hw_version` with constant (0x090001).
static const UInt8 kGcSwInitOriginal[]     = {0x0C, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x41, 0x89, 0xC7};
static const UInt8 kGcSwInitOriginalMask[] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
static const UInt8 kGcSwInitPatched[]      = {0x00, 0xB8, 0x01, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kGcSwInitPatchedMask[]  = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00};

// Force PSP 9.x switch case.
static const UInt8 kPspSwInit1Original[] = {0x8B, 0x43, 0x0C, 0x83, 0xC0, 0xF7, 0x83, 0xF8, 0x04};
static const UInt8 kPspSwInit1Patched[]  = {0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x83, 0xF8, 0x04};

// Ditto. macOS 14.4+
static const UInt8 kPspSwInit1Original1404[] = {0x41, 0x8B, 0x46, 0x08, 0x83, 0xC0, 0xF7, 0x83, 0xF8, 0x04};
static const UInt8 kPspSwInit1Patched1404[]  = {0x90, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x83, 0xF8, 0x04};

// Force PSP x.0.0.
static const UInt8 kPspSwInit2Original[]     = {0x83, 0x7B, 0x10, 0x00, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,
                                                0x83, 0x7B, 0x14, 0x01, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspSwInit2OriginalMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                                                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspSwInit2Patched[]      = {0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90,
                                                0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90};

// Ditto. macOS 14.4+
static const UInt8 kPspSwInit2Original1404[]     = {0x41, 0x83, 0x7E, 0x0C, 0x00, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,
                                                    0x41, 0x83, 0x7E, 0x10, 0x01, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspSwInit2OriginalMask1404[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspSwInit2Patched1404[]      = {0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66,
                                                    0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90};

// Catalina, for Renoir: Force PSP 11.x switch case. PSP 11.0.3 is actually PSP 12 in Catalina HWLibs.
static const UInt8 kPspSwInit1Original1015[] = {0x41, 0x8B, 0x44, 0x24, 0x0C, 0x83, 0xC0, 0xF7};
static const UInt8 kPspSwInit1Patched1015[]  = {0xB8, 0x02, 0x00, 0x00, 0x00, 0x66, 0x90, 0x90};

// Ditto, part 2: Force PSP X.0.3.
static const UInt8 kPspSwInit2Original1015[]     = {0x41, 0x83, 0x7C, 0x24, 0x10, 0x00, 0x0F, 0x85, 0x00,
                                                    0x00, 0x00, 0x00, 0x41, 0x8B, 0x44, 0x24, 0x14, 0x83,
                                                    0xF8, 0x03, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspSwInit2OriginalMask1015[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
                                                    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kPspSwInit2Patched1015[]      = {0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66,
                                                    0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90,
                                                    0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x66, 0x90};

// Use "correct" PowerTuneServices by changing the switch statement case `0x8D` to `0x8E`.
static const UInt8 kCreatePowerTuneServices1Original[] = {0x41, 0x8B, 0x47, 0x18, 0x83, 0xC0, 0x88, 0x83, 0xF8, 0x17};
static const UInt8 kCreatePowerTuneServices1Patched[]  = {0x41, 0x8B, 0x47, 0x18, 0x83, 0xC0, 0x87, 0x83, 0xF8, 0x17};

// Ditto. macOS 12.0+
static const UInt8 kCreatePowerTuneServices1Original12[] = {0xB8, 0x7E, 0xFF, 0xFF, 0xFF, 0x41,
                                                            0x03, 0x47, 0x18, 0x83, 0xF8, 0x0F};
static const UInt8 kCreatePowerTuneServices1Patched12[]  = {0xB8, 0x7D, 0xFF, 0xFF, 0xFF, 0x41,
                                                            0x03, 0x47, 0x18, 0x83, 0xF8, 0x0F};

// Ditto. macOS 14.4+
static const UInt8 kCreatePowerTuneServices1Original1404[] = {0xB8, 0x7E, 0xFF, 0xFF, 0xFF, 0x03,
                                                              0x43, 0x18, 0x83, 0xF8, 0x0F};
static const UInt8 kCreatePowerTuneServices1Patched1404[]  = {0xB8, 0x7D, 0xFF, 0xFF, 0xFF, 0x03,
                                                              0x43, 0x18, 0x83, 0xF8, 0x0F};

// Remove revision check to always use Vega 10 PowerTune.
static const UInt8 kCreatePowerTuneServices2Original[] = {0x41, 0x8B, 0x47, 0x1C, 0x83, 0xF8, 0x13, 0x77, 0x00};
static const UInt8 kCreatePowerTuneServices2Mask[]     = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
static const UInt8 kCreatePowerTuneServices2Patched[]  = {0x41, 0x8B, 0x47, 0x1C, 0x66, 0x90, 0x66, 0x90, 0x90};

// Ditto. macOS 14.4+
static const UInt8 kCreatePowerTuneServices2Original1404[] = {0x8B, 0x43, 0x1C, 0x83, 0xF8, 0x13, 0x77, 0x00};
static const UInt8 kCreatePowerTuneServices2Mask1404[]     = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
static const UInt8 kCreatePowerTuneServices2Patched1404[]  = {0x8B, 0x43, 0x1C, 0x66, 0x90, 0x66, 0x90, 0x90};

// Ventura added an explicit switch case for family ID.
// Now we have to make the switch case 0x8D be 0x8E.
static const UInt8 kCailQueryAdapterInfoOriginal[] = {0x83, 0xC0, 0x92, 0x83, 0xF8, 0x21};
static const UInt8 kCailQueryAdapterInfoPatched[]  = {0x83, 0xC0, 0x91, 0x83, 0xF8, 0x21};

static X5000HWLibs moduleInstance;

X5000HWLibs& X5000HWLibs::singleton() { return moduleInstance; }

X5000HWLibs::X5000HWLibs()
{
    if (currentKernelVersion() <= MACOS_10_15_X) {
        this->pspCommandDataField      = 0xB00;
        this->smuInternalSWInitField   = 0x378;
        this->smuInternalHWInitField   = 0x380;
        this->smuInternalSWExitField   = 0x388;
        this->smuInternalHWExitField   = 0x390;
        this->smuFullAsicResetField    = 0x3A0;
        this->smuNotifyEventField      = 0x3A8;
        this->smuFullscreenEventField  = 0x3B8;
        this->gcSwFirmwareField        = 0x2E8;
        this->dmcuEnablePSPFWLoadField = 0x248;
        this->dmcuABMLevelField        = 0x24C;
        this->sdmaGetFwConstantsField  = 0x270;
        this->sdmaStartEngineField     = 0x298;
    }
    else if (currentKernelVersion().majorMatches(MACOS_11)) {
        this->fwDirField                = 0xB8;
        this->smuSwInitialisedFieldBase = 0x280;
        this->smuInternalSWInitField    = 0x638;
        this->smuInternalHWInitField    = 0x640;
        this->smuInternalSWExitField    = 0x648;
        this->smuInternalHWExitField    = 0x650;
        this->smuFullAsicResetField     = 0x660;
        this->smuNotifyEventField       = 0x668;
        this->smuFullscreenEventField   = 0x680;
        this->smuGetUCodeConstsField    = 0x720;
        this->pspCommandDataField       = 0xAF8;
        this->pspSecurityCapsField      = 0x3120;
        this->pspBootloaderVersionField = 0x3124;
        this->pspTOSVersionField        = 0x3128;
        this->gcSwFirmwareField         = 0x2F0;
        this->dmcuEnablePSPFWLoadField  = 0x248;
        this->dmcuABMLevelField         = 0x24C;
        this->sdmaGetFwConstantsField   = 0x270;
        this->sdmaStartEngineField      = 0x298;
    }
    else if (currentKernelVersion().majorMatches(MACOS_12)) {
        this->fwDirField                = 0xB0;
        this->smuSwInitialisedFieldBase = 0x280;
        this->smuInternalSWInitField    = 0x648;
        this->smuInternalHWInitField    = 0x650;
        this->smuInternalSWExitField    = 0x658;
        this->smuInternalHWExitField    = 0x660;
        this->smuFullAsicResetField     = 0x670;
        this->smuNotifyEventField       = 0x678;
        this->smuFullscreenEventField   = 0x690;
        this->smuGetUCodeConstsField    = 0x730;
        this->pspCommandDataField       = 0xAF8;
        this->pspSecurityCapsField      = 0x3120;
        this->pspBootloaderVersionField = 0x3124;
        this->pspTOSVersionField        = 0x3128;
        this->gcSwFirmwareField         = 0x2F0;
        this->dmcuEnablePSPFWLoadField  = 0x248;
        this->dmcuABMLevelField         = 0x24C;
        this->sdmaGetFwConstantsField   = 0x268;
        this->sdmaStartEngineField      = 0x290;
    }
    else {
        this->fwDirField                = 0xB0;
        this->smuSwInitialisedFieldBase = 0x2D0;
        this->pspCommandDataField       = 0xB48;
        this->smuInternalSWInitField    = 0x6C0;
        this->smuInternalHWInitField    = 0x6C8;
        this->smuInternalSWExitField    = 0x6D0;
        this->smuInternalHWExitField    = 0x6D8;
        this->smuFullAsicResetField     = 0x6E8;
        this->smuNotifyEventField       = 0x6F0;
        this->smuFullscreenEventField   = 0x708;
        this->smuGetUCodeConstsField    = 0x7A8;
        this->pspSecurityCapsField      = 0x3918;
        this->pspBootloaderVersionField = 0x391C;
        this->pspTOSVersionField        = 0x3920;
        this->gcSwFirmwareField         = 0x340;
        this->sdmaGetFwConstantsField   = 0x2C8;
        this->sdmaStartEngineField      = 0x2F0;

        if (currentKernelVersion() <= MACOS_13_4) {
            this->dmcuEnablePSPFWLoadField = 0x248;
            this->dmcuABMLevelField        = 0x24C;
        }
        else {
            this->dmcuEnablePSPFWLoadField = 0x298;
            this->dmcuABMLevelField        = 0x29C;
        }
    }
}

void X5000HWLibs::processKext(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
{
    if (kextRadeonX5000HWLibs.loadIndex != id) { return; }

    NRed::singleton().hwLateInit();

    CAILAsicCapsEntry*     orgCapsTable       = nullptr;
    CAILAsicCapsInitEntry* orgCapsInitTable   = nullptr;
    AMDDeviceTypeEntry*    orgDeviceTypeTable = nullptr;
    AMDDeviceCapabilities* orgDevCapTable     = nullptr;

    if (currentKernelVersion() >= MACOS_11) {
        PenguinWizardry::PatternSolveRequest solveRequests[] = {
            {"__ZL15deviceTypeTable", orgDeviceTypeTable, kDeviceTypeTablePattern},
            {"__ZN11AMDFirmware14createFirmwareEPhjjPKc", this->orgCreateFirmware, kCreateFirmwarePattern,
             kCreateFirmwarePatternMask},
            {"__ZN20AMDFirmwareDirectory11putFirmwareE16_AMD_DEVICE_TYPEP11AMDFirmware", this->orgPutFirmware,
             kPutFirmwarePattern},
        };
        PANIC_COND(!PenguinWizardry::PatternSolveRequest::solveAll(patcher, id, solveRequests, slide, size), "HWLibs",
                   "Failed to resolve symbols");
    }

    PenguinWizardry::PatternSolveRequest solveRequests[] = {
        {"__ZL20CAIL_ASIC_CAPS_TABLE", orgCapsTable, kCailAsicCapsTableHWLibsPattern},
        {"_CAILAsicCapsInitTable", orgCapsInitTable, kCAILAsicCapsInitTablePattern},
        {"_DeviceCapabilityTbl", orgDevCapTable, kDeviceCapabilityTblPattern},
    };
    PANIC_COND(!PenguinWizardry::PatternSolveRequest::solveAll(patcher, id, solveRequests, slide, size), "HWLibs",
               "Failed to resolve symbols");

    PenguinWizardry::JumpPatternSolveRequest jumpPatternSolveRequests[] = {
        {"_smu_9_0_send_message_with_parameter", this->smu90SendMessageWithParameter,
         kSmu90SendMessageWithParameterCallPattern, kSmu90SendMessageWithParameterCallPatternMask,
         kSmu90SendMessageWithParameterCallPatternJumpInstOff},
        {"_sdma_cgs_read_register", this->sdmaCgsReadRegister, kSdmaCgsReadRegisterCallPattern,
         kSdmaCgsReadRegisterCallPatternMask, kSdmaCgsReadRegisterCallPatternJumpInstOff},
        {"_sdma_cgs_write_register", this->sdmaCgsWriteRegister, kSdmaCgsWriteRegisterCallPattern,
         kSdmaCgsWriteRegisterCallPatternMask, kSdmaCgsWriteRegisterCallPatternJumpInstOff},
    };
    PANIC_COND(!PenguinWizardry::JumpPatternSolveRequest::solveAll(patcher, id, jumpPatternSolveRequests, slide, size),
               "HWLibs", "Failed to solve symbols via jump pattern");

    KernelPatcher::SolveRequest smuRequests[] = {
        {"_smu_cos_wait_for", this->smuCosWaitFor},
        {"_smu_cgs_write_register", this->smuCgsWriteRegister},
        {"_smu_cgs_read_register", this->smuCgsReadRegister},
    };
    if (!patcher.solveMultiple(id, smuRequests, slide, size, true)) {
        PenguinWizardry::JumpPatternSolveRequest smuPatternRequests[] = {
            {nullptr, this->smuCosWaitFor, kSmuCosWaitForCallPattern, kSmuCosWaitForCallPatternMask,
             kSmuCosWaitForCallPatternJumpInstOff},
            {nullptr, this->smuCgsWriteRegister, kSmuCgsWriteRegisterCallPattern, kSmuCgsWriteRegisterCallPatternMask,
             kSmuCgsWriteRegisterCallPatternJumpInstOff},
            {nullptr, this->smuCgsReadRegister, kSmuCgsReadRegisterCallPattern, kSmuCgsReadRegisterCallPatternMask,
             kSmuCgsReadRegisterCallPatternJumpInstOff},
        };
        PANIC_COND(!PenguinWizardry::JumpPatternSolveRequest::solveAll(
                       patcher, id, smuPatternRequests,
                       reinterpret_cast<mach_vm_address_t>(this->smu90SendMessageWithParameter), PAGE_SIZE),
                   "HWLibs", "Failed to solve SMU COS/CGS functions");
    }

    if (currentKernelVersion() <= MACOS_10_15_X) {
        PenguinWizardry::PatternRouteRequest request{"__ZN16AmdTtlFwServices7getIpFwEjPKcP10_TtlFwInfo", wrapGetIpFw,
                                                     this->orgGetIpFw};
        PANIC_COND(!request.route(patcher, id, slide, size), "HWLibs", "Failed to route getIpFw");
    }
    else {
        // TODO: Not override the PSP 9 code.
        PenguinWizardry::PatternRouteRequest requests[] = {
            {"__ZN35AMDRadeonX5000_AMDRadeonHWLibsX500025populateFirmwareDirectoryEv", wrapPopulateFirmwareDirectory,
             this->orgGetIpFw},
            {"_psp_bootloader_is_sos_running_3_1", pspIsSosRunning, kPspBootloaderIsSosRunning31Pattern},
            {"_psp_security_feature_caps_set_3_1",
             NRed::singleton().getAttributes().isRenoir() && !NRed::singleton().getAttributes().isPhoenix() ? pspSecurityFeatureCapsSet12 : pspSecurityFeatureCapsSet10,
             currentKernelVersion() >= MACOS_13 ? kPspSecurityFeatureCapsSet31Pattern13 :
                                                  kPspSecurityFeatureCapsSet31Pattern},
        };
        PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, requests, slide, size), "HWLibs",
                   "Failed to route symbols (>10.15)");
        if (currentKernelVersion() >= MACOS_14_4) {
            PenguinWizardry::PatternRouteRequest pspRequests[] = {
                {"_psp_bootloader_load_sysdrv_3_1", retOK, kPspBootloaderLoadSysdrv31Pattern1404},
                {"_psp_bootloader_set_ecc_mode_3_1", retOK, kPspBootloaderSetEccMode31Pattern1404},
                {"_psp_bootloader_load_sos_3_1", pspBootloaderLoadSos10, kPspBootloaderLoadSos31Pattern1404},
                {"_psp_reset_3_1", retUnsupported, kPspReset31Pattern1404},
            };
            PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, pspRequests, slide, size), "HWLibs",
                       "Failed to route symbols (>=14.4)");
        }
        else {
            PenguinWizardry::PatternRouteRequest pspRequests[] = {
                {"_psp_bootloader_load_sysdrv_3_1", retOK, kPspBootloaderLoadSysdrv31Pattern,
                 kPspBootloaderLoadSysdrv31PatternMask},
                {"_psp_bootloader_set_ecc_mode_3_1", retOK, kPspBootloaderSetEccMode31Pattern},
                {"_psp_bootloader_load_sos_3_1", pspBootloaderLoadSos10, kPspBootloaderLoadSos31Pattern,
                 kPspBootloaderLoadSos31PatternMask},
                {"_psp_reset_3_1", retUnsupported, kPspReset31Pattern},
            };
            PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, pspRequests, slide, size), "HWLibs",
                       "Failed to route symbols (<14.4)");
        }
    }

    if (currentKernelVersion() >= MACOS_14_4) {
        PenguinWizardry::PatternRouteRequest request{"_psp_cmd_km_submit", wrapPspCmdKmSubmit, this->orgPspCmdKmSubmit,
                                                     kPspCmdKmSubmitPattern1404, kPspCmdKmSubmitPatternMask1404};
        PANIC_COND(!request.route(patcher, id, slide, size), "HWLibs", "Failed to route psp_cmd_km_submit (14.4+)");
    }
    else {
        PenguinWizardry::PatternRouteRequest request{"_psp_cmd_km_submit", wrapPspCmdKmSubmit, this->orgPspCmdKmSubmit,
                                                     kPspCmdKmSubmitPattern, kPspCmdKmSubmitPatternMask};
        PANIC_COND(!request.route(patcher, id, slide, size), "HWLibs", "Failed to route psp_cmd_km_submit");
    }

    PenguinWizardry::JumpPatternRouteRequest fwRequests[] = {
        {"_gc_set_fw_entry_info", wrapGcSetFwEntryInfo, this->orgGcSetFwEntryInfo, kGcSetFwEntryInfoCallPattern,
         kGcSetFwEntryInfoCallPatternMask, kGcSetFwEntryInfoCallPatternJumpInstOff},
        {"_sdma_init_function_pointer_list", wrapSdmaInitFunctionPointerList, this->orgSdmaInitFunctionPointerList,
         kSdmaInitFuncPtrListCallPattern, kSdmaInitFuncPtrListCallPatternMask,
         kSdmaInitFuncPtrListCallPatternJumpInstOff},
        {"_smu_init_function_pointer_list", wrapSmuInitFunctionPointerList, this->orgSmuInitFunctionPointerList,
         kSmuInitFunctionPointerListCallPattern, kSmuInitFunctionPointerListCallPatternMask,
         kSmuInitFunctionPointerListCallPatternJumpInstOff},
    };
    // ⛔ 曾尝试在此加 {"_smu_9_0_send_message_with_parameter", wrapSmu90SendMessageWithParameter, ...}
    //    实测 route 失败 → 启动早期 panic "Failed to route FW-related functions"（CONFIRMED §16.6，1dbf4c2）。
    //    原因：该符号无 call 调用点（只被 solve 成函数指针）。**不要再加回**。
    PANIC_COND(!PenguinWizardry::JumpPatternRouteRequest::routeAll(patcher, id, fwRequests, slide, size), "HWLibs",
               "Failed to route FW-related functions");

    // 重定向成员到 trampoline：routeFunction 已把原函数入口改为跳回 wrapper，
    // 若不重指，smuSendMessage→成员 会重新进入 wrapper 造成递归（smu13InternalHwInit 内部发消息时）。
    // 保护：route 失败时 org 为 0，此时保留 solve 到的原始指针（避免成员被置空导致后续崩溃）。
    if (this->orgSmu90SendMessageWithParameter != 0) {
        this->smu90SendMessageWithParameter =
            reinterpret_cast<CAILResult (*)(void*, UInt32, UInt32)>(this->orgSmu90SendMessageWithParameter);
    }

    KernelPatcher::RouteRequest dmcuFwRequests[] = {
        {nullptr, getDcn1FwConstants},
        {nullptr, getDcn21FwConstants},
    };

    dmcuFwRequests[0].from = patcher.solveSymbol(id, "_dmcu_get_dcn1_fw_constants", slide, size, true);
    dmcuFwRequests[1].from = patcher.solveSymbol(id, "_dmcu_get_dcn21_fw_constants", slide, size, true);
    if (dmcuFwRequests[0].from == 0 || dmcuFwRequests[1].from == 0) {
        const auto solveCall =
            [](KernelPatcher::RouteRequest& req, const mach_vm_address_t addr, const mach_vm_address_t end)
        {
            for (size_t off = 0; off < 2; off += 1) {
                // call ...
                // test al, al
                // je ...
                auto* data = reinterpret_cast<const UInt8*>(addr + off);
                if (addr + off + 13 > end) { return false; }
                if (data[0] == 0xE8 && data[5] == 0x84 && data[6] == 0xC0 && data[7] == 0x0F && data[8] == 0x84) {
                    req.from = PenguinWizardry::jumpInstDestination(addr + off, end);
                    return true;
                }
            }
            return false;
        };

        size_t offset;
        PANIC_COND(!KernelPatcher::findPattern(kDmcuBackdoorLoadFwBranchPattern, kDmcuBackdoorLoadFwBranchPatternMask,
                                               arrsize(kDmcuBackdoorLoadFwBranchPattern),
                                               reinterpret_cast<const void*>(slide), size, &offset),
                   "HWLibs", "Failed to find `dmcu_backdoor_load_fw` branch pattern");
        const auto branch = PenguinWizardry::jumpInstDestination(
            slide + offset + kDmcuBackdoorLoadFwDcn1ConstantsBranchOff, slide + size);
        PANIC_COND(branch == 0, "HWLibs", "Failed to get `dmcu_backdoor_load_fw` branch destination");

        PANIC_COND(!solveCall(dmcuFwRequests[0], branch + kDmcuGetDcn1FwConstantsCallOff, slide + size), "HWLibs",
                   "Failed to solve `dmcu_get_dcn1_fw_constants` via call pattern");
        PANIC_COND(!solveCall(dmcuFwRequests[1], slide + offset + kDmcuGetDcn21FwConstantsCallOff, slide + size),
                   "HWLibs", "Failed to solve `dmcu_get_dcn21_fw_constants` via call pattern");
    }

    PANIC_COND(!patcher.routeMultiple(id, dmcuFwRequests, slide, size), "HWLibs",
               "Failed to route DMCU FW-related functions");

    PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "HWLibs",
               "Failed to enable kernel writing");
    if (orgDeviceTypeTable != nullptr) {
        *orgDeviceTypeTable = {.deviceId = NRed::singleton().getDeviceID(), .deviceType = kAMDDeviceTypeNavi10};
    }

    const auto targetDeviceId =
        NRed::singleton().getAttributes().isRenoir() ? 0x1636U : NRed::singleton().getDeviceID();
    for (; orgCapsInitTable->deviceId != 0xFFFFFFFF; orgCapsInitTable++) {
        if (orgCapsInitTable->familyId == AMD_FAMILY_RAVEN && orgCapsInitTable->deviceId == targetDeviceId) {
            orgCapsInitTable->deviceId = NRed::singleton().getDeviceID();
            orgCapsInitTable->revision = NRed::singleton().getDevRevision();
            orgCapsInitTable->extRevision =
                static_cast<UInt64>(NRed::singleton().getEnumRevision()) + NRed::singleton().getDevRevision();
            orgCapsInitTable->pciRevision = NRed::singleton().getPciRevision();
            orgCapsInitTable->ddiCaps     = NRed::singleton().getAttributes().isPhoenix() ? ddiCapsRaven :
                                            NRed::singleton().getAttributes().isRenoirE() ? ddiCapsRenoirE :
                                            NRed::singleton().getAttributes().isRenoir()  ? ddiCapsRenoir :
                                                                                           ddiCapsRaven;
            *orgCapsTable                 = {
                .familyId = AMD_FAMILY_RAVEN,
                .deviceId = NRed::singleton().getDeviceID(),
                .revision = NRed::singleton().getDevRevision(),
                .extRevision =
                    static_cast<UInt32>(NRed::singleton().getEnumRevision()) + NRed::singleton().getDevRevision(),
                .pciRevision = NRed::singleton().getPciRevision(),
                .ddiCaps     = orgCapsInitTable->ddiCaps,
                .skeleton    = orgCapsTable->skeleton,
            };
            break;
        }
    }
    PANIC_COND(orgCapsInitTable->deviceId == 0xFFFFFFFF, "HWLibs", "Failed to find init caps table entry");
    for (; orgDevCapTable->familyId != 0; orgDevCapTable++) {
        if (orgDevCapTable->familyId == AMD_FAMILY_RAVEN && orgDevCapTable->deviceId == targetDeviceId) {
            orgDevCapTable->deviceId = NRed::singleton().getDeviceID();
            orgDevCapTable->extRevision =
                static_cast<UInt64>(NRed::singleton().getEnumRevision()) + NRed::singleton().getDevRevision();
            orgDevCapTable->revision     = DEVICE_CAP_ENTRY_REV_DONT_CARE;
            orgDevCapTable->enumRevision = DEVICE_CAP_ENTRY_REV_DONT_CARE;

            orgDevCapTable->asicGoldenSettings->goldenSettings =
                NRed::singleton().getAttributes().isPhoenix() ? goldenSettingsRaven :
                NRed::singleton().getAttributes().isRaven2() ? goldenSettingsRaven2 :
                NRed::singleton().getAttributes().isRenoir() ? goldenSettingsRenoir :
                                                               goldenSettingsRaven;

            break;
        }
    }
    PANIC_COND(orgDevCapTable->familyId == 0, "HWLibs", "Failed to find device capability table entry");
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    DBGLOG("HWLibs", "Applied DDI Caps patches");

    // TODO: Replace this hack with a simple hook.
    if (currentKernelVersion() <= MACOS_10_15_X) {
        if (NRed::singleton().getAttributes().isRenoir()) {
            const PenguinWizardry::MaskedLookupPatch patches[] = {
                {&kextRadeonX5000HWLibs, kPspSwInit1Original1015, kPspSwInit1Patched1015, 1},
                {&kextRadeonX5000HWLibs, kPspSwInit2Original1015, kPspSwInit2OriginalMask1015, kPspSwInit2Patched1015,
                 1},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "HWLibs",
                       "Failed to apply spoof patches");
        }
    }
    else {
        const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000HWLibs, kGcSwInitOriginal,
                                                       kGcSwInitOriginalMask,  kGcSwInitPatched,
                                                       kGcSwInitPatchedMask,   1};
        PANIC_COND(!patch.apply(patcher, slide, size), "HWLibs", "Failed to apply gc_sw_init spoof patch");
        if (currentKernelVersion() >= MACOS_14_4) {
            const PenguinWizardry::MaskedLookupPatch patches[] = {
                {&kextRadeonX5000HWLibs, kPspSwInit1Original1404, kPspSwInit1Patched1404, 1},
                {&kextRadeonX5000HWLibs, kPspSwInit2Original1404, kPspSwInit2OriginalMask1404, kPspSwInit2Patched1404,
                 1},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "HWLibs",
                       "Failed to apply spoof patches (>=14.4)");
        }
        else {
            const PenguinWizardry::MaskedLookupPatch patches[] = {
                {&kextRadeonX5000HWLibs, kPspSwInit1Original, kPspSwInit1Patched, 1},
                {&kextRadeonX5000HWLibs, kPspSwInit2Original, kPspSwInit2OriginalMask, kPspSwInit2Patched, 1},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "HWLibs",
                       "Failed to apply spoof patches (<14.4)");
        }
    }

    // TODO: Replace this hack with a simple hook.
    if (currentKernelVersion() >= MACOS_14_4) {
        const PenguinWizardry::MaskedLookupPatch patches[] = {
            {&kextRadeonX5000HWLibs, kCreatePowerTuneServices1Original1404, kCreatePowerTuneServices1Patched1404, 1},
            {&kextRadeonX5000HWLibs, kCreatePowerTuneServices2Original1404, kCreatePowerTuneServices2Mask1404,
             kCreatePowerTuneServices2Patched1404, 1},
        };
        PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "HWLibs",
                   "Failed to apply PowerTuneServices patches (>=14.4)");
    }
    else {
        if (currentKernelVersion() >= MACOS_12) {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000HWLibs, kCreatePowerTuneServices1Original12,
                                                           kCreatePowerTuneServices1Patched12, 1};
            PANIC_COND(!patch.apply(patcher, slide, size), "HWLibs", "Failed to apply PowerTuneServices patch (<14.4)");
        }
        else {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000HWLibs, kCreatePowerTuneServices1Original,
                                                           kCreatePowerTuneServices1Patched, 1};
            PANIC_COND(!patch.apply(patcher, slide, size), "HWLibs", "Failed to apply PowerTuneServices patch");
        }
        const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000HWLibs, kCreatePowerTuneServices2Original,
                                                       kCreatePowerTuneServices2Mask, kCreatePowerTuneServices2Patched,
                                                       1};
        PANIC_COND(!patch.apply(patcher, slide, size), "HWLibs", "Failed to apply PowerTune patch");
    }

    if (currentKernelVersion() >= MACOS_13) {
        const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000HWLibs, kCailQueryAdapterInfoOriginal,
                                                       kCailQueryAdapterInfoPatched, 1};
        PANIC_COND(!patch.apply(patcher, slide, size), "HWLibs", "Failed to apply CailQueryAdapterInfo patch");
    }
}

// Taking advantage of the fact device type "Navi 10" is not used in the original function.
void X5000HWLibs::wrapPopulateFirmwareDirectory(void* const self)
{
    FunctionCast(wrapPopulateFirmwareDirectory, singleton().orgGetIpFw)(self);

    const auto fwDir = singleton().fwDirField(self);
    assert(fwDir != nullptr);

    const auto ravenFw =
        singleton().orgCreateFirmware(ativvaxy_rv_dat, sizeof(ativvaxy_rv_dat), 0x0100, "ativvaxy_rv.dat");
    assert(ravenFw != nullptr);
    singleton().orgPutFirmware(fwDir, kAMDDeviceTypeNavi10, ravenFw);

    const auto renoirFw =
        singleton().orgCreateFirmware(ativvaxy_nv_dat, sizeof(ativvaxy_nv_dat), 0x0202, "ativvaxy_nv.dat");
    assert(renoirFw != nullptr);
    singleton().orgPutFirmware(fwDir, kAMDDeviceTypeNavi10, renoirFw);

    const auto dmcubFw =
        singleton().orgCreateFirmware(atidmcub_rn_dat, sizeof(atidmcub_rn_dat), 0x0201, "atidmcub_0.dat");
    assert(dmcubFw != nullptr);
    singleton().orgPutFirmware(fwDir, kAMDDeviceTypeNavi10, dmcubFw);
}

template<const UInt32 N>
static bool setIpFwOutForFW(const char (&data)[N], void* const out)
{
    getMember<const void*>(out, 0x0) = data;
    getMember<UInt32>(out, 0x8)      = N;
    return true;
}

bool X5000HWLibs::wrapGetIpFw(void* const self, const UInt32 ipVersion, const char* const name, void* const out)
{
    if (strncmp(name, "ativvaxy_rv.dat", 15) == 0) {    // VCN 1.0
        return setIpFwOutForFW(ativvaxy_rv_dat, out);
    }
    if (strncmp(name, "ativvaxy_nv.dat", 15) == 0) {    // VCN 2.1
        return setIpFwOutForFW(ativvaxy_nv_dat, out);
    }
    if (strncmp(name, "atidmcub_0.dat", 14) == 0) {    // DMCU version B
        return setIpFwOutForFW(atidmcub_rn_dat, out);
    }
    return FunctionCast(wrapGetIpFw, singleton().orgGetIpFw)(self, ipVersion, name, out);
}

CAILResult X5000HWLibs::pspIsSosRunning() { return kCAILResultInvalidParameters; }
CAILResult X5000HWLibs::retUnsupported() { return kCAILResultUnsupported; }
CAILResult X5000HWLibs::retOK() { return kCAILResultOK; }

CAILResult X5000HWLibs::pspBootloaderLoadSos10(void* const ctx)
{
    singleton().pspBootloaderVersionField(ctx)  = NRed::singleton().readReg32(MP0_BASE_0 + MP0_SMN_C2PMSG_100);
    singleton().pspTOSVersionField(ctx)         = NRed::singleton().readReg32(MP0_BASE_0 + MP0_SMN_C2PMSG_58);
    (singleton().pspTOSVersionField + 0x4)(ctx) = NRed::singleton().readReg32(MP0_BASE_0 + MP0_SMN_C2PMSG_58);
    return kCAILResultOK;
}

CAILResult X5000HWLibs::pspSecurityFeatureCapsSet10(void* const ctx)
{
    auto& securityCaps     = singleton().pspSecurityCapsField(ctx);
    securityCaps          &= ~1;
    const auto tOSVersion  = singleton().pspTOSVersionField(ctx);
    if ((tOSVersion & 0xFFFF0000) == 0x80000 && (tOSVersion & 0xFF) > 0x50) {
        const auto policyVer = NRed::singleton().readReg32(MP0_BASE_0 + MP0_SMN_C2PMSG_91);
        SYSLOG_COND((policyVer & 0xFF000000) != 0xA000000, "HWLibs", "Invalid security policy version: 0x%X",
                    policyVer);
        if (policyVer == 0xA02031A || ((policyVer & 0xFFFFFF00) == 0xA020400 && (policyVer & 0xFC) > 0x23)
            || ((policyVer & 0xFFFFFF00) == 0xA020300 && (policyVer & 0xFE) > 0x1D))
        {
            securityCaps |= 1;
        }
    }

    return kCAILResultOK;
}

CAILResult X5000HWLibs::pspSecurityFeatureCapsSet12(void* const ctx)
{
    auto& securityCaps     = singleton().pspSecurityCapsField(ctx);
    securityCaps          &= ~1;
    const auto tOSVersion  = singleton().pspTOSVersionField(ctx);
    if ((tOSVersion & 0xFFFF0000) == 0x110000 && (tOSVersion & 0xFF) > 0x2A) {
        const auto policyVer = NRed::singleton().readReg32(MP0_BASE_0 + MP0_SMN_C2PMSG_91);
        SYSLOG_COND((policyVer & 0xFF000000) != 0xB000000, "HWLibs", "Invalid security policy version: 0x%X",
                    policyVer);
        if ((policyVer & 0xFFFF0000) == 0xB090000 && (policyVer & 0xFE) > 0x35) { securityCaps |= 1; }
    }

    return kCAILResultOK;
}

template<const UInt32 N>
static UInt32 replacePspCmdDataWith(void* const data, const char (&fw)[N])
{
    memcpy(data, fw, N);
    return N;
}

CAILResult X5000HWLibs::wrapPspCmdKmSubmit(void* const ctx, void* const cmd, void* const outData,
                                           void* const outResponse)
{
    const auto pspCmd   = getMember<AMDPSPCommand>(cmd, 0x0);
    auto&      dataSize = getMember<UInt32>(cmd, 0xC);
    const auto data     = singleton().pspCommandDataField(ctx);

    switch (pspCmd) {
        case kPSPCommandLoadTA: {
            const char* name = reinterpret_cast<char*>(data + 0x8DB);
            if (strncmp(name, "AMD DTM Application", 19) == 0) { dataSize = replacePspCmdDataWith(data, psp_dtm_bin); }
            else if (strncmp(name, "AMD HDCP Application", 20) == 0) {
                dataSize = replacePspCmdDataWith(data, psp_hdcp_bin);
            }
            else if (strncmp(name, "AMD AUC Application", 19) == 0) {
                dataSize = replacePspCmdDataWith(data, psp_auc_bin);
            }
            else if (strncmp(name, "AMD FP Application", 18) == 0) {
                dataSize = replacePspCmdDataWith(data, psp_fp_bin);
            }
        } break;
        case kPSPCommandLoadASD: {
            dataSize = replacePspCmdDataWith(data, psp_asd_bin);
        } break;
        default: {
        } break;
    }

    return FunctionCast(wrapPspCmdKmSubmit, singleton().orgPspCmdKmSubmit)(ctx, cmd, outData, outResponse);
}

CAILResult X5000HWLibs::smuSendMessage(void* const ctx, const UInt32 message, const UInt32 param,
                                       UInt32* const outParam) const
{
    if (const auto res = this->smu90SendMessageWithParameter(ctx, message, param); res != kCAILResultOK) { return res; }

    if (outParam != nullptr) { *outParam = this->smuCgsReadRegister(ctx, MP1_SMN_C2PMSG_82, 0, kCAILHWBlockMP1, 0); }

    return kCAILResultOK;
}

CAILResult X5000HWLibs::wrapSmu90SendMessageWithParameter(void* const ctx, const UInt32 message, const UInt32 param)
{
    // Probe: 消息 hook 被触发（第 60 位）
    NRed::singleton().orSmu13ProbeState(1ULL << 60);

    if (!singleton().smu13InitAttempted) {
        singleton().smu13InitAttempted = true;
        singleton().smuCtxCache         = ctx;
        smu13InternalHwInit(ctx);
    }

    return FunctionCast(wrapSmu90SendMessageWithParameter, singleton().orgSmu90SendMessageWithParameter)(ctx, message,
                                                                                                        param);
}

CAILResult X5000HWLibs::smuPowerUpConfigCommon(void* const ctx)
{
    if (const auto res = singleton().smuSendMessage(ctx, PPSMC_MSG_PowerUpSdma); res != kCAILResultOK) { return res; }
    if (const auto res = singleton().smuSendMessage(ctx, PPSMC_MSG_PowerUpGfx); res != kCAILResultOK) { return res; }

    return kCAILResultOK;
}

CAILResult X5000HWLibs::smuInternalSwInit(void* const ctx, void*, AMDSMUSWInitOutput*)
{
    singleton().smuSwInitialisedFieldBase(ctx) = true;
    singleton().smuCtxCache = ctx;
    return kCAILResultOK;
}

CAILResult X5000HWLibs::smuInternalSwInitOld(void* const ctx, void*, AMDSMUSWInitOutput* const output)
{ return singleton().smuSendMessage(ctx, PPSMC_MSG_GetSmuVersion, 0, &output->fwConstants.version); }

CAILResult X5000HWLibs::smuGetUCodeConsts(void* const ctx, AMDSMUUCodeConstants* consts)
{
    if (consts == nullptr) { return kCAILResultInvalidParameters; }
    return singleton().smuSendMessage(ctx, PPSMC_MSG_GetSmuVersion, 0, &consts->version);
}

CAILResult X5000HWLibs::smu10PowerUpConfig(void* const ctx)
{
    if (const auto res = singleton().smuSendMessage(ctx, PPSMC_MSG_ForceGfxContentSave);
        res != kCAILResultOK && res != kCAILResultUnsupported)
    {
        return res;
    }
    if (const auto res = smuPowerUpConfigCommon(ctx); res != kCAILResultOK) { return res; }
    if (const auto res = singleton().smuSendMessage(ctx, PPSMC_MSG_PowerGateMmHub);
        res != kCAILResultOK && res != kCAILResultUnsupported)
    {
        return res;
    }
    return kCAILResultOK;
}

CAILResult X5000HWLibs::smu10InternalHwInit(void* const ctx) { return smu10PowerUpConfig(ctx); }

bool X5000HWLibs::smu12IsFwLoaded(void* const ctx)
{
    return (singleton().smuCgsReadRegister(ctx, MP1_FIRMWARE_FLAGS, 0, kCAILHWBlockMP1, MP1_PUBLIC)
            & MP1_FIRMWARE_FLAGS_INTERRUPTS_ENABLED)
           != 0;
}

CAILResult X5000HWLibs::smu12WaitForFwLoaded(void* const ctx)
{
    return singleton().smuCosWaitFor(ctx, smu12IsFwLoaded, ctx,
                                     /*ctx->waitOnRegisterTimeout*/ PP_WAIT_ON_REGISTER_TIMEOUT_DEFAULT);
}

CAILResult X5000HWLibs::smu12PowerUpConfig(void* const ctx)
{
    if (const auto res = smuPowerUpConfigCommon(ctx); res != kCAILResultOK) { return res; }
    if (const auto res = singleton().smuSendMessage(ctx, PPSMC_MSG_PowerGateAtHub);
        res != kCAILResultOK && res != kCAILResultUnsupported)
    {
        return res;
    }

    return kCAILResultOK;
}

CAILResult X5000HWLibs::smu12InternalHwInit(void* const ctx)
{
    singleton().smuCtxCache = ctx;
    if (const auto res = smu12WaitForFwLoaded(ctx); res != kCAILResultOK) { return res; }

    return smu12PowerUpConfig(ctx);
}

bool X5000HWLibs::smu13IsFwLoaded(void* const ctx)
{
    // SMU13 专用 flags 地址（Linux smnMP1_V13_0_4_FIRMWARE_FLAGS = 0x3010028）
    return (singleton().smuCgsReadRegister(ctx, 0x3010028, 0, kCAILHWBlockMP1, MP1_PUBLIC)
            & MP1_FIRMWARE_FLAGS_INTERRUPTS_ENABLED)
           != 0;
}

CAILResult X5000HWLibs::smu13WaitForFwLoaded(void* const ctx)
{
    return singleton().smuCosWaitFor(ctx, smu13IsFwLoaded, ctx,
                                     /*ctx->waitOnRegisterTimeout*/ PP_WAIT_ON_REGISTER_TIMEOUT_DEFAULT);
}

CAILResult X5000HWLibs::smu13PowerUpConfig(void* const ctx)
{
    // PMFW 初始化序列（macOS 侧精简版，仿 Linux smu_v13_0）：唤起 BGM/IMU 客户端。
    // 顺序不可跳（SetDriverDramAddr → TransferTableDram2Smu → EnableGfxImu）。
    CAILResult res;

    // NRed 直读 MMIO 旁路：四步序列已移至 X6000FB::wrapControllerPowerUp（100% 被调用的挂载点）。
    // 此处保留原 ctx 路径逻辑不变（smu13PowerUpConfig 本身因 wrapper 休眠不会被调用，见 CONFIRMED §16）。

    // a. SetDriverDramAddrHigh (0x0D), param=0
    DBGLOG("HWLibs", "smu13: sending msg 0x%X", PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrHigh);
    if ((res = singleton().smuSendMessage(ctx, PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrHigh, 0)) != kCAILResultOK
        && res != kCAILResultUnsupported)
    {
        SYSLOG("HWLibs", "smu13: msg 0x%X failed: 0x%X", PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrHigh, res);
        // Probe D1 v2: 记录失败步（掩码 + rc），随后按原逻辑返回
        NRed::singleton().orSmu13ProbeState(1ULL << 0);
        NRed::singleton().orSmu13ProbeState((UInt64)(res & 0xFF) << (8 + 8 * 0));
        return res;
    }
    // Probe D1 v2: 记录成功步（掩码 + rc）
    NRed::singleton().orSmu13ProbeState(1ULL << 0);
    NRed::singleton().orSmu13ProbeState((UInt64)(res & 0xFF) << (8 + 8 * 0));

    // b. SetDriverDramAddrLow (0x0E), param=0
    DBGLOG("HWLibs", "smu13: sending msg 0x%X", PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrLow);
    if ((res = singleton().smuSendMessage(ctx, PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrLow, 0)) != kCAILResultOK
        && res != kCAILResultUnsupported)
    {
        SYSLOG("HWLibs", "smu13: msg 0x%X failed: 0x%X", PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrLow, res);
        // Probe D1 v2: 记录失败步（掩码 + rc），随后按原逻辑返回
        NRed::singleton().orSmu13ProbeState(1ULL << 1);
        NRed::singleton().orSmu13ProbeState((UInt64)(res & 0xFF) << (8 + 8 * 1));
        return res;
    }
    // Probe D1 v2: 记录成功步（掩码 + rc）
    NRed::singleton().orSmu13ProbeState(1ULL << 1);
    NRed::singleton().orSmu13ProbeState((UInt64)(res & 0xFF) << (8 + 8 * 1));

    // c. TransferTableDram2Smu (0x10), param=0
    DBGLOG("HWLibs", "smu13: sending msg 0x%X", PhoenixPPSMC::PPSMC_MSG_TransferTableDram2Smu);
    if ((res = singleton().smuSendMessage(ctx, PhoenixPPSMC::PPSMC_MSG_TransferTableDram2Smu, 0)) != kCAILResultOK
        && res != kCAILResultUnsupported)
    {
        SYSLOG("HWLibs", "smu13: msg 0x%X failed: 0x%X", PhoenixPPSMC::PPSMC_MSG_TransferTableDram2Smu, res);
        // Probe D1 v2: 记录失败步（掩码 + rc），随后按原逻辑返回
        NRed::singleton().orSmu13ProbeState(1ULL << 2);
        NRed::singleton().orSmu13ProbeState((UInt64)(res & 0xFF) << (8 + 8 * 2));
        return res;
    }
    // Probe D1 v2: 记录成功步（掩码 + rc）
    NRed::singleton().orSmu13ProbeState(1ULL << 2);
    NRed::singleton().orSmu13ProbeState((UInt64)(res & 0xFF) << (8 + 8 * 2));

    // d. EnableGfxImu (0x16), param=1 (ENABLE_IMU_ARG_GFXOFF_ENABLE)
    DBGLOG("HWLibs", "smu13: sending msg 0x%X", PhoenixPPSMC::PPSMC_MSG_EnableGfxImu);
    if ((res = singleton().smuSendMessage(ctx, PhoenixPPSMC::PPSMC_MSG_EnableGfxImu, 1)) != kCAILResultOK
        && res != kCAILResultUnsupported)
    {
        SYSLOG("HWLibs", "smu13: msg 0x%X failed: 0x%X", PhoenixPPSMC::PPSMC_MSG_EnableGfxImu, res);
        // Probe D1 v2: 记录失败步（掩码 + rc），随后按原逻辑返回
        NRed::singleton().orSmu13ProbeState(1ULL << 3);
        NRed::singleton().orSmu13ProbeState((UInt64)(res & 0xFF) << (8 + 8 * 3));
        return res;
    }
    // Probe D1 v2: 记录成功步（掩码 + rc）
    NRed::singleton().orSmu13ProbeState(1ULL << 3);
    NRed::singleton().orSmu13ProbeState((UInt64)(res & 0xFF) << (8 + 8 * 3));

    return kCAILResultOK;
}

CAILResult X5000HWLibs::smu13InternalHwInit(void* const ctx)
{
    // Probe D1 v2: 每轮序列开始清零累积状态（若 WaitForFwLoaded 早退，state 保持 0 = "序列未执行"）
    NRed::singleton().setSmu13ProbeState(0);
    // Probe: 标记 smu13InternalHwInit 被调用过（第 63 位）
    NRed::singleton().orSmu13ProbeState(1ULL << 63);

    singleton().smuCtxCache = ctx;
    CAILResult ret = smu13WaitForFwLoaded(ctx);
    if (ret != kCAILResultOK) {
        SYSLOG("HWLibs", "smu13: internal HW init done, ret=0x%X", ret);
        // Probe: 标记 WaitForFwLoaded 失败早退（第 61 位）
        NRed::singleton().orSmu13ProbeState(1ULL << 61);
        return ret;
    }

    // Probe: 标记 WaitForFwLoaded 成功（第 62 位）
    NRed::singleton().orSmu13ProbeState(1ULL << 62);
    ret = smu13PowerUpConfig(ctx);
    SYSLOG("HWLibs", "smu13: internal HW init done, ret=0x%X", ret);
    return ret;
}

CAILResult X5000HWLibs::smu13NotifyEvent(void* const ctx, TTLEventInput* const input)
{
    if (input->arg >= SMU_EVENT_COUNT) {
        SYSLOG("HWLibs", "Invalid input event to SMU notify event: %d", input->arg);
        return kCAILResultInvalidParameters;
    }

    if (input->arg == SMU_EVENT_POWER_UP || input->arg == 4 || input->arg == 8 || input->arg == SMU_EVENT_REINITIALISE)
    {
        return smu13PowerUpConfig(ctx);
    }

    return kCAILResultOK;
}

CAILResult X5000HWLibs::smu13FullAsicReset(void* const ctx, void* data)
{ return singleton().smuSendMessage(ctx, PhoenixPPSMC::PPSMC_MSG_GfxDeviceDriverReset, getMember<UInt32>(data, 4)); }

// VBIOSSMC: kHz → MHz (向上取整，对齐 Linux khz_to_mhz_ceil)
static inline UInt32 khzToMhzCeil(UInt32 khz) { return (khz + 999U) / 1000U; }

UInt32 X5000HWLibs::vbiossmcSendMsg(void* ctx, UInt32 msgId, UInt32 paramMHz)
{
    if (ctx == nullptr) {
        SYSLOG("HWLibs", "VBIOSSMC send with null ctx (msg 0x%x)", msgId);
        return VBIOSSMC_Result_Failed;
    }

    auto& hw = singleton();

    // 1) Wait for SMU idle (C2PMSG_91 != BUSY)
    UInt32 res = VBIOSSMC_Status_BUSY;
    for (UInt32 tries = 0; tries < 200000; tries += 1) {
        res = hw.smuCgsReadRegister(ctx, MP1_SMN_C2PMSG_91, 0, kCAILHWBlockMP1, 0);
        if (res != VBIOSSMC_Status_BUSY) break;
        IODelay(10);
    }
    if (res == VBIOSSMC_Status_BUSY) {
        SYSLOG("HWLibs", "VBIOSSMC busy timeout before send (msg 0x%x)", msgId);
        return VBIOSSMC_Result_Failed;
    }

    // 2) Clear response register
    hw.smuCgsWriteRegister(ctx, MP1_SMN_C2PMSG_91, 0, VBIOSSMC_Status_BUSY, kCAILHWBlockMP1, 0);

    // 3) Write parameter (MHz)
    hw.smuCgsWriteRegister(ctx, MP1_SMN_C2PMSG_83, 0, paramMHz, kCAILHWBlockMP1, 0);

    // 4) Write message ID (trigger)
    hw.smuCgsWriteRegister(ctx, MP1_SMN_C2PMSG_67, 0, msgId, kCAILHWBlockMP1, 0);

    // 5) Wait for completion
    res = VBIOSSMC_Status_BUSY;
    for (UInt32 tries = 0; tries < 200000; tries += 1) {
        res = hw.smuCgsReadRegister(ctx, MP1_SMN_C2PMSG_91, 0, kCAILHWBlockMP1, 0);
        if (res != VBIOSSMC_Status_BUSY) break;
        IODelay(10);
    }

    // 6) Handle failure
    if (res == VBIOSSMC_Result_Failed) {
        hw.smuCgsWriteRegister(ctx, MP1_SMN_C2PMSG_91, 0, VBIOSSMC_Result_OK, kCAILHWBlockMP1, 0);
        SYSLOG("HWLibs", "VBIOSSMC msg 0x%x param %u failed", msgId, paramMHz);
        return VBIOSSMC_Result_Failed;
    }
    if (res == VBIOSSMC_Status_BUSY) {
        SYSLOG("HWLibs", "VBIOSSMC timeout after send (msg 0x%x)", msgId);
        return VBIOSSMC_Result_Failed;
    }

    // 7) Return actual frequency (MHz) written back by SMU
    return hw.smuCgsReadRegister(ctx, MP1_SMN_C2PMSG_83, 0, kCAILHWBlockMP1, 0);
}

/*!
 * NRed 直读 MMIO 自实现 PMFW 消息发送 —— 绕开 Apple SMU ctx（smu90SendMessageWithParameter / smuCgsReadWriteRegister）。
 * 完整复刻 vbiossmcSendMsg 的 C2PMSG 时序：等 91 不忙 → 清 91 响应 → 写 83 参数 → 写 67 消息(触发) → 轮询 91 直到响应 → 判结果。
 * 基址采用 SMUIO_BASE_0 + MP1_SMN_C2PMSG_xx（32-bit dword 索引，对齐 NRed::readReg32 约定：L930-942 用 MP0_BASE_0 + MP0_SMN_C2PMSG_*）。
 */
CAILResult X5000HWLibs::smu13SendMsgDirect(const UInt32 msgId, const UInt32 param, UInt32 *rawResp)
{
    auto& nred = NRed::singleton();

    // ══════════════════════════════════════════════════════════════════════════
    // ⭐⭐⭐ §16.78 真根因修正（2026-09-11）：SMU 邮箱必须用 MP1 BASE_IDX **1**！
    //   证据（三源）：
    //   ① `mp_13_0_4_offset.h:300/332/348`：regMP1_SMN_C2PMSG_66/82/90 的 **BASE_IDX = 1**
    //      （不是 0！Renoir 的 mp_10_0_offset.h 才是 0——沿用它导致 Phoenix 上全错）
    //   ② `yellow_carp_offset.h:875-876`：MP1_BASE__INST0_SEG0=0x16000，**SEG1=0x0243FC00**
    //      → SOC15_REG_OFFSET(MP1,0,reg)=reg_offset[MP1][0][1]+reg = SEG1+reg
    //   ③ `mac-amdgpu/dext/amdgpu/smu_v14_0.cpp:68-76` 原文注释：
    //      "declare BASE_IDX 1 … NOT BASE_IDX 0. Using base[0] routes the writes to a
    //       completely different physical register and **SMU never responds**"
    //   ④ `research/mac-amdgpu-bringup.md:132` 项目研究文档早已记载此坑（我没读到！）
    //
    //   地址（dword 索引，BASE_IDX 1）：
    //     C2PMSG_66 = 0x0243FC00 + 0x282 = 0x243FE82
    //     C2PMSG_82 = 0x0243FC00 + 0x292 = 0x243FE92
    //     C2PMSG_90 = 0x0243FC00 + 0x29A = 0x243FE9A
    //   ⚠️ 这些值 ×4 远超 BAR5 窗口 → readReg32/writeReg32 走 PCIE_INDEX2/DATA2 间接路径。
    //      间接路径传【字节地址】（dword×4），见 NRed.cpp:240 与 §16.34。
    constexpr UInt32 kMp1Seg1 = 0x0243FC00;   // MP1_BASE__INST0_SEG1（BASE_IDX 1）
    // ⚠️ 传【MMIO 字节地址】（dword×4）：readReg32 间接分支把入参原样写进 PCIE_INDEX2，
    //    而 Linux 语义（amdgpu_reg_access.c:381）SOC15 dword 走间接时也是 ×4 成字节地址。
    //    0x243FExx*4 ≈ 152MB > BAR5 窗口 → 必走间接路径 → 必须传字节地址。
    const UInt32 regResp = (kMp1Seg1 + 0x29A) * 4;  // C2PMSG_90：响应（0=NoResponse，写 0 清）
    const UInt32 regArg  = (kMp1Seg1 + 0x292) * 4;  // C2PMSG_82：参数
    const UInt32 regMsg  = (kMp1Seg1 + 0x282) * 4;  // C2PMSG_66：命令（写即触发）

    // Linux __smu_msg_v1_send 时序：①清响应(90=0) ②写参数(82) ③写命令(66) ④轮询 90 != 0
    nred.writeReg32(regResp, 0);
    nred.writeReg32(regArg, param);
    nred.writeReg32(regMsg, msgId);

    // 轮询响应（C2PMSG_90 != 0，即 SMU 已写回结果）；0 = kSMUFWResponseNoResponse
    constexpr UInt32 kRespTimeout = 200000;   // 200000 × 10µs = 2s（与 vbiossmc 一致）
    UInt32 res = 0;
    for (UInt32 tries = 0; tries < kRespTimeout; tries += 1) {
        res = nred.readReg32(regResp);
        if (res != 0) { break; }
        IODelay(10);
    }

    if (rawResp != nullptr) { *rawResp = res; }   // 诊断：回传原始响应

    // 结果判定（PMFW 响应值：0x1=OK 0xFE=UnknownCmd 0xFD=RejectedPrereq 0xFC=RejectedBusy 0xFF=Failed）
    if (res == 0) {
        SYSLOG("HWLibs", "smu13Direct: no response (msg 0x%x param %u)", msgId, param);
        return kCAILResultNoResponse;
    }
    if (res == VBIOSSMC_Result_Failed) {
        SYSLOG("HWLibs", "smu13Direct: msg 0x%x param %u returned Failed", msgId, param);
        return kCAILResultFailed;
    }
    if (res != VBIOSSMC_Result_OK) {
        SYSLOG("HWLibs", "smu13Direct: msg 0x%x param %u rejected, resp=0x%X", msgId, param, res);
        return kCAILResultUnsupported;
    }

    return kCAILResultOK;
}

// 空白对照（§16.43-1）：只清 resp、不写 msg，然后走同样的轮询。
// 若也返回非 0 ⇒ 说明"响应"来自垃圾值，判定逻辑假阳性。
UInt32 X5000HWLibs::smu13ProbeBlank()
{
    auto& nred = NRed::singleton();
    const UInt32 regResp = (0x0243FC00 + 0x29A) * 4;   // §16.78: BASE_IDX 1, 字节地址
    nred.writeReg32(regResp, 0);
    UInt32 res = 0;
    for (UInt32 i = 0; i < 200000; i += 1) {
        res = nred.readReg32(regResp);
        if (res != 0) { break; }
        IODelay(10);
    }
    return res;   // 期望 0；非 0 ⇒ 假阳性
}

// 读写一致性（§16.43-4）：向 resp 写已知值再读回，确认该地址真可写可读。
UInt32 X5000HWLibs::smu13ProbeRegRW()
{
    auto& nred = NRed::singleton();
    const UInt32 regResp = (0x0243FC00 + 0x29A) * 4;   // §16.78: BASE_IDX 1, 字节地址
    nred.writeReg32(regResp, 0x5A5A);
    const UInt32 back = nred.readReg32(regResp);
    nred.writeReg32(regResp, 0);
    return back;   // 期望 0x5A5A
}

CAILResult X5000HWLibs::smu13SetupDriverTableAndTransfer()
{
    // 分配物理连续 256B（SmuMetrics_t 168B + 对齐余量），供 SMU TransferTableDram2Smu DMA 写入。
    // 缓冲由本类持有（smu13MetricsBuffer），kext 生命周期内不释放：SMU 通过 Transfer 后仍指向该 DRAM 地址。
    // PAGE_SIZE 用常量（macOS 页 = 4096）保证物理对齐。
    auto& hw = singleton();
    if (hw.smu13MetricsBuffer != nullptr) {
        SYSLOG("HWLibs", "smu13SetupDriverTableAndTransfer: buffer already allocated");
        return kCAILResultOK;
    }

    // 【§16.61 纠正】Phoenix = SMU v13_0_4（非 v13_0_7）。
    //   v13_0_4 的 SmuMetrics_t = 244 字节（Linux: sizeof(SmuMetrics_t)），对齐 256。
    //   原 256B 分配本来就是对的；§16.56 的"2268B"基于错误版本，已作废。
    constexpr UInt32 kMetricsSize = 160U;                 // SmuMetrics_t v13_0_4: sizeof=160 align=4（§16.83 编译器实测；244 是 v13_0_7 残留）
    constexpr UInt32 kBufferSize  = 256U;                 // 244 向上对齐
    constexpr UInt32 kPageAlign   = 4096U;                // PAGE_SIZE

    IOBufferMemoryDescriptor* buf = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryPhysicallyContiguous | kIODirectionInOut, kBufferSize, kPageAlign);
    if (buf == nullptr) {
        SYSLOG("HWLibs", "smu13SetupDriverTableAndTransfer: alloc failed");
        return kCAILResultFailed;
    }
    if (buf->prepare() != kIOReturnSuccess) {
        SYSLOG("HWLibs", "smu13SetupDriverTableAndTransfer: prepare failed");
        buf->release();
        return kCAILResultFailed;
    }

    // 取第一段物理段（应覆盖整块 256B）；addr64_t / IOByteCount 来自 IOMemoryDescriptor.h
    // getPhysicalSegment 第三参为 IOOptionBits（默认 0），传 0 即可。
    IOByteCount segLen = 0;
    const addr64_t phys = buf->getPhysicalSegment(0, &segLen, 0);
    if (phys == 0 || segLen < kMetricsSize) {
        SYSLOG("HWLibs", "smu13SetupDriverTableAndTransfer: bad phys seg phys=0x%llX len=%llu",
               (unsigned long long)phys, (unsigned long long)segLen);
        buf->complete();
        buf->release();
        return kCAILResultFailed;
    }

    // 1) 通知 SMU 驱动表 DRAM 地址（高低 32 位）
    //    ⚠️ 地址域（§16.19）：PMFW 的 DMA 域只保证覆盖 FB carve-out 窗口——
    //    [fbOffset<<24, +visible_vram)。普通系统 RAM 页在窗口外 → Transfer 被拒（第 8/9 次实证）。
    //    方案：直接用 carve-out 窗口内地址 = (fbOffset<<24) + 0x1000（第二页，避开 VBIOS 常驻的第一页）。
    //    该内存的 CPU 侧映射/访问暂不需要（表内容全 0 即可，SMU 只做 DMA 拷贝）。
    //    探针：bit29 = carve-out 地址已使用（1）。
    // §16.40：getFbOffset() 已是 (raw & 0xFFFFFF) << 24 的结果，不能再左移（此前重复 <<24 导致垃圾地址）
    const UInt64 fbOff      = NRed::singleton().getFbOffset();
    // §16.52 第 3 项：把驱动表内容真正写到 BAR0 aperture 内（而非只给个地址）
    //   理由：PMFW DMA 域只覆盖 FB carve-out 窗口（§16.19/16.32-7）；
    //   系统 RAM buffer 的物理地址不被接受。必须让"地址处真的有内存"。
    //   做法：映射 BAR0，在 bar0Virt+0x1000 处清零 2304B（SMU 只做 DMA 拷贝，内容全 0 即可）。
    //   失败则回退（地址仍给 carveout+0x1000，但内容无保障）。
    const addr64_t carveout = fbOff;
    const addr64_t addr     = carveout + 0x1000;

    bool apertureWritten = false;
    {
        IOMemoryMap* bar0Map = NRed::singleton().getIGPU()->mapDeviceMemoryWithRegister(
            kIOPCIConfigBaseAddress0, kIOMapInhibitCache | kIOMapAnywhere);
        if (bar0Map != nullptr && bar0Map->getLength() > 0x1000 + kBufferSize) {
            auto* bar0Virt = reinterpret_cast<volatile UInt8*>(bar0Map->getVirtualAddress());
            if (bar0Virt != nullptr) {
                for (UInt32 off = 0; off < kBufferSize; off += 1) {
                    bar0Virt[0x1000 + off] = 0;
                }
                apertureWritten = true;
                NRed::singleton().orSmu13ProbeState(1ULL << 5);    // 探针 bit5：aperture 已写入（bit20/21 已被占用！）
            }
        }
        if (bar0Map != nullptr) { bar0Map->release(); }
    }

    // §16.63/16.64：HDP flush —— 让 SMU 看到 CPU 刚写入的表内容（Linux amdgpu_hdp_flush 等价）
    //   寄存器：HDP_MISC_CNTL = HDP_BASE(0x0F20) + 0x00D3 = dword 0x0FF3
    //   位：FLUSH_INVALIDATE_CACHE（hdp_v4_0.c:151 WREG32_FIELD15(HDP,0,HDP_MISC_CNTL,FLUSH_INVALIDATE_CACHE,1)）
    //   ⚠️ 未做则 SMU 可能读到旧/无效数据（Linux 在 memcpy 后必做）
    {
        constexpr UInt32 kHdpMiscCntl = 0x0FF3;   // HDP_BASE(0x0F20) + regHDP_MISC_CNTL(0x00D3)
        const UInt32 old = NRed::singleton().readReg32(kHdpMiscCntl);
        NRed::singleton().writeReg32(kHdpMiscCntl, 1U << 0);   // FLUSH_INVALIDATE_CACHE = bit0
        (void)NRed::singleton().readReg32(kHdpMiscCntl);       // 回读触发
        NRed::singleton().orSmu13ProbeState(1ULL << 4);    // 探针 bit4：HDP flush 已执行（bit20/21 已被占用！）
        DBGLOG("HWLibs", "smu13: HDP flush (MISC_CNTL old=0x%X)", old);
    }
    DBGLOG("HWLibs", "smu13: aperture written=%s addr=0x%llX",
           apertureWritten ? "yes" : "no", (unsigned long long)addr);
    NRed::singleton().orSmu13ProbeState(1ULL << 29);   // 已改用 carve-out 地址

    // ═══ §16.70 报文响应矩阵（一次真机回答多个问题）═══
    //   目的：0x0D 无响应（rc=NoResponse）而 0x01/0x02/0x03 有响应（=1）——差异在哪？
    //   做法：每个探针发完【当场记录 resp】到 smu13Resp[]，且发完立刻读 c2p82（arg 寄存器）
    //   ① GetDriverIfVersion(0x03, param=0) → 立即读 c2p82（判定 8 是版本号还是 param）
    //   ② SetDriverDramAddrHigh(0x0D, param=0) → 隔离 param 的影响
    //   ③ SetDriverDramAddrHigh(0x0D, param=0x8) → 与 ② 对比
    {
        UInt32 r0 = 0, r1 = 0, r2 = 0, p0 = 0, p1 = 0, p2 = 0;
        X5000HWLibs::smu13SendMsgDirect(PhoenixPPSMC::PPSMC_MSG_GetDriverIfVersion, 0U, &r0);
        NRed::singleton().smu13Resp[3] = r0;
        p0 = NRed::singleton().readReg32((0x0243FC00 + 0x292) * 4);   // §16.78: BASE_IDX 1 字节地址

        X5000HWLibs::smu13SendMsgDirect(PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrHigh, 0U, &r1);
        NRed::singleton().smu13Resp[4] = r1;
        p1 = NRed::singleton().readReg32((0x0243FC00 + 0x292) * 4);   // §16.78: BASE_IDX 1 字节地址

        X5000HWLibs::smu13SendMsgDirect(PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrHigh, 0x8U, &r2);
        NRed::singleton().smu13Resp[5] = r2;
        p2 = NRed::singleton().readReg32((0x0243FC00 + 0x292) * 4);   // §16.78: BASE_IDX 1 字节地址

        NRed::singleton().smu13Resp[6] = p0;   // c2p82 after 0x03
        NRed::singleton().smu13Resp[7] = p1;   // c2p82 after 0x0D(param=0)
        DBGLOG("HWLibs", "matrix: dif(0x03) rc=%x arg=%x | 0x0D(p=0) rc=%x arg=%x | 0x0D(p=8) rc=%x arg=%x",
               r0, p0, r1, p1, r2, p2);
    }

    // §16.67：记录每步真实 resp（不能只在 panic 时读——那时早被后续消息覆盖）
    UInt32 respHigh = 0, respLow = 0, respXfer = 0;
    const auto rHigh = X5000HWLibs::smu13SendMsgDirect(
        PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrHigh, static_cast<UInt32>(addr >> 32), &respHigh);
    NRed::singleton().smu13Resp[0] = respHigh;
    DBGLOG("HWLibs", "smu13: SetDriverDramAddrHigh resp=0x%X", respHigh);
    if (rHigh != kCAILResultOK) {
        NRed::singleton().orSmu13ProbeState(1ULL << 26);   // High 设置失败
        NRed::singleton().orSmu13ProbeState(static_cast<UInt64>(rHigh & 0xFF) << 48);
        buf->complete();
        buf->release();
        return rHigh;
    }
    const auto rLow = X5000HWLibs::smu13SendMsgDirect(
        PhoenixPPSMC::PPSMC_MSG_SetDriverDramAddrLow, static_cast<UInt32>(addr & 0xFFFFFFFFU), &respLow);
    // §16.67：Low 的 resp 记到诊断（panic 输出用；不复用 bit20-23——那是四步成功位）
    NRed::singleton().smu13Resp[1] = respLow;
    DBGLOG("HWLibs", "smu13: SetDriverDramAddrLow resp=0x%X", respLow);
    if (rLow != kCAILResultOK) {
        NRed::singleton().orSmu13ProbeState(1ULL << 27);   // Low 设置失败
        NRed::singleton().orSmu13ProbeState(static_cast<UInt64>(rLow & 0xFF) << 48);
        buf->complete();
        buf->release();
        return rLow;
    }

    // 2) Transfer：argument=0, table_id=TABLE_SMU_METRICS=7
    //    【§16.83】Phoenix = SMU v13_0_4：driver_if_v13_0_4.h:277 TABLE_SMU_METRICS=7。
    //    （旧值 5 出自 v13_0_7 —— §16.61 版本错误的残留，2026-09-11 沉淀审查发现）
    //    之前 §16.55/16.59 误读了 v13_0_7 的表，结论作废，已回滚。
    //    【§16.19】Phoenix (MP1 13.0.7) 的表号：drvif7.h:1601 TABLE_SMU_METRICS=5
    //    （此前传 7 = TABLE_ACTIVITY_MONITOR_COEFF，来自错误版本的表定义——已修正）
    const CAILResult r = X5000HWLibs::smu13SendMsgDirect(
        PhoenixPPSMC::PPSMC_MSG_TransferTableDram2Smu, static_cast<UInt32>((0U << 16) | 7U), &respXfer);
    NRed::singleton().smu13Resp[2] = respXfer;
    DBGLOG("HWLibs", "smu13: TransferTableDram2Smu resp=0x%X", respXfer);

    // 持有缓冲（不 release），后续 metrics 读取可经 smu13MetricsBuffer 取虚拟地址
    hw.smu13MetricsBuffer = buf;
    return r;
}

SInt32 X5000HWLibs::vbiossmcSetDispclk(void* ctx, UInt32 requestedKhz)
{
    UInt32 mhz = vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetDispclkFreq, khzToMhzCeil(requestedKhz));
    return (mhz == VBIOSSMC_Result_Failed) ? -1 : static_cast<SInt32>(mhz * 1000U);
}

SInt32 X5000HWLibs::vbiossmcSetDppclk(void* ctx, UInt32 requestedKhz)
{
    UInt32 mhz = vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetDppclkFreq, khzToMhzCeil(requestedKhz));
    return (mhz == VBIOSSMC_Result_Failed) ? -1 : static_cast<SInt32>(mhz * 1000U);
}

SInt32 X5000HWLibs::vbiossmcSetDprefclk(void* ctx, UInt32 requestedKhz)
{
    UInt32 mhz = vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetDprefclkFreq, khzToMhzCeil(requestedKhz));
    return (mhz == VBIOSSMC_Result_Failed) ? -1 : static_cast<SInt32>(mhz * 1000U);
}

SInt32 X5000HWLibs::vbiossmcSetHardMinDcfclk(void* ctx, UInt32 requestedKhz)
{
    UInt32 mhz = vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetHardMinDcfclkByFreq, khzToMhzCeil(requestedKhz));
    return (mhz == VBIOSSMC_Result_Failed) ? -1 : static_cast<SInt32>(mhz * 1000U);
}

SInt32 X5000HWLibs::vbiossmcSetMinDeepSleepDcfclk(void* ctx, UInt32 requestedKhz)
{
    UInt32 mhz = vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetMinDeepSleepDcfclk, khzToMhzCeil(requestedKhz));
    return (mhz == VBIOSSMC_Result_Failed) ? -1 : static_cast<SInt32>(mhz * 1000U);
}

void X5000HWLibs::vbiossmcSetDisplayIdleOptimizations(void* ctx, UInt32 idleInfo)
{
    vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetDisplayIdleOptimizations, idleInfo);
}

void X5000HWLibs::vbiossmcSetDisplayCount(void* ctx, UInt32 count)
{
    vbiossmcSendMsg(ctx, VBIOSSMC_MSG_SetDisplayCount, count);
}

SInt32 X5000HWLibs::vbiossmcSetDispclkCached(UInt32 requestedKhz)
{
    void* ctx = singleton().smuCtxCache;
    if (ctx == nullptr || !smu12IsFwLoaded(ctx)) {
        return static_cast<SInt32>(requestedKhz);
    }
    return vbiossmcSetDispclk(ctx, requestedKhz);
}

SInt32 X5000HWLibs::vbiossmcSetDppclkCached(UInt32 requestedKhz)
{
    void* ctx = singleton().smuCtxCache;
    if (ctx == nullptr || !smu12IsFwLoaded(ctx)) {
        return static_cast<SInt32>(requestedKhz);
    }
    return vbiossmcSetDppclk(ctx, requestedKhz);
}

CAILResult X5000HWLibs::smuInternalHwExit(void*) { return kCAILResultOK; }

CAILResult X5000HWLibs::smuFullAsicReset(void* const ctx, void* data)
{ return singleton().smuSendMessage(ctx, PPSMC_MSG_DeviceDriverReset, getMember<UInt32>(data, 4)); }

CAILResult X5000HWLibs::smu10NotifyEvent(void* const ctx, TTLEventInput* const input)
{
    if (input->arg >= SMU_EVENT_COUNT) {
        SYSLOG("HWLibs", "Invalid input event to SMU notify event: %d", input->arg);
        return kCAILResultInvalidParameters;
    }

    if (input->arg == SMU_EVENT_POWER_UP || input->arg == 4 || input->arg == 8 || input->arg == SMU_EVENT_REINITIALISE)
    {
        return smu10PowerUpConfig(ctx);
    }

    return kCAILResultOK;
}

CAILResult X5000HWLibs::smu12NotifyEvent(void* const ctx, TTLEventInput* const input)
{
    if (input->arg >= SMU_EVENT_COUNT) {
        SYSLOG("HWLibs", "Invalid input event to SMU notify event: %d", input->arg);
        return kCAILResultInvalidParameters;
    }

    if (input->arg == SMU_EVENT_POWER_UP || input->arg == 4 || input->arg == 8 || input->arg == SMU_EVENT_REINITIALISE)
    {
        return smu12PowerUpConfig(ctx);
    }

    return kCAILResultOK;
}

CAILResult X5000HWLibs::smuFullScreenEvent(void* const ctx, const TTLFullScreenEvent event)
{
    switch (event) {
        case TTL_FULLSCREEN_EVENT_INCREASE:
            singleton().smuCgsWriteRegister(
                ctx, MP1_SMN_FPS_CNT, 0,
                singleton().smuCgsReadRegister(ctx, MP1_SMN_FPS_CNT, 0, kCAILHWBlockMP1, 0) + 1, kCAILHWBlockMP1, 0);
            return kCAILResultOK;
        case TTL_FULLSCREEN_EVENT_RESET:
            singleton().smuCgsWriteRegister(ctx, MP1_SMN_FPS_CNT, 0, 0, kCAILHWBlockMP1, 0);
            return kCAILResultOK;
        default:
            SYSLOG("HWLibs", "Invalid input event to SMU full screen event: %d", event);
            return kCAILResultInvalidParameters;
    }
}

CAILResult X5000HWLibs::wrapSmuInitFunctionPointerList(void* const ctx, const SWIPIPVersion ipVersion)
{
    const auto ret =
        FunctionCast(wrapSmuInitFunctionPointerList, singleton().orgSmuInitFunctionPointerList)(ctx, ipVersion);
    if (ret == kCAILResultOK && !NRed::singleton().getAttributes().isPhoenix()) { return ret; }

    const auto effectiveMajor = NRed::singleton().getAttributes().isPhoenix() ? 13 : ipVersion.major;
    switch (effectiveMajor) {
        case 10: {
            singleton().smuInternalHWInitField(ctx) = reinterpret_cast<void*>(smu10InternalHwInit);
            singleton().smuNotifyEventField(ctx)    = reinterpret_cast<void*>(smu10NotifyEvent);
        } break;
        case 12: {
            singleton().smuInternalHWInitField(ctx) = reinterpret_cast<void*>(smu12InternalHwInit);
            singleton().smuNotifyEventField(ctx)    = reinterpret_cast<void*>(smu12NotifyEvent);
        } break;
        case 13: {
            SYSLOG("HWLibs", "smu13: case 13 (SMU13 PMFW init) entered");
            singleton().smuInternalHWInitField(ctx) = reinterpret_cast<void*>(smu13InternalHwInit);
            singleton().smuNotifyEventField(ctx)    = reinterpret_cast<void*>(smu13NotifyEvent);
        } break;
        default: return ret;
    }

    if (currentKernelVersion() <= MACOS_10_15_X) {
        singleton().smuInternalSWInitField(ctx) = reinterpret_cast<void*>(smuInternalSwInitOld);
    }
    else {
        singleton().smuInternalSWInitField(ctx) = reinterpret_cast<void*>(smuInternalSwInit);
        singleton().smuGetUCodeConstsField(ctx) = reinterpret_cast<void*>(smuGetUCodeConsts);
    }
    singleton().smuFullscreenEventField(ctx) = reinterpret_cast<void*>(smuFullScreenEvent);
    singleton().smuInternalSWExitField(ctx)  = reinterpret_cast<void*>(retOK);
    singleton().smuInternalHWExitField(ctx)  = reinterpret_cast<void*>(smuInternalHwExit);
    if (effectiveMajor == 13) {
        singleton().smuFullAsicResetField(ctx) = reinterpret_cast<void*>(smu13FullAsicReset);
    }
    else {
        singleton().smuFullAsicResetField(ctx) = reinterpret_cast<void*>(smuFullAsicReset);
    }

    SYSLOG_COND(ADDPR(debugEnabled), "HWLibs", "Ignore error about unsupported SMU HW version.");

    return kCAILResultOK;
}

// Actual code creates an `IOMemoryDescriptor` and does nothing with it.
// I ran into issues so I just gave it a random OSObject it can call `release` on.
static void* allocMemHandle()
{
    const auto v = OSBoolean::withBoolean(false);
    assertf(v != nullptr, "Failed to create memory handle!");
    return v;
}

static inline void setGCFWData(void* const ctx, GCFirmwareInfo* const fwData, const GCFirmwareType i,
                               const GCFirmwareConstant* const entry)
{
    fwData->entry[i]                  = entry;
    fwData->handle[i]                 = allocMemHandle();
    getMember<void*[]>(ctx, 0x18)[i]  = fwData->handle[i];
    fwData->count                    += 1;
}

// TODO: Replace this with `gc_read_config_setting_uint32` on `AsicRevForRlcFw`.
static bool isA0()
{
    return !NRed::singleton().getAttributes().isPicasso()
           || ((NRed::singleton().getPciRevision() >= 0xC8 && NRed::singleton().getPciRevision() <= 0xCF)
               || (NRed::singleton().getPciRevision() >= 0xD8 && NRed::singleton().getPciRevision() <= 0xDF));
}

void X5000HWLibs::gc91GetFwConstants(void* const ctx, GCFirmwareInfo* const fwData)
{
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListCntl, &gc_9_1_rlc_srlist_cntl);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListGPMMem, &gc_9_1_rlc_srlist_gpm_mem);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListSRMMem, &gc_9_1_rlc_srlist_srm_mem);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLC, isA0() ? &gc_9_1_rlc_ucode_a0 : &gc_9_1_rlc_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeME, &gc_9_1_me_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeCE, &gc_9_1_ce_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypePFP, &gc_9_1_pfp_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeMEC1, &gc_9_1_mec_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeMECJT1, &gc_9_1_mec_jt_ucode);
    // AMD: Yes, reuse that shit! Why would we waste a couple of bytes? It's not like we're wasting hundreds of MBs
    // already from the duplicate firmware files.
    fwData->entry[kGCFirmwareTypeMECJT2]   = fwData->entry[kGCFirmwareTypeMECJT1];
    fwData->handle[kGCFirmwareTypeMECJT2]  = fwData->handle[kGCFirmwareTypeMECJT1];
    fwData->count                         += 1;
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCV, &gc_9_1_rlcv_ucode);
}

void X5000HWLibs::gc92GetFwConstants(void* const ctx, GCFirmwareInfo* const fwData)
{
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListCntl, &gc_9_2_rlc_srlist_cntl);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListGPMMem, &gc_9_2_rlc_srlist_gpm_mem);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListSRMMem, &gc_9_2_rlc_srlist_srm_mem);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLC, &gc_9_2_rlc_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeME, &gc_9_2_me_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeCE, &gc_9_2_ce_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypePFP, &gc_9_2_pfp_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeMEC1, &gc_9_2_mec_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeMECJT1, &gc_9_2_mec_jt_ucode);
    fwData->entry[kGCFirmwareTypeMECJT2]   = fwData->entry[kGCFirmwareTypeMECJT1];
    fwData->handle[kGCFirmwareTypeMECJT2]  = fwData->handle[kGCFirmwareTypeMECJT1];
    fwData->count                         += 1;
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCV, &gc_9_2_rlcv_ucode);
}

void X5000HWLibs::gc93GetFwConstants(void* const ctx, GCFirmwareInfo* const fwData)
{
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListCntl, &gc_9_3_rlc_srlist_cntl);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListGPMMem, &gc_9_3_rlc_srlist_gpm_mem);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLCSRListSRMMem, &gc_9_3_rlc_srlist_srm_mem);
    setGCFWData(ctx, fwData, kGCFirmwareTypeRLC, &gc_9_3_rlc_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeME, &gc_9_3_me_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeCE, &gc_9_3_ce_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypePFP, &gc_9_3_pfp_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeMEC1, &gc_9_3_mec_ucode);
    setGCFWData(ctx, fwData, kGCFirmwareTypeMECJT1, &gc_9_3_mec_jt_ucode);
}

// Port of `*_char_to_int` from HWLibs.
static constexpr UInt32 charToInt(const char* str, size_t len)
{
    if (str == nullptr || len == 0) { return 0; }
    UInt32 ret = 0;
    while (len > 0) {
        char c = *str;
        if (c < '0' || c > '9') { ret = 0; }
        else {
            ret *= 10;
            ret += static_cast<UInt32>(c - '0');
        }
        str += 1;
        len -= 1;
    }
    return ret;
}

void X5000HWLibs::processGCFWEntries(void* const ctx, void* const initData)
{
    const auto& fwInfo    = singleton().gcSwFirmwareField(ctx);
    auto&       fwEntries = getMember<GCFirmwareEntry[kGCFirmwareTypeCount]>(initData, 0x18);
    for (UInt32 i = 0, swIndex = 0; i < kGCFirmwareTypeCount; i++) {
        if (fwInfo.entry[i] == nullptr) { continue; }

        fwEntries[swIndex].valid       = true;
        fwEntries[swIndex].id          = static_cast<GCFirmwareType>(i);
        fwEntries[swIndex].rom         = fwInfo.entry[i]->rom;
        fwEntries[swIndex].romSize     = fwInfo.entry[i]->romSize;
        fwEntries[swIndex].handle      = fwInfo.handle[i];
        fwEntries[swIndex].payloadOff  = (i == kGCFirmwareTypeMEC1 || i == kGCFirmwareTypeMEC2) ? 0x1000 : 0x0;
        fwEntries[swIndex].version     = charToInt(fwInfo.entry[i]->version, strlen(fwInfo.entry[i]->version));
        fwEntries[swIndex].field24     = fwInfo.entry[i]->field8;
        swIndex                       += 1;
        if (swIndex == fwInfo.count) { break; }
    }
    getMember<UInt32>(initData, 0x10) = fwInfo.count;
}

CAILResult X5000HWLibs::wrapGcSetFwEntryInfo(void* const ctx, const SWIPIPVersion ipVersion, void* const initData)
{
    auto* fwInfo  = &singleton().gcSwFirmwareField(ctx);
    fwInfo->count = 0;
    switch (ipVersion.toHW()) {
        case SWIPIPVersion(9, 1, 0).toHW(): {
            gc91GetFwConstants(ctx, fwInfo);
        } break;
        case SWIPIPVersion(9, 2, 0).toHW(): {
            gc92GetFwConstants(ctx, fwInfo);
        } break;
        case SWIPIPVersion(9, 3, 0).toHW(): {
            gc93GetFwConstants(ctx, fwInfo);
        } break;
        default: return FunctionCast(wrapGcSetFwEntryInfo, singleton().orgGcSetFwEntryInfo)(ctx, ipVersion, initData);
    }
    processGCFWEntries(ctx, initData);
    return kCAILResultOK;
}

static void setDMCUFWData(void* const ctx, DMCUFirmwareInfo* const fwData, const DMCUFirmwareType i,
                          const DMCUFirmwareConstant* const fwEntry)
{
    fwData->entry[i].loadAddress = fwEntry->loadAddress;
    fwData->entry[i].romSize     = fwEntry->romSize;
    fwData->entry[i].rom         = fwEntry->rom;
    fwData->entry[i].handle      = allocMemHandle();
    assertf(fwData->entry[i].handle != nullptr, "Failed to create memory handle!");
    getMember<void*[]>(ctx, 0x18)[i]  = fwData->entry[i].handle;
    fwData->count                    += 1;
}

bool X5000HWLibs::getDcn1FwConstants(void* const ctx, DMCUFirmwareInfo* const fwData)
{
    const auto enablePSPFWLoad = singleton().dmcuEnablePSPFWLoadField(ctx);
    if (enablePSPFWLoad == 2) { return true; }

    fwData->count = 0;

    const auto abmLevel = singleton().dmcuABMLevelField(ctx);
    switch (abmLevel) {
        case 0: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn10_abm_2_1);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn10_abm_2_1);
        } break;
        case 1: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn10_abm_2_2);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn10_abm_2_2);
        } break;
        case 2: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn10_abm_2_3);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn10_abm_2_3);
        } break;
        default: SYSLOG("HWLibs", "Invalid ABM Level (0x%X) for DCN 1!", abmLevel); return false;
    }

    return true;
}

bool X5000HWLibs::getDcn21FwConstants(void* const ctx, DMCUFirmwareInfo* const fwData)
{
    const auto enablePSPFWLoad = singleton().dmcuEnablePSPFWLoadField(ctx);
    if (enablePSPFWLoad == 2) { return true; }

    fwData->count = 0;

    const auto abmLevel = singleton().dmcuABMLevelField(ctx);
    switch (abmLevel) {
        case 0: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn21_abm_2_1);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn21_abm_2_1);
        } break;
        case 1: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn21_abm_2_2);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn21_abm_2_2);
        } break;
        case 2: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn21_abm_2_3);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn21_abm_2_3);
        } break;
        case 3: {
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeERAM, &dmcu_eram_dcn21_abm_2_4);
            setDMCUFWData(ctx, fwData, kDMCUFirmwareTypeISR, &dmcu_intvectors_dcn21_abm_2_4);
        } break;
        default: SYSLOG("HWLibs", "Invalid ABM Level (0x%X) for DCN 2.1!", abmLevel); return false;
    }

    return true;
}

static bool sdma41GetFWConstants(void*, const SDMAFWConstant** const out)
{
    *out = &sdma_4_1_ucode;
    return true;
}

bool X5000HWLibs::sdma412StartEngine(void* const ctx)
{
    singleton().sdmaCgsWriteRegister(
        ctx, SDMA0_F32_CNTL, 0,
        singleton().sdmaCgsReadRegister(ctx, SDMA0_F32_CNTL, 0, /*ctx->hwblock.id*/ kCAILHWBlockSDMA0)
            & ~SDMA0_F32_CNTL_HALT,
        /*ctx->hwblock.id*/ kCAILHWBlockSDMA0);
    return true;
}

static constexpr UInt32 sdmaGetHWVersion(const UInt32 major, const UInt32 minor) { return minor | (major << 16); }

CAILResult X5000HWLibs::wrapSdmaInitFunctionPointerList(void* const ctx, const UInt32 major, const UInt32 minor,
                                                        const UInt32 patch)
{
    switch (sdmaGetHWVersion(major, minor)) {
        case sdmaGetHWVersion(4, 1): {
            singleton().sdmaGetFwConstantsField(ctx) = sdma41GetFWConstants;
            if (patch == 2) { singleton().sdmaStartEngineField(ctx) = sdma412StartEngine; }
        } break;
        default:
            return FunctionCast(wrapSdmaInitFunctionPointerList,
                                singleton().orgSdmaInitFunctionPointerList)(ctx, major, minor, patch);
    }
    return kCAILResultOK;
}
