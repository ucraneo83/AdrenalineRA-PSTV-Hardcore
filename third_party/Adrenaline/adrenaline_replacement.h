#ifndef ADRENALINE_REPLACEMENT_H
#define ADRENALINE_REPLACEMENT_H

#include <psp2common/kernel/iofilemgr.h>

#define ADRENALINE_START_LOG ADRENALINE_TEST_DATA_ROOT "/kernel-start.txt"
#define ADRENALINE_NOT_FOUND ((int)0x80010002)

static inline int adrenaline_original_app_is_absent(int (*getstat)(const char *, SceIoStat *)) {
	SceIoStat stat;
	if (getstat("ux0:app", &stat) < 0 || !SCE_S_ISDIR(stat.st_mode)) {
		return 0;
	}
	return getstat("ux0:app/PSPEMUCFW", &stat) == ADRENALINE_NOT_FOUND;
}

#endif
