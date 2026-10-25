# K to Treasure Room

Install this exact directory on **all 2–4 peers**, enable the Mod everywhere,
deploy the same Isaac Sync 1.0.4 build, and wait until Isaac Sync becomes `ACTIVE`.
Pressing `K` on either peer queues one deterministic operation. At its target
frame, both games validate the supplied room index and teleport every local
co-op player to the Treasure Room.

This Mod registers the manifest:

```text
syncId  = dev.isaacsync.k-to-treasure-room
version = 1
port    = derived from syncId
opcode  = 1
payload = Treasure Room SafeGridIndex
```

Every peer must enable the same set of Isaac.Sync Mods. `Register` returns the
derived port used by `Send`; the Mod never assigns a manual port.

If the floor has no Treasure Room, the key press is ignored. If the peers do
not have the same room layout, the receiving Mod logs a refusal instead of
teleporting to an unrelated room; repair the underlying session mismatch before
trying again.
