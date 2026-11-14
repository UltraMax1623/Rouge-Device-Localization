r"""Train Random Forest coordinate regressors after collecting real RSSI data.

Run from the project root:
    .venv\Scripts\python.exe ml\train_real.py
Uses the held-out collection split for the final evaluation.
"""
import argparse
import csv
import hashlib
import json
from importlib.metadata import version
from pathlib import Path

import joblib
import numpy as np

from train_demo import COLUMNS, FEATURES, ROOT, SEED, TARGETS
from train_demo import arrays, errors_m, predict, select_models, summarize

DEFAULT_CSV = ROOT / "data/collection.csv"
DEFAULT_OUT = ROOT / "ml/artifacts/real"
MIN_PACKETS = 5


def read_real(path):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != COLUMNS:
            raise ValueError("Use the 24-column CSV exported by Data Collection.")
        rows = list(reader)
    captures = {}
    for line, row in enumerate(rows, 2):
        try:
            if None in row or any(not row[name] for name in COLUMNS):
                raise ValueError("Incomplete row.")
            if row["collection_id"].lower().startswith("synthetic-"):
                raise ValueError("Synthetic collections are excluded.")
            if row["dataset_split"] not in {"train", "test"}:
                raise ValueError("Invalid dataset_split.")
            if not np.isfinite([float(row[name]) for name in FEATURES + TARGETS]).all():
                raise ValueError("Non-finite feature or coordinate.")
            if float(row["side_length_m"]) not in {1, 2, 3, 4, 5} or not 1 <= int(row["wifi_channel"]) <= 13:
                raise ValueError("Invalid square size or Wi-Fi channel.")
            for board in range(1, 5):
                suffix = f"{board:02d}"
                mean, spread = (float(row[f"rssi_{key}_{suffix}"]) for key in ("mean", "std"))
                count = int(row[f"packet_count_{suffix}"])
                if not -127 <= mean <= 0 or not 0 <= spread <= 127 or not MIN_PACKETS <= count <= 10_000_000:
                    raise ValueError(f"Invalid receiver {suffix} statistics.")
            int(row["window_index"])
        except (ValueError, TypeError) as error:
            raise ValueError(f"CSV line {line}: {error}") from error
        captures.setdefault(row["collection_id"], []).append(row)

    group_coordinates, coordinate_groups = {}, {}
    for capture_id, capture in captures.items():
        if sorted(int(row["window_index"]) for row in capture) != [1, 2, 3]:
            raise ValueError(f"Collection {capture_id} must contain exactly windows 1, 2 and 3.")
        for key in ["mesh_id", "environment_id", "side_length_m", "target_id",
                    "target_mac", "wifi_channel", "dataset_split", *TARGETS]:
            if len({row[key] for row in capture}) != 1:
                raise ValueError(f"Collection {capture_id} has inconsistent {key}.")
        row = capture[0]
        group = row["target_id"]
        placement = tuple(round(float(row[key]) / float(row["side_length_m"]), 5) for key in TARGETS)
        if group_coordinates.setdefault(group, placement) != placement:
            raise ValueError(f"Target {group} has inconsistent normalized coordinates.")
        if coordinate_groups.setdefault(placement, group) != group:
            raise ValueError("The same normalized placement has multiple target IDs.")

    train = [row for row in rows if row["dataset_split"] == "train"]
    test = [row for row in rows if row["dataset_split"] == "test"]
    if not train or not test:
        raise ValueError("Collect and approve both training and unseen test placements first.")
    if {row["target_id"] for row in train} & {row["target_id"] for row in test}:
        raise ValueError("Train/test leakage: overlapping target IDs.")
    if {row["side_length_m"] for row in test} - {row["side_length_m"] for row in train}:
        raise ValueError("Each test square size needs training placements at the same size.")
    return train, test


def baseline_parameters(rows, journal_path, fallback):
    captures = {}
    if journal_path.exists():
        journal = json.loads(journal_path.read_text(encoding="utf-8"))
        approved = journal["approved"]
        if journal.get("storageVersion") == 2:
            approved = []
            for index, capture_id in enumerate(journal["approved"]):
                capture = json.loads((journal_path.parent / "captures" / f"{index}.json").read_text(encoding="utf-8"))
                if capture["id"] != capture_id:
                    raise ValueError("Capture storage identity mismatch.")
                approved.append(capture)
        captures = {capture["id"]: capture for capture in approved}
    parameters = []
    for row in rows:
        capture = captures.get(row["collection_id"], {})
        window = capture.get("windows", {}).get(row["window_index"], {})
        receivers = {receiver["board"]: receiver for receiver in window.get("receivers", [])}
        pairs = []
        for board in range(1, 5):
            receiver = receivers.get(board, {})
            a, n = receiver.get("A"), receiver.get("n")
            if a is None or n is None:
                a, n = window.get("A"), window.get("n")
            if (a is None or n is None) and fallback:
                a, n = fallback
            if a is None or n is None:
                raise ValueError("Baseline needs the capture journal or explicit --baseline-a and --baseline-n values.")
            if not np.isfinite([a, n]).all() or not -127 <= a <= 0 or n <= 0:
                raise ValueError("Invalid reference RSSI or path-loss exponent in baseline parameters.")
            pairs.append((a, n))
        parameters.append(pairs)
    return np.asarray(parameters, dtype=np.float64)


