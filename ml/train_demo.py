r"""Train and evaluate Random Forests using the SYNTHETIC 1 m demo CSV.

Run from the project root (no notebook required):
    .venv\Scripts\python.exe ml\train_demo.py

Tune only on train positions with grouped cross-validation, then evaluate the
selected configuration on the reserved test positions. No ESP32 upload/export.
"""
import argparse
import csv
import hashlib
import json
import sys
from importlib.metadata import version
from pathlib import Path

import joblib
import numpy as np
from sklearn.ensemble import RandomForestRegressor
from sklearn.model_selection import GroupKFold

from create_demo_dataset import COLUMNS

ROOT = Path(__file__).resolve().parents[1]
FEATURES = (
    [f"rssi_mean_{i:02d}" for i in range(1, 5)]
    + [f"rssi_std_{i:02d}" for i in range(1, 5)]
    + [f"packet_count_{i:02d}" for i in range(1, 5)]
    + ["side_length_m"]
)
TARGETS = ["target_x_m", "target_y_m"]
SEED = 42


def read_demo(path):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != COLUMNS:
            raise ValueError("CSV headers must match the original 24-column collection schema.")
        rows = list(reader)
    if not rows:
        raise ValueError("CSV has no data rows.")
    for row in rows:
        if not row["collection_id"].startswith("synthetic-"):
            raise ValueError("This demo script accepts only labelled synthetic collections.")
        if row["dataset_split"] not in {"train", "test"}:
            raise ValueError("dataset_split must be train or test.")
        if float(row["side_length_m"]) != 1:
            raise ValueError("This demo supports only a 1 m square.")
        for name in FEATURES + TARGETS:
            if not np.isfinite(float(row[name])):
                raise ValueError(f"Missing/non-finite value in {name}.")
        for i in range(1, 5):
            if float(row[f"rssi_mean_{i:02d}"]) >= 0:
                raise ValueError("RSSI means must be negative dBm; do not use zero for missing data.")
            count = float(row[f"packet_count_{i:02d}"])
            if count < 20 or not count.is_integer() or float(row[f"rssi_std_{i:02d}"]) < 0:
                raise ValueError("Invalid packet count or RSSI standard deviation.")
    captures = {}
    for row in rows:
        captures.setdefault(row["collection_id"], []).append(row)
    for capture in captures.values():
        if sorted(int(row["window_index"]) for row in capture) != [1, 2, 3]:
            raise ValueError("Each collection must contain exactly windows 1, 2 and 3.")
        for key in ["dataset_split", "target_id", *TARGETS]:
            if len({row[key] for row in capture}) != 1:
                raise ValueError(f"Collection has inconsistent {key}.")
    train = [row for row in rows if row["dataset_split"] == "train"]
    test = [row for row in rows if row["dataset_split"] == "test"]
    if not train or not test:
        raise ValueError("Both train and test rows are required.")
    for key in ["collection_id", "target_id"]:
        if {row[key] for row in train} & {row[key] for row in test}:
            raise ValueError(f"Train/test leakage: overlapping {key}.")
    locations = lambda records: {(float(r[TARGETS[0]]), float(r[TARGETS[1]])) for r in records}
    if locations(train) & locations(test):
        raise ValueError("Train/test leakage: overlapping physical target coordinates.")
    return train, test


def arrays(rows):
    return (np.array([[float(r[k]) for k in FEATURES] for r in rows], dtype=np.float32),
            np.array([[float(r[k]) for k in TARGETS] for r in rows], dtype=np.float64))


def fit_models(x, y, parameters):
    return [RandomForestRegressor(**parameters, random_state=SEED, n_jobs=1).fit(x, y[:, axis])
            for axis in range(2)]


def predict(models, x):
    return np.column_stack([model.predict(x) for model in models])


def errors_m(actual, predicted):
    return np.linalg.norm(predicted - actual, axis=1)


