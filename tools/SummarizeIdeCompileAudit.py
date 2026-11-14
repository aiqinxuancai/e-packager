"""按原件是否可编译汇总真实 IDE 对照，明确排除原件错误并保留验证盲区。"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def summarize(roundtrip, directories):
    manifest = {r["id"]: r for r in read(roundtrip / "manifest.json") if r["size"]}
    observations = defaultdict(list)
    for directory in directories:
        for path in sorted(directory.glob("*/result.json")):
            row = read(path)
            if "stages" not in row:
                continue
            item = manifest[row["id"]]
            for label, stage in row["stages"].items():
                source = (roundtrip / "inputs" / item["relative"] if label == "original"
                          else roundtrip / "cases" / f'{row["id"]:04}' / label / "edited.e")
                assert hashlib.sha256(source.read_bytes()).hexdigest() == stage["input_sha256"], source
                if stage["ok"]:
                    artifacts = [p for p in (path.parent / label).iterdir()
                                 if p.suffix.lower() in (".exe", ".dll", ".ec")]
                    if "core_sha256" in stage:
                        payload = read(path.parent / label / "compile.json")
                        assert payload.get("artifact_verified") is True and artifacts, (path, label)
                        assert all(p.stat().st_size > 0 for p in artifacts), (path, label)
                        assert hashlib.sha256(Path(stage["command"][0]).read_bytes()).hexdigest() == stage["core_sha256"]
                    else:
                        assert any(hashlib.sha256(p.read_bytes()).hexdigest() == stage["artifact_sha256"]
                                   for p in artifacts), (path, label)
            # Each pair must use the same launcher, IDE and plugin, even across tool revisions.
            fingerprint_key = "core_sha256" if "core_sha256" in row["stages"]["original"] else "tool_hashes"
            assert all(s[fingerprint_key] == row["stages"]["original"][fingerprint_key]
                       for s in row["stages"].values()), path
            original = row["stages"]["original"]
            assert all((s.get("target", "ecom"), s.get("static_compile", False)) ==
                       (original.get("target", "ecom"), original.get("static_compile", False))
                       for s in row["stages"].values()), path
            observations[row["id"]].append((path, row))

    results = []
    for identifier, item in manifest.items():
        runs = observations[identifier]
        original_successes = [(p, r) for p, r in runs if r["stages"]["original"]["ok"]]
        verified_pairs = [(p, r) for p, r in original_successes
                          if all(r["stages"][a]["ok"] for a in ("Win32", "x64"))]
        errors = []
        if not original_successes:
            for path, row in runs:
                stage = row["stages"]["original"]
                # Core successes are valid compile evidence. Exclusions use actual IDE failures only:
                # the independent core has known event-name bugs on unchanged originals.
                allow_original_error = ("tool_hashes" in stage or
                                        stage.get("error") == "native_load_dependencies_missing")
                for diagnostic in stage.get("diagnostics", []) if allow_original_error else []:
                    if diagnostic.get("severity") == "error":
                        errors.append({"evidence": str(path), "kind": "original_compile_error",
                                       "message": diagnostic["message"]})
                payload_path = path.parent / "original/result.json"
                if payload_path.exists():
                    payload = read(payload_path)
                    for field in ("ide_message_boxes", "launcher_message_boxes"):
                        for box in payload.get(field, []):
                            if box.get("kind") in ("missing_support_library", "missing_ecom_module"):
                                errors.append({"evidence": str(payload_path), "kind": box["kind"],
                                               "message": box.get("text", "")})
        status = ("paired_compile_pass" if verified_pairs else
                  "needs_regression_review" if original_successes else
                  "excluded_original_failure" if errors else "unverified")
        results.append({"id": identifier, "source": item["relative"], "status": status,
                        "paired_evidence": [str(p) for p, _ in verified_pairs],
                        "original_failure_evidence": errors,
                        "attempts": [str(p) for p, _ in runs]})
    statuses = ("paired_compile_pass", "excluded_original_failure", "needs_regression_review", "unverified")
    return {"summary": {s: sum(r["status"] == s for r in results) for s in statuses},
            "policy": "Exclude only originals with explicit compiler/dependency errors and no successful original run. Timeouts, missing results and EideCore compiler errors are not exclusions; explicit native dependency-load failures are exclusions. Successful pairs require matching input hashes, tool hashes and actual artifact hashes.",
            "directories": [str(d) for d in directories], "results": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--roundtrip", type=Path, required=True)
    parser.add_argument("--directories", type=Path, nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = summarize(args.roundtrip, args.directories)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8-sig")
    print(json.dumps(report["summary"]))


if __name__ == "__main__":
    main()
