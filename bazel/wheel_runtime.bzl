"""Stage the frontend's shared-library dependency graph in the wheel."""

load("@//bazel/internal:modular_strip_binary.bzl", "register_strip_action")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "use_cpp_toolchain")
load("@rules_cc//cc/common:cc_shared_library_info.bzl", "CcSharedLibraryInfo")

def _wheel_runtime_impl(ctx):
    libraries = {}
    for dep in ctx.attr.deps:
        shared = dep[CcSharedLibraryInfo]
        inputs = [shared.linker_input] + [item.linker_input for item in shared.dynamic_deps.to_list()]
        for linker_input in inputs:
            for library in linker_input.libraries:
                artifact = library.resolved_symlink_dynamic_library or library.dynamic_library
                if not artifact:
                    continue
                name = artifact.basename
                previous = libraries.get(name)
                if previous and previous != artifact:
                    fail("Conflicting runtime libraries named %s: %s and %s" % (name, previous.path, artifact.path))
                libraries[name] = artifact

    outputs = []
    for name, library in sorted(libraries.items()):
        output = ctx.actions.declare_file("python/xmojo/lib/" + name)
        register_strip_action(ctx = ctx, input_file = library, output_file = output)
        outputs.append(output)
    if not outputs:
        fail("No native runtime libraries found")
    return [DefaultInfo(files = depset(outputs))]

wheel_runtime = rule(
    implementation = _wheel_runtime_impl,
    attrs = {"deps": attr.label_list(mandatory = True, providers = [CcSharedLibraryInfo])},
    toolchains = use_cpp_toolchain(),
    fragments = ["cpp"],
)
