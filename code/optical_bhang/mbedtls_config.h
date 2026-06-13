#pragma once
/* Minimal mbedTLS 3.x config for Pico W HTTPS client (TLS 1.2, GitHub API).
 * Do NOT manually include mbedtls/check_config.h — it is included automatically
 * at the right point since mbedTLS 3.0. */

/* Platform */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
/* Allow direct struct member access for Pico SDK altcp_tls_mbedtls.c which
 * was written against the mbedTLS 2.x public API. In mbedTLS 3.x, members
 * like ssl_context.out_left and ssl_session.start are private by default. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
/* Enables mbedtls_ssl_session.start (session start timestamp, used by altcp). */
#define MBEDTLS_HAVE_TIME
/* Provide mbedtls_ms_time() via the Pico SDK's pico_mbedtls (uses time_us_64).
 * Without this, platform_util.c errors because the RP2 isn't unix/win. */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* Hashes — SHA-512 required by mbedTLS 3.x entropy accumulator */
#define MBEDTLS_MD5_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_C

/* Symmetric ciphers */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC

/* Asymmetric crypto */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15

/* ECC (required for ECDHE key exchange) */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C

/* Key exchange */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/* X.509 / certificate chain */
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_MAX_CONTENT_LEN         4096

/* RNG */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
