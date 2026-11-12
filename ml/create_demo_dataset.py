r"""Create 111 SYNTHETIC rows in the original collection CSV format.

Uses Python standard library only. Run from the project root:
    .venv\Scripts\python.exe ml\create_demo_dataset.py
This script never reads or changes the real collection dataset.
"""
import csv
import json
import math
import random
import statistics
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "outputs" / "ml-demo-20261108"
COLUMNS = [
    "collection_id", "mesh_id", "target_id", "environment_id", "side_length_m",
    "target_x_m", "target_y_m", "target_mac", "wifi_channel", "window_index",
    "recorded_at_utc",
    *[f"{name}_{node:02d}" for name in ("rssi_mean", "rssi_std", "packet_count")
      for node in range(1, 5)],
    "dataset_split",
]
# Same geometry/labels as UI/collection-positions.js, at side length 1 m.
D = .25 / math.sqrt(2)
TRAIN = [
    (.25,.25),(.75,.25),(.25,.75),(.75,.75),(.5,.5),
    (.5,0),(0,.5),(1,.5),(.5,1),
    (.25,-.25),(.5,-.25),(.75,-.25),
    (.25,1.25),(.5,1.25),(.75,1.25),
    (-.25,.25),(-.25,.5),(-.25,.75),
    (1.25,.25),(1.25,.5),(1.25,.75),
    (-D,-D),(1+D,-D),(-D,1+D),(1+D,1+D),
]
TEST = [
    (.25,0),(.75,0),(0,.25),(1,.25),
    (.5,.25),(.25,.5),(.75,.5),(.5,.75),
    (.125,-.25),(1.25,.125),(.875,1.25),(-.25,.875),
]
# Matches sniffer corner numbering in UI/node-map.js.
ANCHORS = [(0,0), (1,0), (0,1), (1,1)]


def main():
    rng = random.Random(20261108)
    rows = []
    base_time = datetime(2026, 11, 8, 0, 0, tzinfo=timezone.utc)
    for split, prefix, points in [("train", "T", TRAIN), ("test", "U", TEST)]:
        for position_index, (x, y) in enumerate(points, 1):
            collection_id = f"synthetic-1m-{prefix}{position_index:02d}"
            setup_bias = [rng.gauss(0, .8) for _ in ANCHORS]
            timestamp = (base_time + timedelta(minutes=2 * (len(rows)//3))).isoformat().replace("+00:00", "Z")
            for window in range(1, 4):
                row = dict(
                    collection_id=collection_id, mesh_id=1,
                    target_id=f"{prefix}{position_index:02d}", environment_id=1,
                    side_length_m=1, target_x_m=round(x, 6), target_y_m=round(y, 6),
                    target_mac="02:00:00:00:00:10", wifi_channel=6,
                    window_index=window, recorded_at_utc=timestamp, dataset_split=split,
                )
                for node, (ax, ay) in enumerate(ANCHORS, 1):
                    distance = max(math.hypot(x-ax, y-ay), .1)
                    mean = -45 - 22 * math.log10(distance) + setup_bias[node-1]
                    mean += rng.gauss(0, .6)
                    sigma = rng.uniform(1.5, 4.5)
                    count = rng.randint(60, 280)
                    packets = [min(-15, max(-100, round(rng.gauss(mean, sigma))))
                               for _ in range(count)]
                    row[f"rssi_mean_{node:02d}"] = round(statistics.mean(packets), 4)
                    row[f"rssi_std_{node:02d}"] = round(statistics.pstdev(packets), 4)
                    row[f"packet_count_{node:02d}"] = count
                rows.append(row)
    assert len(rows) == 111
    OUT.mkdir(parents=True, exist_ok=True)
    with (OUT / "synthetic_collection_1m.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
    metadata = {
        "synthetic": True, "seed": 20261108, "rows": len(rows),
        "train_rows": 75, "test_rows": 36, "captures": 37,
        "side_length_m": 1, "environment_id": 1,
        "environment": "Clear area (simulated)",
        "anchors_01_to_04_m": ANCHORS,
        "method": "log-distance RSSI: A=-45 dBm at 1m, n=2.2, distance floor=0.1m; simulated integer packet RSSI with Gaussian noise, setup and window bias",
        "rssi_std": "population standard deviation of simulated packets",
        "timestamps": "invented UTC capture timestamps, shared by three windows as in original exporter",
        "target_mac": "invented locally administered unicast MAC; not an observed target",
        "use": "Training/export demonstration only; cannot measure real localization accuracy",
    }
    (OUT / "synthetic_metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(f"Created {len(rows)} synthetic rows: 75 train + 36 test; {OUT}")


if __name__ == "__main__":
    main()
