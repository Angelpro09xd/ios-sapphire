# Working on ios-sapphire

- Read `README.md` and the newest entries of `NOTES.md` first: hardware facts, what works,
  boot args, the current blocker and the next concrete action.
- After each milestone and before stopping, add a dated entry to `NOTES.md`: commits, kernel
  build IDs (Codemagic or GitHub Actions), exact boot args and results, unfinished work.
  Distinguish observations from guesses.
- Commit completed changes. Kernels build on Codemagic (`codemagic.yaml`) or GitHub Actions
  (`.github/workflows/xnu.yml`).
- Device bring-up uses temporary `fastboot boot` images; never flash iOS into `boot` (it takes
  the recovery away, see `RECOVERY.md`). The iOS root lives in `userdata`; keep bootloaders,
  recovery, modem, persist, EFS (`modemst1/2`, `fsg`, `fsc`) and backups intact, and confirm
  each destructive step with the user before running it.
- Keep device dumps, serial numbers, boot logs and downloaded Apple artifacts out of git.
