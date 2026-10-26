#ifndef _INCLUDE_LDPC_H_
#define _INCLUDE_LDPC_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// codeword is 174 log-likelihoods.
// plain is a return value, 174 ints, to be 0 or 1.
// iters is how hard to try.
// ok == 87 means success.
void ldpc_decode(float codeword[], int max_iters, uint8_t plain[], int* ok);

/// Belief propagation (sum-product, ft8_lib's fast tanh/atanh). VibeSDR (2026-10-25): bp_decode is the fast one — the
/// same arithmetic in the same order as bp_decode_ref (ft8_lib's original), laid out by edge and four checks or four
/// variables per SIMD vector; bp_decode_ref stays as the reference the test holds it to (test-ft8-fast).
void bp_decode(float codeword[], int max_iters, uint8_t plain[], int* ok);
void bp_decode_ex(float codeword[], int max_iters, int stall, uint8_t plain[], int* ok);
void bp_decode_ref(float codeword[], int max_iters, uint8_t plain[], int* ok);

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_LDPC_H_
