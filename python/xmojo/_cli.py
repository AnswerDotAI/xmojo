import os, sys
from pathlib import Path


def xmojo():
    root = Path(__file__).parent / "_native"
    libraries = root / "lib"
    suffix = ".dylib" if sys.platform == "darwin" else ".so"
    loader_path = "DYLD_LIBRARY_PATH" if sys.platform == "darwin" else "LD_LIBRARY_PATH"
    os.environ[loader_path] = os.pathsep.join(filter(None, [
        str(libraries), os.environ.get(loader_path, "")]))
    os.environ["MODULAR_CRASH_REPORTING_ENABLED"] = "0"
    os.environ["MODULAR_MOJO_MAX_COMPILERRT_PATH"] = str(libraries / f"libKGENCompilerRTShared{suffix}")
    os.environ["XMOJO_COMPILER_STDLIB_PATH"] = str(root / "mojo" / "compiler" / "std.mojoc")
    os.environ["MODULAR_MOJO_MAX_IMPORT_PATH"] = ",".join(filter(None, [
        str(root / "mojo"), os.environ.get("MODULAR_MOJO_MAX_IMPORT_PATH", "")]))
    os.environ["MODULAR_MOJO_MAX_SHARED_LIBS"] = ",".join(filter(None, [
        str(libraries / f"libAsyncRTMojoBindings{suffix}"),
        "-Xlinker", "-rpath", "-Xlinker", str(libraries),
        os.environ.get("MODULAR_MOJO_MAX_SHARED_LIBS", "")]))
    os.execv(root / "bin" / "xmojo", ["xmojo", *sys.argv[1:]])
