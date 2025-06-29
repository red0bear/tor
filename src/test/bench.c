/* Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2021, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file bench.c
 * \brief Benchmarks for lower level Tor modules.
 **/

#include "orconfig.h"

#include "core/or/or.h"
#include "core/crypto/relay_crypto.h"

#include "lib/intmath/weakrng.h"

#ifdef ENABLE_OPENSSL
#include <openssl/opensslv.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/obj_mac.h>
#endif /* defined(ENABLE_OPENSSL) */

#include <math.h>

#include "ext/polyval/polyval.h"
#include "core/or/circuitlist.h"
#include "app/config/config.h"
#include "app/main/subsysmgr.h"
#include "lib/crypt_ops/crypto_curve25519.h"
#include "lib/crypt_ops/crypto_dh.h"
#include "core/crypto/onion_ntor.h"
#include "lib/crypt_ops/crypto_ed25519.h"
#include "lib/crypt_ops/crypto_rand.h"
#include "feature/dircommon/consdiff.h"
#include "lib/compress/compress.h"
#include "core/crypto/relay_crypto_cgo.h"

#include "core/or/cell_st.h"
#include "core/or/or_circuit_st.h"

#include "lib/crypt_ops/digestset.h"
#include "lib/crypt_ops/crypto_init.h"

#include "feature/dirparse/microdesc_parse.h"
#include "feature/nodelist/microdesc.h"

#if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) \
  || defined(_M_X64) || defined(_M_IX86) || defined(__i486)       \
  || defined(__i386__)
#define INTEL
#endif

#ifdef INTEL
#include "x86intrin.h"

static inline uint64_t
cycles(void)
{
  return __rdtsc();
}
#define cpb(start, end, bytes) \
  (((double)(end - start)) / (bytes))
#else
#define cycles() 0
#define cpb(start,end,bytes) ((void)(start+end+bytes), (double)NAN)
#endif

#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_PROCESS_CPUTIME_ID)
static uint64_t nanostart;
static inline uint64_t
timespec_to_nsec(const struct timespec *ts)
{
  return ((uint64_t)ts->tv_sec)*1000000000 + ts->tv_nsec;
}

static void
reset_perftime(void)
{
  struct timespec ts;
  int r;
  r = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  tor_assert(r == 0);
  nanostart = timespec_to_nsec(&ts);
}

static uint64_t
perftime(void)
{
  struct timespec ts;
  int r;
  r = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  tor_assert(r == 0);
  return timespec_to_nsec(&ts) - nanostart;
}

#else /* !(defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_PROCESS_CPUTIME_ID)) */
static struct timeval tv_start = { 0, 0 };
static void
reset_perftime(void)
{
  tor_gettimeofday(&tv_start);
}
static uint64_t
perftime(void)
{
  struct timeval now, out;
  tor_gettimeofday(&now);
  timersub(&now, &tv_start, &out);
  return ((uint64_t)out.tv_sec)*1000000000 + out.tv_usec*1000;
}
#endif /* defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_PROCESS_CPUTIME_ID) */

#define NANOCOUNT(start,end,iters) \
  ( ((double)((end)-(start))) / (iters) )

#define MICROCOUNT(start,end,iters) \
  ( NANOCOUNT((start), (end), (iters)) / 1000.0 )

/** Run AES performance benchmarks. */
static void
bench_aes(void)
{
  int len, i;
  char *b1, *b2;
  crypto_cipher_t *c;
  uint64_t start, end;
  const int bytes_per_iter = (1<<24);
  reset_perftime();
  char key[CIPHER_KEY_LEN];
  crypto_rand(key, sizeof(key));
  c = crypto_cipher_new(key);

  for (len = 1; len <= 8192; len *= 2) {
    int iters = bytes_per_iter / len;
    b1 = tor_malloc_zero(len);
    b2 = tor_malloc_zero(len);
    start = perftime();
    for (i = 0; i < iters; ++i) {
      crypto_cipher_encrypt(c, b1, b2, len);
    }
    end = perftime();
    tor_free(b1);
    tor_free(b2);
    printf("%d bytes: %.2f nsec per byte\n", len,
           NANOCOUNT(start, end, iters*len));
  }
  crypto_cipher_free(c);
}

