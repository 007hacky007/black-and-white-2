/* unshield 1.6.2's lib/unshield_config.h for macOS (arm64, macOS 13+), written by hand in
 * place of CMake's configure_file of lib/unshield_config.h.in. Part of the BW2 port (MIT);
 * not a copy of the template. Every value is what the template gives on this platform with
 * OpenSSL found (USE_OUR_OWN_MD5 off): MD5 then goes through <openssl/evp.h>, which is our
 * CommonCrypto shim in tools/unshield/openssl/evp.h. */

#define HAVE_DLFCN_H 1
#define HAVE_INTTYPES_H 1
/* HAVE_MEMORY_H: unused by the sources, left undefined. */
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1

#define PACKAGE "unshield"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_NAME "unshield"
#define PACKAGE_STRING "unshield 1.6.2"
#define PACKAGE_TARNAME "unshield"
#define PACKAGE_URL ""
#define PACKAGE_VERSION "1.6.2"

#define SIZE_FORMAT "zi"

#define HAVE_FNMATCH 1
#define HAVE_ICONV 1

/* As CMake renders "#cmakedefine01 USE_OUR_OWN_MD5" with the option off. lib/file.c tests it
 * with "#if" (EVP path taken) and once with "#ifndef" around EVP_MD_CTX_free, which this
 * definition skips: one small context per file is not freed, as in a CMake build. */
#define USE_OUR_OWN_MD5 0

#define VERSION "1.6.2"
