/* Copyright (c) 2017-2018, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include "config.h"

#include <odp_posix_extensions.h>
#include <odp/api/crypto.h>
#include <odp_internal.h>
#include <odp/api/atomic.h>
#include <odp/api/spinlock.h>
#include <odp/api/sync.h>
#include <odp/api/debug.h>
#include <odp/api/align.h>
#include <odp/api/shared_memory.h>
#include <odp_debug_internal.h>
#include <odp/api/hints.h>
#include <odp/api/random.h>
#include <odp/api/packet.h>
#include <odp/api/plat/packet_inlines.h>
#include <odp_packet_internal.h>

/* Inlined API functions */
#include <odp/api/plat/event_inlines.h>

#include <rte_config.h>
#include <rte_crypto.h>
#include <rte_cryptodev.h>

#include <string.h>
#include <math.h>

/* default number supported by DPDK crypto */
#define MAX_SESSIONS 2048
#define NB_MBUF  8192
#define NB_DESC_PER_QUEUE_PAIR  4096
#define MAX_IV_LENGTH 16
#define AES_CCM_AAD_OFFSET 18
#define IV_OFFSET	(sizeof(struct rte_crypto_op) + \
			 sizeof(struct rte_crypto_sym_op))

/* Max number of rte_cryptodev_dequeue_burst() retries (1 usec wait between
 * retries). */
#define MAX_DEQ_RETRIES 100000

typedef struct crypto_session_entry_s {
	struct crypto_session_entry_s *next;

	/* Session creation parameters */
	odp_crypto_session_param_t p;
	struct rte_cryptodev_sym_session *rte_session;
	struct rte_crypto_sym_xform cipher_xform;
	struct rte_crypto_sym_xform auth_xform;
	uint16_t cdev_nb_qpairs;
	uint8_t cdev_id;
	uint8_t cipher_iv_data[MAX_IV_LENGTH];
	uint8_t auth_iv_data[MAX_IV_LENGTH];
} crypto_session_entry_t;

typedef struct crypto_global_s {
	odp_spinlock_t                lock;
	uint8_t enabled_crypto_devs;
	uint8_t enabled_crypto_dev_ids[RTE_CRYPTO_MAX_DEVS];
	uint16_t enabled_crypto_dev_nb_qpairs[RTE_CRYPTO_MAX_DEVS];
	crypto_session_entry_t *free;
	crypto_session_entry_t sessions[MAX_SESSIONS];
	int is_crypto_dev_initialized;
	struct rte_mempool *crypto_op_pool;
	struct rte_mempool *session_mempool[RTE_MAX_NUMA_NODES];
} crypto_global_t;

static crypto_global_t *global;
static odp_shm_t crypto_global_shm;

static inline int is_valid_size(uint16_t length,
				const struct rte_crypto_param_range *range)
{
	uint16_t supp_size;

	if (length < range->min)
		return -1;

	if (range->min != length && range->increment == 0)
		return -1;

	for (supp_size = range->min;
	     supp_size <= range->max;
	     supp_size += range->increment) {
		if (length == supp_size)
			return 0;
	}

	return -1;
}

static int cipher_is_bit_mode(odp_cipher_alg_t cipher_alg)
{
	switch (cipher_alg) {
	case ODP_CIPHER_ALG_KASUMI_F8:
	case ODP_CIPHER_ALG_SNOW3G_UEA2:
	case ODP_CIPHER_ALG_ZUC_EEA3:
		return 1;
	default:
		return 0;
	}
}

static int auth_is_bit_mode(odp_auth_alg_t auth_alg)
{
	switch (auth_alg) {
	case ODP_AUTH_ALG_KASUMI_F9:
	case ODP_AUTH_ALG_SNOW3G_UIA2:
	case ODP_AUTH_ALG_ZUC_EIA3:
		return 1;
	default:
		return 0;
	}
}

static int cipher_is_aead(odp_cipher_alg_t cipher_alg)
{
	switch (cipher_alg) {
	case ODP_CIPHER_ALG_AES_GCM:
	case ODP_CIPHER_ALG_AES_CCM:
#if ODP_DEPRECATED_API
	case ODP_CIPHER_ALG_AES128_GCM:
#endif
		return 1;
	default:
		return 0;
	}
}

static int auth_is_aead(odp_auth_alg_t auth_alg)
{
	switch (auth_alg) {
	case ODP_AUTH_ALG_AES_GCM:
	case ODP_AUTH_ALG_AES_CCM:
#if ODP_DEPRECATED_API
	case ODP_AUTH_ALG_AES128_GCM:
#endif
		return 1;
	default:
		return 0;
	}
}

static int cipher_aead_alg_odp_to_rte(odp_cipher_alg_t cipher_alg,
				      struct rte_crypto_sym_xform *aead_xform)
{
	int rc = 0;

	switch (cipher_alg) {
	case ODP_CIPHER_ALG_AES_GCM:
#if ODP_DEPRECATED_API
	case ODP_CIPHER_ALG_AES128_GCM:
#endif
		aead_xform->aead.algo = RTE_CRYPTO_AEAD_AES_GCM;
		break;
	case ODP_CIPHER_ALG_AES_CCM:
		aead_xform->aead.algo = RTE_CRYPTO_AEAD_AES_CCM;
		break;
	default:
		rc = -1;
	}

	return rc;
}

static int auth_aead_alg_odp_to_rte(odp_auth_alg_t auth_alg,
				    struct rte_crypto_sym_xform *aead_xform)
{
	int rc = 0;

	switch (auth_alg) {
	case ODP_AUTH_ALG_AES_GCM:
#if ODP_DEPRECATED_API
	case ODP_AUTH_ALG_AES128_GCM:
#endif
		aead_xform->aead.algo = RTE_CRYPTO_AEAD_AES_GCM;
		break;
	case ODP_AUTH_ALG_AES_CCM:
		aead_xform->aead.algo = RTE_CRYPTO_AEAD_AES_CCM;
		break;
	default:
		rc = -1;
	}

	return rc;
}

static int cipher_alg_odp_to_rte(odp_cipher_alg_t cipher_alg,
				 struct rte_crypto_sym_xform *cipher_xform)
{
	int rc = 0;

	switch (cipher_alg) {
	case ODP_CIPHER_ALG_NULL:
		cipher_xform->cipher.algo = RTE_CRYPTO_CIPHER_NULL;
		break;
	case ODP_CIPHER_ALG_DES:
	case ODP_CIPHER_ALG_3DES_CBC:
		cipher_xform->cipher.algo = RTE_CRYPTO_CIPHER_3DES_CBC;
		break;
	case ODP_CIPHER_ALG_AES_CBC:
#if ODP_DEPRECATED_API
	case ODP_CIPHER_ALG_AES128_CBC:
#endif
		cipher_xform->cipher.algo = RTE_CRYPTO_CIPHER_AES_CBC;
		break;
	case ODP_CIPHER_ALG_AES_CTR:
		cipher_xform->cipher.algo = RTE_CRYPTO_CIPHER_AES_CTR;
		break;
	case ODP_CIPHER_ALG_KASUMI_F8:
		cipher_xform->cipher.algo = RTE_CRYPTO_CIPHER_KASUMI_F8;
		break;
	case ODP_CIPHER_ALG_SNOW3G_UEA2:
		cipher_xform->cipher.algo = RTE_CRYPTO_CIPHER_SNOW3G_UEA2;
		break;
	case ODP_CIPHER_ALG_ZUC_EEA3:
		cipher_xform->cipher.algo = RTE_CRYPTO_CIPHER_ZUC_EEA3;
		break;
	default:
		rc = -1;
	}

	return rc;
}

static int auth_alg_odp_to_rte(odp_auth_alg_t auth_alg,
			       struct rte_crypto_sym_xform *auth_xform)
{
	int rc = 0;

	/* Process based on auth */
	switch (auth_alg) {
	case ODP_AUTH_ALG_NULL:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_NULL;
		break;
	case ODP_AUTH_ALG_MD5_HMAC:
#if ODP_DEPRECATED_API
	case ODP_AUTH_ALG_MD5_96:
#endif
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_MD5_HMAC;
		break;
	case ODP_AUTH_ALG_SHA256_HMAC:
#if ODP_DEPRECATED_API
	case ODP_AUTH_ALG_SHA256_128:
#endif
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_SHA256_HMAC;
		break;
	case ODP_AUTH_ALG_SHA1_HMAC:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_SHA1_HMAC;
		break;
	case ODP_AUTH_ALG_SHA512_HMAC:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_SHA512_HMAC;
		break;
	case ODP_AUTH_ALG_AES_GMAC:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_AES_GMAC;
		break;
	case ODP_AUTH_ALG_AES_CMAC:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_AES_CMAC;
		break;
	case ODP_AUTH_ALG_KASUMI_F9:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_KASUMI_F9;
		break;
	case ODP_AUTH_ALG_SNOW3G_UIA2:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_SNOW3G_UIA2;
		break;
	case ODP_AUTH_ALG_ZUC_EIA3:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_ZUC_EIA3;
		break;
	default:
		rc = -1;
	}