static void
bench_onion_ntor_impl(void)
{
  const int iters = 1<<10;
  int i;
  curve25519_keypair_t keypair1, keypair2;
  uint64_t start, end;
  uint8_t os[NTOR_ONIONSKIN_LEN];
  uint8_t or[NTOR_REPLY_LEN];
  ntor_handshake_state_t *state = NULL;
  uint8_t nodeid[DIGEST_LEN];
  di_digest256_map_t *keymap = NULL;

  curve25519_secret_key_generate(&keypair1.seckey, 0);
  curve25519_public_key_generate(&keypair1.pubkey, &keypair1.seckey);
  curve25519_secret_key_generate(&keypair2.seckey, 0);
  curve25519_public_key_generate(&keypair2.pubkey, &keypair2.seckey);
  dimap_add_entry(&keymap, keypair1.pubkey.public_key, &keypair1);
  dimap_add_entry(&keymap, keypair2.pubkey.public_key, &keypair2);
  crypto_rand((char *)nodeid, sizeof(nodeid));

  reset_perftime();
  start = perftime();
  for (i = 0; i < iters; ++i) {
    onion_skin_ntor_create(nodeid, &keypair1.pubkey, &state, os);
    ntor_handshake_state_free(state);
    state = NULL;
  }
  end = perftime();
  printf("Client-side, part 1: %f usec.\n", NANOCOUNT(start, end, iters)/1e3);

  state = NULL;
  onion_skin_ntor_create(nodeid, &keypair1.pubkey, &state, os);
  start = perftime();
  for (i = 0; i < iters; ++i) {
    uint8_t key_out[CPATH_KEY_MATERIAL_LEN];
    onion_skin_ntor_server_handshake(os, keymap, NULL, nodeid, or,
                                key_out, sizeof(key_out));
  }
  end = perftime();
  printf("Server-side: %f usec\n",
         NANOCOUNT(start, end, iters)/1e3);

  start = perftime();
  for (i = 0; i < iters; ++i) {
    uint8_t key_out[CPATH_KEY_MATERIAL_LEN];
    int s;
    s = onion_skin_ntor_client_handshake(state, or, key_out, sizeof(key_out),
                                         NULL);
    tor_assert(s == 0);
  }
  end = perftime();
  printf("Client-side, part 2: %f usec.\n",
         NANOCOUNT(start, end, iters)/1e3);

  ntor_handshake_state_free(state);
  dimap_free(keymap, NULL);
}

static void
bench_onion_ntor(void)
{
  int ed;

  for (ed = 0; ed <= 1; ++ed) {
    printf("Ed25519-based basepoint multiply = %s.\n",
           (ed == 0) ? "disabled" : "enabled");
    curve25519_set_impl_params(ed);
    bench_onion_ntor_impl();
  }
}

static void
bench_ed25519_impl(void)
{
  uint64_t start, end;
  const int iters = 1<<12;
  int i;
  const uint8_t msg[] = "but leaving, could not tell what they had heard";
  ed25519_signature_t sig;
  ed25519_keypair_t kp;
  curve25519_keypair_t curve_kp;
  ed25519_public_key_t pubkey_tmp;

  ed25519_secret_key_generate(&kp.seckey, 0);
  start = perftime();
  for (i = 0; i < iters; ++i) {
    ed25519_public_key_generate(&kp.pubkey, &kp.seckey);
  }
  end = perftime();
  printf("Generate public key: %.2f usec\n",
         MICROCOUNT(start, end, iters));

  start = perftime();
  for (i = 0; i < iters; ++i) {
    ed25519_sign(&sig, msg, sizeof(msg), &kp);
  }
  end = perftime();
  printf("Sign a short message: %.2f usec\n",
         MICROCOUNT(start, end, iters));

  start = perftime();
  for (i = 0; i < iters; ++i) {
    ed25519_checksig(&sig, msg, sizeof(msg), &kp.pubkey);
  }
  end = perftime();
  printf("Verify signature: %.2f usec\n",
         MICROCOUNT(start, end, iters));

  curve25519_keypair_generate(&curve_kp, 0);
  start = perftime();
  for (i = 0; i < iters; ++i) {
    ed25519_public_key_from_curve25519_public_key(&pubkey_tmp,
                                                  &curve_kp.pubkey, 1);
  }
  end = perftime();
  printf("Convert public point from curve25519: %.2f usec\n",
         MICROCOUNT(start, end, iters));

  curve25519_keypair_generate(&curve_kp, 0);
  start = perftime();
  for (i = 0; i < iters; ++i) {
    ed25519_public_blind(&pubkey_tmp, &kp.pubkey, msg);
  }
  end = perftime();
  printf("Blind a public key: %.2f usec\n",
         MICROCOUNT(start, end, iters));
}

