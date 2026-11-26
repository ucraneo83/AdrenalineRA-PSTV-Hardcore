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

#include <psp2/audioout.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "menu.h"
#if ADRENALINE_TEST_PROFILE
#include "msfs.h"
#endif
#include "pops.h"
#include "ra_logqueue.h"
#include "ra_runtime.h"
#include "utils.h"

int ScePspemuInitAudioOutPatched(void) {
	int res = TAI_CONTINUE(int, ScePspemuInitAudioOutRef);

	res = ScePspemuInitPops();
	if (res < 0) {
		return res;
	}

	SceUID blockid = sceKernelAllocMemBlock("ScePspemuMcWork", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 0x40000, NULL);
	if (blockid < 0) {
		return blockid;
	}

	int (* init_sth)(uint32_t, int) = NULL;
	int base_offset = 0x10100;

	if (module_nid == 0x2714F07D) { // 3.60 retail
		init_sth = (void *)(text_addr + 0x31F90 + 0x1);
	} else if (module_nid == 0x3F75D4D3) { // 3.65-3.70 retail
		init_sth = (void *)(text_addr + 0x31FA4 + 0x1);
	} else if (module_nid == 0xEA8C1AE2) { // 3.71 retail
		init_sth = (void *)(text_addr + 0x32074 + 0x1);
		base_offset = 0x10108;
	} else if (module_nid == 0x5459B715) { // 3.72-3.74 retail
		init_sth = (void *)(text_addr + 0x32014 + 0x1);
	}

	sceKernelGetMemBlockBase(blockid, (void *)(data_addr + base_offset));

	init_sth(*(uint32_t *)(data_addr + base_offset), 0x1E000);

	res = ScePspemuInitPocs();
	if (res < 0) {
		return res;
	}

	return res;
}

static int pops_audio_port = -1;

int sceAudioOutOpenPortPatched(int type, int len, int freq, int mode) {
	int res = TAI_CONTINUE(int, sceAudioOutOpenPortRef, type, len, freq, mode);

	// Use voice port
	if (res == (int)SCE_AUDIO_OUT_ERROR_PORT_FULL && type == SCE_AUDIO_OUT_PORT_TYPE_BGM) {
		pops_audio_port = TAI_CONTINUE(int, sceAudioOutOpenPortRef, SCE_AUDIO_OUT_PORT_TYPE_VOICE, len, freq, mode);
		return pops_audio_port;
	}

	return res;
}

int sceAudioOutOutputPatched(int port, const void *buf) {
	SceAdrenaline *adrenaline = (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);

	if (port == pops_audio_port && !adrenaline->pops_mode) {
		sceDisplayWaitVblankStart();
		return 0;
	}

	return TAI_CONTINUE(int, sceAudioOutOutputRef, port, buf);
}

int ScePspemuDecodePopsAudioPatched(int a1, int a2, int a3, int a4) {
	SceAdrenaline *adrenaline = (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);

	if (!adrenaline->pops_mode) {
		return 0;
	}

	return TAI_CONTINUE(int, ScePspemuDecodePopsAudioRef, a1, a2, a3, a4);
}

char *ScePspemuGetTitleidPatched(void) {
	SceAdrenaline *adrenaline = (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);
	return adrenaline->titleid;
}

int ScePspemuConvertAddressPatched(uint32_t addr, int mode, uint32_t cache_size) {
	if (addr >= 0x09FE0000 && addr < 0x09FE01B0) {
		addr = 0x0BCD0000 | (addr & 0xFFFF);
	}

	return TAI_CONTINUE(int, ScePspemuConvertAddressRef, addr, mode, cache_size);
}

#if ADRENALINE_TEST_PROFILE
static int isTestNativeStorageRoot(const char *path, const char *root) {
	size_t length = strlen(root);
	return strncmp(path, root, length) == 0 &&
	       (path[length] == '\0' || path[length] == '/');
}

static int isTestPspemuSystemRead(const char *file, const char *path) {
	if (strncmp(file, "ur0:", 4) == 0) {
		return strcmp(path, "emu_list") == 0 || strcmp(path, "emu_list/compat_info.dat") == 0;
	}

	const char workspace_prefix[] = "temp/pspemu/w";
	if (strncmp(file, "ux0:", 4) != 0 ||
	    strncmp(path, workspace_prefix, sizeof(workspace_prefix) - 1) != 0) {
		return 0;
	}

	const char *slot = path + sizeof(workspace_prefix) - 1;
	return strlen(slot) == 6 && strcmp(slot + 2, ".bin") == 0 &&
	       slot[1] >= '0' && ((slot[0] == '0' && slot[1] <= '9') ||
	                          (slot[0] == '1' && slot[1] <= '5'));
}

