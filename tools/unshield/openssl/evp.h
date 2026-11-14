/* The part of OpenSSL's EVP digest interface that unshield 1.6.2's lib/file.c uses, answered
 * by CommonCrypto's MD5 so that neither OpenSSL nor a reference MD5 is needed. Part of the BW2
 * port (MIT); written from the calls' documented behaviour, not copied from OpenSSL.
 * The calls (lib/file.c, extracting or testing one file): EVP_MD_CTX_new, EVP_MD_CTX_init,
 * EVP_DigestInit_ex(ctx, EVP_md5(), NULL), EVP_DigestUpdate, EVP_DigestFinal_ex and, depending
 * on the config, EVP_MD_CTX_free. Every call accepts a NULL context (a failed allocation) and
 * then returns failure, as OpenSSL does. */
#ifndef BW2_UNSHIELD_OPENSSL_EVP_H
#define BW2_UNSHIELD_OPENSSL_EVP_H

#include <CommonCrypto/CommonDigest.h>
#include <stdlib.h>
#include <string.h>

typedef CC_MD5_CTX EVP_MD_CTX;
typedef struct bw2_evp_md { int md5; } EVP_MD;
typedef void ENGINE;

static const EVP_MD bw2_evp_md5 = { 1 };

static inline const EVP_MD *EVP_md5(void) { return &bw2_evp_md5; }

static inline EVP_MD_CTX *EVP_MD_CTX_new(void) { return (EVP_MD_CTX *)calloc(1, sizeof(EVP_MD_CTX)); }

static inline void EVP_MD_CTX_free(EVP_MD_CTX *ctx) { free(ctx); }

static inline int EVP_MD_CTX_init(EVP_MD_CTX *ctx)
{
    if (!ctx) return 0;
    memset(ctx, 0, sizeof *ctx);
    return 1;
}

static inline int EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *type, ENGINE *impl)
{
    (void)impl;
    if (!ctx || type != &bw2_evp_md5) return 0;
    return CC_MD5_Init(ctx);
}

static inline int EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    if (!ctx) return 0;
    while (len > 0) {                       /* CC_LONG is 32-bit */
        CC_LONG n = len > 0x40000000u ? 0x40000000u : (CC_LONG)len;
        CC_MD5_Update(ctx, p, n);
        p += n;
        len -= n;
    }
    return 1;
}

static inline int EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *s)
{
    if (!ctx) return 0;
    CC_MD5_Final(md, ctx);
    if (s) *s = CC_MD5_DIGEST_LENGTH;
    return 1;
}

#endif