def summarize(errors):
    return {"mean_error_m": float(np.mean(errors)),
            "median_error_m": float(np.median(errors)),
            "p90_error_m": float(np.percentile(errors, 90)),
            "max_error_m": float(np.max(errors))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, default=ROOT / "outputs/ml-demo-20261108/synthetic_collection_1m.csv")
    parser.add_argument("--out", type=Path, default=ROOT / "ml/artifacts/demo_1m")
    args = parser.parse_args()
    train, test = read_demo(args.csv)
    x_train, y_train = arrays(train)
    groups = np.array([row["target_id"] for row in train])
    if len(np.unique(groups)) < 5:
        raise ValueError("Need at least five training positions for grouped five-fold validation.")
    splits = list(GroupKFold(n_splits=5).split(x_train, y_train, groups))
    candidates = [
        {"n_estimators": 8, "max_depth": 4, "min_samples_leaf": 2},
        {"n_estimators": 16, "max_depth": 5, "min_samples_leaf": 2},
        {"n_estimators": 32, "max_depth": 6, "min_samples_leaf": 1},
    ]
    trials = []
    for parameters in candidates:
        validation_errors = []
        for fit_indices, validation_indices in splits:
            assert set(groups[fit_indices]).isdisjoint(groups[validation_indices])
            models = fit_models(x_train[fit_indices], y_train[fit_indices], parameters)
            validation_errors.extend(errors_m(y_train[validation_indices], predict(models, x_train[validation_indices])))
        trials.append({"parameters": parameters, **summarize(validation_errors)})
    best = min(trials, key=lambda trial: trial["mean_error_m"])
    models = fit_models(x_train, y_train, best["parameters"])
    # Test labels are used for evaluation only after configuration selection.
    x_test, y_test = arrays(test)
    predicted = predict(models, x_test)
    test_errors = errors_m(y_test, predicted)
    baseline_errors = errors_m(y_test, np.tile(y_train.mean(axis=0), (len(test), 1)))
    bundle = {
        "synthetic": True, "feature_order": FEATURES, "target_order": TARGETS,
        "output_units": "metres", "preprocessing": "none; finite float32 inputs; reject missing readings",
        "node_order_m": [[0, 0], [1, 0], [0, 1], [1, 1]],
        "side_length_m": 1, "models": dict(zip(TARGETS, models)),
    }
    args.out.mkdir(parents=True, exist_ok=True)
    model_path = args.out / "random_forest_demo.joblib"
    joblib.dump(bundle, model_path)
    reloaded = joblib.load(model_path)
    np.testing.assert_allclose(predict(list(reloaded["models"].values()), x_test), predicted, rtol=0, atol=0)
    report = {
        "synthetic": True, "limitation": "Demo pipeline only; not evidence of real-world accuracy.",
        "source_csv": str(args.csv.resolve()),
        "source_sha256": hashlib.sha256(args.csv.read_bytes()).hexdigest(),
        "train_rows": len(train), "test_rows": len(test),
        "training_positions": len(np.unique(groups)),
        "test_positions": len({r["target_id"] for r in test}),
        "validation": "5 folds grouped by training target_id; minimum mean Euclidean position error selects configuration",
        "validation_trials": trials, "selected_parameters": best["parameters"],
        "test_random_forest": summarize(test_errors),
        "test_training_coordinate_mean_baseline": summarize(baseline_errors),
        "feature_order": FEATURES, "target_order": TARGETS, "seed": SEED,
        "versions": {name: version(name) for name in ["numpy", "scikit-learn", "emlearn", "joblib"]},
        "python_version": sys.version,
    }
    (args.out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    with (args.out / "test_predictions.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["collection_id", "target_id", "window_index", "actual_x_m", "actual_y_m",
                         "predicted_x_m", "predicted_y_m", "position_error_m"])
        for row, actual, estimate, error in zip(test, y_test, predicted, test_errors):
            writer.writerow([row["collection_id"], row["target_id"], row["window_index"],
                             *actual, *estimate, float(error)])
    print("SYNTHETIC 1 m demonstration; no real-world accuracy claims.")
    print(f"Train: {len(train)} rows; test: {len(test)} rows. Validation: 5 folds grouped by position.")
    print(f"Selected Random Forest parameters per axis: {best['parameters']}")
    for label, metrics in [("Random Forest test", report["test_random_forest"]),
                           ("Training-mean baseline", report["test_training_coordinate_mean_baseline"])]:
        print(f"{label}: mean={metrics['mean_error_m']:.4f} m; median={metrics['median_error_m']:.4f} m; p90={metrics['p90_error_m']:.4f} m")
    print("Saved-model reload predictions match exactly.")
    print(f"Outputs: {args.out.resolve()}")


if __name__ == "__main__":
    main()
