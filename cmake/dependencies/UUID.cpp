#include <uuid/uuid.h>

#include <cstdlib>

#include <openssl/rand.h>

extern "C" void uuid_generate(uuid_t output) {
  if (RAND_bytes(output, 16) != 1)
    std::abort();
  output[6] = static_cast<unsigned char>((output[6] & 0x0f) | 0x40);
  output[8] = static_cast<unsigned char>((output[8] & 0x3f) | 0x80);
}