static void
bench_ed25519(void)
{
  int donna;

  for (donna = 0; donna <= 1; ++donna) {
    printf("Ed25519-donna = %s.\n",
           (donna == 0) ? "disabled" : "enabled");
    ed25519_set_impl_params(donna);
    bench_ed25519_impl();
  }
}

static void
bench_rand_len(int len)
{
  const int N = 100000;
  int i;
  char *buf = tor_malloc(len);
  uint64_t start,end;

  start = perftime();
  for (i = 0; i < N; ++i) {
    crypto_rand(buf, len);
  }
  end = perftime();
  printf("crypto_rand(%d): %f nsec.\n", len, NANOCOUNT(start,end,N));

  crypto_fast_rng_t *fr = crypto_fast_rng_new();
  start = perftime();
  for (i = 0; i < N; ++i) {
    crypto_fast_rng_getbytes(fr,(uint8_t*)buf,len);
  }
  end = perftime();
  printf("crypto_fast_rng_getbytes(%d): %f nsec.\n", len,
         NANOCOUNT(start,end,N));
  crypto_fast_rng_free(fr);

  if (len <= 32) {
    start = perftime();
    for (i = 0; i < N; ++i) {
      crypto_strongest_rand((uint8_t*)buf, len);
    }
    end = perftime();
    printf("crypto_strongest_rand(%d): %f nsec.\n", len,
           NANOCOUNT(start,end,N));
  }

  if (len == 4) {
    tor_weak_rng_t weak;
    tor_init_weak_random(&weak, 1337);

    start = perftime();
    uint32_t t=0;
    for (i = 0; i < N; ++i) {
      t += tor_weak_random(&weak);
      (void) t;
    }
    end = perftime();
    printf("weak_rand(4): %f nsec.\n", NANOCOUNT(start,end,N));
  }

  tor_free(buf);
}

static void
bench_rand(void)
{
  bench_rand_len(4);
  bench_rand_len(16);
  bench_rand_len(128);
}

static void
bench_cell_aes(void)
{
  uint64_t start, end;
  const int len = 509;
  const int iters = (1<<16);
  const int max_misalign = 15;
  char *b = tor_malloc(len+max_misalign);
  crypto_cipher_t *c;
  int i, misalign;
  char key[CIPHER_KEY_LEN];
  crypto_rand(key, sizeof(key));
  c = crypto_cipher_new(key);

  reset_perftime();
  for (misalign = 0; misalign <= max_misalign; ++misalign) {
    start = perftime();
    for (i = 0; i < iters; ++i) {
      crypto_cipher_crypt_inplace(c, b+misalign, len);
    }
    end = perftime();
    printf("%d bytes, misaligned by %d: %.2f nsec per byte\n", len, misalign,
           NANOCOUNT(start, end, iters*len));
  }

  crypto_cipher_free(c);
  tor_free(b);
}

