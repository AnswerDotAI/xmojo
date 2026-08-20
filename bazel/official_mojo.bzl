"""Build rules for the official Mojo compiler installed by Modular's Pixi environment."""

_TOOLCHAIN_TYPE = Label("//bazel:official_mojo_toolchain_type")


def _official_mojo_toolchain_impl(ctx):
    return [platform_common.ToolchainInfo(
        compiler_path = ctx.attr.compiler_path,
        compiler_version = ctx.attr.compiler_version,
        modular_home = ctx.attr.modular_home,
    )]


official_mojo_toolchain = rule(
    implementation = _official_mojo_toolchain_impl,
    attrs = {
        "compiler_path": attr.string(mandatory = True),
        "compiler_version": attr.string(mandatory = True),
        "modular_home": attr.string(mandatory = True),
    },
)


def _official_mojo_shared_library_impl(ctx):
    toolchain = ctx.toolchains[_TOOLCHAIN_TYPE]
    repository_root = ctx.label.workspace_root
    output = ctx.actions.declare_file(ctx.attr.shared_lib_name)
    imports = depset(transitive = [target[DefaultInfo].files for target in ctx.attr.imports])

    ctx.actions.run_shell(
        arguments = [
            repository_root + "/" + toolchain.compiler_path,
            toolchain.compiler_version,
            repository_root + "/" + toolchain.modular_home,
            ctx.file.src.path,
            output.path,
        ],
        command = """
set -euo pipefail
compiler="$1"
expected_version="$2"
modular_home="$3"
source="$4"
output="$5"
actual_version="$($compiler --version)"
if [[ "$actual_version" != "$expected_version" ]]; then
  echo "official Mojo version mismatch: expected '$expected_version', found '$actual_version'" >&2
  exit 1
fi
MODULAR_CRASH_REPORTING_ENABLED=0 \
MODULAR_HOME="$modular_home" \
PATH=/usr/bin:/bin:/usr/sbin:/sbin \
TOOLCHAINS=com.apple.dt.toolchain.Metal \
  "$compiler" build --emit=shared-lib \
  -I max/mojo "$source" -o "$output"
""",
        execution_requirements = {
            "no-remote": "1",
            "no-sandbox": "1",
        },
        inputs = depset([ctx.file.src], transitive = [imports]),
        mnemonic = "OfficialMojoSharedLibrary",
        outputs = [output],
        progress_message = "Building %{label} with the official Mojo compiler",
    )
    return [DefaultInfo(files = depset([output]))]


official_mojo_shared_library = rule(
    implementation = _official_mojo_shared_library_impl,
    attrs = {
        "imports": attr.label_list(allow_files = True),
        "shared_lib_name": attr.string(mandatory = True),
        "src": attr.label(allow_single_file = [".mojo"], mandatory = True),
    },
    toolchains = [_TOOLCHAIN_TYPE],
)
