# Architecture

NXSync separates always-resident observation from operations that allocate more
memory, mount saves, create archives, or use the network.

## Processes

### NRO

`NXSync.nro` is the interactive application. It owns the graphical catalog,
configuration wizard, manual backup, cloud browser, restore flow, diagnostics and
settings.

### Resident observer — `4200000000004E58`

The resident sysmodule watches application lifecycle events through libnx services.
It records launches and exits, creates small queue records, and starts the transient
worker. It must not create ZIP files or initialize libcurl.

### Transient worker — `4200000000004E59`

The worker is launched for one operation and exits afterwards. It performs:

- local save discovery and hashing;
- ZIP creation and verification;
- Nextcloud uploads and global-index publication;
- retention cleanup;
- launch preflight and candidate discovery;
- cloud restore selected through the overlay.

### Ultrahand overlay

The overlay reads the same versioned status files as the NRO. It displays operation
state and lets the user choose a local or cloud revision when preflight cannot make
a safe automatic decision.

### Installer NRO

`NXSyncInstaller.nro` is an offline installer and remover. It identifies an exact
Atmosphère build by hashing both `package3` and `stratosphere.romfs`, selects the
only matching `dmnt` override, and refuses unknown combinations. It also hashes any
existing override. It uses staged writes, post-copy SHA-256 checks and rollback
renames for NXSync-owned files, but never rewrites the active `package3` or ROMFS.

### Atmosphère `dmnt` launch gate

The version-specific patch observes application creation early enough to publish a
launch request before the game has opened its save. A temporary debug attachment
holds the initial thread while the process is started, allowing HOME to terminate
it during preflight. A process left in the kernel's Created state cannot be
terminated. The hold is released on approval or transferred to the cheat manager;
an abort terminates the process before detaching.

The initial check has a 30-second limit. A session-bound `launch.phase` record
gives the user up to two minutes to choose and a confirmed download up to five
minutes, measured from entry into each phase. Repeated, stale, malformed or late
phase records cannot restart an expired timer. Before a restore grant, expiry
continues with the local save. Protocol v2
requires the worker to request a restore lease after download/inspection. The gate
persists a recovery guard before granting the lease. Without a grant the worker
must not modify save data, even if it receives a stale action after timeout.

After a grant, timeout and cancellation never release the game. Successful restore
or confirmed rollback clears the guard; the observer forwards the worker's `allow`
or `abort` decision only after worker exit. At 30 minutes after the grant the gate aborts the game,
leaving the marker for recovery. A crash also leaves the marker. Other titles may
run without another preflight, allowing the NRO to perform recovery. See
[RECOVERY.md](RECOVERY.md).

This is a patch to an internal Atmosphère component, not a stable plugin API. It
must be rebuilt for every supported Atmosphère revision. The official Atmosphère
ROMFS contains `dmnt`; NXSync loads the matching replacement through the standard
`atmosphere/contents/010000000000000D/exefs.nsp` content override. `package3` and
`stratosphere.romfs` are identity inputs and are not modified by NXSync.

## Game launch flow

```text
HOME launch request
        |
        v
patched dmnt publishes launch.request atomically
        |
        v
observer starts transient worker
        |
        v
worker compares local anchor and all cloud heads
        |
        +-- synchronized/local current --> continue
        +-- cloud fast-forward ----------> ask/restore cloud revision
        +-- divergent histories ---------> overlay conflict choice
        +-- pre-write network/timeout ----> continue locally (no restore grant)
        |
        v
dmnt receives launch.decision: allow only after safe completion; abort otherwise
```

## Game exit flow

```text
application exit event
        |
        v
observer queues a backup request
        |
        v
transient worker mounts the closed save
        |
        +-- unchanged --> update status, no new ZIP
        +-- changed ----> create and verify ZIP
                              |
                              v
                     upload archive and metadata
                              |
                              v
                       apply count retention
```

## Atomic protocol files

Coordination files are written to a temporary or pending pathname, closed,
reopened for exact verification, and then published with a retrying rename. Readers
must reject incomplete records and unsupported protocol versions.

Important paths below `sdmc:/config/NXSync/` include:

```text
launch.request
launch.decision
launch.action
launch-publish.status
preflight.request
preflight.status
cloud-worker.status
sysmodule.status
queue/
```

Runtime files can contain device, profile, title and cloud information. They are
diagnostic data and must not be committed to the repository.

## Automation scope

The default `automation_scope` is `emummc`. In sysMMC the observer and launch gate
must remain passive. Environment detection failure is also treated as disabled,
not as permission to run automation.