/** Run digestmap_t performance benchmarks. */
static void
bench_dmap(void)
{
  smartlist_t *sl = smartlist_new();
  smartlist_t *sl2 = smartlist_new();
  uint64_t start, end, pt2, pt3, pt4;
  int iters = 8192;
  const int elts = 4000;
  const int fpostests = 100000;
  char d[20];
  int i,n=0, fp = 0;
  digestmap_t *dm = digestmap_new();
  digestset_t *ds = digestset_new(elts);

  for (i = 0; i < elts; ++i) {
    crypto_rand(d, 20);
    smartlist_add(sl, tor_memdup(d, 20));
  }
  for (i = 0; i < elts; ++i) {
    crypto_rand(d, 20);
    smartlist_add(sl2, tor_memdup(d, 20));
  }
  //printf("nbits=%d\n", ds->mask+1);

  reset_perftime();

  start = perftime();
  for (i = 0; i < iters; ++i) {
    SMARTLIST_FOREACH(sl, const char *, cp, digestmap_set(dm, cp, (void*)1));
  }
  pt2 = perftime();
  printf("digestmap_set: %.2f ns per element\n",
         NANOCOUNT(start, pt2, iters*elts));

  for (i = 0; i < iters; ++i) {
    SMARTLIST_FOREACH(sl, const char *, cp, digestmap_get(dm, cp));
    SMARTLIST_FOREACH(sl2, const char *, cp, digestmap_get(dm, cp));
  }
  pt3 = perftime();
  printf("digestmap_get: %.2f ns per element\n",
         NANOCOUNT(pt2, pt3, iters*elts*2));

  for (i = 0; i < iters; ++i) {
    SMARTLIST_FOREACH(sl, const char *, cp, digestset_add(ds, cp));
  }
  pt4 = perftime();
  printf("digestset_add: %.2f ns per element\n",
         NANOCOUNT(pt3, pt4, iters*elts));

  for (i = 0; i < iters; ++i) {
    SMARTLIST_FOREACH(sl, const char *, cp,
                      n += digestset_probably_contains(ds, cp));
    SMARTLIST_FOREACH(sl2, const char *, cp,
                      n += digestset_probably_contains(ds, cp));
  }
  end = perftime();
  printf("digestset_probably_contains: %.2f ns per element.\n",
         NANOCOUNT(pt4, end, iters*elts*2));
  /* We need to use this, or else the whole loop gets optimized out. */
  printf("Hits == %d\n", n);

  for (i = 0; i < fpostests; ++i) {
    crypto_rand(d, 20);
    if (digestset_probably_contains(ds, d)) ++fp;
  }
  printf("False positive rate on digestset: %.2f%%\n",
         (fp/(double)fpostests)*100);

  digestmap_free(dm, NULL);
  digestset_free(ds);
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  SMARTLIST_FOREACH(sl2, char *, cp, tor_free(cp));
  smartlist_free(sl);
  smartlist_free(sl2);
}

static void
bench_siphash(void)
{
  char buf[128];
  int lens[] = { 7, 8, 15, 16, 20, 32, 111, 128, -1 };
  int i, j;
  uint64_t start, end;
  const int N = 300000;
  crypto_rand(buf, sizeof(buf));

  for (i = 0; lens[i] > 0; ++i) {
    reset_perftime();
    start = perftime();
    for (j = 0; j < N; ++j) {
      siphash24g(buf, lens[i]);
    }
    end = perftime();
    printf("siphash24g(%d): %.2f ns per call\n",
           lens[i], NANOCOUNT(start,end,N));
  }
}

