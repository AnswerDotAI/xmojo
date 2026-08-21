//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
//===----------------------------------------------------------------------===//

#include "OfficialGPUCompiler.h"

#include "Support/FileSystemExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <optional>
#include <system_error>

using llvm::StringRef;
using namespace xmojo;

namespace {

constexpr llvm::StringLiteral cacheMagic = "XMOJOGPU1";

void hashField(llvm::SHA256 &hash, StringRef value) {
  hash.update(std::to_string(value.size()));
  hash.update(":");
  hash.update(value);
}

M::ErrorOrSuccess hashFile(llvm::SHA256 &hash, StringRef label,
                           StringRef path) {
  auto bufferOr = llvm::MemoryBuffer::getFile(path);
  if (!bufferOr)
    return M::Error(("could not fingerprint GPU compiler input '" + path +
                     "': " + bufferOr.getError().message())
                        .str());
  hashField(hash, label);
  hashField(hash, bufferOr.get()->getBuffer());
  return M::success();
}

bool isMojoInput(const std::filesystem::path &path) {
  std::string extension = path.extension().string();
  return extension == ".mojo" || extension == ".mojoc" ||
         extension == ".mojopkg";
}

std::string defaultCacheDirectory() {
  if (const char *configured = std::getenv("XMOJO_GPU_CACHE_DIR"))
    return configured;
  if (const char *configured = std::getenv("XDG_CACHE_HOME"))
    return (std::filesystem::path(configured) / "xmojo" / "gpu").string();
  const char *home = std::getenv("HOME");
  if (!home)
    return {};
#if defined(__APPLE__)
  return (std::filesystem::path(home) / "Library" / "Caches" / "xmojo" / "gpu")
      .string();
#else
  return (std::filesystem::path(home) / ".cache" / "xmojo" / "gpu").string();
#endif
}

std::optional<GPUArtifact> readCacheEntry(StringRef path) {
  auto bufferOr = llvm::MemoryBuffer::getFile(path);
  if (!bufferOr)
    return std::nullopt;
  StringRef bytes = bufferOr.get()->getBuffer();
  if (!bytes.consume_front(cacheMagic) || bytes.size() < sizeof(uint64_t))
    return std::nullopt;

  uint64_t nameSize = 0;
  for (size_t index = 0; index < sizeof(uint64_t); ++index)
    nameSize |= uint64_t(static_cast<unsigned char>(bytes[index]))
                << (index * 8);
  bytes = bytes.drop_front(sizeof(uint64_t));
  if (nameSize > bytes.size())
    return std::nullopt;
  return GPUArtifact{bytes.take_front(nameSize).str(),
                     bytes.drop_front(nameSize).str()};
}

void writeCacheEntry(StringRef path, const GPUArtifact &artifact) {
  llvm::SmallString<256> parent(path);
  llvm::sys::path::remove_filename(parent);
  if (llvm::sys::fs::create_directories(parent))
    return;

  llvm::SmallString<256> model(parent);
  llvm::sys::path::append(model, ".xmojo-gpu-%%%%%%");
  int descriptor = -1;
  llvm::SmallString<256> temporaryPath;
  if (llvm::sys::fs::createUniqueFile(model, descriptor, temporaryPath))
    return;

  llvm::raw_fd_ostream output(descriptor, /*shouldClose=*/true);
  output << cacheMagic;
  uint64_t nameSize = artifact.functionName.size();
  for (size_t index = 0; index < sizeof(uint64_t); ++index)
    output << char(nameSize >> (index * 8));
  output << artifact.functionName << artifact.object;
  output.close();
  if (output.has_error()) {
    llvm::sys::fs::remove(temporaryPath);
    return;
  }

  if (llvm::sys::fs::rename(temporaryPath, path))
    llvm::sys::fs::remove(temporaryPath);
}

bool isIdentifier(StringRef name) {
  auto isAlpha = [](char c) {
    return std::isalpha(static_cast<unsigned char>(c));
  };
  auto isAlnum = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c));
  };
  if (name.empty() || !(isAlpha(name.front()) || name.front() == '_'))
    return false;
  return llvm::all_of(name.drop_front(),
                      [&](char c) { return isAlnum(c) || c == '_'; });
}

bool isTargetName(StringRef name) {
  return !name.empty() && llvm::all_of(name, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == '-' || c == '.';
  });
}

