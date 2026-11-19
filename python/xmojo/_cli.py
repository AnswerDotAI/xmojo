import os, sys
from pathlib import Path


def xmojo():
    root = Path(__file__).parent / "_native"
    os.environ["MODULAR_CRASH_REPORTING_ENABLED"] = "0"
    os.environ["MODULAR_MOJO_MAX_PACKAGE_ROOT"] = str(root)
    os.environ["XMOJO_COMPILER_STDLIB_PATH"] = str(root / "mojo" / "compiler" / "std.mojoc")
    os.environ["MODULAR_MOJO_MAX_IMPORT_PATH"] = ",".join(filter(None, [
        str(root / "mojo"), os.environ.get("MODULAR_MOJO_MAX_IMPORT_PATH", "")]))
    os.execv(root / "bin" / "xmojo", ["xmojo", *sys.argv[1:]])