static void
bench_digest(void)
{
  char buf[8192];
  char out[DIGEST512_LEN];
  const int lens[] = { 1, 16, 32, 64, 128, 512, 1024, 2048, -1 };
  const int N = 300000;
  uint64_t start, end;
  crypto_rand(buf, sizeof(buf));

  for (int alg = 0; alg < N_DIGEST_ALGORITHMS; alg++) {
    for (int i = 0; lens[i] > 0; ++i) {
      reset_perftime();
      start = perftime();
      int failures = 0;
      for (int j = 0; j < N; ++j) {
        switch (alg) {
          case DIGEST_SHA1:
            failures += crypto_digest(out, buf, lens[i]) < 0;
            break;
          case DIGEST_SHA256:
          case DIGEST_SHA3_256:
            failures += crypto_digest256(out, buf, lens[i], alg) < 0;
            break;
          case DIGEST_SHA512:
          case DIGEST_SHA3_512:
            failures += crypto_digest512(out, buf, lens[i], alg) < 0;
            break;
          default:
            tor_assert(0);
        }
      }
      end = perftime();
      printf("%s(%d): %.2f ns per call\n",
             crypto_digest_algorithm_get_name(alg),
             lens[i], NANOCOUNT(start,end,N));
      if (failures)
        printf("ERROR: crypto_digest failed %d times.\n", failures);
    }
  }
}

static void
bench_cell_ops_tor1(void)
{
  const int iters = 1<<20;
  int i;

  /* benchmarks for cell ops at relay. */
  or_circuit_t *or_circ = tor_malloc_zero(sizeof(or_circuit_t));
  cell_t *cell = tor_malloc(sizeof(cell_t));
  int outbound;
  uint64_t start, end;
  uint64_t cstart, cend;

  // TODO CGO: use constant after this is merged or rebased.
  const unsigned payload_len = 498;

  crypto_rand((char*)cell->payload, sizeof(cell->payload));

  /* Mock-up or_circuit_t */
  or_circ->base_.magic = OR_CIRCUIT_MAGIC;
  or_circ->base_.purpose = CIRCUIT_PURPOSE_OR;

  /* Initialize crypto */
  char keys[CPATH_KEY_MATERIAL_LEN];
  crypto_rand(keys, sizeof(keys));
  size_t keylen = sizeof(keys);
  relay_crypto_init(RELAY_CRYPTO_ALG_TOR1,
                    &or_circ->crypto, keys, keylen);

  reset_perftime();

  for (outbound = 0; outbound <= 1; ++outbound) {
    cell_direction_t d = outbound ? CELL_DIRECTION_OUT : CELL_DIRECTION_IN;
    start = perftime();
    cstart = cycles();
    for (i = 0; i < iters; ++i) {
      char recognized = 0;
      crypt_path_t *layer_hint = NULL;
      relay_decrypt_cell(TO_CIRCUIT(or_circ), cell, d,
                         &layer_hint, &recognized);
    }
    cend = cycles();
    end = perftime();
    printf("%sbound cells: %.2f ns per cell. "
           "(%.2f ns per byte of payload, %.2f cpb)\n",
           outbound?"Out":" In",
           NANOCOUNT(start,end,iters),
           NANOCOUNT(start,end,iters * payload_len),
           cpb(cstart, cend, iters * payload_len));
  }

  start = perftime();
  cstart = cycles();
  for (i = 0; i < iters; ++i) {
    relay_encrypt_cell_inbound(cell, or_circ);
  }
  cend = cycles();
  end = perftime();
  printf("originate inbound : %.2f ns per cell. "
         "(%.2f ns per payload byte, %.2f cpb)\n",
         NANOCOUNT(start, end, iters),
         NANOCOUNT(start, end, iters * payload_len),
         cpb(cstart, cend, iters*payload_len));

  relay_crypto_clear(&or_circ->crypto);
  tor_free(or_circ);
  tor_free(cell);
}