std::string artifactSource(StringRef declarations, StringRef functionName,
                           StringRef target) {
  std::string source;
  llvm::raw_string_ostream out(source);
  out << declarations;
  if (!declarations.empty() && !declarations.ends_with("\n"))
    out << '\n';
  out << R"mojo(
from std.compile import compile_info as __xmojo_gpu_compile_info
from std.ffi import c_size_t as __xmojo_gpu_size_t
from std.gpu.host import get_gpu_target as __xmojo_gpu_target

@export("__xmojo_gpu_artifact_data")
def __xmojo_gpu_artifact_data(
    size: Pointer[__xmojo_gpu_size_t, MutAnyOrigin],
) abi("C") -> Pointer[Byte, ImmutAnyOrigin]:
    var info = __xmojo_gpu_compile_info[)mojo"
      << functionName << R"mojo(,
        target=__xmojo_gpu_target[")mojo"
      << target << R"mojo("](),
        emission_kind="object",
    ]()
    size[] = __xmojo_gpu_size_t(info.asm.byte_length())
    return info.asm.as_bytes().unsafe_ptr().unsafe_origin_cast[
        ImmutAnyOrigin
    ]()

@export("__xmojo_gpu_artifact_name")
def __xmojo_gpu_artifact_name(
    size: Pointer[__xmojo_gpu_size_t, MutAnyOrigin],
) abi("C") -> Pointer[Byte, ImmutAnyOrigin]:
    var info = __xmojo_gpu_compile_info[)mojo"
      << functionName << R"mojo(,
        target=__xmojo_gpu_target[")mojo"
      << target << R"mojo("](),
        emission_kind="object",
    ]()
    size[] = __xmojo_gpu_size_t(info.function_name.byte_length())
    return info.function_name.as_bytes().unsafe_ptr().unsafe_origin_cast[
        ImmutAnyOrigin
    ]()
)mojo";
  out.flush();
  return source;
}

M::ErrorOr<std::string> resolveCompiler(StringRef configured) {
  if (!configured.empty())
    return configured.str();
  if (const char *environment = std::getenv("XMOJO_MOJO_COMPILER"))
    return std::string(environment);
  llvm::ErrorOr<std::string> found = llvm::sys::findProgramByName("mojo");
  if (!found)
    return M::Error("could not find the official 'mojo' compiler on PATH");
  return *found;
}

std::string readFile(StringRef path) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  return buffer ? buffer.get()->getBuffer().str() : std::string();
}

} // namespace

OfficialGPUCompiler::OfficialGPUCompiler(std::string compilerPath,
                                         std::string modularHome,
                                         std::vector<std::string> importPaths,
                                         std::string targetAccelerator,
                                         std::string cacheDirectory)
    : compilerPath(std::move(compilerPath)),
      modularHome(std::move(modularHome)), importPaths(std::move(importPaths)),
      targetAccelerator(std::move(targetAccelerator)),
      cacheDirectory(std::move(cacheDirectory)) {
  if (this->modularHome.empty())
    if (const char *environment = std::getenv("XMOJO_MODULAR_HOME"))
      this->modularHome = environment;
  if (this->cacheDirectory.empty())
    this->cacheDirectory = defaultCacheDirectory();
}

M::ErrorOr<std::string> OfficialGPUCompiler::getToolchainFingerprint(
    StringRef resolvedCompilerPath) const {
  llvm::SHA256 hash;
  hash.update("xmojo-gpu-toolchain-v1");
  if (M::ErrorOrSuccess error =
          hashFile(hash, "compiler", resolvedCompilerPath))
    return error.takeError();

  for (size_t index = 0; index < importPaths.size(); ++index) {
    const std::filesystem::path root(importPaths[index]);
    hashField(hash, std::to_string(index));
    hashField(hash, root.string());

    std::error_code error;
    if (!std::filesystem::exists(root, error) || error)
      continue;
    if (std::filesystem::is_regular_file(root, error)) {
      if (error)
        continue;
      if (M::ErrorOrSuccess result =
              hashFile(hash, root.filename().string(), root.string()))
        return result.takeError();
      continue;
    }

    std::vector<std::filesystem::path> files;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied,
        error);
    std::filesystem::recursive_directory_iterator end;
    for (; !error && iterator != end; iterator.increment(error)) {
      if (iterator->is_regular_file(error) && !error &&
          isMojoInput(iterator->path()))
        files.push_back(iterator->path());
    }
    if (error)
      return M::Error(("could not fingerprint GPU import path '" +
                       root.string() + "': " + error.message())
                          .c_str());
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path &file : files) {
      std::filesystem::path relative = file.lexically_relative(root);
      if (M::ErrorOrSuccess result =
              hashFile(hash, relative.generic_string(), file.string()))
        return result.takeError();
    }
  }

  return llvm::toHex(hash.final(), /*LowerCase=*/true);
}