	return rc;
}

static crypto_session_entry_t *alloc_session(void)
{
	crypto_session_entry_t *session = NULL;

	odp_spinlock_lock(&global->lock);
	session = global->free;
	if (session) {
		global->free = session->next;
		session->next = NULL;
	}
	odp_spinlock_unlock(&global->lock);

	return session;
}

static void free_session(crypto_session_entry_t *session)
{
	odp_spinlock_lock(&global->lock);
	session->next = global->free;
	global->free = session;
	odp_spinlock_unlock(&global->lock);
}

int odp_crypto_init_global(void)
{
	size_t mem_size;
	int idx;
	int16_t cdev_id, cdev_count;
	int rc = -1;
	unsigned int cache_size = 0;
	unsigned int nb_queue_pairs = 0, queue_pair;
	uint32_t max_sess_sz = 0, sess_sz;

	/* Calculate the memory size we need */
	mem_size  = sizeof(*global);
	mem_size += (MAX_SESSIONS * sizeof(crypto_session_entry_t));

	/* Allocate our globally shared memory */
	crypto_global_shm = odp_shm_reserve("crypto_pool", mem_size,
					    ODP_CACHE_LINE_SIZE, 0);

	if (crypto_global_shm != ODP_SHM_INVALID) {
		global = odp_shm_addr(crypto_global_shm);

		if (global == NULL) {
			ODP_ERR("Failed to find the reserved shm block");
			return -1;
		}
	} else {
		ODP_ERR("Shared memory reserve failed.\n");
		return -1;
	}

	/* Clear it out */
	memset(global, 0, mem_size);

	/* Initialize free list and lock */
	for (idx = 0; idx < MAX_SESSIONS; idx++) {
		global->sessions[idx].next = global->free;
		global->free = &global->sessions[idx];
	}

	global->enabled_crypto_devs = 0;
	odp_spinlock_init(&global->lock);

	odp_spinlock_lock(&global->lock);
	if (global->is_crypto_dev_initialized)
		return 0;

	if (RTE_MEMPOOL_CACHE_MAX_SIZE > 0) {
		unsigned int j;

		j = ceil((double)NB_MBUF / RTE_MEMPOOL_CACHE_MAX_SIZE);
		j = RTE_MAX(j, 2UL);
		for (; j <= (NB_MBUF / 2); ++j)
			if ((NB_MBUF % j) == 0) {
				cache_size = NB_MBUF / j;
				break;
			}
		if (odp_unlikely(cache_size > RTE_MEMPOOL_CACHE_MAX_SIZE ||
				 (uint32_t)cache_size * 1.5 > NB_MBUF)) {
			ODP_ERR("cache_size calc failure: %d\n", cache_size);
			cache_size = 0;
		}
	}

	cdev_count = rte_cryptodev_count();
	if (cdev_count == 0) {
		printf("No crypto devices available\n");
		return 0;
	}

	for (cdev_id = 0; cdev_id < rte_cryptodev_count(); cdev_id++) {
		sess_sz = rte_cryptodev_get_private_session_size(cdev_id);
		if (sess_sz > max_sess_sz)
			max_sess_sz = sess_sz;
	}

	for (cdev_id = cdev_count - 1; cdev_id >= 0; cdev_id--) {
		struct rte_cryptodev_info dev_info;

		rte_cryptodev_info_get(cdev_id, &dev_info);
		nb_queue_pairs = odp_cpu_count();
		if (nb_queue_pairs > dev_info.max_nb_queue_pairs)
			nb_queue_pairs = dev_info.max_nb_queue_pairs;

		struct rte_cryptodev_qp_conf qp_conf;
		uint8_t socket_id = rte_cryptodev_socket_id(cdev_id);

		struct rte_cryptodev_config conf = {
			.nb_queue_pairs = nb_queue_pairs,
			.socket_id = socket_id,
		};

		if (global->session_mempool[socket_id] == NULL) {
			char mp_name[RTE_MEMPOOL_NAMESIZE];
			struct rte_mempool *sess_mp;

			snprintf(mp_name, RTE_MEMPOOL_NAMESIZE,
				 "sess_mp_%u", socket_id);

			/*
			 * Create enough objects for session headers and
			 * device private data
			 */
			sess_mp = rte_mempool_create(mp_name,
						     NB_MBUF,
						     max_sess_sz,
						     cache_size,
						     0, NULL, NULL, NULL,
						     NULL, socket_id,
						     0);

			if (sess_mp == NULL) {
				ODP_ERR("Cannot create session pool on socket %d\n",
					socket_id);
				return -1;
			}

			printf("Allocated session pool on socket %d\n",
			       socket_id);
			global->session_mempool[socket_id] = sess_mp;
		}

		rc = rte_cryptodev_configure(cdev_id, &conf);
		if (rc < 0) {
			ODP_ERR("Failed to configure cryptodev %u", cdev_id);
			return -1;
		}

		qp_conf.nb_descriptors = NB_DESC_PER_QUEUE_PAIR;

		for (queue_pair = 0; queue_pair < nb_queue_pairs;
							queue_pair++) {
			struct rte_mempool *crypto_pool =
				global->session_mempool[socket_id];
			rc = rte_cryptodev_queue_pair_setup(cdev_id,
							    queue_pair,
							    &qp_conf,
							    socket_id,
							    crypto_pool);
			if (rc < 0) {
				ODP_ERR("Fail to setup queue pair %u on dev %u",
					queue_pair, cdev_id);
				return -1;
			}
		}

		rc = rte_cryptodev_start(cdev_id);
		if (rc < 0) {
			ODP_ERR("Failed to start device %u: error %d\n",
				cdev_id, rc);
			return -1;
		}

		global->enabled_crypto_dev_ids[global->enabled_crypto_devs] =
			cdev_id;
		global->enabled_crypto_dev_nb_qpairs[global->enabled_crypto_devs] =
			nb_queue_pairs;
		global->enabled_crypto_devs++;
	}

	/* create crypto op pool */
	global->crypto_op_pool =
		rte_crypto_op_pool_create("crypto_op_pool",
					  RTE_CRYPTO_OP_TYPE_SYMMETRIC,
					  NB_MBUF, cache_size,
					  2 * MAX_IV_LENGTH,
					  rte_socket_id());

	if (global->crypto_op_pool == NULL) {
		ODP_ERR("Cannot create crypto op pool\n");
		return -1;
	}

	global->is_crypto_dev_initialized = 1;
	odp_spinlock_unlock(&global->lock);

	return 0;
}

int _odp_crypto_init_local(void)
{
	return 0;
}

int _odp_crypto_term_local(void)
{
	return 0;
}