static void
bench_polyval(void)
{
  polyval_t pv;
  polyvalx_t pvx;
  uint8_t key[16];
  uint8_t input[512];
  uint64_t start, end, cstart, cend;
  crypto_rand((char*) key, sizeof(key));
  crypto_rand((char*) input, sizeof(input));

  const int iters = 1<<20;

  polyval_init(&pv, key);
  start = perftime();
  cstart = cycles();
  for (int i = 0; i < iters; ++i) {
    polyval_add_block(&pv, input);
  }
  cend = cycles();
  end = perftime();
  printf("polyval (add 16): %.2f ns; %.2f cpb\n",
         NANOCOUNT(start, end, iters),
         cpb(cstart, cend, iters * 16));

  start = perftime();
  cstart = cycles();
  for (int i = 0; i < iters; ++i) {
    polyval_add_zpad(&pv, input, 512);
  }
  cend = cycles();
  end = perftime();
  printf("polyval (add 512): %.2f ns; %.2f cpb\n",
         NANOCOUNT(start, end, iters),
         cpb(cstart, cend, iters * 512));

  polyvalx_init(&pvx, key);
  start = perftime();
  cstart = cycles();
  for (int i = 0; i < iters; ++i) {
    polyvalx_add_zpad(&pvx, input, 512);
  }
  cend = cycles();
  end = perftime();
  printf("polyval (add 512, pre-expanded key): %.2f ns; %.2f cpb\n",
         NANOCOUNT(start, end, iters),
         cpb(cstart, cend, iters * 512));
}

static void
bench_cell_ops_cgo(void)
{
  const int iters = 1<<20;

  /* benchmarks for cell ops at relay. */
  cell_t *cell = tor_malloc(sizeof(cell_t));

  uint64_t start, end;
  uint64_t cstart, cend;

  const uint8_t *tag = NULL;
  size_t  keylen = cgo_key_material_len(128);
  uint8_t *keys = tor_malloc(keylen);
  crypto_rand((char*) keys, keylen);

  // We're using the version of this constant that _does_ include
  // stream IDs, for an apples-to-apples comparison with tor1.
  //
  // TODO CGO: use constant after this is merged or rebased.
  const unsigned payload_len = 488;

  memset(cell, 0, sizeof(*cell));

#define SHOW(operation) \
  printf("%s: %.2f per cell (%.2f cpb)\n",              \
         (operation),                                   \
         NANOCOUNT(start,end,iters),                    \
         cpb(cstart, cend, (double)iters * payload_len))

  // Initialize crypto
  cgo_crypt_t *r_f = cgo_crypt_new(CGO_MODE_RELAY_FORWARD, 128, keys, keylen);
  cgo_crypt_t *r_b = cgo_crypt_new(CGO_MODE_RELAY_BACKWARD, 128, keys, keylen);

  reset_perftime();

  start = perftime();
  cstart = cycles();
  for (int i=0; i < iters; ++i) {
    cgo_crypt_relay_forward(r_f, cell, &tag);
  }
  cend = cycles();
  end = perftime();
  SHOW("CGO outbound at relay");

  start = perftime();
  cstart = cycles();
  for (int i=0; i < iters; ++i) {
    cgo_crypt_relay_backward(r_b, cell);
  }
  cend = cycles();
  end = perftime();
  SHOW("CGO inbound at relay");

  start = perftime();
  cstart = cycles();
  for (int i=0; i < iters; ++i) {
    cgo_crypt_relay_originate(r_b, cell, &tag);
  }
  cend = cycles();
  end = perftime();
  SHOW("CGO originate at relay");

  tor_free(cell);
  tor_free(keys);
  cgo_crypt_free(r_f);
  cgo_crypt_free(r_b);

#undef SHOW
}

static void
bench_dh(void)
{
  const int iters = 1<<10;
  int i;
  uint64_t start, end;

  reset_perftime();
  start = perftime();
  for (i = 0; i < iters; ++i) {
    char dh_pubkey_a[DH1024_KEY_LEN], dh_pubkey_b[DH1024_KEY_LEN];
    char secret_a[DH1024_KEY_LEN], secret_b[DH1024_KEY_LEN];
    ssize_t slen_a, slen_b;
    crypto_dh_t *dh_a = crypto_dh_new(DH_TYPE_TLS);
    crypto_dh_t *dh_b = crypto_dh_new(DH_TYPE_TLS);
    crypto_dh_generate_public(dh_a);
    crypto_dh_generate_public(dh_b);
    crypto_dh_get_public(dh_a, dh_pubkey_a, sizeof(dh_pubkey_a));
    crypto_dh_get_public(dh_b, dh_pubkey_b, sizeof(dh_pubkey_b));
    slen_a = crypto_dh_compute_secret(LOG_NOTICE,
                                      dh_a, dh_pubkey_b, sizeof(dh_pubkey_b),
                                      secret_a, sizeof(secret_a));
    slen_b = crypto_dh_compute_secret(LOG_NOTICE,
                                      dh_b, dh_pubkey_a, sizeof(dh_pubkey_a),
                                      secret_b, sizeof(secret_b));
    tor_assert(slen_a == slen_b);
    tor_assert(fast_memeq(secret_a, secret_b, slen_a));
    crypto_dh_free(dh_a);
    crypto_dh_free(dh_b);
  }
  end = perftime();
  printf("Complete DH handshakes (1024 bit, public and private ops):\n"
         "      %f millisec each.\n", NANOCOUNT(start, end, iters)/1e6);
}

