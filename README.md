# Rogue Device Localization

## Upload the root and normal nodes

Install the ESP32 Arduino board package and painlessMesh with its dependencies.
The current code targets ESP32 core 3.3.12 and painlessMesh 1.5.7. Select your
actual board and port. The root is a normal ESP32 and always station 01: select its actual
ESP32 board model for the root upload. ESP32-S3 is also supported when selected. Use Serial Monitor at 115200 baud. ESP32-S3 native USB
may require USB CDC On Boot enabled.

Keep the repository folders together: both upload sketches include firmware
from `shared/`, which uses the protocol headers in `root_node/`.

| Board | Sketch | BOARD_NUMBER | Station MAC | AP MAC |
| --- | --- | --- | --- | --- |
| Root | `root_node/root_node.ino` | `0x01` | `44:33:22:11:00:01` | `54:44:33:22:11:01` |
| Normal 2 | `normal_node/normal_node.ino` | `0x02` | `44:33:22:11:00:02` | Factory AP MAC |
| Normal 3 | `normal_node/normal_node.ino` | `0x03` | `44:33:22:11:00:03` | Factory AP MAC |
| Optional normal 4 (set count to 4) | `normal_node/normal_node.ino` | `0x04` | `44:33:22:11:00:04` | Factory AP MAC |

Upload the root sketch once. For each normal board, change `BOARD_NUMBER` in
its sketch before uploading. Never give two powered boards the same station
MAC. Normal boards retain their factory AP identity; painlessMesh uses both
AP and station interfaces. Only the root receives a custom AP address.
The approved `54` prefix is unicast; the originally requested `55` prefix is
multicast and unsuitable for an interface MAC.

## Commands on the root

The current setup is three total boards: root 01 and normal nodes 02, 03.
`TEST_BOARD_COUNT` controls the root's station table. No round starts
on a timer anymore. Open the root Serial Monitor at **115200 baud** with
**Newline** or **Both NL & CR**, wait for all peers and three seconds of mesh
stability, then enter:

```text
conf
sniff 443322110010
```

`conf` runs the existing board-to-board measurement round. The root builds
and distributes the station table, waits for each ready reply, schedules a
shared start, runs the four-second slots, reconnects, and collects reports.
Three boards take 12 seconds of measurement, plus control/reconnection time.

`sniff <12-hex-MAC>` runs this two-stage process automatically:

1. The root distributes the target and a channel-discovery round. After the
   ready/start handshake, every board stops mesh communication and hops its
   locally permitted 2.4 GHz channels for **10 seconds**, dwelling 200 ms per
   channel. Country settings determine the supported scan range (up to 13).
2. All boards reconnect to the mesh and send target packet counts per channel.
   The root selects the channel with the most target observations. A silent
   or unheard target produces an error and stops the operation.
3. After at least three seconds for ACK delivery and mesh settling, the root
   distributes that detected channel in a fresh, validated round. Every board
   acknowledges it before the synchronized start.
4. Every board stops the mesh and listens for the target on that one channel
   for **20 seconds**, then reconnects and sends averaged RSSI, sample count,
   model parameters, and estimated distance to the root.
5. Steps 3–4 repeat until **three fixed-channel capture cycles** finish,
   using the detected channel for each cycle. Boards reconnect/report between
   cycles. Input stays locked throughout the sequence. The root prints cycle
   numbers and report counts, then one **average-distance table per board**.
   It takes the arithmetic mean of valid cycle distances and prints the
   contributing count (for example `2/3`). Failed captures and null/no-report
   values are excluded;
   no valid cycles means `unavailable`.