static void capability_process(struct rte_cryptodev_info *dev_info,
			       odp_crypto_cipher_algos_t *ciphers,
			       odp_crypto_auth_algos_t *auths)
{
	const struct rte_cryptodev_capabilities *cap;

	/* NULL is always supported, it is done in software */
	ciphers->bit.null = 1;
	auths->bit.null = 1;

	for (cap = &dev_info->capabilities[0];
	     cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED;
	     cap++) {
		if (cap->sym.xform_type == RTE_CRYPTO_SYM_XFORM_CIPHER) {
			enum rte_crypto_cipher_algorithm cap_cipher_algo;

			cap_cipher_algo = cap->sym.cipher.algo;
			if (cap_cipher_algo == RTE_CRYPTO_CIPHER_3DES_CBC) {
				ciphers->bit.trides_cbc = 1;
				ciphers->bit.des = 1;
			}
			if (cap_cipher_algo == RTE_CRYPTO_CIPHER_AES_CBC) {
				ciphers->bit.aes_cbc = 1;
#if ODP_DEPRECATED_API
				ciphers->bit.aes128_cbc = 1;
#endif
			}
			if (cap_cipher_algo == RTE_CRYPTO_CIPHER_AES_CTR)
				ciphers->bit.aes_ctr = 1;
			if (cap_cipher_algo == RTE_CRYPTO_CIPHER_KASUMI_F8)
				ciphers->bit.kasumi_f8 = 1;
			if (cap_cipher_algo == RTE_CRYPTO_CIPHER_SNOW3G_UEA2)
				ciphers->bit.snow3g_uea2 = 1;
			if (cap_cipher_algo == RTE_CRYPTO_CIPHER_ZUC_EEA3)
				ciphers->bit.zuc_eea3 = 1;
		}

		if (cap->sym.xform_type == RTE_CRYPTO_SYM_XFORM_AUTH) {
			enum rte_crypto_auth_algorithm cap_auth_algo;

			cap_auth_algo = cap->sym.auth.algo;
			if (cap_auth_algo == RTE_CRYPTO_AUTH_MD5_HMAC) {
				auths->bit.md5_hmac = 1;
#if ODP_DEPRECATED_API
				auths->bit.md5_96 = 1;
#endif
			}
			if (cap_auth_algo == RTE_CRYPTO_AUTH_SHA256_HMAC) {
				auths->bit.sha256_hmac = 1;
#if ODP_DEPRECATED_API
				auths->bit.sha256_128 = 1;
#endif
			}
			if (cap_auth_algo == RTE_CRYPTO_AUTH_SHA1_HMAC)
				auths->bit.sha1_hmac = 1;
			if (cap_auth_algo == RTE_CRYPTO_AUTH_SHA512_HMAC)
				auths->bit.sha512_hmac = 1;
			if (cap_auth_algo == RTE_CRYPTO_AUTH_AES_GMAC)
				auths->bit.aes_gmac = 1;
			if (cap_auth_algo == RTE_CRYPTO_AUTH_AES_CMAC)
				auths->bit.aes_cmac = 1;
			/* KASUMI_F9 disabled for now because DPDK requires IV
			 * to part of the packet, while ODP insists on IV being
			 * present in iv part of operation
			if (cap_auth_algo == RTE_CRYPTO_AUTH_KASUMI_F9)
				auths->bit.kasumi_f9 = 1;
				*/
			if (cap_auth_algo == RTE_CRYPTO_AUTH_SNOW3G_UIA2)
				auths->bit.snow3g_uia2 = 1;
			if (cap_auth_algo == RTE_CRYPTO_AUTH_ZUC_EIA3)
				auths->bit.zuc_eia3 = 1;
		}

		if (cap->sym.xform_type == RTE_CRYPTO_SYM_XFORM_AEAD) {
			enum rte_crypto_aead_algorithm cap_aead_algo;

			cap_aead_algo = cap->sym.aead.algo;
			if (cap_aead_algo == RTE_CRYPTO_AEAD_AES_GCM) {
				ciphers->bit.aes_gcm = 1;
				auths->bit.aes_gcm = 1;
#if ODP_DEPRECATED_API
				ciphers->bit.aes128_gcm = 1;
				auths->bit.aes128_gcm = 1;
#endif
			}
			/* AES-CCM algorithm produces errors in Ubuntu Trusty,
			 * so it is disabled for now
			if (cap_aead_algo == RTE_CRYPTO_AEAD_AES_CCM) {
				ciphers->bit.aes_ccm = 1;
				auths->bit.aes_ccm = 1;
			}
			*/
		}
	}
}

int odp_crypto_capability(odp_crypto_capability_t *capability)
{
	uint8_t cdev_id, cdev_count;

	if (NULL == capability)
		return -1;

	/* Initialize crypto capability structure */
	memset(capability, 0, sizeof(odp_crypto_capability_t));

	capability->sync_mode = ODP_SUPPORT_YES;
	capability->async_mode = ODP_SUPPORT_PREFERRED;

	cdev_count = rte_cryptodev_count();
	if (cdev_count == 0) {
		ODP_ERR("No crypto devices available\n");
		return -1;
	}

	for (cdev_id = 0; cdev_id < cdev_count; cdev_id++) {
		struct rte_cryptodev_info dev_info;

		rte_cryptodev_info_get(cdev_id, &dev_info);
		capability_process(&dev_info, &capability->ciphers,
				   &capability->auths);
		if ((dev_info.feature_flags &
		     RTE_CRYPTODEV_FF_HW_ACCELERATED)) {
			capability->hw_ciphers = capability->ciphers;
			capability->hw_auths = capability->auths;
		}

		/* Read from the device with the lowest max_nb_sessions */
		if (capability->max_sessions > dev_info.sym.max_nb_sessions)
			capability->max_sessions = dev_info.sym.max_nb_sessions;

		if (capability->max_sessions == 0)
			capability->max_sessions = dev_info.sym.max_nb_sessions;
	}

	/* Make sure the session count doesn't exceed MAX_SESSIONS */
	if (capability->max_sessions > MAX_SESSIONS)
		capability->max_sessions = MAX_SESSIONS;

	return 0;
}

static int cipher_gen_capability(const struct rte_crypto_param_range *key_size,
				 const struct rte_crypto_param_range *iv_size,
				 odp_bool_t bit_mode,
				 odp_crypto_cipher_capability_t *src,
				 int num_copy)
{
	int idx = 0;

	uint32_t key_size_min = key_size->min;
	uint32_t key_size_max = key_size->max;
	uint32_t key_inc = key_size->increment;
	uint32_t iv_size_max = iv_size->max;
	uint32_t iv_size_min = iv_size->min;
	uint32_t iv_inc = iv_size->increment;

	for (uint32_t key_len = key_size_min; key_len <= key_size_max;
	     key_len += key_inc) {
		for (uint32_t iv_size = iv_size_min;
		     iv_size <= iv_size_max; iv_size += iv_inc) {
			if (idx < num_copy) {
				src[idx].key_len = key_len;
				src[idx].iv_len = iv_size;
				src[idx].bit_mode = bit_mode;
			}
			idx++;

			if (iv_inc == 0)
				break;
		}

		if (key_inc == 0)
			break;
	}

	return idx;
}

static int cipher_aead_capability(odp_cipher_alg_t cipher,
				  odp_crypto_cipher_capability_t dst[],
				  int num_copy)
{
	odp_crypto_cipher_capability_t src[num_copy];
	int idx = 0, rc = 0;
	int size = sizeof(odp_crypto_cipher_capability_t);

	uint8_t cdev_id, cdev_count;
	const struct rte_cryptodev_capabilities *cap;
	struct rte_crypto_sym_xform aead_xform;

	rc = cipher_aead_alg_odp_to_rte(cipher, &aead_xform);

	/* Check result */
	if (rc)
		return -1;

	cdev_count = rte_cryptodev_count();
	if (cdev_count == 0) {
		ODP_ERR("No crypto devices available\n");
		return -1;
	}

	for (cdev_id = 0; cdev_id < cdev_count; cdev_id++) {
		struct rte_cryptodev_info dev_info;

		rte_cryptodev_info_get(cdev_id, &dev_info);

		for (cap = &dev_info.capabilities[0];
		     cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED &&
		     !(cap->sym.xform_type == RTE_CRYPTO_SYM_XFORM_AEAD &&
		       cap->sym.aead.algo == aead_xform.aead.algo);
		     cap++)
			;

		if (cap->op == RTE_CRYPTO_OP_TYPE_UNDEFINED)
			continue;

		idx += cipher_gen_capability(&cap->sym.aead.key_size,
					     &cap->sym.aead.iv_size,
					     cipher_is_bit_mode(cipher),
					     src + idx,
					     num_copy - idx);
	}

	if (idx < num_copy)
		num_copy = idx;

	memcpy(dst, src, num_copy * size);

	return idx;
}

static int cipher_capability(odp_cipher_alg_t cipher,
			     odp_crypto_cipher_capability_t dst[],
			     int num_copy)
{
	odp_crypto_cipher_capability_t src[num_copy];
	int idx = 0, rc = 0;
	int size = sizeof(odp_crypto_cipher_capability_t);

	uint8_t cdev_id, cdev_count;
	const struct rte_cryptodev_capabilities *cap;
	struct rte_crypto_sym_xform cipher_xform;

	rc = cipher_alg_odp_to_rte(cipher, &cipher_xform);

	/* Check result */
	if (rc)
		return -1;

	cdev_count = rte_cryptodev_count();
	if (cdev_count == 0) {
		ODP_ERR("No crypto devices available\n");
		return -1;
	}

	for (cdev_id = 0; cdev_id < cdev_count; cdev_id++) {
		struct rte_cryptodev_info dev_info;

		rte_cryptodev_info_get(cdev_id, &dev_info);

		for (cap = &dev_info.capabilities[0];
		     cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED &&
		     !(cap->sym.xform_type == RTE_CRYPTO_SYM_XFORM_CIPHER &&
		       cap->sym.cipher.algo == cipher_xform.cipher.algo);
		     cap++)
			;

		if (cap->op == RTE_CRYPTO_OP_TYPE_UNDEFINED)
			continue;

		idx += cipher_gen_capability(&cap->sym.cipher.key_size,
					     &cap->sym.cipher.iv_size,
					     cipher_is_bit_mode(cipher),
					     src + idx,
					     num_copy - idx);
	}

	if (idx < num_copy)
		num_copy = idx;

	memcpy(dst, src, num_copy * size);

	return idx;
}

