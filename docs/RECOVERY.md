# Recovery after an interrupted launch restore

Before granting a launch restore, dmnt persists
`sdmc:/config/NXSync/launch.restore-guard`. It records the launch sequence, title ID
and target profile UID. The worker clears it only after success or confirmed
rollback. The marker is deliberately retained across reboots and when automation
is disabled. Do not delete it to bypass an incomplete restore.
Backups, queued uploads and automatic retention for the affected title also pause
until recovery succeeds, preserving the safety archive and pending operations.

If the game cannot launch after a failed restore:

1. Let the cloud worker finish, or reboot after a confirmed crash. Preserve the
   entire `switch/NXSync/backups` directory and `config/NXSync` directory first.
2. Open NXSync. If full-memory mode is needed, use homebrew override with a
   **different title**. An unresolved marker blocks the affected title, while
   other titles run without offering another automatic restore.
3. In Restore, select a verified safety backup (or another known-good archive)
   for the title and the exact profile UID recorded in the marker. Safety archives
   live in the normal local backup tree; the worker error also reports their path.
4. Complete the explicit restore and check the result. The NRO clears the marker
   only when that restore succeeds for the matching title and profile. A restore
   to another profile does not clear it.

A malformed marker is not silently discarded. Preserve it and the backups for
diagnosis. Do not attach either to a public issue without removing personal data.

The guard protects the automatic launch restore path; it is not a general
transaction journal for manual save editing. SD hardware failure can also prevent
durable writes. Keep an independent backup and complete the hardware fault tests
before treating this release candidate as stable.
