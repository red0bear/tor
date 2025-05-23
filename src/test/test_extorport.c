/* Copyright (c) 2013-2021, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#define CONNECTION_PRIVATE
#define EXT_ORPORT_PRIVATE
#define MAINLOOP_PRIVATE
#include "core/or/or.h"
#include "lib/buf/buffers.h"
#include "core/mainloop/connection.h"
#include "core/or/connection_or.h"
#include "app/config/config.h"
#include "feature/control/control_events.h"
#include "lib/crypt_ops/crypto_rand.h"
#include "feature/relay/ext_orport.h"
#include "core/mainloop/mainloop.h"

#include "core/or/or_connection_st.h"

#include "test/test.h"
#include "test/test_helpers.h"
#include "test/rng_test_helpers.h"

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

/* Simple connection_write_to_buf_impl_ replacement that unconditionally
 * writes to outbuf. */
static void
connection_write_to_buf_impl_replacement(const char *string, size_t len,
                                         connection_t *conn, int compressed)
{
  (void) compressed;

  tor_assert(string);
  tor_assert(conn);
  buf_add(conn->outbuf, string, len);
}

static void
test_ext_or_write_command(void *arg)
{
  or_connection_t *c1;
  char *cp = NULL;
  char *buf = NULL;
  size_t sz;

  (void) arg;
  MOCK(connection_write_to_buf_impl_,
       connection_write_to_buf_impl_replacement);

  c1 = or_connection_new(CONN_TYPE_EXT_OR, AF_INET);
  tt_assert(c1);

  /* Length too long */
  tt_int_op(connection_write_ext_or_command(TO_CONN(c1), 100, "X", 100000),
            OP_LT, 0);

  /* Empty command */
  tt_int_op(connection_write_ext_or_command(TO_CONN(c1), 0x99, NULL, 0),
            OP_EQ, 0);
  cp = buf_get_contents(TO_CONN(c1)->outbuf, &sz);
  tt_int_op(sz, OP_EQ, 4);
  tt_mem_op(cp, OP_EQ, "\x00\x99\x00\x00", 4);
  tor_free(cp);

  /* Medium command. */
  tt_int_op(connection_write_ext_or_command(TO_CONN(c1), 0x99,
                                            "Wai\0Hello", 9), OP_EQ, 0);
  cp = buf_get_contents(TO_CONN(c1)->outbuf, &sz);
  tt_int_op(sz, OP_EQ, 13);
  tt_mem_op(cp, OP_EQ, "\x00\x99\x00\x09Wai\x00Hello", 13);
  tor_free(cp);

  /* Long command */
  buf = tor_malloc(65535);
  memset(buf, 'x', 65535);
  tt_int_op(connection_write_ext_or_command(TO_CONN(c1), 0xf00d,
                                            buf, 65535), OP_EQ, 0);
  cp = buf_get_contents(TO_CONN(c1)->outbuf, &sz);
  tt_int_op(sz, OP_EQ, 65539);
  tt_mem_op(cp, OP_EQ, "\xf0\x0d\xff\xff", 4);
  tt_mem_op(cp+4, OP_EQ, buf, 65535);
  tor_free(cp);

 done:
  if (c1)
    connection_free_minimal(TO_CONN(c1));
  tor_free(cp);
  tor_free(buf);
  UNMOCK(connection_write_to_buf_impl_);
}

static int
write_bytes_to_file_fail(const char *fname, const char *str, size_t len,
                         int bin)
{
  (void) fname;
  (void) str;
  (void) len;
  (void) bin;

  return -1;
}

