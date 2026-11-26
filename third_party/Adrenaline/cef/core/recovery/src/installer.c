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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <pspctrl.h>
#include <pspiofilemgr.h>

#include <pspextratypes.h>
#include <systemctrl.h>
#include <systemctrl_se.h>
#include <libpsardumper.h>
#include <pspdecrypt.h>

#include "main.h"
#include "menu.h"
#include "utils.h"
#include "files.h"
#include "psperror.h"
#include <systemctrl_epi.h>

#define BIG_BUFFER_SIZE 8 * 1024 * 1024
#define SMALL_BUFFER_SIZE 2 * 1024 * 1024

#define FLASH0_LIVE "ms0:/__ADRENALINE__/flash0"
#ifdef CEF_TEST_RECOVERY
#define FLASH0_EXTRACT FLASH0_LIVE ".installing"
#else
#define FLASH0_EXTRACT FLASH0_LIVE
#endif

static char *g_flash0_dirs[] = {
	FLASH0_EXTRACT "/codepage",
	FLASH0_EXTRACT "/data",
	FLASH0_EXTRACT "/dic",
	FLASH0_EXTRACT "/font",
	FLASH0_EXTRACT "/kd",
	FLASH0_EXTRACT "/vsh",
	FLASH0_EXTRACT "/data/cert",
	FLASH0_EXTRACT "/kd/resource",
	FLASH0_EXTRACT "/vsh/etc",
	FLASH0_EXTRACT "/vsh/module",
	FLASH0_EXTRACT "/vsh/resource",
};

static char *g_flash1_dirs[] = {
	"ms0:/__ADRENALINE__/flash1/dic",
	"ms0:/__ADRENALINE__/flash1/gps",
	"ms0:/__ADRENALINE__/flash1/net",
	"ms0:/__ADRENALINE__/flash1/net/http",
	"ms0:/__ADRENALINE__/flash1/registry",
	"ms0:/__ADRENALINE__/flash1/vsh",
	"ms0:/__ADRENALINE__/flash1/vsh/theme",
};

static int FindTablePath(char *table, int table_size, char *number, char *szOut) {
	int i, j, k;
	for (i = 0; i < table_size - 5; i++) {
		if (strncmp(number, table+i, 5) == 0) {
			for (j = 0, k = 0; ; j++, k++) {
				if (table[i+j+6] < 0x20) {
					szOut[k] = 0;
					break;
				}

				if (table[i+5] == '|' && strncmp(table+i+6, "flash", 5) == 0 && j == 6) {
					szOut[6] = ':';
					szOut[7] = '/';
					k++;
				} else if (table[i+5] == '|' && strncmp(table+i+6, "ipl", 3) == 0 && j == 3) {
					szOut[3] = ':';
					szOut[4] = '/';
					k++;
				} else
				{
					szOut[k] = table[i+j+6];
				}
			}

			return 1;
		}
	}

	return 0;
}

static const char *GetVersion(const char *buf, size_t capacity) {
	const char *version = NULL;
	for (size_t i = 0; i < capacity; i++) {
		if (buf[i] == '\0') {
			return version && buf + i - version >= 4 ? version : NULL;
		}
		if (buf[i] == ',') {
			version = buf + i + 1;
		}
	}
	return NULL;
}

static int LoadFirmwarePsar(SceUID fd, u8 *buffer) {
	int file_size = sceIoLseek32(fd, 0, PSP_SEEK_END);
	PBPHeader header;
	if (file_size < (int)sizeof(header) || sceIoLseek32(fd, 0, PSP_SEEK_SET) != 0 ||
	    sceIoRead(fd, &header, sizeof(header)) != sizeof(header) ||
	    memcmp(&header.magic, "\0PBP", 4) != 0 ||
	    header.psar_offset < sizeof(header) || header.psar_offset > (u32)file_size) {
		return -1;
	}

	int size = file_size - (int)header.psar_offset;
	if (size < 16 || sceIoLseek32(fd, header.psar_offset, PSP_SEEK_SET) != (int)header.psar_offset) {
		return -1;
	}

	int count = size < BIG_BUFFER_SIZE ? size : BIG_BUFFER_SIZE;
	if (sceIoRead(fd, buffer, count) != count || memcmp(buffer, "PSAR", 4) != 0) {
		return -1;
	}
	memset(buffer + count, 0, BIG_BUFFER_SIZE - count);
	return size;
}

