"Collect the notices shipped with the Rust kernel and its standard library."

import json, shutil, subprocess, sys
from pathlib import Path


def write_licenses(destination):
    manifest = Path(__file__).resolve().parents[1] / "kernel/Cargo.toml"
    command = ["cargo", "metadata", "--format-version=1", "--manifest-path", str(manifest)]
    metadata = json.loads(subprocess.check_output(command, text=True))
    notices = []
    for package in sorted(metadata["packages"], key=lambda p: p["id"]):
        root = Path(package["manifest_path"]).parent
        files = {p for pattern in ("LICENSE*", "COPYING*", "NOTICE*") for p in root.glob(pattern) if p.is_file()}
        if package["license_file"]: files.add(root / package["license_file"])
        for path in sorted(files): notices.append(f'{package["name"]} {package["version"]}: {path.name}\n\n{path.read_text()}')

    rust = Path(subprocess.check_output(["rustc", "--print", "sysroot"], text=True).strip()) / "share/doc/rust"
    for path in sorted((rust / "licenses").iterdir()):
        if path.is_file(): notices.append(f"Rust standard library: {path.name}\n\n{path.read_text()}")
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "Rust-ThirdPartyNotices.txt").write_text("\n\n".join(notices))
    shutil.copyfile(rust / "COPYRIGHT-library.html", destination / "Rust-COPYRIGHT-library.html")


if __name__ == "__main__": write_licenses(sys.argv[1])