static void
test_ext_or_init_auth(void *arg)
{
  or_options_t *options = get_options_mutable();
  const char *fn;
  char *cp = NULL;
  struct stat st;
  char cookie0[32];
  (void)arg;

  /* Check default filename location */
  tor_free(options->DataDirectory);
  options->DataDirectory = tor_strdup("foo");
  cp = get_ext_or_auth_cookie_file_name();
  tt_str_op(cp, OP_EQ, "foo"PATH_SEPARATOR"extended_orport_auth_cookie");
  tor_free(cp);

  /* Shouldn't be initialized already, or our tests will be a bit
   * meaningless */
  ext_or_auth_cookie = tor_malloc_zero(32);
  tt_assert(fast_mem_is_zero((char*)ext_or_auth_cookie, 32));

  /* Now make sure we use a temporary file */
  fn = get_fname("ext_cookie_file");
  options->ExtORPortCookieAuthFile = tor_strdup(fn);
  cp = get_ext_or_auth_cookie_file_name();
  tt_str_op(cp, OP_EQ, fn);
  tor_free(cp);

  /* Test the initialization function with a broken
     write_bytes_to_file(). See if the problem is handled properly. */
  MOCK(write_bytes_to_file, write_bytes_to_file_fail);
  tt_int_op(-1, OP_EQ, init_ext_or_cookie_authentication(1));
  tt_int_op(ext_or_auth_cookie_is_set, OP_EQ, 0);
  UNMOCK(write_bytes_to_file);

  /* Now do the actual initialization. */
  tt_int_op(0, OP_EQ, init_ext_or_cookie_authentication(1));
  tt_int_op(ext_or_auth_cookie_is_set, OP_EQ, 1);
  cp = read_file_to_str(fn, RFTS_BIN, &st);
  tt_ptr_op(cp, OP_NE, NULL);
  tt_u64_op((uint64_t)st.st_size, OP_EQ, 64);
  tt_mem_op(cp,OP_EQ, "! Extended ORPort Auth Cookie !\x0a", 32);
  tt_mem_op(cp+32,OP_EQ, ext_or_auth_cookie, 32);
  memcpy(cookie0, ext_or_auth_cookie, 32);
  tt_assert(!fast_mem_is_zero((char*)ext_or_auth_cookie, 32));

  /* Operation should be idempotent. */
  tt_int_op(0, OP_EQ, init_ext_or_cookie_authentication(1));
  tt_mem_op(cookie0,OP_EQ, ext_or_auth_cookie, 32);

 done:
  tor_free(cp);
  ext_orport_free_all();
}