int odp_crypto_cipher_capability(odp_cipher_alg_t cipher,
				 odp_crypto_cipher_capability_t dst[],
				 int num_copy)
{
	/* We implement NULL in software, so always return capability */
	if (cipher == ODP_CIPHER_ALG_NULL) {
		if (num_copy >= 1)
			memset(dst, 0, sizeof(odp_crypto_cipher_capability_t));
		if (num_copy >= 2) {
			memset(&dst[1], 0, sizeof(odp_crypto_cipher_capability_t));
			dst[1].bit_mode = true;
		}
		return 2;
	}

	if (cipher_is_aead(cipher))
		return cipher_aead_capability(cipher, dst, num_copy);
	else
		return cipher_capability(cipher, dst, num_copy);
}

static int auth_gen_capability(const struct rte_crypto_param_range *key_size,
			       const struct rte_crypto_param_range *iv_size,
			       const struct rte_crypto_param_range *digest_size,
			       const struct rte_crypto_param_range *aad_size,
			       odp_bool_t bit_mode,
			       odp_crypto_auth_capability_t *src,
			       int num_copy)
{
	int idx = 0;

	uint16_t key_size_min = key_size->min;
	uint16_t key_size_max = key_size->max;
	uint16_t key_inc = key_size->increment;
	uint16_t iv_size_max = iv_size->max;
	uint16_t iv_size_min = iv_size->min;
	uint16_t iv_inc = iv_size->increment;
	uint16_t digest_size_min = digest_size->min;
	uint16_t digest_size_max = digest_size->max;
	uint16_t digest_inc = digest_size->increment;

	for (uint16_t digest_len = digest_size_min;
	     digest_len <= digest_size_max;
	     digest_len += digest_inc) {
		for (uint16_t key_len = key_size_min;
		     key_len <= key_size_max;
		     key_len += key_inc) {
			for (uint16_t iv_size = iv_size_min;
			     iv_size <= iv_size_max;
			     iv_size += iv_inc) {
				if (idx < num_copy) {
					src[idx].key_len = key_len;
					src[idx].digest_len = digest_len;
					src[idx].iv_len = iv_size;
					src[idx].aad_len.min =
						aad_size->min;
					src[idx].aad_len.max =
						aad_size->max;
					src[idx].aad_len.inc =
						aad_size->increment;
					src[idx].bit_mode = bit_mode;
				}
				idx++;
				if (iv_inc == 0)
					break;
			}

			if (key_inc == 0)
				break;
		}

		if (digest_inc == 0)
			break;
	}

	return idx;
}

static const struct rte_crypto_param_range zero_range = {
	.min = 0, .max = 0, .increment = 0
};

static int auth_aead_capability(odp_auth_alg_t auth,
				odp_crypto_auth_capability_t dst[],
				int num_copy)
{
	odp_crypto_auth_capability_t src[num_copy];
	int idx = 0, rc = 0;
	int size = sizeof(odp_crypto_auth_capability_t);

	uint8_t cdev_id, cdev_count;
	const struct rte_cryptodev_capabilities *cap;
	struct rte_crypto_sym_xform aead_xform;

	rc = auth_aead_alg_odp_to_rte(auth, &aead_xform);

	/* Check result */
	if (rc)
		return -1;

	cdev_count = rte_cryptodev_count();
	if (cdev_count == 0) {
		ODP_ERR("No crypto devices available\n");
		return -1;
	}

	for (cdev_id = 0; cdev_id < cdev_count; cdev_id++) {
		struct rte_cryptodev_info dev_info;

		rte_cryptodev_info_get(cdev_id, &dev_info);

		for (cap = &dev_info.capabilities[0];
		     cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED &&
		     !(cap->sym.xform_type == RTE_CRYPTO_SYM_XFORM_AEAD &&
		       cap->sym.auth.algo == aead_xform.auth.algo);
		     cap++)
			;

		if (cap->op == RTE_CRYPTO_OP_TYPE_UNDEFINED)
			continue;

		idx += auth_gen_capability(&zero_range,
					   &zero_range,
					   &cap->sym.aead.digest_size,
					   &cap->sym.aead.aad_size,
					   auth_is_bit_mode(auth),
					   src + idx,
					   num_copy - idx);
	}

	if (idx < num_copy)
		num_copy = idx;

	memcpy(dst, src, num_copy * size);

	return idx;
}

static int auth_capability(odp_auth_alg_t auth,
			   odp_crypto_auth_capability_t dst[],
			   int num_copy)
{
	odp_crypto_auth_capability_t src[num_copy];
	int idx = 0, rc = 0;
	int size = sizeof(odp_crypto_auth_capability_t);
	uint8_t cdev_id, cdev_count;
	const struct rte_cryptodev_capabilities *cap;
	struct rte_crypto_sym_xform auth_xform;
	uint16_t key_size_override;
	struct rte_crypto_param_range key_range_override;

	rc = auth_alg_odp_to_rte(auth, &auth_xform);

	/* Check result */
	if (rc)
		return -1;

	/* Don't generate thousands of useless capabilities for HMAC
	 * algorithms. In ODP we need support for small amount of key
	 * lengths. So we limit key size to what is practical for ODP. */
	switch (auth) {
	case ODP_AUTH_ALG_MD5_HMAC:
		key_size_override = 16;
		break;
	case ODP_AUTH_ALG_SHA1_HMAC:
		key_size_override = 20;
		break;
	case ODP_AUTH_ALG_SHA256_HMAC:
		key_size_override = 32;
		break;
	case ODP_AUTH_ALG_SHA384_HMAC:
		key_size_override = 48;
		break;
	case ODP_AUTH_ALG_SHA512_HMAC:
		key_size_override = 64;
		break;
	default:
		key_size_override = 0;
		break;
	}

	key_range_override.min = key_size_override;
	key_range_override.max = key_size_override;
	key_range_override.increment = 0;

	cdev_count = rte_cryptodev_count();
	if (cdev_count == 0) {
		ODP_ERR("No crypto devices available\n");
		return -1;
	}

	for (cdev_id = 0; cdev_id < cdev_count; cdev_id++) {
		struct rte_cryptodev_info dev_info;

		rte_cryptodev_info_get(cdev_id, &dev_info);

		for (cap = &dev_info.capabilities[0];
		     cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED &&
		     !(cap->sym.xform_type == RTE_CRYPTO_SYM_XFORM_AUTH &&
		       cap->sym.auth.algo == auth_xform.auth.algo);
		     cap++)
			;

		if (cap->op == RTE_CRYPTO_OP_TYPE_UNDEFINED)
			continue;

		if (key_size_override != 0 &&
		    is_valid_size(key_size_override,
				  &cap->sym.auth.key_size) != 0)
			continue;

		idx += auth_gen_capability(key_size_override ?
					   &key_range_override :
					   &cap->sym.auth.key_size,
					   &cap->sym.auth.iv_size,
					   &cap->sym.auth.digest_size,
					   &cap->sym.auth.aad_size,
					   auth_is_bit_mode(auth),
					   src + idx,
					   num_copy - idx);
	}

	if (idx < num_copy)
		num_copy = idx;

	memcpy(dst, src, num_copy * size);

	return idx;
}

int odp_crypto_auth_capability(odp_auth_alg_t auth,
			       odp_crypto_auth_capability_t dst[],
			       int num_copy)
{
	/* We implement NULL in software, so always return capability */
	if (auth == ODP_AUTH_ALG_NULL) {
		if (num_copy >= 1)
			memset(dst, 0, sizeof(odp_crypto_auth_capability_t));
		if (num_copy >= 2) {
			memset(&dst[1], 0, sizeof(odp_crypto_auth_capability_t));
			dst[1].bit_mode = true;
		}
		return 2;
	}

	if (auth_is_aead(auth))
		return auth_aead_capability(auth, dst, num_copy);
	else
		return auth_capability(auth, dst, num_copy);
}

