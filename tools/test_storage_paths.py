#!/usr/bin/env python3
"""Host-side simulation of the Test profile storage redirection.

Extracts the real path-translation functions from user/msfs.c, user/pops.c and
user/utils.c, compiles them with the host C compiler against stubbed Vita I/O
calls, and checks where every PSP memory-stick and native PSPemu access lands.

Run from the repository root:  python3 tools/test_storage_paths.py
Needs only Python 3 and gcc/cc (no VitaSDK).
"""
import ctypes as ct
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
USER = ROOT / "third_party" / "Adrenaline" / "user"


def functions(file, *names):
    source = (USER / file).read_text(encoding="utf-8")
    out = []
    for name in names:
        match = re.search(r"^(?:static )?(?:const )?\w+ \*?" + name + r"\([^\n]*\) \{.*?^}", source, re.M | re.S)
        assert match, f"{name} not found in {file}"
        out.append(match[0])
    return "\n".join(out)


PRELUDE = r"""
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stddef.h>
#define SCE_EINVAL (-22)
#define SCE_ENOENT (-2)
#define SCE_O_RDONLY 1
#define SCE_O_WRONLY 2
#define SCE_O_NBLOCK 0x0004
#define SCE_O_RDLOCK 0x0010
#define SCE_O_CREAT 0x200
#define SCE_O_TRUNC 0x400
#define SCE_O_NOBUF 0x4000
#define SCE_O_NOWAIT 0x8000
#define SCE_O_FDEXCL 0x01000000
#define SCE_O_PWLOCK 0x02000000
#define MAX_PATH_LENGTH 1024
#define ADRENALINE_TITLEID "ADRA00001"
#define ADRENALINE_TEST_DATA_ROOT "ux0:data/ADRA00001"
#define ADRENALINE_TEST_MS_ROOT ADRENALINE_TEST_DATA_ROOT "/pspemu"
#define ADRENALINE_TEST_ISO_ROOT "ux0:pspemu/ISO"
#define ADRENALINE_TEST_GAME_ROOT "ux0:pspemu/PSP/GAME"
#define ADRENALINE_ADDRESS 1
#define ADRENALINE_SIZE 1
#define KERMIT_INPUT_MODE 1
typedef int SceUID;
typedef int SceMode;
typedef struct {char filename[256]; char titleid[12];} SceAdrenaline;
typedef struct {int value;} SceIoStat;
enum FlashPath {FLASH_DATA, FLASH_APP};
static void logTestPathError(const char *operation, const char *name, int flags, int result) {}
static char *getPspemuEfLocation(void) { return NULL; }
static char *getPspemuMemoryStickLocation(void) { return ADRENALINE_TEST_MS_ROOT; }
static SceAdrenaline shared;
static void *ScePspemuConvertAddress(int address, int mode, int size) { return &shared; }
static int calls, open_flags, mkdirs;
static char first[1024], last[1024], new_name[1024], last_mkdir[1024];
static const char *existing = "";
static int record(const char *name) { if (!calls) strcpy(first, name); calls++; strcpy(last, name); return SCE_ENOENT; }
static int sceIoOpen(const char *name, int flags, int mode) { open_flags = flags; return record(name); }
static int sceIoClose(int fd) { return 0; }
static int sceIoRemove(const char *name) { return record(name); }
static int sceIoRename(const char *old, const char *new) { strcpy(new_name, new); return record(old); }
static int sceIoMkdir(const char *name, int mode) { mkdirs++; strcpy(last_mkdir, name); return 0; }
/* The translator's own existence probe; not counted as an emulator call. */
static int sceIoGetstat(const char *name, SceIoStat *stat) { return strcmp(name, existing) == 0 ? 0 : SCE_ENOENT; }
static int ScePspemuMsfsAddDescriptor(int fd, char *path, int flags, int trunc, int folder) { return fd; }
static int sceIoOpenRef(const char *name, int flags, int mode) { return record(name); }
static int sceIoGetstatRef(const char *name, SceIoStat *stat) { return record(name); }
#define TAI_CONTINUE(type, function, ...) function(__VA_ARGS__)
"""