static void
test_ext_or_cookie_auth(void *arg)
{
  char *reply=NULL, *reply2=NULL, *client_hash=NULL, *client_hash2=NULL;
  size_t reply_len=0;
  char hmac1[32], hmac2[32];

  NONSTRING const char client_nonce[32] =
    "Who is the third who walks alway";
  char server_hash_input[] =
    "ExtORPort authentication server-to-client hash"
    "Who is the third who walks alway"
    "................................";
  char client_hash_input[] =
    "ExtORPort authentication client-to-server hash"
    "Who is the third who walks alway"
    "................................";

  (void)arg;

  tt_int_op(strlen(client_hash_input), OP_EQ, 46+32+32);
  tt_int_op(strlen(server_hash_input), OP_EQ, 46+32+32);

  ext_or_auth_cookie = tor_malloc_zero(32);
  memcpy(ext_or_auth_cookie, "s beside you? When I count, ther", 32);
  ext_or_auth_cookie_is_set = 1;

  /* For this authentication, the client sends 32 random bytes (ClientNonce)
   * The server replies with 32 byte ServerHash and 32 byte ServerNonce,
   * where ServerHash is:
   * HMAC-SHA256(CookieString,
   *   "ExtORPort authentication server-to-client hash" | ClientNonce |
   *    ServerNonce)"
   * The client must reply with 32-byte ClientHash, which we compute as:
   *   ClientHash is computed as:
   *        HMAC-SHA256(CookieString,
   *           "ExtORPort authentication client-to-server hash" | ClientNonce |
   *            ServerNonce)
   */

  /* Wrong length */
  tt_int_op(-1, OP_EQ,
            handle_client_auth_nonce(client_nonce, 33, &client_hash, &reply,
                                     &reply_len));
  tt_int_op(-1, OP_EQ,
            handle_client_auth_nonce(client_nonce, 31, &client_hash, &reply,
                                     &reply_len));

  /* Now let's try this for real! */
  tt_int_op(0, OP_EQ,
            handle_client_auth_nonce(client_nonce, 32, &client_hash, &reply,
                                     &reply_len));
  tt_int_op(reply_len, OP_EQ, 64);
  tt_ptr_op(reply, OP_NE, NULL);
  tt_ptr_op(client_hash, OP_NE, NULL);
  /* Fill in the server nonce into the hash inputs... */
  memcpy(server_hash_input+46+32, reply+32, 32);
  memcpy(client_hash_input+46+32, reply+32, 32);
  /* Check the HMACs are correct... */
  crypto_hmac_sha256(hmac1, (char*)ext_or_auth_cookie, 32, server_hash_input,
                     46+32+32);
  crypto_hmac_sha256(hmac2, (char*)ext_or_auth_cookie, 32, client_hash_input,
                     46+32+32);
  tt_mem_op(hmac1,OP_EQ, reply, 32);
  tt_mem_op(hmac2,OP_EQ, client_hash, 32);

  /* Now do it again and make sure that the results are *different* */
  tt_int_op(0, OP_EQ,
            handle_client_auth_nonce(client_nonce, 32, &client_hash2, &reply2,
                                     &reply_len));
  tt_mem_op(reply2,OP_NE, reply, reply_len);
  tt_mem_op(client_hash2,OP_NE, client_hash, 32);
  /* But that this one checks out too. */
  memcpy(server_hash_input+46+32, reply2+32, 32);
  memcpy(client_hash_input+46+32, reply2+32, 32);
  /* Check the HMACs are correct... */
  crypto_hmac_sha256(hmac1, (char*)ext_or_auth_cookie, 32, server_hash_input,
                     46+32+32);
  crypto_hmac_sha256(hmac2, (char*)ext_or_auth_cookie, 32, client_hash_input,
                     46+32+32);
  tt_mem_op(hmac1,OP_EQ, reply2, 32);
  tt_mem_op(hmac2,OP_EQ, client_hash2, 32);

 done:
  tor_free(reply);
  tor_free(client_hash);
  tor_free(reply2);
  tor_free(client_hash2);
}

static void
test_ext_or_cookie_auth_testvec(void *arg)
{
  char *reply=NULL, *client_hash=NULL;
  size_t reply_len;
  char *mem_op_hex_tmp=NULL;

  const char client_nonce[] = "But when I look ahead up the whi";
  (void)arg;

  ext_or_auth_cookie = tor_malloc_zero(32);
  memcpy(ext_or_auth_cookie, "Gliding wrapt in a brown mantle," , 32);
  ext_or_auth_cookie_is_set = 1;

  testing_enable_prefilled_rng("te road There is always another ", 32);

  tt_int_op(0, OP_EQ,
            handle_client_auth_nonce(client_nonce, 32, &client_hash, &reply,
                                     &reply_len));
  tt_ptr_op(reply, OP_NE, NULL );
  tt_uint_op(reply_len, OP_EQ, 64);
  tt_mem_op(reply+32,OP_EQ, "te road There is always another ", 32);
  /* HMACSHA256("Gliding wrapt in a brown mantle,"
   *     "ExtORPort authentication server-to-client hash"
   *     "But when I look ahead up the write road There is always another ");
   */
  test_memeq_hex(reply,
                 "ec80ed6e546d3b36fdfc22fe1315416b"
                 "029f1ade7610d910878b62eeb7403821");
  /* HMACSHA256("Gliding wrapt in a brown mantle,"
   *     "ExtORPort authentication client-to-server hash"
   *     "But when I look ahead up the write road There is always another ");
   * (Both values computed using Python CLI.)
   */
  test_memeq_hex(client_hash,
                 "ab391732dd2ed968cd40c087d1b1f25b"
                 "33b3cd77ff79bd80c2074bbf438119a2");

 done:
  testing_disable_prefilled_rng();
  tor_free(reply);
  tor_free(client_hash);
  tor_free(mem_op_hex_tmp);
}

