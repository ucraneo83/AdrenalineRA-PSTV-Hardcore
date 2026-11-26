# PSTV 3.65 build and validation plan — Phase 4

## Verified from uploaded source
- CMake requires VitaSDK and Adrenaline **8.0.2** base, enforced in `third_party/Adrenaline/CMakeLists.txt`.
- `README.md` documents the VPK target `Adrenaline.vpk-vpk` and output `bubble/Adrenaline.vpk`.
- `README.md` says `libvita2dfbo` and `vitashadercollection` must be built first.
- Prebuilt PSP modules are in `third_party/Adrenaline/user/flash0/kd/`; rebuilding these requires a separate PSP CFW toolchain.
- Phase 3 guardrails are static text-presence checks only.

## Reproducible local/cloud runner entrypoint
From repository root, with VitaSDK installed and `VITASDK` exported:

```bash
./tools/build_pstv.sh
```

The script fails early when SDK or tools are missing; then runs guardrails, configures Release, builds graphics prerequisites, builds VPK, and emits SHA256. **It has not been compiled in this environment**. The graphics target names may need adjustment after the first real VitaSDK configure; capture the first failure log rather than claiming success.

## Hardware validation gate
1. Back up `ux0:pspemu/PSP/SAVEDATA`, Adrenaline VPK, `ur0:tai/config.txt` and relevant PSP plugin configurations.
2. Confirm stock Adrenaline works on PSTV 3.65 and a test PSP title boots.
3. Use a separate test environment where possible; don't overwrite the only working installation before baseline verification.
4. With no RA account logged in, verify normal PSP launch, controller input, system menu, and safe exit.
5. Test Casual achievement handling, connection loss, and restart.
6. Test save-state load rejection, cheat plugin rejection, fast-forward prohibition, and attempts to enable Hardcore mid-session. Record **both** UI state and actual PSP-side behavior.
7. Verify PSP game physically resets when a new Hardcore session starts; `rc_client_reset` alone is not sufficient proof.
8. Keep server Hardcore submission disabled until RA client approval. Never bypass RA client identity validation.

## Known blockers
- No VitaSDK/PSP toolchain available in this execution environment.
- No hardware PSTV available for runtime validation.
- RA approval is external to this project.
- Existing prebuilt PSP PRX modules must be audited for source/binary parity before making enforcement claims.
