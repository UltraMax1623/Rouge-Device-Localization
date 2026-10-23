# Node-to-node RSSI measurement rounds

Use `root_node/root_node.ino` on the root and
`normal_node/normal_node.ino` on the other boards. Both include
`shared/node_firmware.h`. Only the root has a custom AP MAC,
`54:44:33:22:11:01`; normal boards retain their factory AP MACs.
Station MACs are `44:33:22:11:00:01` for the root and endings `02`, `03`,
`04` for normal boards. Set `BOARD_NUMBER` before each normal-board upload.
The root AP identity is shared in the firmware for coordinator recognition.

The root populates `macList` with stations 01 through `TEST_BOARD_COUNT`
(default 3: root 01 and normal nodes 02, 03). Type `conf` on root serial at
115200 baud with newline to start a round after the mesh is stable. Automatic
periodic rounds are disabled. Input during an active operation is rejected.

`sniff <12-hex-MAC>` first runs a 10-second distributed channel-hopping
round. All boards reconnect and report target observations by channel. The
root chooses the channel with the most observations, then redistributes it
in a fresh round after ACKs/mesh settling. All boards listen on that channel
for 20 seconds, reconnect, and report target RSSI/distance. The fixed-channel
capture repeats three times, with a new round ID and validated cycle number
for each cycle. Input stays locked between cycles. The root retains reports
in `sniffCycleReports`, then stores/prints `sniffAverageDistances` and
`sniffAverageCounts`. Only valid distance readings contribute to the mean;
missing/unheard readings do not count as zero. Readiness, round
IDs, job type, target, channel and capture-duration fields are validated.
If no target is heard, discovery stops with an error. An optional channel
argument skips discovery. See README.md for commands, limitations and output.

`runConfig()` starts a nonblocking round; its return value does not indicate
that measurements have finished. Keep calling `mesh.update()` while the
mesh is active and `runTimeSlicing()` from `loop()` as the sketch does.

1. The root broadcasts configuration until each table entry acknowledges it.
   Replies identify the station MAC and its one-based slot. The immutable
   round snapshot and its hash prevent reordering during measurement.
2. Once the mesh has settled and all ready replies have arrived, the root
   repeatedly announces a start time ten seconds ahead in mesh time.
3. Nodes convert that deadline to their local monotonic clock three seconds
   before the start, stop the mesh, and prepare disconnected station Wi-Fi.
   Late preparation fails the round locally rather than shifting slots.
4. Each four-second slot has a 500 ms guard, three seconds of sampling, and
   another 500 ms guard. The listed station sends ESP-NOW broadcast frames
   at up to 10 packets/second; all others listen in promiscuous mode. The
   channel is fixed at 1 and requested transmit power is 15 dBm.
5. Nodes disable capture, restore their station identities (and the root's custom AP identity), and restart the mesh.
   They retry their report once per second after the root becomes reachable,
   until acknowledged or a 120-second report timeout expires.

ESP-NOW payloads are compact binary measurement probes, not the inter-mesh
JSON protocol. Capture filters their marker, round, ordered-table hash, slot,
transmitter MAC, sequence number, and active sampling window. Callback work
is bounded and shared sample counters are protected by a critical section.

Results remain available locally as `slotRSSI` and `slotPacketCounts`:

```json
{"1": -61.5, "2": null, "3": -70.2, "4": -65.8}
```

JSON key `"1"` means `macList[0]`. The board's own slot has `null` and zero
samples. An unheard station also has `null` and zero samples; distinguish it
using the board's own slot/MAC. A zero-sample slot is never converted to a
distance of zero. At the root, `roundReports["2"]` is the report **from** the
station in slot 2; its nested `RSSI` object contains that station's readings
**of** every other slot. Missing reports are absent, not fabricated.

Each result message also contains `ReporterMAC`, `A`, `n`, and `Distances`.
`Distances` maps each other station MAC to estimated metres, excluding the
reporter's own MAC (three entries for four boards). The path-loss formula
is in `shared/rssi_distance.h`; global calibration variables `A` and `n`
are defined in `shared/node_firmware.h`. Each board
uses its calibration snapshot from configuration for that round's report.
Unheard stations remain `null`. Reports retain the existing retries and ACKs.
The root stores all distance maps, including its own, in the global
`roundDistanceReports[reporterMAC][targetMAC]`; `distanceReportRoundId`
identifies the current table. These are reset at each new configuration.
The root prints one formatted table per configured reporter at collection completion
or timeout, with full space-separated reporter MACs and target suffixes. `macDistances` holds each board's own map.
Re-upload all boards together because distance fields are now required in
the validated result messages.

The mesh JSON adds `Action`, `RoundId`, `ConfigHash`, `Channel`, `SlotMs`,
`GuardMs`, `StartTimeUs`, `ReporterSlot`, `StationMAC`, `Samples`, and status
fields as needed. Modes remain `conf`, `mesh`, and `sniff`; target sniffing uses the same ready/start/report transport with explicit
`Job`, `TargetMAC`, `DiscoverChannel`, `SniffChannel`, and `SniffMs` fields. Every mesh message includes
`RootDirection` and `Sender ID/MAC`.

Configuration retries are idempotent. A node that misses all start messages
remains in the mesh and produces no measurement report. Root readiness times
out after 30 seconds. A cancelled/failed round is logged; this protocol does
not promise atomic delivery of control messages during a radio/network fault.
Clock synchronization is supplied by painlessMesh; three seconds without a
large clock adjustment/topology change is a settling heuristic, not proof of
a bounded synchronization error. Guards accommodate modest skew. Validate
timing on real boards, particularly for long tables or changing topology.

The custom-MAC setup targets the installed Arduino ESP32 core 3.3.12 and
painlessMesh 1.5.7. `MeasurementMesh` releases the stopped mesh's protected
mutex because that library version allocates a new one on every `init()`.
Recheck that adapter if upgrading painlessMesh. No library files are modified.

## Hardware checks

Flash the root and uniquely numbered normal sketches, keep boards close
initially, wait for the mesh to settle, and type `conf` on the root.
Verify one ready reply per station, non-overlapping transmit slots, three
seconds of samples per heard station, and root report acknowledgements.
Run another round to check reconnection and memory behavior. Power a listed
node off before configuration to verify readiness timeout; place a node out
of direct radio range to verify a null reading rather than a fabricated RSSI.
This code has build/host checks; radio timing and reception require hardware.