Discovery and every capture cycle retain the 10-second synchronized start
lead and three-second radio preparation. There are **60 seconds of target
capture**, plus the initial 10-second discovery, start leads, ACK/settling
intervals and mesh reconnection/report collection. Keep the phone generating
Wi-Fi traffic until the final average appears. Input is never unlocked between
cycles. A control/network failure may stop the sequence with an error. Radio observations
infer the target channel; a moving/hopping target may change channels before
capture. A passive scan cannot find a target that never transmits during it.
Use the actual over-the-air transmitter MAC, including any randomized MAC.
Use an external, actively transmitting target: participating sensor boards
stop their mesh transmissions during discovery/capture, so sniffing one of
their own station/AP MACs normally produces no samples.
ESP32 capture is 2.4 GHz; 5/6 GHz traffic cannot be measured.

An optional known-channel shortcut is `sniff <12-hex-MAC> <channel 1..13>`.
It skips discovery and is subject to each board's Wi-Fi country limits.
Examples use real MAC-shaped values; substitute the actual target address.

All other user input is rejected with `[ERROR] Round running: user input
disabled until completion` while configuration, discovery, capture,
reconnection, result collection, or channel redistribution is active. Busy
commands are discarded, including partial lines spanning the end of a round.
Malformed commands/overlong lines produce errors. Normal-board serial input
cannot start rounds; enter commands on root 01 only. A rejected command must
be entered again; it is never queued.

Ready replies retry once per second with a 30-second timeout; reports retry
until acknowledged or 120 seconds elapse. Root logs identify missing ready
stations and report timeouts. Failed mesh starts retry every five seconds
outside radio capture, resuming preserved result exchange after recovery.
No router or internet connection is required.

## Built-in LED mode indicator

| State | RGB color (S3 / RGB boards) | LED pattern |
| --- | --- | --- |
| Mesh / reconnecting / reporting, without a broadcast | Green | Steady on |
| Mesh application broadcast queued by this board | Cyan | Fast blink: 60 ms on/off for 600 ms after each send |
| Configuration / waiting / scheduled start | Yellow | Slow blink: 500 ms on, 500 ms off |
| Channel discovery (`scan`) | Purple | 200 ms on, 200 ms off |
| Measurement radio active (`sniff`) | Blue | Two short flashes every 2 seconds |
| Mesh unavailable outside measurement, or failed controller | Red | Three short flashes every 2 seconds |

Broadcast activity is local to the sender; receiving a broadcast leaves the
mesh LED steady. Configuration, discovery and measurement patterns take precedence
over broadcast activity. The activity burst indicates an accepted/queued
application broadcast, not confirmed radio delivery. Internal painlessMesh
beacons, topology traffic, relaying, and direct reports/ACKs do not trigger it.
The round protocol sends preparation/start controls over the mesh and
sends results directly. The operation-specific LED pattern takes precedence
during controls; reporting uses steady mesh LEDs; a mesh-mode application
`mesh.sendBroadcast(...)` activates the fast burst. No extra traffic is added.

`shared/mode_indicator.h` uses the selected board's `RGB_BUILTIN` for a
built-in RGB LED, on S3/RGB boards. The normal ESP32 root uses its monochrome LED.
If your S3 LED data pin differs from
the board variant, define `MODE_RGB_PIN` (for example `38`) above the shared
firmware include in the root sketch. The standard S3 variant defaults to
GPIO 48; use the actual board's pin. Color changes take effect even when the
LED stays on across a mode change. Brightness is limited to 16/255.

Non-RGB boards use `LED_BUILTIN`, falling back to GPIO 2, with the same blink
patterns. Define `MODE_LED_PIN` and/or `MODE_LED_ACTIVE_LOW 1` for another
monochrome LED or active-low wiring. The indicator is nonblocking.
The measurement pattern covers radio preparation and the capture window;
it does not indicate capture of the target rogue device.

## Results and calibration

After each measurement round, every normal node sends its MAC-distance
readings in the existing acknowledged result report. With four boards,
`Distances` contains three target station MAC keys, excluding the reporter.
Distances are estimated in metres from averaged RSSI using the same formula
as `Localization/Localization.ino`:

```cpp
pow(10.0, (A - rssi) / (10.0 * n))
```

