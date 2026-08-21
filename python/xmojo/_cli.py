import os, subprocess, sys
from importlib.metadata import PackageNotFoundError, distribution
from pathlib import Path


def _fail(message):
    print(f"xmojo: {message}", file=sys.stderr)
    raise SystemExit(2)


def _modular_gpu():
    try: dist = distribution("max-core")
    except PackageNotFoundError: _fail("--modular-gpu requires xmojo[modular-gpu] from Modular's package index")

    modular = Path(dist.locate_file("modular"))
    suffix = ".dylib" if sys.platform == "darwin" else ".so"
    runtime = modular / "lib" / f"libAsyncRTMojoBindings{suffix}"
    compiler = modular / "bin" / "mojo"
    query = modular / "bin" / "gpu-query"
    imports = modular / "lib" / "mojo"
    missing = [str(path) for path in (runtime, compiler, query, imports / "max.mojoc") if not path.exists()]
    if missing: _fail("max-core installation is missing " + ", ".join(missing))

    result = subprocess.run([query, "--target-accelerator"], capture_output=True, text=True)
    if result.returncode: _fail("gpu-query failed: " + (result.stderr.strip() or f"exit status {result.returncode}"))
    target = result.stdout.strip()
    if not target: _fail("gpu-query found no compatible Modular GPU")
    args = ["--target-accelerator", target, "--gpu-runtime-library", str(runtime),
        "--mojo-compiler", str(compiler), "--modular-home", str(modular)]
    return imports, args


def _run(name):
    root = Path(__file__).parent
    args = sys.argv[1:]
    package_dir = root / "mojo" / "base"
    imports = [str(package_dir), str(root / "mojo")]
    if "--modular-gpu" in args:
        args = [arg for arg in args if arg != "--modular-gpu"]
        max_imports, gpu_args = _modular_gpu()
        package_dir = root / "mojo" / "modular_gpu"
        imports[0] = str(package_dir)
        imports.append(str(max_imports))
        args = [*gpu_args, *args]
    os.environ["MODULAR_CRASH_REPORTING_ENABLED"] = "0"
    os.environ["MODULAR_MOJO_MAX_COMPILERRT_PATH"] = str(root / "lib" / "libKGENCompilerRTShared.dylib")
    os.environ["XMOJO_COMPILER_STDLIB_PATH"] = str(root / "mojo" / "compiler" / "std.mojoc")
    imports.append(os.environ.get("MODULAR_MOJO_MAX_IMPORT_PATH", ""))
    os.environ["MODULAR_MOJO_MAX_IMPORT_PATH"] = ",".join(filter(None, imports))
    executable = root / "bin" / name
    os.execv(executable, [name, *args])


def xmojo(): _run("xmojo")
