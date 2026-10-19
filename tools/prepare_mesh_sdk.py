"""Materialize a hash-pinned, project-local bt component. Never edit the SDK cache."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

PATCH_VERSION = "pump-sar-1"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def tree_hashes(folder):
    return {p.relative_to(folder).as_posix(): sha(p.read_bytes())
            for p in sorted(folder.rglob('*')) if p.is_file()}


def prepare(framework, project):
    assets = Path(__file__).resolve().parent / "mesh_sdk"
    patch = json.loads((assets / "patch.json").read_text(encoding="utf-8"))
    source = Path(framework) / "components/bt"
    target = Path(project) / ".mesh-sdk/bt"
    # Fail closed even when an already-patched component is cached.
    for rel, expected in patch["inputs"].items():
        if sha((Path(framework) / rel).read_bytes()) != expected:
            raise RuntimeError("SDK input differs from reviewed baseline: " + rel)
    baseline = json.loads((assets / "bt-baseline.json").read_text(encoding="utf-8"))
    if tree_hashes(source) != baseline:
        raise RuntimeError("SDK bt component differs from the pinned complete baseline")
    marker = target.parent / "manifest.json"
    identity = sha((assets / "patch.json").read_bytes() + (assets / "bt-baseline.json").read_bytes())
    if marker.exists():
        previous = json.loads(marker.read_text(encoding="utf-8"))
        if previous.get("patch_sha256") == identity:
            if tree_hashes(target) != previous["outputs"]:
                raise RuntimeError("Project SDK was modified (complete component manifest)")
            return
        raise RuntimeError("SDK patch changed; remove the project .mesh-sdk cache and rebuild")
    target.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, target, dirs_exist_ok=True)
    outputs = {}
    for rel, edits in patch["edits"].items():
        file = target / rel
        text = file.read_text(encoding="utf-8")
        for edit in edits:
            if text.count(edit["old"]) != edit.get("count", 1):
                raise RuntimeError("SDK patch anchor differs: " + rel)
            text = text.replace(edit["old"], edit["new"])
        file.write_text(text, encoding="utf-8", newline="\n")
        outputs[rel] = sha(file.read_bytes())
    for rel, text in patch["additions"].items():
        file = target / rel
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text, encoding="utf-8", newline="\n")
        outputs[rel] = sha(file.read_bytes())
    marker.write_text(json.dumps({"version": PATCH_VERSION, "patch_sha256": identity,
                                 "outputs": tree_hashes(target)}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", type=Path, required=True)
    parser.add_argument("--project", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.framework, args.project)