static void
ignore_bootstrap_problem(const char *warn, int reason,
                         or_connection_t *conn)
{
  (void)warn;
  (void)reason;
  (void)conn;
}

static int is_reading = 1;
static int handshake_start_called = 0;

static void
note_read_stopped(connection_t *conn)
{
  (void)conn;
  is_reading=0;
}
static void
note_read_started(connection_t *conn)
{
  (void)conn;
  is_reading=1;
}
static int
handshake_start(or_connection_t *conn, int receiving)
{
  if (!conn || !receiving)
    TT_FAIL(("Bad arguments to handshake_start"));
  handshake_start_called = 1;
  return 0;
}

#define WRITE(s,n)                                                      \
  do {                                                                  \
    buf_add(TO_CONN(conn)->inbuf, (s), (n));                           \
  } while (0)
#define CONTAINS(s,n)                                           \
  do {                                                          \
    tt_int_op((n), OP_LE, sizeof(b));                              \
    tt_int_op(buf_datalen(TO_CONN(conn)->outbuf), OP_EQ, (n));     \
    if ((n)) {                                                  \
      buf_get_bytes(TO_CONN(conn)->outbuf, b, (n));                \
      tt_mem_op(b, OP_EQ, (s), (n));                               \
    }                                                           \
  } while (0)

/* Helper: Do a successful Extended ORPort authentication handshake. */
static void
do_ext_or_handshake(or_connection_t *conn)
{
  char b[256];

  tt_int_op(0, OP_EQ, connection_ext_or_start_auth(conn));
  CONTAINS("\x01\x00", 2);
  WRITE("\x01", 1);
  WRITE("But when I look ahead up the whi", 32);
  testing_enable_prefilled_rng("te road There is always another ", 32);
  tt_int_op(0, OP_EQ, connection_ext_or_process_inbuf(conn));
  testing_disable_prefilled_rng();
  tt_int_op(TO_CONN(conn)->state, OP_EQ,
            EXT_OR_CONN_STATE_AUTH_WAIT_CLIENT_HASH);
  CONTAINS("\xec\x80\xed\x6e\x54\x6d\x3b\x36\xfd\xfc\x22\xfe\x13\x15\x41\x6b"
           "\x02\x9f\x1a\xde\x76\x10\xd9\x10\x87\x8b\x62\xee\xb7\x40\x38\x21"
           "te road There is always another ", 64);
  /* Send the right response this time. */
  WRITE("\xab\x39\x17\x32\xdd\x2e\xd9\x68\xcd\x40\xc0\x87\xd1\xb1\xf2\x5b"
        "\x33\xb3\xcd\x77\xff\x79\xbd\x80\xc2\x07\x4b\xbf\x43\x81\x19\xa2",
        32);
  tt_int_op(0, OP_EQ, connection_ext_or_process_inbuf(conn));
  CONTAINS("\x01", 1);
  tt_assert(! TO_CONN(conn)->marked_for_close);
  tt_int_op(TO_CONN(conn)->state, OP_EQ, EXT_OR_CONN_STATE_OPEN);

 done: ;
}