static int is5Dnum(char *str) {
	int len = strlen(str);
	if (len != 5) {
		return 0;
	}

	for (int i = 0; i < len; i++) {
		if (str[i] < '0' || str[i] > '9') {
			return 0;
		}
	}

	return 1;
}

static void DrawProgress(int progress) {
	VGraphGoto(2, 9);
	for (int i = 2; i < 58; i++) {
		VGraphPrintf("\xB1");
	}

	VGraphGoto(2, 9);
	int cprogress = ((float)progress / ((float)N_FILES / 56.f));
	for (int i = 0; i < cprogress+1; i++) {
		VGraphPrintf("\xDB");
	}
}


#ifdef CEF_TEST_RECOVERY
static int RequireNewFlash0(void) {
	SceIoStat stat;
	int res = sceIoGetstat(FLASH0_LIVE, &stat);
	return res == SCE_ENOENT ? 0 : res < 0 ? res : SCE_EEXIST;
}

static int PublishStagedFlash0(int files_written) {
	if (files_written != N_FILES) {
		return SCE_EINVAL;
	}
	int res = RequireNewFlash0();
	if (res < 0) {
		return res;
	}
	return sceIoRename(FLASH0_EXTRACT, FLASH0_LIVE);
}
#endif

void Installer() {
#ifdef CEF_TEST_RECOVERY
	u8 written[N_FILES] = {0};
	int stage_created = 0;
#endif
	SceUID fd = -1;
	u32 size_written = 0;
	int res = 0;
	int error = 0;
	int psar_pos = 0;

	u8 *sm_buffer1 = memalign(64, SMALL_BUFFER_SIZE);
	u8 *sm_buffer2 = memalign(64, SMALL_BUFFER_SIZE);
	u8 *big_buffer = memalign(64, BIG_BUFFER_SIZE);

	static char flash_table[4][0x4000];
	static int flash_table_size[4];

	memset(flash_table, 0, sizeof(flash_table));
	memset(flash_table_size, 0, sizeof(flash_table_size));

	VGraphSetBackColor(0x0);
	VGraphClear();

	VGraphSetTextColor(0xF, 0x0);

	VGraphPrintf("\xC9");
	for (int i = 1; i < 59; i++) {
		VGraphPrintf("\xCD");
	}
	VGraphPrintf("\xBB");

	for (int i = 1; i < 32; i++) {
		VGraphGoto(0, i);
		VGraphClearLine(0x0);
		VGraphPrintf("\xBA");
		VGraphGoto(59, i);
		VGraphPrintf("\xBA");
	}

	VGraphGoto(0, 32);
	VGraphSetTextColor(0xF, 0x0);
	VGraphPrintf("\xC8");
	for (int i = 1; i < 59; i++) {
		VGraphPrintf("\xCD");
	}
	VGraphPrintf("\xBC");


	VGraphGoto(15, 2);
	VGraphPrintf("Press X to install the PSP 6.61");
	VGraphGoto(16, 3);
	VGraphPrintf("firmware on your Memory Card.");

	while (1) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(&pad, 1);

		if (pad.Buttons & PSP_CTRL_CROSS) {
			break;
		}

		sceKernelDelayThread(10 * 1000);
	}

	VGraphGoto(2, 5);
	VGraphPrintf("Loading 661.PBP... ");

	if (!sm_buffer1 || !sm_buffer2 || !big_buffer) {
		VGraphGoto(2, 24);
		VGraphSetTextColor(0x4, 0x0);
		VGraphPrintf("Not enough memory to read PSP firmware.");
		goto EXIT;
	}

	// Open file
	const char *firmware_path = "ms0:/__ADRENALINE__/661.PBP";
	fd = sceIoOpen(firmware_path, PSP_O_RDONLY, 0);
	if (fd < 0) {
		VGraphGoto(2, 24);
		// TODO:show error dialog
		VGraphSetTextColor(0x4, 0x0);
		VGraphPrintf("Cannot open PSP firmware at %s.", firmware_path);
		goto EXIT;
	}

	int size = LoadFirmwarePsar(fd, big_buffer);
	if (size < 0) {
		VGraphGoto(2, 24);
		VGraphSetTextColor(0x4, 0x0);
		VGraphPrintf("Invalid or incomplete 661.PBP.");
		goto EXIT;
	}

	VGraphSetTextColor(0x2, 0x0);
	VGraphPrintf("OK");
	VGraphSetTextColor(0xF, 0x0);

	int psarVersion = big_buffer[4];

	memset(sm_buffer1, 0, SMALL_BUFFER_SIZE);
	res = pspPSARInit(big_buffer, sm_buffer1, sm_buffer2);
	if (res < 0) {
		VGraphGoto(2, 24);
		VGraphSetTextColor(0x4, 0x0);
		VGraphPrintf("pspPSARInit failed (0x%08X).", res);
		goto EXIT;
	}

	const char *version = GetVersion((const char *)sm_buffer1 + 0x10, SMALL_BUFFER_SIZE - 0x10);
	if (!version || memcmp(version, "6.61", 4) != 0) {
		VGraphGoto(2, 24);
		VGraphSetTextColor(0x4, 0x0);
		VGraphPrintf("Please obtain the correct EBOOT.PBP for 6.61.");
		goto EXIT;
	}

	if (psarVersion == 5) {
		VGraphGoto(2, 24);
		VGraphSetTextColor(0x4, 0x0);
		VGraphPrintf("Please obtain the correct EBOOT.PBP");
		VGraphGoto(2, 25);
		VGraphPrintf("for model 1000/2000/3000.");
		goto EXIT;
	}

	VGraphGoto(2, 6);
	VGraphPrintf("Creating directories... ");