static int get_crypto_aead_dev(struct rte_crypto_sym_xform *aead_xform,
			       uint8_t *dev_id)
{
	uint8_t cdev_id, id;
	const struct rte_cryptodev_capabilities *cap;

	for (id = 0; id < global->enabled_crypto_devs; id++) {
		struct rte_cryptodev_info dev_info;

		cdev_id = global->enabled_crypto_dev_ids[id];
		rte_cryptodev_info_get(cdev_id, &dev_info);

		for (cap = &dev_info.capabilities[0];
		     cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED &&
		     !(cap->sym.xform_type == RTE_CRYPTO_SYM_XFORM_AEAD &&
		       cap->sym.aead.algo == aead_xform->aead.algo);
		     cap++)
			;

		if (cap->op == RTE_CRYPTO_OP_TYPE_UNDEFINED)
			continue;

		/* Check if key size is supported by the algorithm. */
		if (is_valid_size(aead_xform->aead.key.length,
				  &cap->sym.aead.key_size) != 0) {
			ODP_ERR("Unsupported aead key length\n");
			continue;
		}

		/* Check if iv length is supported by the algorithm. */
		if (aead_xform->aead.iv.length > MAX_IV_LENGTH ||
		    is_valid_size(aead_xform->aead.iv.length,
				  &cap->sym.aead.iv_size) != 0) {
			ODP_ERR("Unsupported iv length\n");
			continue;
		}

		/* Check if digest size is supported by the algorithm. */
		if (is_valid_size(aead_xform->aead.digest_length,
				  &cap->sym.aead.digest_size) != 0) {
			ODP_ERR("Unsupported digest length\n");
			continue;
		}

		*dev_id = cdev_id;
		return 0;
	}

	return -1;
}

static int get_crypto_dev(struct rte_crypto_sym_xform *cipher_xform,
			  struct rte_crypto_sym_xform *auth_xform,
			  uint8_t *dev_id)
{
	uint8_t cdev_id, id;
	const struct rte_cryptodev_capabilities *cap;

	for (id = 0; id < global->enabled_crypto_devs; id++) {
		struct rte_cryptodev_info dev_info;

		cdev_id = global->enabled_crypto_dev_ids[id];
		rte_cryptodev_info_get(cdev_id, &dev_info);
		if (cipher_xform->cipher.algo == RTE_CRYPTO_CIPHER_NULL)
			goto check_auth;

		for (cap = &dev_info.capabilities[0];
		     cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED &&
		     !(cap->sym.xform_type == RTE_CRYPTO_SYM_XFORM_CIPHER &&
		       cap->sym.cipher.algo == cipher_xform->cipher.algo);
		     cap++)
			;

		if (cap->op == RTE_CRYPTO_OP_TYPE_UNDEFINED)
			continue;

		/* Check if key size is supported by the algorithm. */
		if (is_valid_size(cipher_xform->cipher.key.length,
				  &cap->sym.cipher.key_size) != 0) {
			ODP_ERR("Unsupported cipher key length\n");
			continue;
		}

		/* Check if iv length is supported by the algorithm. */
		if (cipher_xform->cipher.iv.length > MAX_IV_LENGTH ||
		    is_valid_size(cipher_xform->cipher.iv.length,
				  &cap->sym.cipher.iv_size) != 0) {
			ODP_ERR("Unsupported iv length\n");
			continue;
		}

check_auth:
		if (auth_xform->auth.algo == RTE_CRYPTO_AUTH_NULL &&
		    cipher_xform->cipher.algo != RTE_CRYPTO_CIPHER_NULL)
			goto check_finish;

		for (cap = &dev_info.capabilities[0];
		     cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED &&
		     !(cap->sym.xform_type == RTE_CRYPTO_SYM_XFORM_AUTH &&
		       cap->sym.auth.algo == auth_xform->auth.algo);
		     cap++)
			;

		if (cap->op == RTE_CRYPTO_OP_TYPE_UNDEFINED)
			continue;

		/* Check if key size is supported by the algorithm. */
		if (is_valid_size(auth_xform->auth.key.length,
				  &cap->sym.auth.key_size) != 0) {
			ODP_ERR("Unsupported auth key length\n");
			continue;
		}

		/* Check if digest size is supported by the algorithm. */
		if (is_valid_size(auth_xform->auth.digest_length,
				  &cap->sym.auth.digest_size) != 0) {
			ODP_ERR("Unsupported digest length\n");
			continue;
		}

		/* Check if iv length is supported by the algorithm. */
		if (auth_xform->auth.iv.length > MAX_IV_LENGTH ||
		    is_valid_size(auth_xform->auth.iv.length,
				  &cap->sym.auth.iv_size) != 0) {
			ODP_ERR("Unsupported iv length\n");
			continue;
		}

check_finish:
		*dev_id = cdev_id;
		return 0;
	}

	return -1;
}

static int crypto_init_key(uint8_t **data, uint16_t *length,
			   odp_crypto_key_t *key, const char *type)
{
	uint8_t *p = NULL;

	if (key->length) {
		p = rte_malloc(type, key->length, 0);
		if (p == NULL) {
			ODP_ERR("Failed to allocate memory for %s\n", type);
			return -1;
		}

		memcpy(p, key->data, key->length);
	}

	*data = p;
	*length = key->length;

	return 0;
}

static int crypto_fill_cipher_xform(struct rte_crypto_sym_xform *cipher_xform,
				    odp_crypto_session_param_t *param)
{
	cipher_xform->type = RTE_CRYPTO_SYM_XFORM_CIPHER;
	cipher_xform->next = NULL;

	if (cipher_alg_odp_to_rte(param->cipher_alg, cipher_xform))
		return -1;

	if (crypto_init_key(&cipher_xform->cipher.key.data,
			    &cipher_xform->cipher.key.length,
			    &param->cipher_key,
			    "cipher key"))
		return -1;
	cipher_xform->cipher.iv.offset = IV_OFFSET;
	cipher_xform->cipher.iv.length = param->cipher_iv.length;

	/* Derive order */
	if (ODP_CRYPTO_OP_ENCODE == param->op)
		cipher_xform->cipher.op = RTE_CRYPTO_CIPHER_OP_ENCRYPT;
	else
		cipher_xform->cipher.op = RTE_CRYPTO_CIPHER_OP_DECRYPT;

	return 0;
}

static int crypto_fill_auth_xform(struct rte_crypto_sym_xform *auth_xform,
				  odp_crypto_session_param_t *param)
{
	auth_xform->type = RTE_CRYPTO_SYM_XFORM_AUTH;
	auth_xform->next = NULL;

	if (auth_alg_odp_to_rte(param->auth_alg, auth_xform))
		return -1;

	auth_xform->auth.digest_length = param->auth_digest_len;
	if (auth_xform->auth.digest_length > PACKET_DIGEST_MAX) {
		ODP_ERR("Requested too long digest\n");
		return -1;
	}

	if (crypto_init_key(&auth_xform->auth.key.data,
			    &auth_xform->auth.key.length,
			    &param->auth_key,
			    "auth key"))
		return -1;

	auth_xform->auth.iv.offset = IV_OFFSET + MAX_IV_LENGTH;
	auth_xform->auth.iv.length = param->auth_iv.length;

	if (ODP_CRYPTO_OP_ENCODE == param->op)
		auth_xform->auth.op = RTE_CRYPTO_AUTH_OP_GENERATE;
	else
		auth_xform->auth.op = RTE_CRYPTO_AUTH_OP_VERIFY;

	return 0;
}