def baseline_predict(features, parameters):
    a, n = parameters[:, :, 0], parameters[:, :, 1]
    squared = np.power(10.0, (a - features[:, :4]) / (5 * n))
    d1, d2, d3, d4 = squared.T
    side = features[:, -1].astype(np.float64)
    return np.column_stack(((d1 - d2 + d3 - d4 + 2 * side**2) / (4 * side),
                            (d1 - d3 + d2 - d4 + 2 * side**2) / (4 * side)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, default=DEFAULT_CSV)
    parser.add_argument("--journal", type=Path)
    parser.add_argument("--baseline-a", type=float)
    parser.add_argument("--baseline-n", type=float)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    if (args.baseline_a is None) != (args.baseline_n is None):
        parser.error("--baseline-a and --baseline-n must be supplied together")
    fallback = None if args.baseline_a is None else (args.baseline_a, args.baseline_n)
    journal = args.journal or args.csv.with_name("collection-state.json")
    train, test = read_real(args.csv)
    parameters = baseline_parameters(test, journal, fallback)
    models, trials, best, groups = select_models(train)
    x_test, y_test = arrays(test)
    forest = predict(models, x_test)
    baseline = baseline_predict(x_test, parameters)
    if not np.isfinite(baseline).all():
        raise ValueError("Baseline produced a non-finite estimate; check the saved A/n parameters.")
    forest_errors = errors_m(y_test, forest)
    baseline_errors = errors_m(y_test, baseline)
    csv_hash = hashlib.sha256(args.csv.read_bytes()).hexdigest()
    bundle = {
        "synthetic": False, "feature_order": FEATURES, "target_order": TARGETS,
        "output_units": "metres", "preprocessing": "Finite float32 inputs in recorded receiver order.",
        "node_order_normalized": [[0, 0], [1, 0], [0, 1], [1, 1]],
        "trained_side_lengths_m": sorted({float(row["side_length_m"]) for row in train}),
        "minimum_packets_per_receiver": MIN_PACKETS, "source_csv_sha256": csv_hash,
        "models": dict(zip(TARGETS, models)),
    }
    test_capture_ids = np.array([row["collection_id"] for row in test])
    capture_metrics = {}
    for label, estimates in [("random_forest", forest), ("rssi_baseline", baseline)]:
        capture_errors = []
        for capture_id in dict.fromkeys(test_capture_ids):
            mask = test_capture_ids == capture_id
            capture_errors.append(np.linalg.norm(estimates[mask].mean(axis=0) - y_test[mask][0]))
        capture_metrics[label] = summarize(capture_errors)
    report = {
        "synthetic": False, "source_csv_sha256": csv_hash, "seed": SEED,
        "train_rows": len(train), "test_rows": len(test), "training_positions": len(np.unique(groups)),
        "test_positions": len({row["target_id"] for row in test}),
        "validation": "Five folds grouped by target ID across environments and square sizes.",
        "validation_trials": trials, "selected_parameters": best["parameters"],
        "held_out_windows": {"random_forest": summarize(forest_errors), "rssi_baseline": summarize(baseline_errors)},
        "held_out_captures": capture_metrics,
        "baseline": "Square multilateration using recorded per-receiver or per-window A/n.",
        "baseline_fallback_A_n": fallback,
        "versions": {name: version(name) for name in ["numpy", "scikit-learn", "emlearn", "joblib"]},
    }
    args.out.mkdir(parents=True, exist_ok=True)
    model_path = args.out / "random_forest_real.joblib"
    joblib.dump(bundle, model_path)
    reloaded = joblib.load(model_path)
    np.testing.assert_array_equal(predict(list(reloaded["models"].values()), x_test), forest)
    with (args.out / "test_predictions.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["collection_id", "target_id", "window_index", *TARGETS,
                         "forest_x_m", "forest_y_m", "forest_error_m",
                         "baseline_x_m", "baseline_y_m", "baseline_error_m"])
        for row, actual, rf, rf_error, rss, rss_error in zip(test, y_test, forest, forest_errors, baseline, baseline_errors):
            writer.writerow([row["collection_id"], row["target_id"], row["window_index"],
                             *actual, *rf, rf_error, *rss, rss_error])
    (args.out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Held-out mean position error: forest {forest_errors.mean():.3f} m; RSSI baseline {baseline_errors.mean():.3f} m.")
    print(f"Saved model and evaluation in {args.out.resolve()}")


if __name__ == "__main__":
    main()
