# NXSync Overlay

Experimental fail-open overlay that displays NXSync status and resolves launch
preflight checks.

- reads `sdmc:/config/NXSync/overlay.catalog`, exported by the homebrew app;
- reads sysmodule and worker status;
- queues manual checks through `preflight.request`;
- during the automatic launch gate, writes `launch.action` to continue with the
  local save or ask the worker to restore a cloud revision;
- publishes every request atomically and associates it with the current launch
  sequence;
- does not mount save data or perform restores directly;
- contains no HTTP client: transfers remain in the transient worker.

Like every Tesla/Ultrahand overlay, the framework uses the normal `pm:dmnt` PID
queries to assign controller focus correctly. NXSync does not use debugging,
suspension, resume, or application-launch interception APIs from the overlay.

Install the compiled file at `sdmc:/switch/.overlays/NXSync.ovl`. A compatible
overlay loader/menu is required for the installed Atmosphère and HOS versions.

The vendored libultrahand copy is release `v2.5.3`, commit
`856ddbddd796fc4a59ad2e0bf939c5963e6f9dd2`.
