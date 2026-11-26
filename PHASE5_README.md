# Phase 5 — experimental GitHub Actions cloud build

This is a **build attempt**, not a compiled or tested VPK. The container image and its SDK package contents have not been validated in GitHub Actions. The job will fail visibly if the SDK or Ninja is missing. It does not disable RetroAchievements eligibility safeguards.

## Without a development computer
1. Create an empty GitHub repository using the GitHub website.
2. Upload the **contents** of this source directory, preserving `.github/workflows/cloud-build.yml`. GitHub's browser upload may hide dotfiles or limit bulk uploads; GitHub Codespaces or a desktop Git client may be needed for the initial import.
3. Open Actions → PSTV experimental VPK build → Run workflow.
4. Check the build log. If successful, download `pstv-experimental-vpk` from the workflow run's Artifacts section.
5. Do not install on PSTV until the module matching and rollback tests are complete.

## Known limitations
- No remote build has been run, and the VitaSDK container tag is unverified here.
- Precompiled `systemctrl.prx` and `pentazemin.prx` have not been rebuilt or verified against source.
- Static checks inspect text patterns, not behavior.
- Hardcore approval remains pending; never force a server-approved Hardcore state.
- The upstream README says 3.60 was tested; PSTV 3.65 has not been hardware tested.
