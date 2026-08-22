#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char uuid_t[16];

void uuid_generate(uuid_t output);

#ifdef __cplusplus
}
#endif
