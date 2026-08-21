import ast, os, platform, shutil, subprocess
from pathlib import Path

from setuptools import Command, setup
from setuptools.command.bdist_wheel import bdist_wheel as _bdist_wheel
from setuptools.command.build import build as _build


root = Path(__file__).parent.resolve()
package_root = root / "python/xmojo"


def _bzl_value(name):
    for line in (root / "bazel/versions.bzl").read_text().splitlines():
        if line.startswith(name + "="): return ast.literal_eval(line.split("=", 1)[1])
    raise RuntimeError(f"{name} is missing from bazel/versions.bzl")


def _wrapper(environment):
    path = Path(environment.get("XMOJO_BAZEL_WRAPPER", root / "bazelw"))
    return path if path.is_absolute() else root / path


def _build_assets(optimized):
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise RuntimeError("xmojo wheels currently support only Apple Silicon")

    environment = os.environ.copy()
    deps_root = Path(environment.get("XMOJO_DEPS_ROOT", root.parent)).resolve()
    modular_root = Path(environment.get("XMOJO_MODULAR_ROOT", deps_root / "modular")).resolve()
    environment.update(XMOJO_DEPS_ROOT=str(deps_root), XMOJO_MODULAR_ROOT=str(modular_root))
    config = ["-c", "opt"] if optimized else []
    wrapper = _wrapper(environment)
    subprocess.run([wrapper, "build", *config, "@xmojo//:wheel_assets"], cwd=root, env=environment, check=True)
    result = subprocess.run([wrapper, "cquery", *config, "@xmojo//:wheel_assets", "--output=files"],
        cwd=root, env=environment, check=True, stdout=subprocess.PIPE, text=True)

    marker = "python/xmojo/"
    assets = []
    for line in result.stdout.splitlines():
        if marker not in line: continue
        source = Path(line)
        if not source.is_absolute(): source = modular_root / source
        assets.append((source, Path(line.split(marker, 1)[1])))
    if not assets: raise RuntimeError("Bazel returned no xmojo wheel assets")
    return assets


class build_native(Command):
    description = "build xmojo's native runtime and Mojo packages"
    user_options = []
    editable_mode = False

    def initialize_options(self): self.build_lib = None
    def finalize_options(self): self.set_undefined_options("build_py", ("build_lib", "build_lib"))

    def run(self):
        destination = package_root / "_native" if self.editable_mode else Path(self.build_lib) / "xmojo/_native"
        if destination.exists(): shutil.rmtree(destination)
        for source, relative in _build_assets(not self.editable_mode):
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)

    def get_outputs(self):
        source = package_root / "_native" if self.editable_mode else Path(self.build_lib) / "xmojo/_native"
        if not source.exists(): return []
        destination = Path(self.build_lib) / "xmojo/_native"
        return [str(destination / path.relative_to(source)) for path in source.rglob("*") if path.is_file()]

    def get_output_mapping(self):
        if not self.editable_mode: return {}
        source = package_root / "_native"
        if not source.exists(): return {}
        destination = Path(self.build_lib) / "xmojo/_native"
        return {str(destination / path.relative_to(source)): str(path.relative_to(root))
            for path in source.rglob("*") if path.is_file()}


class build(_build):
    sub_commands = [("build_native", None), *_build.sub_commands]


class bdist_wheel(_bdist_wheel):
    def finalize_options(self):
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self): return "py3", "none", _bzl_value("WHEEL_PLATFORM")


setup(cmdclass=dict(bdist_wheel=bdist_wheel, build=build, build_native=build_native))
