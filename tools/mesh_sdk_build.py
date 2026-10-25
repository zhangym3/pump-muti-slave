"""PlatformIO pre-build hook; the root CMake file selects our bt component."""
Import("env")
import importlib.util
from pathlib import Path

root = Path(env["PROJECT_DIR"])
spec = importlib.util.spec_from_file_location("prepare_mesh_sdk", root / "tools/prepare_mesh_sdk.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.prepare(env.PioPlatform().get_package_dir("framework-espidf"), root)

import hashlib, json
inputs = {}
inputs["@environment"] = env["PIOENV"]
for folder in ('include', 'src', 'tools/mesh_sdk'):
    for file in sorted((root / folder).rglob('*')):
        if file.is_file(): inputs[file.relative_to(root).as_posix()] = hashlib.sha256(file.read_bytes()).hexdigest()
for name in ('platformio.ini', 'CMakeLists.txt', 'tools/prepare_mesh_sdk.py', 'tools/mesh_sdk_build.py', 'partitions.csv'):
    inputs[name] = hashlib.sha256((root/name).read_bytes()).hexdigest()
for file in sorted(root.glob('sdkconfig*defaults')):
    inputs[file.name] = hashlib.sha256(file.read_bytes()).hexdigest()
identity = hashlib.sha256(json.dumps(inputs,sort_keys=True).encode()).hexdigest()
env.Append(CPPDEFINES=[('PUMP_BUILD_ID', '0x'+identity[:8]+'U')])
(root/'.mesh-sdk/build.json').write_text(json.dumps({'build_id': identity[:8], 'source_sha256':identity, 'inputs':inputs},indent=2)+'\n')