static int translateTestNativePath(const char *file, int read_only, char *output, size_t capacity) {
	if (!file || !memchr(file, '\0', MAX_PATH_LENGTH)) {
		return 0;
	}

	const char *device_end = strchr(file, ':');
	if (!device_end || device_end - file != 3) {
		return 0;
	}
	const char *path = device_end + 1;
	while (*path == '/') {
		path++;
	}
	if (!isTestPspRelativePath(path)) {
		return 0;
	}

	int length;
	const char *ms_path = NULL;
	if (strncmp(file, "ms0:", 4) == 0) {
		ms_path = path;
	} else if (strncmp(file, "ux0:", 4) == 0 && isTestNativeStorageRoot(path, "pspemu")) {
		ms_path = path + sizeof("pspemu") - 1;
		while (*ms_path == '/') {
			ms_path++;
		}
	}

	if (ms_path) {
		const char *iso_suffix = getTestIsoSuffix(ms_path);
		if (iso_suffix && !read_only) {
			return 0;
		}
		length = iso_suffix
			? snprintf(output, capacity, "%s%s", ADRENALINE_TEST_ISO_ROOT, iso_suffix)
			: snprintf(output, capacity, "%s/%s", ADRENALINE_TEST_MS_ROOT, ms_path);
	} else if ((strncmp(file, "ux0:", 4) == 0 &&
	            (isTestNativeStorageRoot(path, "data/" ADRENALINE_TITLEID) ||
	             (read_only && isTestNativeStorageRoot(path, "app/" ADRENALINE_TITLEID)))) ||
	           (read_only && (strncmp(file, "os0:", 4) == 0 || strncmp(file, "vs0:", 4) == 0 ||
	                         isTestPspemuSystemRead(file, path)))) {
		length = snprintf(output, capacity, "%.4s%s", file, path);
	} else {
		return 0;
	}
	return length >= 0 && (size_t)length < capacity;
}

