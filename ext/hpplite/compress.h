/*
** HPPLite batch compression (#12): deflate (zlib) on the batch JSON before
** it is sealed or posted. SQL and row records shrink five to twelve times.
**
** The posted form: a sealed envelope carries HPPLITE_ENCODING_DEFLATE in
** its encoding byte and the compressed bytes inside; a plaintext batch
** (encrypt=off) is framed as HPPLITE_PLAIN_MAGIC ("HPZ1") + the compressed
** bytes. Compressed bytes are a 4-byte big-endian original length followed
** by a zlib stream. A batch that would not shrink is posted as before, so
** every reader keeps accepting plain JSON and HPPLITE_ENCODING_NONE.
*/
#ifndef HPPLITE_COMPRESS_H
#define HPPLITE_COMPRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HPPLITE_PLAIN_MAGIC "HPZ1"
#define HPPLITE_PLAIN_MAGIC_LEN 4
#define HPPLITE_INFLATE_MAX (256u * 1024 * 1024)   /* an original length above this is refused, not allocated */

/* Compress: malloc'd [length][zlib]; NULL on failure or for empty input */
uint8_t *hpplite_deflate(const uint8_t *in, size_t n, size_t *pOut);

/* Decompress what hpplite_deflate made: malloc'd, terminated, *pOut set; NULL when malformed or over the limit */
uint8_t *hpplite_inflate(const uint8_t *in, size_t n, size_t *pOut);

/* Is this a plaintext compressed batch (the HPZ1 frame)? */
int hpplite_is_plain_compressed(const uint8_t *data, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_COMPRESS_H */
