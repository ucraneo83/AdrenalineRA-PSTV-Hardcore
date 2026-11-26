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

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/avconfig.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>

#include <taihen.h>

#include "../adrenaline_vita.h"

char *sceLsdbGetName(void *a1);

static tai_hook_ref_t sceSysmoduleLoadModuleInternalWithArgRef;
static tai_hook_ref_t sceSysmoduleUnloadModuleInternalWithArgRef;
static tai_hook_ref_t scePafMiscLoadModuleRef;
static tai_hook_ref_t sceLsdbGetTypeRef;

static SceUID hooks[4] = { -1, -1, -1, -1 };
static int hooks_busy;
static int hooks_closing;

static int lock_hooks(void) {
	return !__atomic_exchange_n(&hooks_busy, 1, __ATOMIC_ACQUIRE);
}

static void unlock_hooks(void) {
	__atomic_store_n(&hooks_busy, 0, __ATOMIC_RELEASE);
}

// The caller holds the hook lock. Keep the ID if taiHEN cannot release it.
static int release_hook(int index, tai_hook_ref_t ref) {
	if (hooks[index] < 0) {
		return 0;
	}
	int res = taiHookRelease(hooks[index], ref);
	if (res >= 0) {
		hooks[index] = -1;
	}
	return res;
}

static int sceLsdbGetTypePatched(void *a1) {
	int val = TAI_CONTINUE(int, sceLsdbGetTypeRef, a1);

	if (!__atomic_load_n(&hooks_closing, __ATOMIC_ACQUIRE) && val == 1) { // type normal
		char *name = sceLsdbGetName(a1);
		if (name && sceClibStrcmp(name, ADRENALINE_TITLEID) == 0) {
			return 2; // type pspemu
		}
	}

	return val;
}

static int scePafMiscLoadModulePatched(int a1, const char *path, int a3, int a4, int a5) {
	int res = TAI_CONTINUE(int, scePafMiscLoadModuleRef, a1, path, a3, a4, a5);

	if (res >= 0 && path && sceClibStrncmp(path, "vs0:vsh/shell/livespace_db.suprx", 32) == 0 && lock_hooks()) {
		if (!__atomic_load_n(&hooks_closing, __ATOMIC_RELAXED) && hooks[3] < 0) {
			hooks[3] = taiHookFunctionImport(&sceLsdbGetTypeRef, "SceShell", 0x6BC25E17, 0xDEC358E4, sceLsdbGetTypePatched);
		}
		unlock_hooks();
	}

	return res;
}

static int sceSysmoduleLoadModuleInternalWithArgPatched(SceUInt32 id, SceSize args, void *argp, void *unk) {
	int res = TAI_CONTINUE(int, sceSysmoduleLoadModuleInternalWithArgRef, id, args, argp, unk);

	if (res >= 0 && id == 0x80000008 && lock_hooks()) {
		if (!__atomic_load_n(&hooks_closing, __ATOMIC_RELAXED) && hooks[2] < 0) {
			hooks[2] = taiHookFunctionImport(&scePafMiscLoadModuleRef, "SceShell", 0x3D643CE8, 0xB3B5DF38, scePafMiscLoadModulePatched);
		}
		unlock_hooks();
	}

	return res;
}

static int sceSysmoduleUnloadModuleInternalWithArgPatched(SceUInt32 id, SceSize args, void *argp, void *unk) {
	int res = TAI_CONTINUE(int, sceSysmoduleUnloadModuleInternalWithArgRef, id, args, argp, unk);

	if (res >= 0 && id == 0x80000008 && lock_hooks()) {
		if (!__atomic_load_n(&hooks_closing, __ATOMIC_RELAXED)) {
			release_hook(2, scePafMiscLoadModuleRef);
		}
		unlock_hooks();
	}

	return res;
}

// real name is sceAppMgrLaunchApp2, but oh well...
int sceAppMgrLaunchAppByPath4(const char* path, const char* titleid, int unk1, char* params, int unk3, void* unk4);

int adrStartBlanking(int vol);
uint32_t adrStopBlanking(void);

int sceAppMgrGetStatusByName(const char *name, SceAppMgrAppState *state);
int sceAVConfigGetMasterVol(int *vol);
int sceAVConfigSetMasterVol(int vol);

static int handoff_started;
static int handoff_volume;
static int (*handoff_volume_setter)(int);

static int finish_pspemu_handoff(void) {
	if (handoff_started) {
		handoff_started = 0;
		handoff_volume = (int)adrStopBlanking();
	}
	if (handoff_volume > 0 && handoff_volume_setter && handoff_volume_setter(handoff_volume) < 0) {
		return -1;
	}
	handoff_volume = 0;
	handoff_volume_setter = NULL;
	return 0;
}