CHECKS = r"""
void reset(const char *exists) {
    calls = mkdirs = 0;
    first[0] = last[0] = new_name[0] = last_mkdir[0] = 0;
    existing = exists ? exists : "";
    strcpy(shared.titleid, "SLUS12345");
    strcpy(shared.filename, "ms0:/PSP/GAME/DISC/EBOOT.PBP");
}
int action(int command, const char *name, const char *other, int flags) {
    switch (command) {
    case 0: return ScePspemuMsfsOpen(name, flags, 0);
    case 1: return ScePspemuMsfsRemove(name);
    case 2: return ScePspemuMsfsRename(name, other);
    case 3: return sceIoOpenPatched(name, flags, 0);
    case 4: return sceIoGetstatPatched(name, NULL);
    }
    return -22;
}
int count_calls(void) { return calls; }
int count_mkdirs(void) { return mkdirs; }
const char *get_first(void) { return first; }
const char *get_path(void) { return last; }
const char *get_mkdir(void) { return last_mkdir; }
"""

STICK = b"ux0:data/ADRA00001/pspemu/"
GAME = b"ux0:pspemu/PSP/GAME/"

# name, op, path, other, flags, existing-shared-file, result, calls, first path, last path
CASES = [
    ("MSFS reads shared ISO", 0, b"/ISO/ULUS10041.iso", None, 1, None, -2, 2, b"ux0:pspemu/ISO/ULUS10041.iso", b"ux0:pspemu/ISO/ULUS10041.iso"),
    ("MSFS keeps PSP ISO driver mode", 0, b"/ISO/ULUS10160.iso", None, 0x000F0001, None, -2, 2, b"ux0:pspemu/ISO/ULUS10160.iso", b"ux0:pspemu/ISO/ULUS10160.iso"),
    ("MSFS rejects writes to shared ISO", 0, b"/ISO/ULUS10041.iso", None, 2, None, -22, 0, b"", b""),
    ("MSFS rejects deleting shared ISO", 1, b"/ISO/ULUS10041.iso", None, 0, None, -22, 0, b"", b""),
    ("MSFS reads PS1 EBOOT from shared PSP/GAME", 0, b"/PSP/GAME/SLUS00594/EBOOT.PBP", None, 1, None, -2, 2, GAME + b"SLUS00594/EBOOT.PBP", STICK + b"PSP/GAME/SLUS00594/EBOOT.PBP"),
    ("MSFS PSP/GAME match ignores case", 0, b"/psp/game/SLUS00594/EBOOT.PBP", None, 1, None, -2, 2, GAME + b"SLUS00594/EBOOT.PBP", STICK + b"psp/game/SLUS00594/EBOOT.PBP"),
    ("MSFS writes in PSP/GAME stay in Test", 0, b"/PSP/GAME/HOMEBREW/config.txt", None, 2 | 0x200, None, -2, 1, STICK + b"PSP/GAME/HOMEBREW/config.txt", STICK + b"PSP/GAME/HOMEBREW/config.txt"),
    ("MSFS deletes in PSP/GAME never touch shared", 1, b"/PSP/GAME/SLUS00594/EBOOT.PBP", None, 0, None, -2, 1, STICK + b"PSP/GAME/SLUS00594/EBOOT.PBP", STICK + b"PSP/GAME/SLUS00594/EBOOT.PBP"),
    ("MSFS separates GAME prefix from GAME150", 0, b"/PSP/GAME150/x/EBOOT.PBP", None, 1, None, -2, 2, STICK + b"PSP/GAME150/x/EBOOT.PBP", STICK + b"PSP/GAME150/x/EBOOT.PBP"),
    ("MSFS savedata stays in Test", 0, b"/PSP/SAVEDATA/ULUS10160/DATA.BIN", None, 1, None, -2, 2, STICK + b"PSP/SAVEDATA/ULUS10160/DATA.BIN", STICK + b"PSP/SAVEDATA/ULUS10160/DATA.BIN"),
    ("MSFS rejects traversal out of PSP/GAME", 0, b"/PSP/GAME/../../x", None, 1, None, -22, 0, b"", b""),
    ("Native POPS opens shared EBOOT", 3, b"ms0:/PSP/GAME/SLUS00594/EBOOT.PBP", None, 1, GAME + b"SLUS00594/EBOOT.PBP", -2, 1, GAME + b"SLUS00594/EBOOT.PBP", GAME + b"SLUS00594/EBOOT.PBP"),
    ("Native ux0:pspemu EBOOT opens shared", 3, b"ux0:pspemu/PSP/GAME/SLUS00594/EBOOT.PBP", None, 1, GAME + b"SLUS00594/EBOOT.PBP", -2, 1, GAME + b"SLUS00594/EBOOT.PBP", GAME + b"SLUS00594/EBOOT.PBP"),
    ("Native EBOOT missing in shared uses Test", 3, b"ms0:/PSP/GAME/DISC/EBOOT.PBP", None, 1, None, -2, 1, STICK + b"PSP/GAME/DISC/EBOOT.PBP", STICK + b"PSP/GAME/DISC/EBOOT.PBP"),
    ("Native write in PSP/GAME stays in Test", 3, b"ms0:/PSP/GAME/DISC/KEYS.BIN", None, 2, GAME + b"DISC/KEYS.BIN", -2, 1, STICK + b"PSP/GAME/DISC/KEYS.BIN", STICK + b"PSP/GAME/DISC/KEYS.BIN"),
    ("Native ISO write blocked", 3, b"ux0:pspemu/ISO/ULUS10490.iso", None, 2, None, -22, 0, b"", b""),
    ("POPS memory card rebased into Test", 3, b"ux0:pspemu/PSP/SAVEDATA/SLUS12345/SCEVMC0.VMP", None, 2 | 0x200, None, -2, 1, STICK + b"PSP/SAVEDATA/SLUS12345/SCEVMC0.VMP", STICK + b"PSP/SAVEDATA/SLUS12345/SCEVMC0.VMP"),
    ("POPS menu info rebased into Test", 4, b"ms0:/__sce_menuinfo", None, 0, None, -2, 1, STICK + b"PSP/GAME/DISC/__sce_menuinfo", STICK + b"PSP/GAME/DISC/__sce_menuinfo"),
    ("Native workspace read", 3, b"ux0:temp/pspemu/w00.bin", None, 1, None, -2, 1, b"ux0:temp/pspemu/w00.bin", b"ux0:temp/pspemu/w00.bin"),
    ("Native rejects EF", 3, b"ef0:/PSP/SCEVMC0.VMP", None, 2, None, -22, 0, b"", b""),
]


