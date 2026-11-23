# Phase 3 — Verified source observations and release gates

Target: PSTV 3.65; source snapshot: AdrenalineRA 1.0.1 (`9e7bd5c6e99832c5538b8f39e751390492ff3257`).

## Confirmed by source inspection

- `app/runtime/ra_runtime_platform.c`: `client_apply_hardcore` rejects enabling Hardcore when `ready` is true; disabling is allowed.
- `app/runtime/ra_runtime_platform.c`: `ra_runtime_downgrade_platform` clears the saved Hardcore preference, republishes the gate, and disables Hardcore in `rc_client`.
- `app/runtime/ra_runtime_events.c`: `events_handle_reset` calls `rc_client_reset(rc)` only. This does not prove a PSP game reset occurred.
- `app/menu/ra_menu_settings.c`: the UI's Hardcore-active value is based on local settings and sign-in, not confirmed server eligibility.
- `README.md`: explicitly says RetroAchievements approval is pending, Hardcore/leaderboards are unavailable, and hardware testing was on Vita 3.60 only.

## Security recommendation (not implemented yet)

Implement a single authoritative session policy with distinct fields for requested mode, local enforcement, runtime mode, and server eligibility. Never use a locally enabled flag as proof of official Hardcore status. Before changing enforcement code, map the full game-reset lifecycle and the server policy API; a premature change can accidentally weaken existing safeguards.

## Build gates

1. Static guardrail script passes (only checks known code patterns).
2. Compile VPK and matching PSP PRX modules using pinned SDK/dependencies.
3. Perform PSTV 3.65 smoke test and rollback test.
4. Complete bypass tests for loaded plugins, alternate state-loading paths, resets, and suspend/resume.
5. Obtain official RA client approval before advertising Hardcore leaderboard eligibility.

No VPK has been compiled, and no security patch has been applied. This is a static-audit automation deliverable.