M::ErrorOr<GPUArtifact>
OfficialGPUCompiler::compile(StringRef declarationSource,
                             StringRef functionName) const {
  if (!isIdentifier(functionName))
    return M::Error(("GPU compilation currently requires a top-level, "
                     "non-parameterized function with an ordinary identifier; "
                     "received '" +
                     functionName.str() + "' (" +
                     std::to_string(functionName.size()) + " bytes)")
                        .c_str());
  if (!isTargetName(targetAccelerator))
    return M::Error(
        "GPU compilation requires SessionOptions.targetAccelerator");

  M::ErrorOr<std::string> compilerOr = resolveCompiler(compilerPath);
  if (compilerOr.isError())
    return compilerOr.takeError();

  std::string source =
      artifactSource(declarationSource, functionName, targetAccelerator);
  std::string cachePath;
  std::string inputFingerprint;
  if (!cacheDirectory.empty()) {
    M::ErrorOr<std::string> fingerprintOr =
        getToolchainFingerprint(*compilerOr);
    if (!fingerprintOr.isError()) {
      inputFingerprint = *fingerprintOr;
      llvm::SHA256 cacheKey;
      cacheKey.update("xmojo-gpu-artifact-v1");
      hashField(cacheKey, inputFingerprint);
      hashField(cacheKey, source);
      std::string key = llvm::toHex(cacheKey.final(), /*LowerCase=*/true);
      cachePath = (std::filesystem::path(cacheDirectory) / (key + ".artifact"))
                      .string();
      if (std::optional<GPUArtifact> cached = readCacheEntry(cachePath))
        return std::move(*cached);
    }
  }

  M::ErrorOr<M::TempDir> directoryOr = M::TempDir::create("xmojo-gpu.%%%%%%");
  if (directoryOr.isError())
    return directoryOr.takeError();
  M::TempDir directory = std::move(*directoryOr);
  std::string sourcePath = (directory.getPath() / "artifact.mojo").string();
#if defined(__APPLE__)
  std::string libraryPath =
      (directory.getPath() / "libartifact.dylib").string();
#else
  std::string libraryPath = (directory.getPath() / "libartifact.so").string();
#endif
  std::string diagnosticsPath =
      (directory.getPath() / "diagnostics.txt").string();

  std::error_code fileError;
  llvm::raw_fd_ostream sourceFile(sourcePath, fileError);
  if (fileError)
    return M::Error("could not create temporary GPU compilation source");
  sourceFile << source;
  sourceFile.close();

  std::vector<std::string> storage{"/usr/bin/env", "-u",
                                   "MODULAR_MOJO_MAX_IMPORT_PATH",
                                   "MODULAR_CRASH_REPORTING_ENABLED=0"};
  if (!modularHome.empty())
    storage.push_back("MODULAR_HOME=" + modularHome);
#if defined(__APPLE__)
  storage.push_back("TOOLCHAINS=com.apple.dt.toolchain.Metal");
#endif
  storage.insert(storage.end(), {*compilerOr, "build", "--emit=shared-lib"});
  for (const std::string &path : importPaths) {
    storage.push_back("-I");
    storage.push_back(path);
  }
  storage.insert(storage.end(), {sourcePath, "-o", libraryPath});
  std::vector<StringRef> arguments;
  arguments.reserve(storage.size());
  for (const std::string &argument : storage)
    arguments.push_back(argument);

  std::string executionError;
  int exitCode = llvm::sys::ExecuteAndWait(
      arguments.front(), arguments, /*Env=*/std::nullopt,
      {/*stdin=*/std::nullopt, /*stdout=*/diagnosticsPath,
       /*stderr=*/diagnosticsPath},
      /*SecondsToWait=*/0, /*MemoryLimit=*/0, &executionError);
  if (exitCode != 0) {
    std::string diagnostics = readFile(diagnosticsPath);
    if (!executionError.empty())
      diagnostics =
          executionError + (diagnostics.empty() ? "" : "\n") + diagnostics;
    return M::Error(
        "official Mojo GPU compilation failed" +
        (diagnostics.empty() ? std::string() : ":\n" + diagnostics));
  }

  void *library = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library)
    return M::Error(
        ("could not load official Mojo GPU artifact: " + std::string(dlerror()))
            .c_str());
  using Getter = const char *(*)(size_t *);
  auto data =
      reinterpret_cast<Getter>(dlsym(library, "__xmojo_gpu_artifact_data"));
  auto name =
      reinterpret_cast<Getter>(dlsym(library, "__xmojo_gpu_artifact_name"));
  if (!data || !name) {
    dlclose(library);
    return M::Error("official Mojo GPU artifact omitted its metadata exports");
  }

  size_t dataSize = 0;
  size_t nameSize = 0;
  const char *dataBytes = data(&dataSize);
  const char *nameBytes = name(&nameSize);
  GPUArtifact artifact{std::string(nameBytes, nameSize),
                       std::string(dataBytes, dataSize)};
  dlclose(library);
  if (!cachePath.empty()) {
    M::ErrorOr<std::string> finalFingerprintOr =
        getToolchainFingerprint(*compilerOr);
    if (!finalFingerprintOr.isError() &&
        *finalFingerprintOr == inputFingerprint)
      writeCacheEntry(cachePath, artifact);
  }
  return artifact;
}