static int crypto_fill_aead_xform(struct rte_crypto_sym_xform *aead_xform,
				  odp_crypto_session_param_t *param)
{
	aead_xform->type = RTE_CRYPTO_SYM_XFORM_AEAD;
	aead_xform->next = NULL;

	if (cipher_aead_alg_odp_to_rte(param->cipher_alg, aead_xform))
		return -1;

	if (crypto_init_key(&aead_xform->aead.key.data,
			    &aead_xform->aead.key.length,
			    &param->cipher_key,
			    "aead key"))
		return -1;

	aead_xform->aead.iv.offset = IV_OFFSET;
	aead_xform->aead.iv.length = param->cipher_iv.length;

	aead_xform->aead.aad_length = param->auth_aad_len;
	if (aead_xform->aead.aad_length > PACKET_AAD_MAX) {
		ODP_ERR("Requested too long AAD\n");
		return -1;
	}

	if (aead_xform->aead.algo == RTE_CRYPTO_AEAD_AES_CCM &&
	    aead_xform->aead.aad_length + AES_CCM_AAD_OFFSET >
	    PACKET_AAD_MAX) {
		ODP_ERR("Requested too long AAD for CCM\n");
		return -1;
	}

	aead_xform->aead.digest_length = param->auth_digest_len;
	if (aead_xform->aead.digest_length > PACKET_DIGEST_MAX) {
		ODP_ERR("Requested too long digest\n");
		return -1;
	}

	/* Derive order */
	if (ODP_CRYPTO_OP_ENCODE == param->op)
		aead_xform->aead.op = RTE_CRYPTO_AEAD_OP_ENCRYPT;
	else
		aead_xform->aead.op = RTE_CRYPTO_AEAD_OP_DECRYPT;

	return 0;
}

int odp_crypto_session_create(odp_crypto_session_param_t *param,
			      odp_crypto_session_t *session_out,
			      odp_crypto_ses_create_err_t *status)
{
	int rc = 0;
	uint8_t cdev_id = 0;
	uint8_t socket_id;
	struct rte_crypto_sym_xform cipher_xform;
	struct rte_crypto_sym_xform auth_xform;
	struct rte_crypto_sym_xform *first_xform;
	struct rte_cryptodev_sym_session *rte_session;
	struct rte_mempool *sess_mp;
	crypto_session_entry_t *session = NULL;

	if (rte_cryptodev_count() == 0) {
		ODP_ERR("No crypto devices available\n");
		*status = ODP_CRYPTO_SES_CREATE_ERR_ENOMEM;
		goto err;
	}

	/* Allocate memory for this session */
	session = alloc_session();
	if (session == NULL) {
		ODP_ERR("Failed to allocate a session session");
		*status = ODP_CRYPTO_SES_CREATE_ERR_ENOMEM;
		goto err;
	}

	/* Copy parameters */
	session->p = *param;

#if ODP_DEPRECATED_API
	/* Fixed digest tag length with deprecated algo */
	switch (param->auth_alg) {
	case ODP_AUTH_ALG_MD5_96:
		session->p.auth_digest_len = 96 / 8;
		break;
	case ODP_AUTH_ALG_SHA256_128:
		/* Fixed digest tag length with deprecated algo */
		session->p.auth_digest_len = 128 / 8;
		break;
	case ODP_AUTH_ALG_AES128_GCM:
		session->p.auth_digest_len = 16;
		break;
	default:
		break;
	}
#endif

	if (cipher_is_aead(param->cipher_alg)) {
		if (crypto_fill_aead_xform(&cipher_xform, &session->p) < 0) {
			*status = ODP_CRYPTO_SES_CREATE_ERR_INV_CIPHER;
			goto err;
		}

		first_xform = &cipher_xform;

		rc = get_crypto_aead_dev(&cipher_xform,
					 &cdev_id);
	} else {
		odp_bool_t do_cipher_first;

		if (crypto_fill_cipher_xform(&cipher_xform, &session->p) < 0) {
			*status = ODP_CRYPTO_SES_CREATE_ERR_INV_CIPHER;
			goto err;
		}

		if (crypto_fill_auth_xform(&auth_xform, &session->p) < 0) {
			*status = ODP_CRYPTO_SES_CREATE_ERR_INV_AUTH;
			goto err;
		}

		/* Derive order */
		if (ODP_CRYPTO_OP_ENCODE == param->op)
			do_cipher_first =  param->auth_cipher_text;
		else
			do_cipher_first = !param->auth_cipher_text;

		/* Derive order */
		if (param->cipher_alg == ODP_CIPHER_ALG_NULL) {
			first_xform = &auth_xform;
		} else if (param->auth_alg == ODP_AUTH_ALG_NULL) {
			first_xform = &cipher_xform;
		} else if (do_cipher_first) {
			first_xform = &cipher_xform;
			first_xform->next = &auth_xform;
		} else {
			first_xform = &auth_xform;
			first_xform->next = &cipher_xform;
		}

		rc = get_crypto_dev(&cipher_xform,
				    &auth_xform,
				    &cdev_id);
	}
	if (rc) {
		ODP_ERR("Couldn't find a crypto device");
		*status = ODP_CRYPTO_SES_CREATE_ERR_ENOMEM;
		goto err;
	}

	socket_id = rte_cryptodev_socket_id(cdev_id);
	sess_mp = global->session_mempool[socket_id];

	/* Setup session */
	rte_session = rte_cryptodev_sym_session_create(sess_mp);
	if (rte_session == NULL) {
		*status = ODP_CRYPTO_SES_CREATE_ERR_ENOMEM;
		goto err;
	}

	if (rte_cryptodev_sym_session_init(cdev_id, rte_session,
					   first_xform, sess_mp) < 0) {
		/* remove the crypto_session_entry_t */
		rte_cryptodev_sym_session_free(rte_session);
		*status = ODP_CRYPTO_SES_CREATE_ERR_ENOMEM;
		goto err;
	}

	session->rte_session  = rte_session;
	session->cdev_id = cdev_id;
	session->cdev_nb_qpairs = global->enabled_crypto_dev_nb_qpairs[cdev_id];
	session->cipher_xform = cipher_xform;
	session->auth_xform = auth_xform;
	if (param->cipher_iv.data)
		memcpy(session->cipher_iv_data,
		       param->cipher_iv.data,
		       param->cipher_iv.length);
	if (param->auth_iv.data)
		memcpy(session->auth_iv_data,
		       param->auth_iv.data,
		       param->auth_iv.length);

	/* We're happy */
	*session_out = (intptr_t)session;
	*status = ODP_CRYPTO_SES_CREATE_ERR_NONE;

	return 0;

err:
	/* error status should be set at this moment */
	if (session != NULL) {
		memset(session, 0, sizeof(*session));
		free_session(session);
	}
	*session_out = ODP_CRYPTO_SESSION_INVALID;
	return -1;
}

int odp_crypto_session_destroy(odp_crypto_session_t _session)
{
	struct rte_cryptodev_sym_session *rte_session = NULL;
	crypto_session_entry_t *session;

	session = (crypto_session_entry_t *)(intptr_t)_session;

	rte_session = session->rte_session;

	if (rte_cryptodev_sym_session_clear(session->cdev_id, rte_session) < 0)
		return -1;

	if (rte_cryptodev_sym_session_free(rte_session) < 0)
		return -1;

	/* remove the crypto_session_entry_t */
	memset(session, 0, sizeof(*session));
	free_session(session);

	return 0;
}

int odp_crypto_term_global(void)
{
	int rc = 0;
	int ret;
	int count = 0;
	crypto_session_entry_t *session;

	odp_spinlock_init(&global->lock);
	odp_spinlock_lock(&global->lock);
	for (session = global->free; session != NULL; session = session->next)
		count++;
	if (count != MAX_SESSIONS) {
		ODP_ERR("crypto sessions still active\n");
		rc = -1;
	}

	if (global->crypto_op_pool != NULL)
		rte_mempool_free(global->crypto_op_pool);

	odp_spinlock_unlock(&global->lock);

	ret = odp_shm_free(crypto_global_shm);
	if (ret < 0) {
		ODP_ERR("shm free failed for crypto_pool\n");
		rc = -1;
	}

	return rc;
}

odp_crypto_compl_t odp_crypto_compl_from_event(odp_event_t ev)
{
	/* This check not mandated by the API specification */
	if (odp_event_type(ev) != ODP_EVENT_CRYPTO_COMPL)
		ODP_ABORT("Event not a crypto completion");
	return (odp_crypto_compl_t)ev;
}

odp_event_t odp_crypto_compl_to_event(odp_crypto_compl_t completion_event)
{
	return (odp_event_t)completion_event;
}

void odp_crypto_compl_result(odp_crypto_compl_t completion_event,
			     odp_crypto_op_result_t *result)
{
	(void)completion_event;
	(void)result;

	/* We won't get such events anyway, so there can be no result */
	ODP_ASSERT(0);
}

void odp_crypto_compl_free(odp_crypto_compl_t completion_event)
{
	odp_event_t ev = odp_crypto_compl_to_event(completion_event);

	odp_buffer_free(odp_buffer_from_event(ev));
}