#ifdef ENABLE_OPENSSL
static void
bench_ecdh_impl(int nid, const char *name)
{
  const int iters = 1<<10;
  int i;
  uint64_t start, end;

  reset_perftime();
  start = perftime();
  for (i = 0; i < iters; ++i) {
    char secret_a[DH1024_KEY_LEN], secret_b[DH1024_KEY_LEN];
    ssize_t slen_a, slen_b;
    EC_KEY *dh_a = EC_KEY_new_by_curve_name(nid);
    EC_KEY *dh_b = EC_KEY_new_by_curve_name(nid);
    if (!dh_a || !dh_b) {
      puts("Skipping.  (No implementation?)");
      return;
    }

    EC_KEY_generate_key(dh_a);
    EC_KEY_generate_key(dh_b);
    slen_a = ECDH_compute_key(secret_a, DH1024_KEY_LEN,
                              EC_KEY_get0_public_key(dh_b), dh_a,
                              NULL);
    slen_b = ECDH_compute_key(secret_b, DH1024_KEY_LEN,
                              EC_KEY_get0_public_key(dh_a), dh_b,
                              NULL);

    tor_assert(slen_a == slen_b);
    tor_assert(fast_memeq(secret_a, secret_b, slen_a));
    EC_KEY_free(dh_a);
    EC_KEY_free(dh_b);
  }
  end = perftime();
  printf("Complete ECDH %s handshakes (2 public and 2 private ops):\n"
         "      %f millisec each.\n", name, NANOCOUNT(start, end, iters)/1e6);
}

static void
bench_ecdh_p256(void)
{
  bench_ecdh_impl(NID_X9_62_prime256v1, "P-256");
}

static void
bench_ecdh_p224(void)
{
  bench_ecdh_impl(NID_secp224r1, "P-224");
}
#endif /* defined(ENABLE_OPENSSL) */

static void
bench_md_parse(void)
{
  uint64_t start, end;
  const int N = 100000;
  // selected arbitrarily
  const char md_text[] =
    "@last-listed 2018-12-14 18:14:14\n"
    "onion-key\n"
    "-----BEGIN RSA PUBLIC KEY-----\n"
    "MIGJAoGBAMHkZeXNDX/49JqM2BVLmh1Fnb5iMVnatvZZTLJyedqDLkbXZ1WKP5oh\n"
    "7ec14dj/k3ntpwHD4s2o3Lb6nfagWbug4+F/rNJ7JuFru/PSyOvDyHGNAuegOXph\n"
    "3gTGjdDpv/yPoiadGebbVe8E7n6hO+XxM2W/4dqheKimF0/s9B7HAgMBAAE=\n"
    "-----END RSA PUBLIC KEY-----\n"
    "ntor-onion-key QgF/EjqlNG1wRHLIop/nCekEH+ETGZSgYOhu26eiTF4=\n"
    "family $00E9A86E7733240E60D8435A7BBD634A23894098 "
    "$329BD7545DEEEBBDC8C4285F243916F248972102 "
    "$69E06EBB2573A4F89330BDF8BC869794A3E10E4D "
    "$DCA2A3FAE50B3729DAA15BC95FB21AF03389818B\n"
    "p accept 53,80,443,5222-5223,25565\n"
    "id ed25519 BzffzY99z6Q8KltcFlUTLWjNTBU7yKK+uQhyi1Ivb3A\n";

  reset_perftime();
  start = perftime();
  for (int i = 0; i < N; ++i) {
    smartlist_t *s = microdescs_parse_from_string(md_text, NULL, 1,
                                                  SAVED_IN_CACHE, NULL);
    SMARTLIST_FOREACH(s, microdesc_t *, md, microdesc_free(md));
    smartlist_free(s);
  }

  end = perftime();
  printf("Microdesc parse: %f nsec\n", NANOCOUNT(start, end, N));
}