#ifdef CEF_TEST_RECOVERY
	res = RequireNewFlash0();
	if (res < 0) {
		VGraphGoto(2, 24);
		VGraphSetTextColor(0x4, 0x0);
		VGraphPrintf("Test requires a new flash0 directory (0x%08X).", res);
		goto EXIT;
	}
	res = sceIoMkdir(FLASH0_EXTRACT, 0777);
	if (res < 0) {
		VGraphGoto(2, 24);
		VGraphSetTextColor(0x4, 0x0);
		VGraphPrintf("Cannot create Test staging directory (0x%08X).", res);
		goto EXIT;
	}
	stage_created = 1;
#else
	removePath(FLASH0_LIVE);
	removePath("ms0:/__ADRENALINE__/flash1");
	sceIoMkdir(FLASH0_LIVE, 0777);
#endif
	sceIoMkdir("ms0:/__ADRENALINE__/flash1", 0777);
	sceIoMkdir("ms0:/__ADRENALINE__/flash2", 0777);
	sceIoMkdir("ms0:/__ADRENALINE__/flash3", 0777);

	res = CreateDirectories(g_flash0_dirs, sizeof(g_flash0_dirs) / sizeof(char **));
	if (res < 0) {
		VGraphGoto(2, 24);
		VGraphSetTextColor(0x4, 0x0);
		VGraphPrintf("Error creating directories (0x%08X).", res);
		goto EXIT;
	}

	res = CreateDirectories(g_flash1_dirs, sizeof(g_flash1_dirs) / sizeof(char **));
	if (res < 0) {
		VGraphGoto(2, 24);
		VGraphSetTextColor(0x4, 0x0);
		VGraphPrintf("Error creating directories (0x%08X).", res);
		goto EXIT;
	}

	VGraphSetTextColor(0x2, 0x0);
	VGraphPrintf("OK");
	VGraphSetTextColor(0xF, 0x0);

	VGraphGoto(2, 7);
	VGraphPrintf("Extracting firmware...");
	DrawProgress(0);

	int progress = 0;

	while (1) {
		char name[128];
		int cbExpanded;
		int pos;
		int signcheck;

		res = pspPSARGetNextFile(big_buffer, size, sm_buffer1, sm_buffer2, name, &cbExpanded, &pos, &signcheck);

		if (res < 0) {
			if (error) {
				VGraphGoto(2, 24);
				VGraphSetTextColor(0x4, 0x0);
				VGraphPrintf("Error decoding PSAR (0x%08X).", pos);
				goto EXIT;
			}

			int dpos = pos - psar_pos;
			psar_pos = pos;

			error = 1;
			memmove(big_buffer, big_buffer + dpos, BIG_BUFFER_SIZE - dpos);
			res = sceIoRead(fd, big_buffer + (BIG_BUFFER_SIZE - dpos), dpos);
			if (res <= 0) {
				VGraphGoto(2, 24);
				VGraphSetTextColor(0x4, 0x0);
				VGraphPrintf("Error reading 661.PBP (0x%08X).", res);
				goto EXIT;
			}

			pspPSARSetBufferPosition(psar_pos);

			continue;
		} else if (res == 0) { // no more files
			break;
		}

		if (cbExpanded > 0) {
			if (is5Dnum(name)) {
				int num = strtoul(name, NULL, 10);

				// Files from 1g-3g
				if (num >= 1 && num <= 3) {
					flash_table_size[num] = pspDecryptTable(sm_buffer2, sm_buffer1, cbExpanded, 4);
					if (flash_table_size[num] <= 0) {
						VGraphGoto(2, 24);
						VGraphSetTextColor(0x4, 0x0);
						VGraphPrintf("Error decrypting table (0x%08X).", flash_table_size[num]);
						goto EXIT;
					}

					memcpy(flash_table[num], sm_buffer2, flash_table_size[num]);

					error = 0;
					continue;
				} else {
					if (num > 12) { //model 12g
						int found = 0;

						for (int i = 1; i <= 3; i++) {
							if (flash_table_size[i] > 0) {
								found = FindTablePath(flash_table[i], flash_table_size[i], name, name);
								if (found)
									break;
							}
						}

						if (found) {
							if (strncmp(name, "flash0", 6) == 0) {
								for (int i = 0; i < N_FILES; i++) {
									if (strcmp(name + 7, g_files[i]) == 0) {
										char path[256];
										sprintf(path, FLASH0_EXTRACT "/%s", name + 8);
										res = WriteFile(path, sm_buffer2, cbExpanded);
										if (res != cbExpanded) {
											VGraphGoto(2, 24);
											VGraphSetTextColor(0x4, 0x0);
											VGraphPrintf("Error writing file (0x%08X).", res);
											goto EXIT;
										}
#ifdef CEF_TEST_RECOVERY
										progress += !written[i];
										written[i] = 1;
#else
										progress++;
#endif
										DrawProgress(progress);
									}
								}
							}
						} else {
							error = 0;
							continue;
						}
					} else {
						error = 0;
						continue;
					}
				}
			}
		}

		error = 0;
	}