uint64_t odp_crypto_compl_to_u64(odp_crypto_compl_t hdl)
{
	return _odp_pri(hdl);
}

void odp_crypto_session_param_init(odp_crypto_session_param_t *param)
{
	memset(param, 0, sizeof(odp_crypto_session_param_t));
}

uint64_t odp_crypto_session_to_u64(odp_crypto_session_t hdl)
{
	return (uint64_t)hdl;
}

odp_packet_t odp_crypto_packet_from_event(odp_event_t ev)
{
	/* This check not mandated by the API specification */
	ODP_ASSERT(odp_event_type(ev) == ODP_EVENT_PACKET);
	ODP_ASSERT(odp_event_subtype(ev) == ODP_EVENT_PACKET_CRYPTO);

	return odp_packet_from_event(ev);
}

odp_event_t odp_crypto_packet_to_event(odp_packet_t pkt)
{
	return odp_packet_to_event(pkt);
}

static
odp_crypto_packet_result_t *get_op_result_from_packet(odp_packet_t pkt)
{
	odp_packet_hdr_t *hdr = packet_hdr(pkt);

	return &hdr->crypto_op_result;
}

int odp_crypto_result(odp_crypto_packet_result_t *result,
		      odp_packet_t packet)
{
	odp_crypto_packet_result_t *op_result;

	ODP_ASSERT(odp_event_subtype(odp_packet_to_event(packet)) ==
		   ODP_EVENT_PACKET_CRYPTO);

	op_result = get_op_result_from_packet(packet);

	memcpy(result, op_result, sizeof(*result));

	return 0;
}

static uint8_t *crypto_prepare_digest(crypto_session_entry_t *session,
				      odp_packet_t pkt,
				      const odp_crypto_packet_op_param_t *param,
				      odp_bool_t verify,
				      rte_iova_t *phys_addr)
{
	struct rte_mbuf *mb;
	uint8_t *data;
	odp_packet_hdr_t *pkt_hdr = packet_hdr(pkt);

	if (verify)
		odp_packet_copy_to_mem(pkt, param->hash_result_offset,
				       session->p.auth_digest_len,
				       pkt_hdr->crypto_digest_buf);
	_odp_packet_set_data(pkt, param->hash_result_offset, 0,
			     session->p.auth_digest_len);
	data = pkt_hdr->crypto_digest_buf;
	mb = &pkt_hdr->buf_hdr.mb;
	*phys_addr =
		rte_pktmbuf_iova_offset(mb, data -
					rte_pktmbuf_mtod(mb, uint8_t *));

	return data;
}

static void crypto_fill_aead_param(crypto_session_entry_t *session,
				   odp_packet_t pkt,
				   const odp_crypto_packet_op_param_t *param,
				   struct rte_crypto_op *op,
				   odp_crypto_alg_err_t *rc_cipher,
				   odp_crypto_alg_err_t *rc_auth ODP_UNUSED)
{
	odp_packet_hdr_t *pkt_hdr = packet_hdr(pkt);
	struct rte_crypto_sym_xform *aead_xform;
	uint8_t *iv_ptr;

	aead_xform = &session->cipher_xform;

	op->sym->aead.digest.data =
		crypto_prepare_digest(session, pkt, param,
				      aead_xform->aead.op ==
				      RTE_CRYPTO_AEAD_OP_DECRYPT,
				      &op->sym->aead.digest.phys_addr);

	if (aead_xform->aead.algo == RTE_CRYPTO_AEAD_AES_CCM)
		memcpy(pkt_hdr->crypto_aad_buf + AES_CCM_AAD_OFFSET,
		       param->aad_ptr,
		       aead_xform->aead.aad_length);
	else
		memcpy(pkt_hdr->crypto_aad_buf,
		       param->aad_ptr,
		       aead_xform->aead.aad_length);
	op->sym->aead.aad.data = pkt_hdr->crypto_aad_buf;
	op->sym->aead.aad.phys_addr =
		rte_pktmbuf_iova_offset(&pkt_hdr->buf_hdr.mb,
					op->sym->aead.aad.data -
					rte_pktmbuf_mtod(&pkt_hdr->buf_hdr.mb,
							 uint8_t *));
	iv_ptr = rte_crypto_op_ctod_offset(op, uint8_t *, IV_OFFSET);
	if (aead_xform->aead.algo == RTE_CRYPTO_AEAD_AES_CCM) {
		*iv_ptr = aead_xform->aead.iv.length;
		iv_ptr++;
	}

	if (param->cipher_iv_ptr)
		memcpy(iv_ptr,
		       param->cipher_iv_ptr,
		       aead_xform->aead.iv.length);
	else if (session->p.cipher_iv.data)
		memcpy(iv_ptr,
		       session->cipher_iv_data,
		       aead_xform->aead.iv.length);
	else if (aead_xform->aead.iv.length != 0)
		*rc_cipher = ODP_CRYPTO_ALG_ERR_IV_INVALID;

	op->sym->aead.data.offset = param->cipher_range.offset;
	op->sym->aead.data.length = param->cipher_range.length;
}

static void crypto_fill_sym_param(crypto_session_entry_t *session,
				  odp_packet_t pkt,
				  const odp_crypto_packet_op_param_t *param,
				  struct rte_crypto_op *op,
				  odp_crypto_alg_err_t *rc_cipher,
				  odp_crypto_alg_err_t *rc_auth)
{
	struct rte_crypto_sym_xform *cipher_xform;
	struct rte_crypto_sym_xform *auth_xform;

	cipher_xform = &session->cipher_xform;
	auth_xform = &session->auth_xform;

	if (session->p.auth_digest_len == 0) {
		op->sym->auth.digest.data = NULL;
		op->sym->auth.digest.phys_addr = 0;
	} else {
		op->sym->auth.digest.data =
			crypto_prepare_digest(session, pkt, param,
					      auth_xform->auth.op ==
					      RTE_CRYPTO_AUTH_OP_VERIFY,
					      &op->sym->auth.digest.phys_addr);
	}

	if (param->cipher_iv_ptr) {
		uint8_t *iv_ptr;

		iv_ptr = rte_crypto_op_ctod_offset(op, uint8_t *, IV_OFFSET);
		memcpy(iv_ptr,
		       param->cipher_iv_ptr,
		       cipher_xform->cipher.iv.length);
	} else if (session->p.cipher_iv.data) {
		uint8_t *iv_ptr;

		iv_ptr = rte_crypto_op_ctod_offset(op, uint8_t *, IV_OFFSET);
		memcpy(iv_ptr,
		       session->cipher_iv_data,
		       cipher_xform->cipher.iv.length);
	} else if (cipher_xform->cipher.iv.length != 0) {
		*rc_cipher = ODP_CRYPTO_ALG_ERR_IV_INVALID;
	}

	if (param->auth_iv_ptr) {
		uint8_t *iv_ptr;

		iv_ptr = rte_crypto_op_ctod_offset(op, uint8_t *,
						   IV_OFFSET + MAX_IV_LENGTH);
		memcpy(iv_ptr,
		       param->auth_iv_ptr,
		       auth_xform->auth.iv.length);
	} else if (session->p.auth_iv.data) {
		uint8_t *iv_ptr;

		iv_ptr = rte_crypto_op_ctod_offset(op, uint8_t *,
						   IV_OFFSET + MAX_IV_LENGTH);
		memcpy(iv_ptr,
		       session->auth_iv_data,
		       auth_xform->auth.iv.length);
	} else if (auth_xform->auth.iv.length != 0) {
		*rc_auth = ODP_CRYPTO_ALG_ERR_IV_INVALID;
	}

	op->sym->cipher.data.offset = param->cipher_range.offset;
	op->sym->cipher.data.length = param->cipher_range.length;

	op->sym->auth.data.offset = param->auth_range.offset;
	op->sym->auth.data.length = param->auth_range.length;
}

