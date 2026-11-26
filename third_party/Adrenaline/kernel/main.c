/*
	Adrenaline
	Copyright (C) 2016-2018, TheFloW

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <psp2kern/ctrl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2/kernel/error.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/lowio/iftu.h>
#include <psp2common/display.h>

#include <stdio.h>
#include <string.h>

#include <taihen.h>

#include "../adrenaline_vita.h"
#include "../adrenaline_replacement.h"
#include "utils.h"

int ksceKernelSysrootGetShellPid(void);

static tai_hook_ref_t ksceKernelAllocMemBlockRef;
static tai_hook_ref_t ksceKernelFreeMemBlockRef;
static tai_hook_ref_t ksceKernelUnmapMemBlockRef;
static tai_hook_ref_t SceGrabForDriver_E9C25A28_ref;
static tai_hook_ref_t sceCompatSecSetSSRAMAclRef;
static tai_hook_ref_t ksceSblAimgrIsDEXRef;
static tai_hook_ref_t ksceKernelStartPreloadedModulesRef;
static tai_hook_ref_t ksceDisplaySetFrameBufInternalRef;
static tai_hook_ref_t ksceIftuSetInputFrameBufferRef;

static volatile int g_pspemu_direct_sharp_scale = 0;

enum KernelHookId {
	HOOK_SRAM,
	HOOK_ALLOC,
	HOOK_FREE,
	HOOK_UNMAP,
	HOOK_GRAB,
	HOOK_DEX,
	HOOK_PRELOAD,
	HOOK_DISPLAY,
	HOOK_SHARP_SCALE,
	HOOK_COUNT,
};

static struct {
	SceUID id;
	tai_hook_ref_t *ref;
} hooks[HOOK_COUNT] = {
	[HOOK_SRAM] = { -1, &sceCompatSecSetSSRAMAclRef },
	[HOOK_ALLOC] = { -1, &ksceKernelAllocMemBlockRef },
	[HOOK_FREE] = { -1, &ksceKernelFreeMemBlockRef },
	[HOOK_UNMAP] = { -1, &ksceKernelUnmapMemBlockRef },
	[HOOK_GRAB] = { -1, &SceGrabForDriver_E9C25A28_ref },
	[HOOK_DEX] = { -1, &ksceSblAimgrIsDEXRef },
	[HOOK_PRELOAD] = { -1, &ksceKernelStartPreloadedModulesRef },
	[HOOK_DISPLAY] = { -1, &ksceDisplaySetFrameBufInternalRef },
	[HOOK_SHARP_SCALE] = { -1, &ksceIftuSetInputFrameBufferRef },
};
static int hooks_active;

#define VSH_MODULE_PATH "ux0:app/" ADRENALINE_TITLEID "/sce_module/adrenaline_vsh.suprx"
#define USER_MODULE_PATH "ux0:app/" ADRENALINE_TITLEID "/sce_module/adrenaline_user.suprx"
// TAI_ERROR_NOT_FOUND is internal to taiHEN and absent from its public header.
#define TAIHEN_MODULE_NOT_FOUND ((int)0x90010002)

typedef struct {
	SceUID pid;
	SceUID module_id;
	int owned;
	int busy;
} ProcessModule;

static ProcessModule vsh_module = { .pid = -1, .module_id = -1 };
static ProcessModule user_module = { .pid = -1, .module_id = -1 };

static SceUID extra_1_blockid = -1, extra_2_blockid = -1;
static uint32_t module_nid;

static const uint32_t blackline[1024] = { 0 };
static int blanking_volume = -1;

static int kernel_hooks_active(void) {
	return __atomic_load_n(&hooks_active, __ATOMIC_ACQUIRE);
}

static int lock_process_module(ProcessModule *module) {
	return !__atomic_exchange_n(&module->busy, 1, __ATOMIC_ACQUIRE);
}

static void unlock_process_module(ProcessModule *module) {
	__atomic_store_n(&module->busy, 0, __ATOMIC_RELEASE);
}

// Call only while holding the module's lock. A process exit releases its modules.
static int process_module_is_present(ProcessModule *module, const char *name) {
	if (module->module_id < 0) {
		return 0;
	}

	tai_module_info_t info = { .size = sizeof(info) };
	int res = taiGetModuleInfoForKernel(module->pid, name, &info);
	if (res == TAIHEN_MODULE_NOT_FOUND || res == (int)SCE_KERNEL_ERROR_MODULEMGR_NO_PROCESS ||
	    res == (int)SCE_KERNEL_ERROR_MODULEMGR_INVALID_PROCESS_UID || res == (int)SCE_KERNEL_ERROR_UNKNOWN_UID) {
		module->module_id = -1;
		module->pid = -1;
		module->owned = 0;
		return 0;
	}
	if (res >= 0 && info.modid != module->module_id) {
		module->module_id = info.modid;
		module->owned = 0;
	}
	// A failed query with an unknown result does not prove the module has exited.
	return 1;
}

static void load_process_module(ProcessModule *module, SceUID pid, const char *name, const char *path) {
	if (pid <= 0 || !lock_process_module(module)) {
		return;
	}
	// A record from another process (normally the Adrenaline instance that was
	// closed before this launch) says nothing about the new process. Querying the
	// exited PID can return an error that is not recognised as "gone", which used
	// to keep the stale record, skip the injection and crash the reopened bubble
	// with C1-2758-2. Only the new PID decides whether the module is present.
	if (module->module_id >= 0 && module->pid != pid) {
		module->module_id = -1;
		module->pid = -1;
		module->owned = 0;
	}
	if (kernel_hooks_active() && !process_module_is_present(module, name)) {
		tai_module_info_t info = { .size = sizeof(info) };
		int lookup = taiGetModuleInfoForKernel(pid, name, &info);
		if (lookup >= 0) {
			module->pid = pid;
			module->module_id = info.modid;
			module->owned = 0;
		} else if (lookup == TAIHEN_MODULE_NOT_FOUND) {
			int status = SCE_KERNEL_START_FAILED;
			SceUID module_id = ksceKernelLoadStartModuleForPid(pid, path, 0, NULL, 0, NULL, &status);
			if (module_id >= 0) {
				module->pid = pid;
				module->module_id = module_id;
				module->owned = 1;
				if (status != SCE_KERNEL_START_SUCCESS) {
					debug_printf("Adrenaline process module did not report a successful start.\n");
				}
			}
		}
	}
	unlock_process_module(module);
}

static int stop_owned_vsh(void) {
	if (!process_module_is_present(&vsh_module, ADRENALINE_VSH_MODULE_NAME)) {
		return 0;
	}
	if (!vsh_module.owned) {
		return -1;
	}

	// Only stop the module loaded by us in the current SceShell process.
	tai_module_info_t info = { .size = sizeof(info) };
	if (vsh_module.pid != ksceKernelSysrootGetShellPid() ||
	    taiGetModuleInfoForKernel(vsh_module.pid, ADRENALINE_VSH_MODULE_NAME, &info) < 0 ||
	    info.modid != vsh_module.module_id) {
		return -1;
	}

	int status = SCE_KERNEL_STOP_FAIL;
	int res = ksceKernelStopUnloadModuleForPid(vsh_module.pid, vsh_module.module_id,
		0, NULL, 0, NULL, &status);
	if (res < 0 || status != SCE_KERNEL_STOP_SUCCESS) {
		return -1;
	}
	vsh_module.module_id = -1;
	vsh_module.pid = -1;
	vsh_module.owned = 0;
	return 0;
}

static SceUID ksceKernelAllocMemBlockPatched(const char *name, SceKernelMemBlockType type, int size, SceKernelAllocMemBlockKernelOpt *optp) {
	SceUID blockid = TAI_CONTINUE(SceUID, ksceKernelAllocMemBlockRef, name, type, size, optp);
	if (!kernel_hooks_active() || blockid < 0) {
		return blockid;
	}

	uint32_t addr;
	if (ksceKernelGetMemBlockBase(blockid, (void *)&addr) < 0) {
		return blockid;
	}

	if (addr == 0x23000000) {
		extra_1_blockid = blockid;
	} else if (addr == 0x24000000) {
		extra_2_blockid = blockid;
	}

	return blockid;
}

static int ksceKernelFreeMemBlockPatched(SceUID uid) {
	if (!kernel_hooks_active()) {
		return TAI_CONTINUE(int, ksceKernelFreeMemBlockRef, uid);
	}

	if (uid == extra_1_blockid) {
		return 0;
	}

	int res = TAI_CONTINUE(int, ksceKernelFreeMemBlockRef, uid);

	if (uid == extra_2_blockid) {
		ksceKernelFreeMemBlock(extra_1_blockid);
		extra_1_blockid = -1;
		extra_2_blockid = -1;
	}

	return res;
}

static int ksceKernelUnmapMemBlockPatched(SceUID uid) {
	return kernel_hooks_active() ? 0 : TAI_CONTINUE(int, ksceKernelUnmapMemBlockRef, uid);
}

static int SceGrabForDriver_E9C25A28_patched(int unk, uint32_t paddr) {
	if (kernel_hooks_active() && unk == 2 && paddr == 0x21000001) {
		paddr = 0x22000001;
	}

	return TAI_CONTINUE(int, SceGrabForDriver_E9C25A28_ref, unk, paddr);
}

static int sceCompatSecSetSSRAMAclPatched(void) {
	if (!kernel_hooks_active()) {
		return TAI_CONTINUE(int, sceCompatSecSetSSRAMAclRef);
	}

	uint32_t a;

	a = 0;
	ksceKernelMemcpyUserToKernel(&a, (void*)0x70FC0000, sizeof(uint32_t));

	if (a != 0) {
		// jal 0x88FC0000
		a = 0x0E3F0000;
		if (module_nid == 0x8F2D0378) { // 3.60 retail
			ksceKernelMemcpyKernelToUser((void*)0x70602D58, &a, sizeof(uint32_t));
			ksceKernelDcacheCleanRangeForL1WBWA((void *)0x70602D58, sizeof(uint32_t));

		} else if (module_nid == 0x07937779 || module_nid == 0x71BF9CC5
				|| module_nid == 0x7C185186 || module_nid == 0x52DFE3A7
				|| module_nid == 0xE0E3AA51 || module_nid == 0x2F5A9526
				|| module_nid == 0x8BD7640C || module_nid == 0x6E2F2888
				|| module_nid == 0x0ED0A986) { // 3.65-3.74 retail

			ksceKernelMemcpyKernelToUser((void*)0x70602D70, &a, sizeof(uint32_t));
			ksceKernelDcacheCleanRangeForL1WBWA((void *)0x70602D70, sizeof(uint32_t));
		}
	}

	return TAI_CONTINUE(int, sceCompatSecSetSSRAMAclRef);
}

static int ksceSblAimgrIsDEXPatched(void) {
	int res = TAI_CONTINUE(int, ksceSblAimgrIsDEXRef);
	return kernel_hooks_active() ? 1 : res;
}

static int ksceKernelStartPreloadedModulesPatched(SceUID pid) {
	int res = TAI_CONTINUE(int, ksceKernelStartPreloadedModulesRef, pid);
	if (res < 0 || !kernel_hooks_active()) {
		return res;
	}

	char titleid[32] = { 0 };
	if (ksceKernelSysrootGetProcessTitleId(pid, titleid, sizeof(titleid) - 1) < 0) {
		return res;
	}

	if (strcmp(titleid, "main") == 0) {
		load_process_module(&vsh_module, pid, ADRENALINE_VSH_MODULE_NAME, VSH_MODULE_PATH);
	} else if (strcmp(titleid, ADRENALINE_TITLEID) == 0) {
		load_process_module(&user_module, pid, ADRENALINE_USER_MODULE_NAME, USER_MODULE_PATH);
	}

	return res;
}

static int ksceDisplaySetFrameBufInternalPatched(int head, int index, const SceDisplayFrameBuf *pParam, int sync) {
	if (!kernel_hooks_active() || !head || !pParam) {
		goto DISPLAY_HOOK_RET;
	}

	if (__atomic_load_n(&blanking_volume, __ATOMIC_ACQUIRE) < 0 ||
	    pParam->pitch > sizeof(blackline) / sizeof(blackline[0])) {
		goto DISPLAY_HOOK_RET;
	}

	for (unsigned int i = 0; i < pParam->height; i++) {
		int off = i * pParam->pitch * 4;
		ksceKernelMemcpyKernelToUser(pParam->base + off, blackline, pParam->pitch * 4);
	}

DISPLAY_HOOK_RET:
	return TAI_CONTINUE(int, ksceDisplaySetFrameBufInternalRef, head, index, pParam, sync);
}

static int ksceIftuSetInputFrameBufferPatched(int plane, SceIftuPlaneState *state, int bilinear, int sync_mode) {
	if (kernel_hooks_active() && g_pspemu_direct_sharp_scale && bilinear == 1 && state &&
		state->fb.width == PSP_SCREEN_WIDTH && state->fb.height == PSP_SCREEN_HEIGHT)
	{
		bilinear = 0;
	}

	return TAI_CONTINUE(int, ksceIftuSetInputFrameBufferRef, plane, state, bilinear, sync_mode);
}

int kuCtrlPeekBufferPositive(int port, SceCtrlData *pad_data, int count) {
	uint32_t state;
	ENTER_SYSCALL(state);

	SceCtrlData pad;
	uint32_t off;

	// set cpu offset to zero
	asm volatile ("mrc p15, 0, %0, c13, c0, 4" : "=r" (off));
	asm volatile ("mcr p15, 0, %0, c13, c0, 4" :: "r" (0));

	int res = ksceCtrlPeekBufferPositive(port, &pad, count);

	// restore cpu offset
	asm volatile ("mcr p15, 0, %0, c13, c0, 4" :: "r" (off));

	ksceKernelMemcpyKernelToUser((void*)pad_data, &pad, sizeof(SceCtrlData));

	EXIT_SYSCALL(state);
	return res;
}

int adrStartBlanking(int vol) {
	uint32_t state;
	ENTER_SYSCALL(state);

	int expected = -1;
	int started = vol >= 0 && kernel_hooks_active() &&
		__atomic_compare_exchange_n(&blanking_volume, &expected, vol, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
	EXIT_SYSCALL(state);
	return started ? 0 : -1;
}

int adrSetPspemuDirectSharpScale(int enabled) {
	if (!kernel_hooks_active()) {
		g_pspemu_direct_sharp_scale = 0;
		return -1;
	}
	if (hooks[HOOK_SHARP_SCALE].id < 0) {
		g_pspemu_direct_sharp_scale = 0;
		return hooks[HOOK_SHARP_SCALE].id;
	}

	g_pspemu_direct_sharp_scale = enabled != 0;
	__sync_synchronize();
	return 0;
}

uint32_t adrStopBlanking(void) {
	uint32_t state;
	ENTER_SYSCALL(state);
	int vol = __atomic_exchange_n(&blanking_volume, -1, __ATOMIC_ACQ_REL);
	EXIT_SYSCALL(state);
	return vol < 0 ? 0 : (uint32_t)vol;
}

static int release_kernel_hooks(void) {
	int result = 0;
	for (int i = HOOK_COUNT - 1; i >= 0; --i) {
		if (hooks[i].id < 0) {
			continue;
		}
		int res = taiHookReleaseForKernel(hooks[i].id, *hooks[i].ref);
		if (res < 0) {
			result = res;
		} else {
			hooks[i].id = -1;
		}
	}
	return result;
}

#if ADRENALINE_TEST_REPLACEMENT
static void report_start_error(const char *step, int result) {
	char text[128];
	int length = snprintf(text, sizeof(text), "%s: 0x%08X", step, (unsigned int)result);
	if (length > 0 && length < (int)sizeof(text)) {
		WriteFile(ADRENALINE_START_LOG, text, length);
	}
}

static int replacement_can_start(void) {
	if (!adrenaline_original_app_is_absent(ksceIoGetstat)) {
		report_start_error("Original app or ux0:app unavailable", 0);
		return 0;
	}

	SceUID shell_pid = ksceKernelSysrootGetShellPid();
	if (shell_pid <= 0) {
		report_start_error("SceShell PID unavailable", shell_pid);
		return 0;
	}

	tai_module_info_t info = { .size = sizeof(info) };
	int result = taiGetModuleInfoForKernel(KERNEL_PID, "AdrenalineKernel", &info);
	if (result != TAIHEN_MODULE_NOT_FOUND) {
		report_start_error("Original kernel lookup", result);
		return 0;
	}
	info.size = sizeof(info);
	result = taiGetModuleInfoForKernel(shell_pid, "AdrenalineVsh", &info);
	if (result != TAIHEN_MODULE_NOT_FOUND) {
		report_start_error("Original VSH lookup", result);
		return 0;
	}
	return 1;
}
#endif

void _start() __attribute__ ((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
	if (ADRENALINE_TEST_PROFILE && !ADRENALINE_TEST_REPLACEMENT) {
		return SCE_KERNEL_START_FAILED;
	}

#if ADRENALINE_TEST_REPLACEMENT
	if (!replacement_can_start()) {
		debug_printf("AdrenalineRA Test detected the original or could not confirm its absence.\n");
		return SCE_KERNEL_START_FAILED;
	}
#endif

	tai_module_info_t info;
	info.size = sizeof(tai_module_info_t);
	int res = taiGetModuleInfoForKernel(KERNEL_PID, "SceCompat", &info);
	if (res < 0) {
#if ADRENALINE_TEST_REPLACEMENT
		report_start_error("SceCompat lookup", res);
#endif
		return SCE_KERNEL_START_FAILED;
	}

	module_nid = info.module_nid;
	size_t sram_hook_offset;
	switch (module_nid) {
		case 0x8F2D0378: // 3.60 retail
			sram_hook_offset = 0x2AA4;
			break;
		case 0x07937779: // 3.65 retail
		case 0x71BF9CC5: // 3.67 retail
		case 0x7C185186: // 3.68 retail
		case 0x52DFE3A7: // 3.69 retail
		case 0xE0E3AA51: // 3.70 retail
		case 0x2F5A9526: // 3.71 retail
		case 0x8BD7640C: // 3.72 retail
		case 0x6E2F2888: // 3.73 retail
		case 0x0ED0A986: // 3.74 retail
			sram_hook_offset = 0x2AE8;
			break;
		default:
#if ADRENALINE_TEST_REPLACEMENT
		report_start_error("Unsupported SceCompat NID", (int)module_nid);
#endif
			return SCE_KERNEL_START_FAILED;
	}

	hooks[HOOK_SRAM].id = taiHookFunctionOffsetForKernel(KERNEL_PID,
		hooks[HOOK_SRAM].ref, info.modid, 0, sram_hook_offset, 1, sceCompatSecSetSSRAMAclPatched);
	if (hooks[HOOK_SRAM].id < 0) {
#if ADRENALINE_TEST_REPLACEMENT
		report_start_error("HOOK_SRAM", hooks[HOOK_SRAM].id);
#endif
		goto failed;
	}

	// SceCompat imports.
	hooks[HOOK_ALLOC].id = taiHookFunctionImportForKernel(KERNEL_PID, hooks[HOOK_ALLOC].ref,
		"SceCompat", 0x6F25E18A, 0xC94850C9, ksceKernelAllocMemBlockPatched);
	if (hooks[HOOK_ALLOC].id < 0) {
#if ADRENALINE_TEST_REPLACEMENT
		report_start_error("HOOK_ALLOC", hooks[HOOK_ALLOC].id);
#endif
		goto failed;
	}
	hooks[HOOK_FREE].id = taiHookFunctionImportForKernel(KERNEL_PID, hooks[HOOK_FREE].ref,
		"SceCompat", 0x6F25E18A, 0x009E1C61, ksceKernelFreeMemBlockPatched);
	if (hooks[HOOK_FREE].id < 0) {
#if ADRENALINE_TEST_REPLACEMENT
		report_start_error("HOOK_FREE", hooks[HOOK_FREE].id);
#endif
		goto failed;
	}
	hooks[HOOK_UNMAP].id = taiHookFunctionImportForKernel(KERNEL_PID, hooks[HOOK_UNMAP].ref,
		"SceCompat", 0x6F25E18A, 0xFFCD9B60, ksceKernelUnmapMemBlockPatched);
	if (hooks[HOOK_UNMAP].id < 0) {
#if ADRENALINE_TEST_REPLACEMENT
		report_start_error("HOOK_UNMAP", hooks[HOOK_UNMAP].id);
#endif
		goto failed;
	}
	hooks[HOOK_GRAB].id = taiHookFunctionImportForKernel(KERNEL_PID, hooks[HOOK_GRAB].ref,
		"SceCompat", 0x81C54BED, 0xE9C25A28, SceGrabForDriver_E9C25A28_patched);
	if (hooks[HOOK_GRAB].id < 0) {
#if ADRENALINE_TEST_REPLACEMENT
		report_start_error("HOOK_GRAB", hooks[HOOK_GRAB].id);
#endif
		goto failed;
	}
	hooks[HOOK_DEX].id = taiHookFunctionImportForKernel(KERNEL_PID, hooks[HOOK_DEX].ref,
		"SceCompat", 0xFD00C69A, 0xF4B98F66, ksceSblAimgrIsDEXPatched);
	if (hooks[HOOK_DEX].id < 0) {
#if ADRENALINE_TEST_REPLACEMENT
		report_start_error("HOOK_DEX", hooks[HOOK_DEX].id);
#endif
		goto failed;
	}

	hooks[HOOK_PRELOAD].id = taiHookFunctionExportForKernel(KERNEL_PID, hooks[HOOK_PRELOAD].ref,
		"SceKernelModulemgr", 0xC445FA63, 0x432DCC7A, ksceKernelStartPreloadedModulesPatched);
	if (hooks[HOOK_PRELOAD].id < 0) {
		hooks[HOOK_PRELOAD].id = taiHookFunctionExportForKernel(KERNEL_PID, hooks[HOOK_PRELOAD].ref,
			"SceKernelModulemgr", 0x92C9FFC2, 0x998C7AE9, ksceKernelStartPreloadedModulesPatched);
	}
	if (hooks[HOOK_PRELOAD].id < 0) {
#if ADRENALINE_TEST_REPLACEMENT
		report_start_error("HOOK_PRELOAD", hooks[HOOK_PRELOAD].id);
#endif
		goto failed;
	}

	hooks[HOOK_DISPLAY].id = taiHookFunctionExportForKernel(KERNEL_PID, hooks[HOOK_DISPLAY].ref,
		"SceDisplay", 0x9FED47AC, 0x16466675, ksceDisplaySetFrameBufInternalPatched);
	if (hooks[HOOK_DISPLAY].id < 0) {
#if ADRENALINE_TEST_REPLACEMENT
		report_start_error("HOOK_DISPLAY", hooks[HOOK_DISPLAY].id);
#endif
		goto failed;
	}

	// Direct sharp scaling is optional.
	hooks[HOOK_SHARP_SCALE].id = taiHookFunctionImportForKernel(KERNEL_PID, hooks[HOOK_SHARP_SCALE].ref,
		"SceDisplay", 0xCAFCFE50, 0x7CE0C4DA, ksceIftuSetInputFrameBufferPatched);

	__atomic_store_n(&hooks_active, 1, __ATOMIC_RELEASE);

	load_process_module(&vsh_module, ksceKernelSysrootGetShellPid(),
		ADRENALINE_VSH_MODULE_NAME, VSH_MODULE_PATH);
	return SCE_KERNEL_START_SUCCESS;

failed:
	if (release_kernel_hooks() < 0) {
		// Unreleased hooks may still reach this module. Keep its code resident and inactive.
		debug_printf("Adrenaline kernel startup failed and taiHEN did not release every hook.\n");
		return SCE_KERNEL_START_RESIDENT;
	}
	return SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize args, void *argp) {
	if (ADRENALINE_TEST_PROFILE && !ADRENALINE_TEST_REPLACEMENT) {
		return SCE_KERNEL_STOP_SUCCESS;
	}

	if (!lock_process_module(&vsh_module)) {
		return SCE_KERNEL_STOP_CANCEL;
	}
	if (!lock_process_module(&user_module)) {
		unlock_process_module(&vsh_module);
		return SCE_KERNEL_STOP_CANCEL;
	}
	if (process_module_is_present(&user_module, ADRENALINE_USER_MODULE_NAME) ||
	    (process_module_is_present(&vsh_module, ADRENALINE_VSH_MODULE_NAME) && !vsh_module.owned)) {
		unlock_process_module(&user_module);
		unlock_process_module(&vsh_module);
		return SCE_KERNEL_STOP_CANCEL;
	}

	__atomic_store_n(&hooks_active, 0, __ATOMIC_RELEASE);
	g_pspemu_direct_sharp_scale = 0;
	int stop_status = SCE_KERNEL_STOP_SUCCESS;
	if (stop_owned_vsh() < 0) {
		debug_printf("Adrenaline kernel remains resident because its VSH could not be stopped.\n");
		stop_status = SCE_KERNEL_STOP_CANCEL;
	} else if (release_kernel_hooks() < 0) {
		debug_printf("Adrenaline kernel remains inactive and resident after a hook release failure.\n");
		stop_status = SCE_KERNEL_STOP_CANCEL;
	}
	unlock_process_module(&user_module);
	unlock_process_module(&vsh_module);
	return stop_status;
}