Edit the global `A = -60.0` and `n = 2.0` in `shared/node_firmware.h` to
calibrate the model. Each board snapshots its globals when a round is
configured; changing them mid-round does not change that report. Use finite
values and `n > 0`. Reports include the sender's `A` and `n`.

The root stores its own readings and incoming node readings in these globals:

```cpp
// Reporter station MAC -> target station MAC -> metres (or null).
roundDistanceReports["44:33:22:11:00:02"]["44:33:22:11:00:01"];
// Latest cycle's full target-capture reports, keyed by reporting station MAC.
sniffReports;
// Three cycles retained separately: cycle string -> reporter MAC -> full report.
sniffCycleReports;
// Final arithmetic mean: reporter MAC -> target MAC -> mean metres or null.
sniffAverageDistances;
// How many valid cycles contributed to each reporter's mean.
sniffAverageCounts;
// Full channel-discovery reports, retained across the fixed-channel stage.
channelDiscoveryReports;
// Last channel selected by root discovery (0 if no channel found).
true_channel;
// Round identifier associated with the table.
distanceReportRoundId;
// This board's own target-MAC -> metres readings.
macDistances;
```

These tables are cleared for a new round and filled as results become
available. Missing node reports remain absent; unheard targets have `null`
distance and print `unavailable`, never zero metres. The root validates
the sender identity, target keys, and distance consistency before accepting
a report. When collection completes or times out, it prints one formatted
table per configured board in station order, including itself. Headers use the full reporter MAC
with spaces; targets use the final byte (01, 02, 03, 04). Missing reports are
labelled `NO REPORT`. For example (illustrative distances):

```text
44 33 22 11 00 02 measures
+--------+----------------+
| Target | Distance (m)   |
+--------+----------------+
| 01     |         1.1885 |
| 03     |         2.5000 |
| 04     |    unavailable |
+--------+----------------+
```

The current three-board setup prints three tables with two target rows each.
The four-board example above illustrates the optional larger setup. The root
is always 01.
The original RSSI/count reports remain in `roundReports`. Re-upload both
sketches so all boards use the updated report format.

See `root_node/TIME_SLICING.md` for the measurement protocol and hardware checks.
`Localization/Localization.ino` and its transmitter subfolder are separate,
older standalone RSSI/distance experiments. Position calculation and an extended PC control interface remain unfinished.
Target sniffing and the two root serial commands are implemented.

## Before the first hardware test

- Upload root station 01 and normal stations 02 and 03 with unique numbers.
  Keep `TEST_BOARD_COUNT` equal to the number of powered test boards (currently 3).
- Select the actual board and port for every upload; select the normal ESP32 board model for the
  current root. Use USB CDC On Boot if required for its native USB serial output,
  open the monitor at 115200 baud with Newline, and reset to see startup messages.
- Keep all boards near one another initially, powered by stable USB supplies.
  An indirect mesh route does not guarantee a direct RSSI measurement; an
  unheard transmitter is shown as unavailable.
- Verify printed station/AP identities, then enter `conf` and look for one `Ready:`
  line for each normal station, then `All stations ready; start in 10000 ms`.
- Expect the mesh to disconnect during measurement and reconnect afterward.
  Check all three result tables and run several rounds to test restart behavior.
- RGB uses the selected S3 variant's data pin; override `MODE_RGB_PIN` if your
  hardware uses a different pin. A power indicator LED is not software driven.
- RSSI distances require calibration of `A` and `n`; matching boards and
  antenna placement improve comparison. This is an estimate, not a precise
  geometric distance measurement.

Start-control delivery and clock alignment still need real-board validation.
The protocol retries control messages and uses timing guards, but does not
provide an atomic guarantee that every board starts during a network fault.
A permanently unreachable report can hold collection for up to 120 seconds;
the root then prints the partial tables. Board-level radio timing, USB/RGB
wiring, power stability, and repeated reconnection cannot be proven by host
checks or firmware compilation alone.