static void
test_ext_or_handshake(void *arg)
{
  or_connection_t *conn=NULL;
  char b[256];

  (void) arg;
  MOCK(connection_write_to_buf_impl_,
       connection_write_to_buf_impl_replacement);
  /* Use same authenticators as for test_ext_or_cookie_auth_testvec */
  ext_or_auth_cookie = tor_malloc_zero(32);
  memcpy(ext_or_auth_cookie, "Gliding wrapt in a brown mantle," , 32);
  ext_or_auth_cookie_is_set = 1;

  tor_init_connection_lists();

  conn = or_connection_new(CONN_TYPE_EXT_OR, AF_INET);
  tt_int_op(0, OP_EQ, connection_ext_or_start_auth(conn));
  /* The server starts by telling us about the one supported authtype. */
  CONTAINS("\x01\x00", 2);
  /* Say the client hasn't responded yet. */
  tt_int_op(0, OP_EQ, connection_ext_or_process_inbuf(conn));
  /* Let's say the client replies badly. */
  WRITE("\x99", 1);
  tt_int_op(-1, OP_EQ, connection_ext_or_process_inbuf(conn));
  CONTAINS("", 0);
  tt_assert(TO_CONN(conn)->marked_for_close);
  close_closeable_connections();
  conn = NULL;

  /* Okay, try again. */
  conn = or_connection_new(CONN_TYPE_EXT_OR, AF_INET);
  tt_int_op(0, OP_EQ, connection_ext_or_start_auth(conn));
  CONTAINS("\x01\x00", 2);
  /* Let's say the client replies sensibly this time. "Yes, AUTHTYPE_COOKIE
   * sounds delicious. Let's have some of that!" */
  WRITE("\x01", 1);
  /* Let's say that the client also sends part of a nonce. */
  WRITE("But when I look ", 16);
  tt_int_op(0, OP_EQ, connection_ext_or_process_inbuf(conn));
  CONTAINS("", 0);
  tt_int_op(TO_CONN(conn)->state, OP_EQ,
            EXT_OR_CONN_STATE_AUTH_WAIT_CLIENT_NONCE);
  /* Pump it again. Nothing should happen. */
  tt_int_op(0, OP_EQ, connection_ext_or_process_inbuf(conn));
  /* send the rest of the nonce. */
  WRITE("ahead up the whi", 16);
  testing_enable_prefilled_rng("te road There is always another ", 32);
  tt_int_op(0, OP_EQ, connection_ext_or_process_inbuf(conn));
  testing_disable_prefilled_rng();
  /* We should get the right reply from the server. */
  CONTAINS("\xec\x80\xed\x6e\x54\x6d\x3b\x36\xfd\xfc\x22\xfe\x13\x15\x41\x6b"
           "\x02\x9f\x1a\xde\x76\x10\xd9\x10\x87\x8b\x62\xee\xb7\x40\x38\x21"
           "te road There is always another ", 64);
  /* Send the wrong response. */
  WRITE("not with a bang but a whimper...", 32);
  MOCK(control_event_bootstrap_prob_or, ignore_bootstrap_problem);
  tt_int_op(-1, OP_EQ, connection_ext_or_process_inbuf(conn));
  CONTAINS("\x00", 1);
  tt_assert(TO_CONN(conn)->marked_for_close);
  /* XXXX Hold-open-until-flushed. */
  close_closeable_connections();
  conn = NULL;
  UNMOCK(control_event_bootstrap_prob_or);

  MOCK(connection_start_reading, note_read_started);
  MOCK(connection_stop_reading, note_read_stopped);
  MOCK(connection_tls_start_handshake, handshake_start);

  /* Okay, this time let's succeed. */
  conn = or_connection_new(CONN_TYPE_EXT_OR, AF_INET);
  do_ext_or_handshake(conn);

  /* Now let's run through some messages. */
  /* First let's send some junk and make sure it's ignored. */
  WRITE("\xff\xf0\x00\x03""ABC", 7);
  tt_int_op(0, OP_EQ, connection_ext_or_process_inbuf(conn));
  CONTAINS("", 0);
  /* Now let's send a USERADDR command. */
  WRITE("\x00\x01\x00\x0c""1.2.3.4:5678", 16);
  tt_int_op(0, OP_EQ, connection_ext_or_process_inbuf(conn));
  tt_int_op(TO_CONN(conn)->port, OP_EQ, 5678);
  tt_int_op(tor_addr_to_ipv4h(&TO_CONN(conn)->addr), OP_EQ, 0x01020304);
  /* Now let's send a TRANSPORT command. */
  WRITE("\x00\x02\x00\x07""rfc1149", 11);
  tt_int_op(0, OP_EQ, connection_ext_or_process_inbuf(conn));
  tt_ptr_op(NULL, OP_NE, conn->ext_or_transport);
  tt_str_op("rfc1149", OP_EQ, conn->ext_or_transport);
  tt_int_op(is_reading,OP_EQ,1);
  tt_int_op(TO_CONN(conn)->state, OP_EQ, EXT_OR_CONN_STATE_OPEN);
  /* DONE */
  WRITE("\x00\x00\x00\x00", 4);
  tt_int_op(0, OP_EQ, connection_ext_or_process_inbuf(conn));
  tt_int_op(TO_CONN(conn)->state, OP_EQ, EXT_OR_CONN_STATE_FLUSHING);
  tt_int_op(is_reading,OP_EQ,0);
  CONTAINS("\x10\x00\x00\x00", 4);
  tt_int_op(handshake_start_called,OP_EQ,0);
  tt_int_op(0, OP_EQ, connection_ext_or_finished_flushing(conn));
  tt_int_op(is_reading,OP_EQ,1);
  tt_int_op(handshake_start_called,OP_EQ,1);
  tt_int_op(TO_CONN(conn)->type, OP_EQ, CONN_TYPE_OR);
  tt_int_op(TO_CONN(conn)->state, OP_EQ, 0);
  connection_free_(TO_CONN(conn));
  conn = NULL;

  /* Okay, this time let's succeed the handshake but fail the USERADDR
     command. */
  conn = or_connection_new(CONN_TYPE_EXT_OR, AF_INET);
  do_ext_or_handshake(conn);
  /* USERADDR command with an extra NUL byte */
  WRITE("\x00\x01\x00\x0d""1.2.3.4:5678\x00", 17);
  MOCK(control_event_bootstrap_prob_or, ignore_bootstrap_problem);
  tt_int_op(-1, OP_EQ, connection_ext_or_process_inbuf(conn));
  CONTAINS("", 0);
  tt_assert(TO_CONN(conn)->marked_for_close);
  close_closeable_connections();
  conn = NULL;
  UNMOCK(control_event_bootstrap_prob_or);

  /* Now fail the TRANSPORT command. */
  conn = or_connection_new(CONN_TYPE_EXT_OR, AF_INET);
  do_ext_or_handshake(conn);
  /* TRANSPORT command with an extra NUL byte */
  WRITE("\x00\x02\x00\x08""rfc1149\x00", 12);
  MOCK(control_event_bootstrap_prob_or, ignore_bootstrap_problem);
  tt_int_op(-1, OP_EQ, connection_ext_or_process_inbuf(conn));
  CONTAINS("", 0);
  tt_assert(TO_CONN(conn)->marked_for_close);
  close_closeable_connections();
  conn = NULL;
  UNMOCK(control_event_bootstrap_prob_or);

  /* Now fail the TRANSPORT command. */
  conn = or_connection_new(CONN_TYPE_EXT_OR, AF_INET);
  do_ext_or_handshake(conn);
  /* TRANSPORT command with transport name with symbols (not a
     C-identifier) */
  WRITE("\x00\x02\x00\x07""rf*1149", 11);
  MOCK(control_event_bootstrap_prob_or, ignore_bootstrap_problem);
  tt_int_op(-1, OP_EQ, connection_ext_or_process_inbuf(conn));
  CONTAINS("", 0);
  tt_assert(TO_CONN(conn)->marked_for_close);
  close_closeable_connections();
  conn = NULL;
  UNMOCK(control_event_bootstrap_prob_or);

 done:
  UNMOCK(connection_write_to_buf_impl_);
  testing_disable_prefilled_rng();
  if (conn)
    connection_free_minimal(TO_CONN(conn));
#undef CONTAINS
#undef WRITE
}

struct testcase_t extorport_tests[] = {
  { "write_command", test_ext_or_write_command, TT_FORK, NULL, NULL },
  { "init_auth", test_ext_or_init_auth, TT_FORK, NULL, NULL },
  { "cookie_auth", test_ext_or_cookie_auth, TT_FORK, NULL, NULL },
  { "cookie_auth_testvec", test_ext_or_cookie_auth_testvec, TT_FORK,
    NULL, NULL },
  { "handshake", test_ext_or_handshake, TT_FORK, &helper_pubsub_setup, NULL },
  END_OF_TESTCASES
};