static int buildTestPopsPath(const char *file, int read_only, char *output, size_t capacity) {
	if (!file || !memchr(file, '\0', MAX_PATH_LENGTH)) {
		return 0;
	}

	const char *name = strrchr(file, '/');
	name = name ? name + 1 : file;
	int menu_info = strcmp(name, "__sce_menuinfo") == 0;
	int savedata = strcmp(name, "PARAM.SFO") == 0 || strcmp(name, "SCEVMC0.VMP") == 0 ||
	               strcmp(name, "SCEVMC1.VMP") == 0;
	if (!menu_info && !savedata) {
		return translateTestNativePath(file, read_only, output, capacity);
	}

	// POPS can supply a native path from the original memory stick.
	// Validate its syntax before rebasing the special file into Test data.
	const char *device_end = strchr(file, ':');
	const char *relative = file;
	if (device_end) {
		if (device_end - file != 3 || (strncmp(file, "ux0:", 4) != 0 && strncmp(file, "ms0:", 4) != 0)) {
			return 0;
		}
		relative = device_end + 1;
	}
	if (!isTestPspRelativePath(relative)) {
		return 0;
	}

	const SceAdrenaline *adrenaline = (const SceAdrenaline *)ScePspemuConvertAddress(
		ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);
	if (!adrenaline) {
		return 0;
	}

	int length;
	if (menu_info) {
		char filename[sizeof(adrenaline->filename)];
		memcpy(filename, adrenaline->filename, sizeof(filename));
		if (!memchr(filename, '\0', sizeof(filename)) || strncmp(filename, "ms0:/", 5) != 0 ||
		    !isTestPspRelativePath(filename + 5)) {
			return 0;
		}
		char *directory_end = strrchr(filename, '/');
		if (directory_end[1] == '\0') {
			return 0;
		}
		*directory_end = '\0';
		const char *directory = directory_end == filename + 4 ? "" : filename + 5;
		length = snprintf(output, capacity, "%s/%s%s%s", ADRENALINE_TEST_MS_ROOT,
		                  directory, *directory ? "/" : "", name);
	} else {
		char title_id[sizeof(adrenaline->titleid)];
		memcpy(title_id, adrenaline->titleid, sizeof(title_id));
		if (!memchr(title_id, '\0', sizeof(title_id)) || title_id[0] == '\0' ||
		    title_id[strspn(title_id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")] != '\0') {
			return 0;
		}
		length = snprintf(output, capacity, "%s/PSP/SAVEDATA/%s/%s", ADRENALINE_TEST_MS_ROOT, title_id, name);
	}
	return length >= 0 && (size_t)length < capacity;
}
#endif

SceUID sceIoOpenPatched(const char *file, int flags, SceMode mode) {
#if ADRENALINE_TEST_PROFILE
	char new_file[MAX_PATH_LENGTH];
	if (!buildTestPopsPath(file, isTestReadOnlyOpen(flags), new_file, sizeof(new_file))) {
		logTestPathError("Native open rejected", file, flags, SCE_EINVAL);
		return SCE_EINVAL;
	}
	file = new_file;
#else
	char *p = strrchr(file, '/');
	if (p) {
		static char new_file[256];

		SceAdrenaline *adrenaline = (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);

		if (strcmp(p+1, "__sce_menuinfo") == 0) {
			char *filename = adrenaline->filename;
			if (strncmp(filename, "ms0:/", 5) == 0) {
				char *q = strrchr(filename, '/');
				if (q) {
					char path[128];
					strncpy(path, filename+5, q-(filename+5));
					path[q-(filename+5)] = '\0';

					snprintf(new_file, sizeof(new_file), "%s/%s/__sce_menuinfo", getPspemuMemoryStickLocation(), path);
					file = new_file;
				}
			}
		} else if (strcmp(p+1, "PARAM.SFO") == 0 || strcmp(p+1, "SCEVMC0.VMP") == 0 || strcmp(p+1, "SCEVMC1.VMP") == 0) {
			snprintf(new_file, sizeof(new_file), "%s/PSP/SAVEDATA/%s/%s", getPspemuMemoryStickLocation(), adrenaline->titleid, p+1);
			file = new_file;
		}
	}
#endif

	return TAI_CONTINUE(SceUID, sceIoOpenRef, file, flags, mode);
}

int sceIoGetstatPatched(const char *file, SceIoStat *stat) {
#if ADRENALINE_TEST_PROFILE
	char new_file[MAX_PATH_LENGTH];
	if (!buildTestPopsPath(file, 1, new_file, sizeof(new_file))) {
		logTestPathError("Native stat rejected", file, 0, SCE_EINVAL);
		return SCE_EINVAL;
	}
	file = new_file;
#else
	char *p = strrchr(file, '/');
	if (p) {
		static char new_file[256];

		SceAdrenaline *adrenaline = (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);

		if (strcmp(p+1, "PARAM.SFO") == 0 || strcmp(p+1, "SCEVMC0.VMP") == 0 || strcmp(p+1, "SCEVMC1.VMP") == 0) {
			snprintf(new_file, sizeof(new_file), "%s/PSP/SAVEDATA/%s/%s", getPspemuMemoryStickLocation(), adrenaline->titleid, p+1);
			file = new_file;
		}
	}
#endif

	return TAI_CONTINUE(int, sceIoGetstatRef, file, stat);
}

/* Last surface pspemu was seen rendering into (see ra_pops_redirect_active). */
static volatile int g_pops_redirect;

int ra_pops_redirect_active(void) {
	return g_pops_redirect;
}

/* Diagnostic for the quit-window investigation (docs/54 row 21).
 *
 * The question it answers is the one nobody has measured yet: where does pspemu
 * point its display when a POPS title runs and when the quit starts? The compat
 * call carries the answer in its own argument - a NULL `pParam` is pspemu asking
 * "where should I draw?", a non-NULL one is pspemu saying where it *is* drawing
 * (or the shell re-pointing the display). Logging both, with the value we last
 * saw, tells us who moves the display and when, which is exactly what decides
 * whether our presents can ever be visible during the transition.
 *
 * Budgeted: 24 lines per session, then only changes, so a long session cannot
 * flood the log. */
#define FB_LOG_CALLS 24

static int g_fb_log_budget = FB_LOG_CALLS;
static void *g_fb_log_last_base;
static int g_fb_log_last_query = -1;

int sceDisplaySetFrameBufForCompatPatched(int a1, int a2, int a3, int a4, int a5, SceDisplayFrameBuf *pParam) {
	/* Stock Adrenaline behaviour, restored after the 00.10.35-00.10.49 attempts:
	 * only a non-Original filter redirects POPS to Adrenaline's own texture. With
	 * "Original" the redirect stays off, pspemu keeps its own 480x272 PSP screen,
	 * and because the menu loop presents nothing in that mode the display stays
	 * pspemu's - which is what lets Adrenaline's own quit sequence (sce "Please
	 * wait…" then the XMB) reach the screen. Every variant that redirected while a
	 * PS1 title ran also redirected the shell's frames, and the loop then showed a
	 * stale copy for ever. */
	{
		int is_query = (pParam == NULL);
		void *base = is_query ? NULL : pParam->base;
		/* 00.10.130: pspemu triple-buffers, so `base` changes *every frame* and
		 * this line fired 1753 times in 24 minutes - 43% of the log, written to
		 * the same card the game streams its ISO from, for a diagnostic that was
		 * meant to fire when something changes. A change in `is_query`, or a new
		 * base, still logs immediately; a repeated base change (the rotation) is
		 * reported at most twice a second. */
		static unsigned long long next_rotation_us;
		unsigned long long now_us = (unsigned long long)sceKernelGetSystemTimeWide();
		int wanted = (is_query != g_fb_log_last_query) ||
		             (base != g_fb_log_last_base && now_us >= next_rotation_us);

		if (g_fb_log_budget > 0 || wanted) {
			if (g_fb_log_budget > 0)
				--g_fb_log_budget;
			g_fb_log_last_query = is_query;
			g_fb_log_last_base = base;
			next_rotation_us = now_us + 500000ULL;
			ra_logq_printf("fb: query=%d base=%p pops_data=%p redirect=%d "
			               "title_pops=%d shell=%d\n",
			               is_query, base, pops_data, g_pops_redirect,
			               ra_runtime_title_is_pops(), ra_runtime_in_shell());
		}
	}

	/* The redirect follows the earliest signal that the game is gone.
	 *
	 * `pops_mode` alone flips too late: by the time it drops, the shell has already
	 * asked where to draw and been given `pops_data`, so its "Please wait…" is
	 * composited by us, then pspemu's final POPS frame lands in VRAM, then the
	 * shell draws again - the exit screen, game frame, exit screen sequence the
	 * operator reported. `adrenaline->title` turns into "XMB" one step earlier
	 * (00.10.67 log: `pops=0 ... title='XMB™'`), so the answer flips before the
	 * shell asks and its frames go straight into VRAM. */
	{
		SceAdrenaline *adrenaline = (SceAdrenaline *)ScePspemuConvertAddress(
			ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);
		int game_live = 0;

		if (adrenaline != NULL && adrenaline->pops_mode != 0) {
			const char *title = adrenaline->title;

			/* "XMB™" (or an empty title) means the shell owns the screen, even if
			 * `pops_mode` has not been cleared yet. */
			game_live = title != NULL && title[0] != '\0' &&
			            strncmp(title, "XMB", 3) != 0;
		}

		if (config.graphics_filtering != 0 || game_live) {
			if (pParam == NULL) {
				static SceDisplayFrameBuf param;
				param.size = sizeof(SceDisplayFrameBuf);
				param.base = pops_data;
				param.pitch = SCREEN_LINE;
				param.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
				param.width = SCREEN_WIDTH;
				param.height = SCREEN_HEIGHT;
				pParam = &param;
				g_pops_redirect = 1;
			}
		} else if (pParam == NULL) {
			/* No game: the query passes through, so pspemu's own buffer is what
			 * the exit screen and the XMB are painted into. */
			g_pops_redirect = 0;
		}
	}

	/* A non-NULL call carries the surface pspemu is about to draw into, so it
	 * answers the same question without any guessing. Comparing it with
	 * `pops_data` is what lets the menu stop presenting our texture the moment
	 * pspemu takes its own buffer back - the case hash sampling could only infer
	 * after the fact. */
	if (pParam != NULL)
		g_pops_redirect = (pParam->base == pops_data) ? 1 : 0;

	return TAI_CONTINUE(int, sceDisplaySetFrameBufForCompatRef, a1, a2, a3, a4, a5, pParam);
}
