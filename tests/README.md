# Terminator manual test scripts

These are command fixtures for the Terminator interpreter, not standalone Bash tests. They do not produce pass/fail exit codes. Launch Terminator from the project root so relative paths resolve, then run a fixture with `source tests/<name>.sh` (or `.` in place of `source`). Review its output and follow the interactive steps.

## Test cases

- `test_commands.sh`: basic commands, working-directory changes, and history.
- `test_pipes.sh`: two and three-stage pipelines.
- `test_redirection.sh`: overwrite, append, and standard-error redirection. Writes temporary files under `/tmp`.
- `test_jobs.sh`: starts a background job. Run `jobs`, then try `fg` and Ctrl+Z; run `bg` and `jobs` again.
- `test_multithreading.sh`: basic fixture for default mode. For a multi-tab concurrency check, open a second tab, run long commands in each tab, and enter commands in both while they run. Run `history` in each tab to check shared history.
- `test_multiwatch.sh`: starts two watcher commands. Press Ctrl+C to stop it.

Repeat the multi-tab check in both modes:

```sh
./terminator
./terminator useProcess=True
```

These are manual feature checks, not automated coverage or a guarantee beyond the documented feature set.