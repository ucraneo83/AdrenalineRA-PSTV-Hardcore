#!/usr/bin/env python3
"""Static guardrail checks for AdrenalineRA 1.0.1. Not a certification or runtime test."""
from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[1]
checks=[
 ("Runtime refuses live Hardcore promotion", "app/runtime/ra_runtime_platform.c", ["else if (!ready)", "hardcore enable ignored while a session is live"]),
 ("Unsupported platform disables client Hardcore", "app/runtime/ra_runtime_platform.c", ["ra_runtime_downgrade_platform", "rc_client_set_hardcore_enabled(rc, 0)"]),
 ("RESET event calls client reset (physical game reset NOT established)", "app/runtime/ra_runtime_events.c", ["events_handle_reset", "rc_client_reset(rc)"]),
 ("Save-state menu reflects local Hardcore policy only", "app/menu/ra_menu_settings.c", ["ra_menu_hardcore_active", "ra_settings_hardcore_effective"]),
 ("Upstream eligibility disclaimer preserved", "README.md", ["Until it is complete, Hardcore mode and", "downgrades"]),
]
failed=0
for label,filename,needles in checks:
    p=ROOT/filename
    data=p.read_text(encoding="utf-8",errors="replace") if p.exists() else ""
    ok=all(n in data for n in needles)
    print(("PASS" if ok else "FAIL")+": "+label+" ["+filename+"]")
    failed+=not ok
print("NOTE: Passing checks prove only text presence, NOT enforcement or RA approval.")
sys.exit(1 if failed else 0)
