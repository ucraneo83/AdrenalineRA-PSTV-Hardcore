# AdrenalineRA Test 01.07 — fixes and PSTV checklist

Builds on the hardware-tested 01.06 replacement profile (`ADRA00001`).

## Fixed

### Reopening the Test bubble without rebooting (C1-2758-2)
`kernel/main.c` remembered the user module of the *previous* Adrenaline process.
On a second launch it asked taiHEN about the exited PID; any error it did not
recognise was treated as "module still loaded", so the new PSP emulator process
started **without** `adrenaline_user.suprx` and crashed. The record is now reset
whenever the preload hook sees a different PID, so every launch gets its user
module. (Simulated: three launches in a row now inject three times; the old code
injected once.)

### PS1 (POPS) games and PSP EBOOTs not listed
Test mapped all of `ms0:` to `ux0:data/ADRA00001/pspemu` except `ISO`, so games
in the normal `ux0:pspemu/PSP/GAME` folder were invisible. `PSP/GAME` is now
handled like `ISO`:

| Access | Goes to |
| --- | --- |
| Read / list / stat `ms0:/PSP/GAME/...` | `ux0:pspemu/PSP/GAME/...` (falls back to the Test stick if missing there) |
| Write / create / delete / rename in `PSP/GAME` | `ux0:data/ADRA00001/pspemu/PSP/GAME/...` (shared folder is never modified) |
| POPS native open of the EBOOT | shared folder if the file exists, else Test stick |
| PS1 memory cards, `__sce_menuinfo` | Test stick (`.../pspemu/PSP/SAVEDATA/<id>/`), folders created on demand |
| RetroAchievements hashing | same order as the emulator |

Note: if `ux0:pspemu/PSP/GAME` exists, copies placed only in
`ux0:data/ADRA00001/pspemu/PSP/GAME` are not listed. Keep games in
`ux0:pspemu/PSP/GAME`. PS1/PSP saves stay separate from the original install.

`python3 tools/test_storage_paths.py` runs 21 host-side cases against the real
translation functions (needs only `cc`).

## Hardcore — status

* Hardcore unlocks **cannot** be earned with this client until RetroAchievements
  approves the `AdrenalineRA` client identity. The server answers a Hardcore
  session from an unknown client with its warning achievement (101000001); the
  app then downgrades the session to Casual ("UP" banner). This is by design and
  must not be bypassed (no spoofing of another emulator's identity).
* The crash reported with Hardcore enabled is **not reproduced or root-caused
  yet**. No source change in 01.07 claims to fix it. To diagnose it, enable
  logging in the RA tab, reproduce once, and collect:
  - `ux0:data/AdrenalineRA-Test/logs/` (all files)
  - `ux0:data/ADRA00001/psp-io.txt`, `relaunch.txt`, `kernel-start.txt`
  - the error code / photo of the screen.

## PSTV checklist for 01.07
1. Install the VPK, full restart of the PSTV.
2. Launch Test, start a PSP ISO game, exit to LiveArea, close the bubble,
   **reopen without rebooting** → must reach the PSP menu (no C1-2758-2).
3. Put a single-disc PS1 `EBOOT.PBP` in `ux0:pspemu/PSP/GAME/<folder>/` → it must
   appear in the PSP menu, boot, be recognised by RA, and save to its memory card.
4. Casual: progress and an unlock, as in 01.06.
5. Hardcore: enable, launch, note exactly when it fails, and send the logs above.
