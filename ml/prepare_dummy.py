"""Train SYNTHETIC models and export a standalone ESP32 smoke test.

This checks deployment plumbing only. Synthetic errors are not field accuracy.
Run from the repository root: .venv/Scripts/python ml/prepare_dummy.py
"""
import json
import pickle
from importlib.metadata import version
from pathlib import Path

import emlearn
import numpy as np
from sklearn.ensemble import RandomForestRegressor

HERE = Path(__file__).resolve().parent
OUT = HERE / "esp32_dummy" / "generated"
ARTIFACTS = HERE / "artifacts"
FEATURES = (
    [f"rssi_mean_{i:02d}" for i in range(1, 5)]
    + [f"rssi_std_{i:02d}" for i in range(1, 5)]
    + [f"packet_count_{i:02d}" for i in range(1, 5)]
    + ["side_length_m"]
)


def synthetic_data(count, seed):
    rng = np.random.default_rng(seed)
    side = rng.integers(1, 6, count).astype(float)
    normalized_xy = rng.uniform(-0.25, 1.25, (count, 2))
    xy = normalized_xy * side[:, None]
    # Assumed node order for SYNTHETIC data only. Confirm actual geometry later.
    anchors = np.array([[0, 0], [1, 0], [1, 1], [0, 1]])
    distance = np.linalg.norm(xy[:, None, :] - side[:, None, None] * anchors, axis=2)
    std = rng.uniform(1, 5, (count, 4))
    mean = -45 - 22 * np.log10(np.maximum(distance, 0.1))
    mean += rng.normal(size=(count, 4)) * std
    packets = rng.integers(20, 301, (count, 4))
    features = np.column_stack([mean, std, packets, side]).astype(np.float32)
    return features, normalized_xy


def cfloat(value):
    # Scientific notation always produces a valid C floating-point literal.
    return f"{float(value):.9e}f"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    x_train, y_train = synthetic_data(2000, 42)
    x_check, _ = synthetic_data(128, 2026)
    models = []
    for axis, name in enumerate(["dummy_x", "dummy_y"]):
        model = RandomForestRegressor(
            n_estimators=8, max_depth=4, min_samples_leaf=5, random_state=42
        ).fit(x_train, y_train[:, axis])
        # Float inputs preserve fractional RSSI/std and square dimensions.
        converted = emlearn.convert(model, method="inline", dtype="float")
        converted.save(file=str(OUT / f"{name}.h"), name=name)
        models.append(model)

    expected = np.column_stack([model.predict(x_check) for model in models])
    rows = ["{" + ", ".join(map(cfloat, row)) + "}" for row in x_check]
    answers = ["{" + ", ".join(map(cfloat, row)) + "}" for row in expected]
    header = (
        "#pragma once\n// SYNTHETIC export parity fixtures; not field validation.\n"
        f"static const int kFeatureCount = {len(FEATURES)};\n"
        f"static const int kCaseCount = {len(x_check)};\n"
        "static const float kInputs[kCaseCount][kFeatureCount] = {\n"
        + ",\n".join(rows) + "\n};\n"
        "static const float kExpected[kCaseCount][2] = {\n"
        + ",\n".join(answers) + "\n};\n"
    )
    (OUT / "parity_vectors.h").write_text(header, encoding="utf-8")
    metadata = {
        "purpose": "SYNTHETIC deployment smoke test; not a localization model",
        "feature_order": FEATURES,
        "outputs": ["x / side_length_m", "y / side_length_m"],
        "node_order_synthetic_only": [[0, 0], [1, 0], [1, 1], [0, 1]],
        "trees_per_axis": 8, "max_depth": 4,
        "parity_cases": len(x_check), "parity_tolerance_normalized": 1e-5,
        "versions": {name: version(name) for name in ["numpy", "scikit-learn", "emlearn"]},
    }
    (ARTIFACTS / "dummy_metadata.json").write_text(json.dumps(metadata, indent=2))
    with (ARTIFACTS / "dummy_models.pkl").open("wb") as stream:
        pickle.dump(models, stream)
    print(f"SYNTHETIC models exported to {OUT}")
    print(f"Generated {len(x_check)} Python reference predictions; compile and run host_parity.cpp next.")
    print(json.dumps(metadata["versions"], indent=2))


if __name__ == "__main__":
    main()
