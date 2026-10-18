"""Build and run hardware-independent slave valve tests."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", help="Path to zig, clang++, or g++ (otherwise detect from PATH)")
args = parser.parse_args()
compiler = args.compiler or next((p for name in ("zig", "clang++", "g++") if (p := shutil.which(name))), None)
if not compiler:
    parser.error("Pass --compiler with a C++17 compiler or Zig executable")
root = Path(__file__).resolve().parents[1]
out = root / ".pio" / "host-tests"
out.mkdir(parents=True, exist_ok=True)
exe = out / ("core_tests.exe" if sys.platform == "win32" else "core_tests")
cmd = [compiler]
if Path(compiler).stem.lower() == "zig": cmd += ["c++"]
cmd += ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-O0", "-g", "-I", str(root / "include")]
cmd += [str(root / p) for p in ("test/core_tests.cpp", "src/protocol.cpp", "src/flow_job_controller.cpp")]
cmd += ["-o", str(exe)]
subprocess.run(cmd, cwd=root, check=True)
subprocess.run([str(exe)], cwd=root, check=True)
