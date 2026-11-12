r"""Export the trained synthetic 1 m forests with emlearn and check host C++.

    .venv\Scripts\python.exe ml\export_demo.py

Requires a C++ compiler (g++ on PATH, or --compiler PATH). Exports only;
does not modify production firmware, train another model, or flash a board.
Only load the trusted joblib file created by this project's train_demo.py.
"""
import argparse
import csv
import hashlib
import json
import re
import shutil
import subprocess
from importlib.metadata import version
from pathlib import Path

import emlearn
import joblib
import numpy as np

from train_demo import FEATURES, TARGETS, ROOT, arrays, predict, read_demo

TOLERANCE_M = 1e-5


def export_float32(model):
    """Retain sklearn's <= decisions on float32 inputs.

    emlearn 0.23.2 emits < and rounds float constants to six decimals. Export
    thresholds as double literals first to retain their original precision,
    then use <= with the greatest float32 value <= each original threshold.
    This keeps comparisons in float32 on the device, without retraining.
    Leaves retain emlearn's float output rounding, checked by parity tolerance.
    """
    converted = emlearn.convert(model, method="inline", dtype="double")
    return converted


def fix_float32_comparisons(code, expected_nodes):
    def replace(match):
        feature, literal = match.groups()
        threshold = float(literal)
        rounded = np.float32(threshold)
        if float(rounded) > threshold:
            rounded = np.nextafter(rounded, np.float32(-np.inf))
        return f"if (features[{feature}] <= {float(rounded):.9e}f) {{"
    code, count = re.subn(r"if \(features\[(\d+)\] < ([^\s]+)\) \{", replace, code)
    if count != expected_nodes:
        raise RuntimeError(f"Exporter structure changed: expected {expected_nodes} splits, found {count}.")
    if "const double *features" not in code:
        raise RuntimeError("Exporter input signature changed; inspect generated code.")
    return code.replace("const double *features", "const float *features")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, default=ROOT / "ml/artifacts/demo_1m/random_forest_demo.joblib")
    parser.add_argument("--csv", type=Path, default=ROOT / "outputs/ml-demo-20261108/synthetic_collection_1m.csv")
    parser.add_argument("--out", type=Path, default=ROOT / "ml/artifacts/demo_1m/c_export")
    parser.add_argument("--compiler", default=shutil.which("g++"))
    args = parser.parse_args()
    if not args.compiler:
        parser.error("A C++ compiler is required; pass --compiler with the path to g++.")
    bundle = joblib.load(args.model)
    if not bundle.get("synthetic") or bundle["side_length_m"] != 1:
        raise ValueError("Expected the synthetic 1 m demo model bundle.")
    if bundle["feature_order"] != FEATURES or bundle["target_order"] != TARGETS:
        raise ValueError("Model input/output order differs from the training contract.")
    if bundle["output_units"] != "metres":
        raise ValueError("Expected direct metre outputs.")
    models = [bundle["models"][name] for name in TARGETS]
    train, test = read_demo(args.csv)
    x_data, _ = arrays(train + test)
    # Additional deterministic inputs exercise code conversion, not accuracy.
    rng = np.random.default_rng(20261108)
    extra = rng.uniform(x_data.min(axis=0), x_data.max(axis=0), (1024, len(FEATURES))).astype(np.float32)
    extra[:, 8:12] = np.rint(extra[:, 8:12])
    # Exercise both sides and equality at every learned split. Use dataset rows
    # that reached the node so the boundary is tested on a relevant path.
    boundaries = []
    for model in models:
        for estimator in model.estimators_:
            tree = estimator.tree_
            paths = estimator.decision_path(x_data).tocsc()
            for node in np.flatnonzero(tree.children_left >= 0):
                feature = tree.feature[node]
                row = x_data[paths.indices[paths.indptr[node]]].copy()
                pivot = np.float32(tree.threshold[node])
                for value in [np.nextafter(pivot, np.float32(-np.inf)), pivot,
                              np.nextafter(pivot, np.float32(np.inf))]:
                    case = row.copy()
                    case[feature] = value
                    boundaries.append(case)
    x_check = np.vstack([x_data, extra, np.array(boundaries, dtype=np.float32)])
    expected = predict(models, x_check)
    args.out.mkdir(parents=True, exist_ok=True)
    # Remove a previous success report before attempting a fresh validation.
    report_path = args.out / "export_report.json"
    report_path.unlink(missing_ok=True)
    headers = []
    for axis, model in zip(["x", "y"], models):
        name = f"localization_{axis}"
        destination = args.out / f"{name}.h"
        converted = export_float32(model)
        converted.save(name=name, file=str(destination))
        # These are metre outputs, unlike the earlier standalone dummy generator.
        nodes = sum(int(np.count_nonzero(t.tree_.children_left >= 0)) for t in model.estimators_)
        code = fix_float32_comparisons(destination.read_text(encoding="utf-8"), nodes)
        destination.write_text(
            "#pragma once\n// SYNTHETIC 1 m DEMO ONLY. Output: metres.\n"
            "// Feature and node order: feature_contract.json.\n" + code,
            encoding="utf-8",
        )
        headers.append(destination)
    contract = {
        "synthetic": True,
        "limitation": "1 m square, simulated environment only; not a field-validated model.",
        "features": FEATURES, "feature_dtype": "float32", "feature_count": len(FEATURES),
        "preprocessing": bundle["preprocessing"], "outputs": TARGETS,
        "output_units": "metres", "side_length_m": 1,
        "node_order_m": bundle["node_order_m"],
        "frame": "Dataset frame: node 01 origin; node 02 +x; node 03 +y. Not yet mapped to conf layout.",
        "functions": ["localization_x_predict(features, 13)", "localization_y_predict(features, 13)"],
        "export_compatibility": "Float32 thresholds floored from original double values; <= splits preserve sklearn decisions. Leaves use emlearn float rounding.",
        "source_model_sha256": hashlib.sha256(args.model.read_bytes()).hexdigest(),
        "source_csv_sha256": hashlib.sha256(args.csv.read_bytes()).hexdigest(),
        "versions": {name: version(name) for name in ["emlearn", "numpy", "scikit-learn"]},
    }
    (args.out / "feature_contract.json").write_text(json.dumps(contract, indent=2), encoding="utf-8")
    driver = args.out / "verify_export.cpp"
    driver.write_text(r'''#include <cstdio>
#include "localization_x.h"
#include "localization_y.h"
int main() {
  int cases = 0;
  if (std::scanf("%d", &cases) != 1 || cases < 1) return 2;
  for (int row = 0; row < cases; ++row) {
    float features[13];
    for (int i = 0; i < 13; ++i)
      if (std::scanf("%f", &features[i]) != 1) return 3;
    std::printf("%.9g %.9g\n", localization_x_predict(features, 13),
                localization_y_predict(features, 13));
  }
  return 0;
}
''', encoding="utf-8")
    executable = args.out / "verify_export.exe"
    compile_command = [args.compiler, "-std=c++11", "-O2", str(driver.resolve()), "-o", str(executable.resolve())]
    subprocess.run(compile_command, check=True, capture_output=True, text=True)
    payload = str(len(x_check)) + "\n" + "\n".join(" ".join(f"{v:.9g}" for v in row) for row in x_check) + "\n"
    result = subprocess.run([str(executable.resolve())], input=payload, capture_output=True, text=True, check=True)
    actual = np.loadtxt(result.stdout.splitlines(), dtype=np.float64)
    if actual.shape != expected.shape or not np.isfinite(actual).all():
        raise RuntimeError("C++ returned an invalid number of predictions or non-finite values.")
    differences = np.abs(actual - expected)
    with (args.out / "parity_predictions.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["case", "source", "python_x_m", "python_y_m", "cpp_x_m", "cpp_y_m", "abs_diff_x_m", "abs_diff_y_m"])
        for index, (reference, exported, difference) in enumerate(zip(expected, actual, differences)):
            source = "dataset" if index < len(x_data) else "sampled_input" if index < len(x_data)+len(extra) else "split_boundary"
            writer.writerow([index + 1, source, *reference, *exported, *difference])
    report = {
        "synthetic": True, "passed": bool(np.all(differences <= TOLERANCE_M)),
        "dataset_cases": len(x_data), "additional_sampled_cases": len(extra),
        "split_boundary_cases": len(boundaries),
        "total_cases": len(x_check), "absolute_tolerance_per_axis_m": TOLERANCE_M,
        "max_abs_difference_per_axis_m": dict(zip(TARGETS, differences.max(axis=0).tolist())),
        "failed_cases": int(np.count_nonzero(np.any(differences > TOLERANCE_M, axis=1))),
        "headers": {p.name: {"source_bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in headers},
        "compiler_command": compile_command,
        "scope": "Dataset, sampled inputs and split-boundary Python/host C++ parity; not exhaustive input coverage or ESP32 size/timing validation.",
    }
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    if not report["passed"]:
        raise RuntimeError("Export parity failed; inspect parity_predictions.csv before using the headers.")
    print(f"PASS: {len(x_check)} Python/C++ cases agree within {TOLERANCE_M} m per axis.")
    print(f"Outputs: {args.out.resolve()}")


if __name__ == "__main__":
    main()
