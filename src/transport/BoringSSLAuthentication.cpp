//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include "xauthentication.hpp"
#include "xeus/xstring_utils.hpp"

namespace xeus {

xraw_buffer::xraw_buffer(const unsigned char *data, size_t size)
    : m_data(data), m_size(size) {}

const unsigned char *xraw_buffer::data() const { return m_data; }

size_t xraw_buffer::size() const { return m_size; }

std::string xauthentication::sign(const xraw_buffer &header,
                                  const xraw_buffer &parentHeader,
                                  const xraw_buffer &metadata,
                                  const xraw_buffer &content) const {
  return sign_impl(header, parentHeader, metadata, content);
}

bool xauthentication::verify(const xraw_buffer &signature,
                             const xraw_buffer &header,
                             const xraw_buffer &parentHeader,
                             const xraw_buffer &metadata,
                             const xraw_buffer &content) const {
  return verify_impl(signature, header, parentHeader, metadata, content);
}

std::string xauthentication::sign(const xraw_buffer &content) const {
  return sign_impl(content);
}

bool xauthentication::verify(const xraw_buffer &signature,
                             const xraw_buffer &content) const {
  return verify_impl(signature, content);
}

namespace {

class NoAuthentication final : public xauthentication {
  std::string sign_impl(const xraw_buffer &, const xraw_buffer &,
                        const xraw_buffer &,
                        const xraw_buffer &) const override {
    return {};
  }

  bool verify_impl(const xraw_buffer &, const xraw_buffer &,
                   const xraw_buffer &, const xraw_buffer &,
                   const xraw_buffer &) const override {
    return true;
  }

  std::string sign_impl(const xraw_buffer &) const override { return {}; }

  bool verify_impl(const xraw_buffer &, const xraw_buffer &) const override {
    return true;
  }
};

class HmacSha256Authentication final : public xauthentication {
public:
  explicit HmacSha256Authentication(std::string key)
      : key(std::move(key)), context(HMAC_CTX_new(), HMAC_CTX_free) {
    if (!context)
      throw std::runtime_error("could not allocate BoringSSL HMAC context");
  }

private:
  std::string
  signBuffers(std::initializer_list<const xraw_buffer *> buffers) const {
    std::lock_guard lock(mutex);
    if (!HMAC_Init_ex(context.get(), key.data(), key.size(), EVP_sha256(),
                      nullptr))
      throw std::runtime_error("could not initialize BoringSSL HMAC");
    for (const auto *buffer : buffers) {
      if (!HMAC_Update(context.get(), buffer->data(), buffer->size()))
        throw std::runtime_error("could not update BoringSSL HMAC");
    }

    std::vector<unsigned char> digest(EVP_MAX_MD_SIZE);
    unsigned int size = 0;
    if (!HMAC_Final(context.get(), digest.data(), &size))
      throw std::runtime_error("could not finalize BoringSSL HMAC");
    digest.resize(size);
    return hex_string(digest);
  }

  static bool matches(const xraw_buffer &signature,
                      const std::string &expected) {
    return signature.size() == expected.size() &&
           CRYPTO_memcmp(signature.data(), expected.data(), expected.size()) ==
               0;
  }

  std::string sign_impl(const xraw_buffer &header,
                        const xraw_buffer &parentHeader,
                        const xraw_buffer &metadata,
                        const xraw_buffer &content) const override {
    return signBuffers({&header, &parentHeader, &metadata, &content});
  }

  bool verify_impl(const xraw_buffer &signature, const xraw_buffer &header,
                   const xraw_buffer &parentHeader, const xraw_buffer &metadata,
                   const xraw_buffer &content) const override {
    return matches(signature,
                   signBuffers({&header, &parentHeader, &metadata, &content}));
  }

  std::string sign_impl(const xraw_buffer &content) const override {
    return signBuffers({&content});
  }

  bool verify_impl(const xraw_buffer &signature,
                   const xraw_buffer &content) const override {
    return matches(signature, signBuffers({&content}));
  }

  std::string key;
  std::unique_ptr<HMAC_CTX, decltype(&HMAC_CTX_free)> context;
  mutable std::mutex mutex;
};

} // namespace

std::unique_ptr<xauthentication> make_xauthentication(const std::string &scheme,
                                                      const std::string &key) {
  if (scheme == "none")
    return std::make_unique<NoAuthentication>();
  if (scheme == "hmac-sha256")
    return std::make_unique<HmacSha256Authentication>(key);
  throw std::invalid_argument("unsupported Jupyter authentication scheme: " +
                              scheme);
}

} // namespace xeus