#if ADRENALINE_TEST_REPLACEMENT
static void report_pspemu_launch(int close, int launch, int activate) {
	char text[120];
	int length = sceClibSnprintf(text, sizeof(text),
		"AppMgr close=0x%08X path=0x%08X name=0x%08X\n",
		(unsigned int)close, (unsigned int)launch, (unsigned int)activate);
	if (length <= 0 || length >= (int)sizeof(text)) {
		return;
	}
	SceUID fd = sceIoOpen(ADRENALINE_TEST_DATA_ROOT "/relaunch.txt",
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, text, length);
		sceIoClose(fd);
	}
}
#endif

static void relaunch_pspemu(void) {
	SceAppMgrAppState state;
	sceClibMemset(&state, 0, sizeof(state));
	if (sceAppMgrGetStatusByName(ADRENALINE_TITLEID, &state) == 0) {
		int volume = 0;
		int (*set_volume)(int) = NULL;
		if (sceAVConfigGetMasterVol(&volume) >= 0) {
			set_volume = sceAVConfigSetMasterVol;
		} else if (sceAVConfigGetSystemVol(&volume) >= 0) {
			set_volume = sceAVConfigSetSystemVol;
		}

		int muted = volume > 0 && set_volume && set_volume(0) >= 0;
		if (adrStartBlanking(muted ? volume : 0) >= 0) {
			handoff_started = 1;
			handoff_volume_setter = muted ? set_volume : NULL;
		} else if (muted) {
			handoff_volume = volume;
			handoff_volume_setter = set_volume;
			finish_pspemu_handoff();
		}
	}

	int close_result = sceAppMgrDestroyAppByName(ADRENALINE_TITLEID);
	int launch_result = -1;
	int activate_result = -1;
	if (close_result >= 0) {
		char params[0x400];
		sceClibSnprintf(params, sizeof(params),
			"originalpath=ux0:app/" ADRENALINE_TITLEID "&selfpath=ux0:app/" ADRENALINE_TITLEID
			"/eboot.bin&discid=" ADRENALINE_TITLEID "&parentallevel=0&gamedataid=&appver=00.00&bootable=&category=gd");

		// Launch by path creates the PSP emulator process. Launch by name activates it.
		launch_result = sceAppMgrLaunchAppByPath4("vs0:app/NPXS10028/eboot.bin", ADRENALINE_TITLEID, 0, params, 0, 0);
		if (launch_result >= 0) {
			activate_result = sceAppMgrLaunchAppByName(0x60000, ADRENALINE_TITLEID, params);
		}
	}
#if ADRENALINE_TEST_REPLACEMENT
	report_pspemu_launch(close_result, launch_result, activate_result);
#else
	(void)activate_result;
#endif
	// AppMgr can return before the user module starts, so do not leave the output muted.
	finish_pspemu_handoff();
}

void _start() __attribute__ ((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
	if (ADRENALINE_TEST_PROFILE && !ADRENALINE_TEST_REPLACEMENT) {
		return SCE_KERNEL_START_FAILED;
	}
	if (!lock_hooks()) {
		return SCE_KERNEL_START_FAILED;
	}
	__atomic_store_n(&hooks_closing, 0, __ATOMIC_RELEASE);

	tai_module_info_t info;
	info.size = sizeof(info);
	if (taiGetModuleInfo("SceLsdb", &info) < 0) {
		hooks[0] = taiHookFunctionImport(&sceSysmoduleLoadModuleInternalWithArgRef, "SceShell", 0x03FCF19D, 0xC3C26339, sceSysmoduleLoadModuleInternalWithArgPatched);
		hooks[1] = taiHookFunctionImport(&sceSysmoduleUnloadModuleInternalWithArgRef, "SceShell", 0x03FCF19D, 0xA2F40C4C, sceSysmoduleUnloadModuleInternalWithArgPatched);
	} else {
		hooks[3] = taiHookFunctionImport(&sceLsdbGetTypeRef, "SceShell", 0x6BC25E17, 0xDEC358E4, sceLsdbGetTypePatched);
	}
	unlock_hooks();

	relaunch_pspemu();

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
	if (!lock_hooks()) {
		return SCE_KERNEL_STOP_CANCEL;
	}
	__atomic_store_n(&hooks_closing, 1, __ATOMIC_RELEASE);
	int failed = finish_pspemu_handoff() < 0;
	failed |= release_hook(3, sceLsdbGetTypeRef) < 0;
	failed |= release_hook(2, scePafMiscLoadModuleRef) < 0;
	failed |= release_hook(1, sceSysmoduleUnloadModuleInternalWithArgRef) < 0;
	failed |= release_hook(0, sceSysmoduleLoadModuleInternalWithArgRef) < 0;
	unlock_hooks();
	return failed ? SCE_KERNEL_STOP_CANCEL : SCE_KERNEL_STOP_SUCCESS;
}
