/*
 * gowa_lle.h -- the libsre exports this port runs as the lifted firmware code.
 *
 * `scripts/gowa.py lle` writes firmware/libsre/lifted/gowa_libsre_exports.c,
 * which defines these; stubs.cpp registers one HLE handler per entry that calls
 * into the lifted function.
 */
#ifndef GOWA_LLE_H
#define GOWA_LLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t    nid;
    uint32_t    opd;    /* guest address of the export's function descriptor */
    const char* name;
} gowa_lle_export;

extern const gowa_lle_export gowa_libsre_lle[];
extern const uint32_t          gowa_libsre_lle_count;
extern const uint32_t          gowa_libsre_base;
/* Code address of libsre's _cellSpursSendSignal; stubs.cpp bridges it to HLE. */
extern const uint32_t          gowa_libsre_send_signal_code;
/* Code address of the internal hook CompletePushPointer[2] calls to wake a
 * queue's waiter (the function pointer PushBody loads from its TOC). */
extern const uint32_t          gowa_libsre_lfqueue_signal_code;

#ifdef __cplusplus
}
#endif

#endif /* GOWA_LLE_H */
