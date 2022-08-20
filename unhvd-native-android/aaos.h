#ifndef AAOS_H
#define AAOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct aaos;

struct aaos* aaos_init();

void aaos_close(struct aaos* a);

// push the samples directly to the buffer
int32_t aaos_write(struct aaos* a, const int16_t* buffer, const int32_t buflen);

#ifdef __cplusplus
}
#endif

#endif