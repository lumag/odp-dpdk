/* Copyright (c) 2014-2018, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */
#ifndef TEST_VECTORS_LEN_
#define TEST_VECTORS_LEN_

/* Maximum */
#define MAX_KEY_LEN         64
#define MAX_IV_LEN          16
#define MAX_DATA_LEN        270
#define MAX_AAD_LEN         12
#define MAX_DIGEST_LEN      64

/* TDES-CBC */
#define TDES_CBC_KEY_LEN        24
#define TDES_CBC_IV_LEN         8

/* AES common */

#define AES128_KEY_LEN        16

#define AES192_KEY_LEN        24

#define AES256_KEY_LEN        32

/* AES-CBC */
#define AES_CBC_IV_LEN         16

/* AES-CTR */
#define AES_CTR_IV_LEN         16

/* AES-GCM */
#define AES_GCM_IV_LEN         12
#define AES_GCM_DIGEST_LEN     16

/* HMAC-MD5 */
#define HMAC_MD5_KEY_LEN        16
#define HMAC_MD5_96_CHECK_LEN   12
#define HMAC_MD5_CHECK_LEN      16

/* HMAC-SHA256 */
#define HMAC_SHA256_KEY_LEN        32
#define HMAC_SHA256_128_CHECK_LEN  16
#define HMAC_SHA256_CHECK_LEN      32

/* HMAC-SHA1 */
#define HMAC_SHA1_KEY_LEN        20
#define HMAC_SHA1_96_CHECK_LEN   12
#define HMAC_SHA1_CHECK_LEN      20

/* HMAC-SHA384 */
#define HMAC_SHA384_KEY_LEN        48
#define HMAC_SHA384_192_CHECK_LEN  24
#define HMAC_SHA384_CHECK_LEN      48

/* HMAC-SHA512 */
#define HMAC_SHA512_KEY_LEN        64
#define HMAC_SHA512_256_CHECK_LEN  32
#define HMAC_SHA512_CHECK_LEN      64

/* ChaCha20-Poly1305 */
#define CHACHA20_POLY1305_KEY_LEN  32
#define CHACHA20_POLY1305_IV_LEN   12
#define CHACHA20_POLY1305_CHECK_LEN 16

/* AES-XCBC-MAC */
#define AES_XCBC_MAC_KEY_LEN	   16
#define AES_XCBC_MAC_96_CHECK_LEN  12
#define AES_XCBC_MAC_CHECK_LEN     16

/* KASUMI_F8 */
#define KASUMI_F8_KEY_LEN        16
#define KASUMI_F8_IV_LEN         8

/* SNOW3G_UEA2 */
#define SNOW3G_UEA2_KEY_LEN      16
#define SNOW3G_UEA2_IV_LEN       16

/* ZUC_EEA3 */
#define ZUC_EEA3_KEY_LEN         16
#define ZUC_EEA3_IV_LEN          16

/* KASUMI_F9 */
#define KASUMI_F9_KEY_LEN        16
#define KASUMI_F9_IV_LEN         9
#define KASUMI_F9_DIGEST_LEN     4

/* SNOW3G_UIA2 */
#define SNOW3G_UIA2_KEY_LEN      16
#define SNOW3G_UIA2_IV_LEN       16
#define SNOW3G_UIA2_DIGEST_LEN   4

/* ZUC_EIA3 */
#define ZUC_EIA3_KEY_LEN         16
#define ZUC_EIA3_IV_LEN          16
#define ZUC_EIA3_DIGEST_LEN      4

#endif