static
int odp_crypto_int(odp_packet_t pkt_in,
		   odp_packet_t *pkt_out,
		   const odp_crypto_packet_op_param_t *param)
{
	crypto_session_entry_t *session;
	odp_crypto_alg_err_t rc_cipher = ODP_CRYPTO_ALG_ERR_NONE;
	odp_crypto_alg_err_t rc_auth = ODP_CRYPTO_ALG_ERR_NONE;
	struct rte_cryptodev_sym_session *rte_session = NULL;
	struct rte_crypto_op *op;
	odp_bool_t allocated = false;
	odp_packet_t out_pkt = *pkt_out;
	odp_crypto_packet_result_t *op_result;
	odp_packet_hdr_t *pkt_hdr;

	session = (crypto_session_entry_t *)(intptr_t)param->session;
	if (session == NULL)
		return -1;

	rte_session = session->rte_session;
	if (rte_session == NULL)
		return -1;

	/* Resolve output buffer */
	if (ODP_PACKET_INVALID == out_pkt &&
	    ODP_POOL_INVALID != session->p.output_pool) {
		out_pkt = odp_packet_alloc(session->p.output_pool,
					   odp_packet_len(pkt_in));
		allocated = true;
	}

	if (odp_unlikely(ODP_PACKET_INVALID == out_pkt)) {
		ODP_DBG("Alloc failed.\n");
		return -1;
	}

	if (pkt_in != out_pkt) {
		int ret;

		ret = odp_packet_copy_from_pkt(out_pkt,
					       0,
					       pkt_in,
					       0,
					       odp_packet_len(pkt_in));
		if (odp_unlikely(ret < 0))
			goto err;

		_odp_packet_copy_md_to_packet(pkt_in, out_pkt);
		odp_packet_free(pkt_in);
		pkt_in = ODP_PACKET_INVALID;
	}

	odp_spinlock_lock(&global->lock);
	op = rte_crypto_op_alloc(global->crypto_op_pool,
				 RTE_CRYPTO_OP_TYPE_SYMMETRIC);
	if (op == NULL) {
		ODP_ERR("Failed to allocate crypto operation");
		goto err;
	}

	odp_spinlock_unlock(&global->lock);

	if (cipher_is_aead(session->p.cipher_alg))
		crypto_fill_aead_param(session, out_pkt, param, op,
				       &rc_cipher, &rc_auth);
	else
		crypto_fill_sym_param(session, out_pkt, param, op,
				      &rc_cipher, &rc_auth);

	if (rc_cipher == ODP_CRYPTO_ALG_ERR_NONE &&
	    rc_auth == ODP_CRYPTO_ALG_ERR_NONE) {
		int retry_count = 0;
		int queue_pair = odp_cpu_id() % session->cdev_nb_qpairs;
		int rc;

		/* Set crypto operation data parameters */
		rte_crypto_op_attach_sym_session(op, rte_session);

		op->sym->m_src = (struct rte_mbuf *)(intptr_t)out_pkt;
		rc = rte_cryptodev_enqueue_burst(session->cdev_id,
						 queue_pair, &op, 1);
		if (rc == 0) {
			ODP_ERR("Failed to enqueue packet\n");
			goto err_op_free;
		}

		/* There may be a delay until the crypto operation is
		 * completed. */
		while (1) {
			rc = rte_cryptodev_dequeue_burst(session->cdev_id,
							 queue_pair, &op, 1);
			if (rc == 0 && retry_count < MAX_DEQ_RETRIES) {
				odp_time_wait_ns(ODP_TIME_USEC_IN_NS);
				retry_count++;
				continue;
			}
			break;
		}
		if (rc == 0) {
			ODP_ERR("Failed to dequeue packet");
			goto err_op_free;
		}

		out_pkt = (odp_packet_t)op->sym->m_src;
		switch (op->status) {
		case RTE_CRYPTO_OP_STATUS_SUCCESS:
			rc_cipher = ODP_CRYPTO_ALG_ERR_NONE;
			rc_auth = ODP_CRYPTO_ALG_ERR_NONE;
			break;
		case RTE_CRYPTO_OP_STATUS_AUTH_FAILED:
			rc_cipher = ODP_CRYPTO_ALG_ERR_NONE;
			rc_auth = ODP_CRYPTO_ALG_ERR_ICV_CHECK;
			break;
		default:
			rc_cipher = ODP_CRYPTO_ALG_ERR_NONE;
			rc_auth = ODP_CRYPTO_ALG_ERR_NONE;
			break;
		}
	}

	if (session->p.auth_digest_len != 0 &&
	    op->status == RTE_CRYPTO_OP_STATUS_SUCCESS) {
		odp_packet_hdr_t *pkt_hdr = packet_hdr(out_pkt);

		odp_packet_copy_from_mem(out_pkt, param->hash_result_offset,
					 session->p.auth_digest_len,
					 pkt_hdr->crypto_digest_buf);
	}

	/* Fill in result */
	packet_subtype_set(out_pkt, ODP_EVENT_PACKET_CRYPTO);
	op_result = get_op_result_from_packet(out_pkt);
	op_result->cipher_status.alg_err = rc_cipher;
	op_result->cipher_status.hw_err = ODP_CRYPTO_HW_ERR_NONE;
	op_result->auth_status.alg_err = rc_auth;
	op_result->auth_status.hw_err = ODP_CRYPTO_HW_ERR_NONE;
	op_result->ok =
		(rc_cipher == ODP_CRYPTO_ALG_ERR_NONE) &&
		(rc_auth == ODP_CRYPTO_ALG_ERR_NONE);

	pkt_hdr = packet_hdr(out_pkt);
	pkt_hdr->p.flags.crypto_err = !op_result->ok;
	rte_crypto_op_free(op);

	/* Synchronous, simply return results */
	*pkt_out = out_pkt;

	return 0;

err_op_free:
	rte_crypto_op_free(op);

err:
	if (allocated) {
		odp_packet_free(out_pkt);
		out_pkt = ODP_PACKET_INVALID;
	}

	return -1;
}

int odp_crypto_operation(odp_crypto_op_param_t *param,
			 odp_bool_t *posted,
			 odp_crypto_op_result_t *result)
{
	odp_crypto_packet_op_param_t packet_param;
	odp_packet_t out_pkt = param->out_pkt;
	odp_crypto_packet_result_t packet_result;
	odp_crypto_op_result_t local_result;
	int rc;

	packet_param.session = param->session;
	packet_param.cipher_iv_ptr = param->cipher_iv_ptr;
	packet_param.auth_iv_ptr = param->auth_iv_ptr;
	packet_param.hash_result_offset = param->hash_result_offset;
	packet_param.aad_ptr = param->aad_ptr;
	packet_param.cipher_range = param->cipher_range;
	packet_param.auth_range = param->auth_range;

	rc = odp_crypto_int(param->pkt, &out_pkt, &packet_param);
	if (rc < 0)
		return rc;

	rc = odp_crypto_result(&packet_result, out_pkt);
	if (rc < 0)
		return rc;

	/* Indicate to caller operation was sync */
	*posted = 0;

	packet_subtype_set(out_pkt, ODP_EVENT_PACKET_BASIC);

	/* Fill in result */
	local_result.ctx = param->ctx;
	local_result.pkt = out_pkt;
	local_result.cipher_status = packet_result.cipher_status;
	local_result.auth_status = packet_result.auth_status;
	local_result.ok = packet_result.ok;

	/*
	 * Be bug-to-bug compatible. Return output packet also through params.
	 */
	param->out_pkt = out_pkt;

	*result = local_result;

	return 0;
}

int odp_crypto_op(const odp_packet_t pkt_in[],
		  odp_packet_t pkt_out[],
		  const odp_crypto_packet_op_param_t param[],
		  int num_pkt)
{
	crypto_session_entry_t *session;
	int i, rc;

	session = (crypto_session_entry_t *)(intptr_t)param->session;
	ODP_ASSERT(ODP_CRYPTO_SYNC == session->p.op_mode);

	for (i = 0; i < num_pkt; i++) {
		rc = odp_crypto_int(pkt_in[i], &pkt_out[i], &param[i]);
		if (rc < 0)
			break;
	}

	return i;
}

int odp_crypto_op_enq(const odp_packet_t pkt_in[],
		      const odp_packet_t pkt_out[],
		      const odp_crypto_packet_op_param_t param[],
		      int num_pkt)
{
	odp_packet_t pkt;
	odp_event_t event;
	crypto_session_entry_t *session;
	int i, rc;

	session = (crypto_session_entry_t *)(intptr_t)param->session;
	ODP_ASSERT(ODP_CRYPTO_ASYNC == session->p.op_mode);
	ODP_ASSERT(ODP_QUEUE_INVALID != session->p.compl_queue);

	for (i = 0; i < num_pkt; i++) {
		pkt = pkt_out[i];
		rc = odp_crypto_int(pkt_in[i], &pkt, &param[i]);
		if (rc < 0)
			break;

		event = odp_packet_to_event(pkt);
		if (odp_queue_enq(session->p.compl_queue, event)) {
			odp_event_free(event);
			break;
		}
	}

	return i;
}