typedef void (*bench_fn)(void);

typedef struct benchmark_t {
  const char *name;
  bench_fn fn;
  int enabled;
} benchmark_t;

#define ENT(s) { #s , bench_##s, 0 }

static struct benchmark_t benchmarks[] = {
  ENT(dmap),
  ENT(siphash),
  ENT(digest),
  ENT(polyval),
  ENT(aes),
  ENT(onion_ntor),
  ENT(ed25519),
  ENT(rand),

  ENT(cell_aes),
  ENT(cell_ops_tor1),
  ENT(cell_ops_cgo),
  ENT(dh),

#ifdef ENABLE_OPENSSL
  ENT(ecdh_p256),
  ENT(ecdh_p224),
#endif

  ENT(md_parse),
  {NULL,NULL,0}
};

static benchmark_t *
find_benchmark(const char *name)
{
  benchmark_t *b;
  for (b = benchmarks; b->name; ++b) {
    if (!strcmp(name, b->name)) {
      return b;
    }
  }
  return NULL;
}

/** Main entry point for benchmark code: parse the command line, and run
 * some benchmarks. */
int
main(int argc, const char **argv)
{
  int i;
  int list=0, n_enabled=0;
  char *errmsg;
  or_options_t *options;

  subsystems_init_upto(SUBSYS_LEVEL_LIBS);
  flush_log_messages_from_startup();

  tor_compress_init();

  if (argc == 4 && !strcmp(argv[1], "diff")) {
    const int N = 200;
    char *f1 = read_file_to_str(argv[2], RFTS_BIN, NULL);
    char *f2 = read_file_to_str(argv[3], RFTS_BIN, NULL);
    if (! f1 || ! f2) {
      perror("X");
      return 1;
    }
    size_t f1len = strlen(f1);
    size_t f2len = strlen(f2);
    for (i = 0; i < N; ++i) {
      char *diff = consensus_diff_generate(f1, f1len, f2, f2len);
      tor_free(diff);
    }
    char *diff = consensus_diff_generate(f1, f1len, f2, f2len);
    printf("%s", diff);
    tor_free(f1);
    tor_free(f2);
    tor_free(diff);
    return 0;
  }

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--list")) {
      list = 1;
    } else {
      benchmark_t *benchmark = find_benchmark(argv[i]);
      ++n_enabled;
      if (benchmark) {
        benchmark->enabled = 1;
      } else {
        printf("No such benchmark as %s\n", argv[i]);
      }
    }
  }

  reset_perftime();

  if (crypto_global_init(0, NULL, NULL) < 0) {
    printf("Couldn't seed RNG; exiting.\n");
    return 1;
  }

  init_protocol_warning_severity_level();
  options = options_new();
  options->command = CMD_RUN_UNITTESTS;
  options->DataDirectory = tor_strdup("");
  options->KeyDirectory = tor_strdup("");
  options->CacheDirectory = tor_strdup("");
  options_init(options);
  if (set_options(options, &errmsg) < 0) {
    printf("Failed to set initial options: %s\n", errmsg);
    tor_free(errmsg);
    return 1;
  }

  for (benchmark_t *b = benchmarks; b->name; ++b) {
    if (b->enabled || n_enabled == 0) {
      printf("===== %s =====\n", b->name);
      if (!list)
        b->fn();
    }
  }

  return 0;
}