def main():
    msfs = functions("msfs.c", "buildPspemuMsfsPath", "isReadOnlyOpen", "canRetryInApp",
                     "ScePspemuMsfsOpen", "ScePspemuMsfsRemove", "ScePspemuMsfsRename")
    pops = functions("pops.c", "isTestNativeStorageRoot", "isTestPspemuSystemRead", "translateTestNativePath",
                     "ensureTestParentDirectories", "buildTestPopsPath", "sceIoOpenPatched", "sceIoGetstatPatched")
    utils = functions("utils.c", "isTestPspRelativePath", "isTestReadOnlyOpen", "getTestIsoSuffix", "getTestGameSuffix")
    code = "#define ADRENALINE_TEST_PROFILE 1\n" + "\n".join((PRELUDE, utils, msfs, pops, CHECKS))

    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "paths.c"
        library = Path(tmp) / "paths.so"
        source.write_text(code)
        subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-w", "-o", str(library), str(source)], check=True)
        lib = ct.CDLL(str(library))
        lib.reset.argtypes = [ct.c_char_p]
        lib.action.argtypes = [ct.c_int, ct.c_char_p, ct.c_char_p, ct.c_int]
        for name in ("get_first", "get_path", "get_mkdir"):
            getattr(lib, name).restype = ct.c_char_p

        for name, op, path, other, flags, exists, result, total, first, last in CASES:
            lib.reset(exists)
            actual = (lib.action(op, path, other, flags), lib.count_calls(), lib.get_first(), lib.get_path())
            expected = (result, total, first, last)
            if actual == expected:
                print("PASS", name)
            else:
                failures += 1
                print("FAIL", name, "\n  expected", expected, "\n  actual  ", actual)

        lib.reset(None)
        lib.action(3, b"ux0:pspemu/PSP/SAVEDATA/SLUS12345/SCEVMC0.VMP", None, 2 | 0x200)
        if lib.count_mkdirs() == 3 and lib.get_mkdir() == STICK + b"PSP/SAVEDATA/SLUS12345":
            print("PASS POPS memory card creates its Test folder")
        else:
            failures += 1
            print("FAIL POPS memory card folder", lib.count_mkdirs(), lib.get_mkdir())

    print(f"{len(CASES) + 1 - failures}/{len(CASES) + 1} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