#ifdef CEF_TEST_RECOVERY
	res = PublishStagedFlash0(progress);
	if (res < 0) {
		VGraphGoto(2, 24);
		VGraphSetTextColor(0x4, 0x0);
		VGraphPrintf("Cannot publish Test flash0 (0x%08X).", res);
		goto EXIT;
	}
	stage_created = 0;
#endif

	VGraphGoto(8, 15);
	VGraphSetTextColor(0x2, 0x0);
	VGraphPrintf("The firmware has been installed successfully.");

	VGraphGoto(16, 16);
	VGraphSetTextColor(0xF, 0x0);
	VGraphPrintf("Press X to boot the PSP XMB.");

	while (1) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(&pad, 1);

		if (pad.Buttons & PSP_CTRL_CROSS) {
			break;
		}

		sceKernelDelayThread(10 * 1000);
	}

	sceKernelDelayThread(500 * 1000);
	sctrlKernelExitVSH(NULL);

EXIT:
#ifdef CEF_TEST_RECOVERY
	if (stage_created)
		removePath(FLASH0_EXTRACT);
#endif
	if (fd >= 0)
		sceIoClose(fd);

	free(sm_buffer1);
	free(sm_buffer2);
	free(big_buffer);

	VGraphGoto(5, 29);
	VGraphPrintf("To exit please hold the PS button, open 'Settings'");
	VGraphGoto(10, 30);
	VGraphPrintf("and select 'Exit PspEmu Application'.");

	while (1) {
		sceKernelDelayThread(1 * 1000 * 1000);
	}
}

void ResetSettings() {
	removePath("ms0:/__ADRENALINE__/flash1");

	sceIoMkdir("ms0:/__ADRENALINE__/flash1", 0777);

	int res = CreateDirectories(g_flash1_dirs, sizeof(g_flash1_dirs) / sizeof(char **));
	if (res < 0) {
		VGraphGoto(2, 24);
		VGraphSetTextColor(0x4, 0x0);
		VGraphPrintf("Error creating directories (0x%08X).\n", res);
		goto EXIT;
	}

	sctrlKernelExitVSH(NULL);

EXIT:
	sceKernelDelayThread(1 * 1000 * 1000);
}
