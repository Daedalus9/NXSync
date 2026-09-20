# Cloud format and revision lineage

New archives use `profile-<32 uppercase hexadecimal UID>` for the profile folder,
on the SD card and in the cloud. The nickname remains display metadata. Profiles
with equal nicknames therefore have separate retention namespaces. Legacy
nickname-based directories remain readable but are never automatically pruned;
no destructive migration is performed by this release.

NXSync separates recoverable archive blobs from small immutable revision records.

## Archive layout

```text
/NXSync/<device-id>/<profile>/<title-id>/<timestamp>_<archive-hash>.zip
```

Each v2 archive contains save files below `save/` and a root
`nxsync-metadata.json`. The manifest identifies the game, source profile, device,
payload hash, archive contents, revision and parent revisions.

The canonical `payload_sha256` describes the uncompressed logical save payload. It
is used to recognize equal content even when two ZIP archives have different binary
hashes.

## Global index

```text
/NXSync/_index/
├── titles/<title-id>/<device-and-profile>.json
└── revisions/<title-id>/<revision-id>.json
```

- A title head is mutable and points to the current revision of one device/profile
  branch.
- A revision record is immutable and contains its parent revision IDs, payload hash,
  provenance and archive location.
- Cloud discovery reads every head for the selected title and walks revision parents
  as necessary. A cached response must never replace a forced validation used for a
  launch decision.

## Conflict detection

Suppose both consoles last synchronized revision `41`:

```text
      41
     /  \
  42A    42B
 OLED   Lite
```

If the payloads differ and neither revision is an ancestor of the other, the result
is a conflict. A timestamp alone cannot resolve it safely.

If a console still has `42` and the cloud head is the descendant `43`, the result is
`cloud-update-available`, not a conflict.

## Logical resolution, not file merge

NXSync does not understand proprietary game save semantics and does not merge files
or in-game progress. When a user selects one side of a conflict, that payload wins.
The next modified backup records both competing revisions as parents:

```json
{
  "parent_revision_ids": [
    "selected-revision",
    "discarded-conflicting-revision"
  ]
}
```

This joins the genealogy and prevents the same resolved conflict from being shown
again. It does not combine achievements, inventory, story progress, or other data
from the discarded payload.

## Download and restore identity

Downloads must retain their generated hash-bearing filename. The downloaded hash
must match that filename; any server checksum must agree as well. Before an
automatic restore, the inspected revision ID and payload hash must both match the
revision selected during preflight. A mismatch leaves the destination untouched.

## Retention

Retention is based only on the user-selected count and defaults to five recoverable
archives per console/profile/title branch. It is not time-based.

The active indexed archive and unresolved conflict revisions are protected while a
cloud transaction is incomplete. Removing an old archive does not require removing
its tiny revision record; keeping metadata preserves ancestry and conflict
detection. Equal payload hashes do not require duplicate uploads.
