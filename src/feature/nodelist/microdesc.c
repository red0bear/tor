/* Copyright (c) 2009-2021, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file microdesc.c
 *
 * \brief Implements microdescriptors -- an abbreviated description of
 *  less-frequently-changing router information.
 */

#include "core/or/or.h"

#include "lib/fdio/fdio.h"

#include "app/config/config.h"
#include "core/or/circuitbuild.h"
#include "core/or/policies.h"
#include "feature/client/entrynodes.h"
#include "feature/dircache/dirserv.h"
#include "feature/dirclient/dlstatus.h"
#include "feature/dirclient/dirclient_modes.h"
#include "feature/dircommon/directory.h"
#include "feature/dirparse/microdesc_parse.h"
#include "feature/nodelist/dirlist.h"
#include "feature/nodelist/microdesc.h"
#include "feature/nodelist/networkstatus.h"
#include "feature/nodelist/nodefamily.h"
#include "feature/nodelist/nodelist.h"
#include "feature/nodelist/routerlist.h"
#include "feature/relay/router.h"

#include "feature/nodelist/microdesc_st.h"
#include "feature/nodelist/networkstatus_st.h"
#include "feature/nodelist/node_st.h"
#include "feature/nodelist/routerstatus_st.h"

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

/** A data structure to hold a bunch of cached microdescriptors.  There are
 * two active files in the cache: a "cache file" that we mmap, and a "journal
 * file" that we append to.  Periodically, we rebuild the cache file to hold
 * only the microdescriptors that we want to keep */
struct microdesc_cache_t {
  /** Map from sha256-digest to microdesc_t for every microdesc_t in the
   * cache. */
  HT_HEAD(microdesc_map, microdesc_t) map;

  /** Name of the cache file. */
  char *cache_fname;
  /** Name of the journal file. */
  char *journal_fname;
  /** Mmap'd contents of the cache file, or NULL if there is none. */
  tor_mmap_t *cache_content;
  /** Number of bytes used in the journal file. */
  size_t journal_len;
  /** Number of bytes in descriptors removed as too old. */
  size_t bytes_dropped;

  /** Total bytes of microdescriptor bodies we have added to this cache */
  uint64_t total_len_seen;
  /** Total number of microdescriptors we have added to this cache */
  unsigned n_seen;

  /** True iff we have loaded this cache from disk ever. */
  int is_loaded;
};

static microdesc_cache_t *get_microdesc_cache_noload(void);
static void warn_if_nul_found(const char *inp, size_t len, int64_t offset,
                              const char *activity);

/** Helper: computes a hash of <b>md</b> to place it in a hash table. */
static inline unsigned int
microdesc_hash_(microdesc_t *md)
{
  return (unsigned) siphash24g(md->digest, sizeof(md->digest));
}

/** Helper: compares <b>a</b> and <b>b</b> for equality for hash-table
 * purposes. */
static inline int
microdesc_eq_(microdesc_t *a, microdesc_t *b)
{
  return tor_memeq(a->digest, b->digest, DIGEST256_LEN);
}

HT_PROTOTYPE(microdesc_map, microdesc_t, node,
             microdesc_hash_, microdesc_eq_);
HT_GENERATE2(microdesc_map, microdesc_t, node,
             microdesc_hash_, microdesc_eq_, 0.6,
             tor_reallocarray_, tor_free_);

/************************* md fetch fail cache *****************************/

/* If we end up with too many outdated dirservers, something probably went
 * wrong so clean up the list. */
#define TOO_MANY_OUTDATED_DIRSERVERS 30

/** List of dirservers with outdated microdesc information. The smartlist is
 *  filled with the hex digests of outdated dirservers. */
static smartlist_t *outdated_dirserver_list = NULL;

/** Note that we failed to fetch a microdescriptor from the relay with
 *  <b>relay_digest</b> (of size DIGEST_LEN). */
void
microdesc_note_outdated_dirserver(const char *relay_digest)
{
  char relay_hexdigest[HEX_DIGEST_LEN+1];

  /* If we have a reasonably live consensus, then most of our dirservers should
   * still be caching all the microdescriptors in it. Reasonably live
   * consensuses are up to a day old (or a day in the future). But
   * microdescriptors expire 7 days after the last consensus that referenced
   * them. */
  if (!networkstatus_get_reasonably_live_consensus(approx_time(),
                                                   FLAV_MICRODESC)) {
    return;
  }

  if (!outdated_dirserver_list) {
    outdated_dirserver_list = smartlist_new();
  }

  tor_assert(outdated_dirserver_list);

  /* If the list grows too big, clean it up */
  if (smartlist_len(outdated_dirserver_list) > TOO_MANY_OUTDATED_DIRSERVERS) {
    log_info(LD_GENERAL,"Too many outdated directory servers (%d). Resetting.",
             smartlist_len(outdated_dirserver_list));
    microdesc_reset_outdated_dirservers_list();
  }

  /* Turn the binary relay digest to a hex since smartlists have better support
   * for strings than digests. */
  base16_encode(relay_hexdigest,sizeof(relay_hexdigest),
                relay_digest, DIGEST_LEN);

  /* Make sure we don't add a dirauth as an outdated dirserver */
  if (router_get_trusteddirserver_by_digest(relay_digest)) {
    log_info(LD_GENERAL, "Auth %s gave us outdated dirinfo.", relay_hexdigest);
    return;
  }

  /* Don't double-add outdated dirservers */
  if (smartlist_contains_string(outdated_dirserver_list, relay_hexdigest)) {
    return;
  }

  /* Add it to the list of outdated dirservers */
  smartlist_add_strdup(outdated_dirserver_list, relay_hexdigest);

  log_info(LD_GENERAL, "Noted %s as outdated md dirserver", relay_hexdigest);
}

/** Return True if the relay with <b>relay_digest</b> (size DIGEST_LEN) is an
 *  outdated dirserver */
int
microdesc_relay_is_outdated_dirserver(const char *relay_digest)
{
  char relay_hexdigest[HEX_DIGEST_LEN+1];

  if (!outdated_dirserver_list) {
    return 0;
  }

  /* Convert identity digest to hex digest */
  base16_encode(relay_hexdigest, sizeof(relay_hexdigest),
                relay_digest, DIGEST_LEN);

  /* Last time we tried to fetch microdescs, was this directory mirror missing
   * any mds we asked for? */
  if (smartlist_contains_string(outdated_dirserver_list, relay_hexdigest)) {
    return 1;
  }

  return 0;
}

/** Reset the list of outdated dirservers. */
void
microdesc_reset_outdated_dirservers_list(void)
{
  if (!outdated_dirserver_list) {
    return;
  }

  SMARTLIST_FOREACH(outdated_dirserver_list, char *, cp, tor_free(cp));
  smartlist_clear(outdated_dirserver_list);
}

/****************************************************************************/

/** Write the body of <b>md</b> into <b>f</b>, with appropriate annotations.
 * On success, return the total number of bytes written, and set
 * *<b>annotation_len_out</b> to the number of bytes written as
 * annotations. */
static ssize_t
dump_microdescriptor(int fd, microdesc_t *md, size_t *annotation_len_out)
{
  ssize_t r = 0;
  ssize_t written;
  if (md->body == NULL) {
    *annotation_len_out = 0;
    return 0;
  }
  /* XXXX drops unknown annotations. */
  if (md->last_listed) {
    char buf[ISO_TIME_LEN+1];
    char annotation[ISO_TIME_LEN+32];
    format_iso_time(buf, md->last_listed);
    tor_snprintf(annotation, sizeof(annotation), "@last-listed %s\n", buf);
    if (write_all_to_fd(fd, annotation, strlen(annotation)) < 0) {
      log_warn(LD_DIR,
               "Couldn't write microdescriptor annotation: %s",
               strerror(errno));
      return -1;
    }
    r += strlen(annotation);
    *annotation_len_out = r;
  } else {
    *annotation_len_out = 0;
  }

  md->off = tor_fd_getpos(fd);
  warn_if_nul_found(md->body, md->bodylen, (int64_t) md->off,
                    "dumping a microdescriptor");
  written = write_all_to_fd(fd, md->body, md->bodylen);
  if (written != (ssize_t)md->bodylen) {
    written = written < 0 ? 0 : written;
    log_warn(LD_DIR,
             "Couldn't dump microdescriptor (wrote %ld out of %lu): %s",
             (long)written, (unsigned long)md->bodylen,
             strerror(errno));
    return -1;
  }
  r += md->bodylen;
  return r;
}

/** Holds a pointer to the current microdesc_cache_t object, or NULL if no
 * such object has been allocated. */
static microdesc_cache_t *the_microdesc_cache = NULL;

/** Return a pointer to the microdescriptor cache, loading it if necessary. */
microdesc_cache_t *
get_microdesc_cache(void)
{
  microdesc_cache_t *cache = get_microdesc_cache_noload();
  if (PREDICT_UNLIKELY(cache->is_loaded == 0)) {
    microdesc_cache_reload(cache);
  }
  return cache;
}

/** Return a pointer to the microdescriptor cache, creating (but not loading)
 * it if necessary. */
static microdesc_cache_t *
get_microdesc_cache_noload(void)
{
  if (PREDICT_UNLIKELY(the_microdesc_cache==NULL)) {
    microdesc_cache_t *cache = tor_malloc_zero(sizeof(*cache));
    HT_INIT(microdesc_map, &cache->map);
    cache->cache_fname = get_cachedir_fname("cached-microdescs");
    cache->journal_fname = get_cachedir_fname("cached-microdescs.new");
    the_microdesc_cache = cache;
  }
  return the_microdesc_cache;
}

/* There are three sources of microdescriptors:
   1) Generated by us while acting as a directory authority.
   2) Loaded from the cache on disk.
   3) Downloaded.
*/

/** Decode the microdescriptors from the string starting at <b>s</b> and
 * ending at <b>eos</b>, and store them in <b>cache</b>.  If <b>no_save</b>,
 * mark them as non-writable to disk.  If <b>where</b> is SAVED_IN_CACHE,
 * leave their bodies as pointers to the mmap'd cache.  If where is
 * <b>SAVED_NOWHERE</b>, do not allow annotations.  If listed_at is not -1,
 * set the last_listed field of every microdesc to listed_at.  If
 * requested_digests is non-null, then it contains a list of digests we mean
 * to allow, so we should reject any non-requested microdesc with a different
 * digest, and alter the list to contain only the digests of those microdescs
 * we didn't find.
 * Return a newly allocated list of the added microdescriptors, or NULL  */
smartlist_t *
microdescs_add_to_cache(microdesc_cache_t *cache,
                        const char *s, const char *eos, saved_location_t where,
                        int no_save, time_t listed_at,
                        smartlist_t *requested_digests256)
{
  void * const DIGEST_REQUESTED = (void*)1;
  void * const DIGEST_RECEIVED = (void*)2;
  void * const DIGEST_INVALID = (void*)3;

  smartlist_t *descriptors, *added;
  const int allow_annotations = (where != SAVED_NOWHERE);
  smartlist_t *invalid_digests = smartlist_new();

  descriptors = microdescs_parse_from_string(s, eos,
                                             allow_annotations,
                                             where, invalid_digests);
  if (listed_at != (time_t)-1) {
    SMARTLIST_FOREACH(descriptors, microdesc_t *, md,
                      md->last_listed = listed_at);
  }
  if (requested_digests256) {
    digest256map_t *requested;
    requested = digest256map_new();
    /* Set requested[d] to DIGEST_REQUESTED for every md we requested. */
    SMARTLIST_FOREACH(requested_digests256, const uint8_t *, cp,
      digest256map_set(requested, cp, DIGEST_REQUESTED));
    /* Set requested[d] to DIGEST_INVALID for every md we requested which we
     * will never be able to parse.  Remove the ones we didn't request from
     * invalid_digests.
     */
    SMARTLIST_FOREACH_BEGIN(invalid_digests, uint8_t *, cp) {
      if (digest256map_get(requested, cp)) {
        digest256map_set(requested, cp, DIGEST_INVALID);
      } else {
        tor_free(cp);
        SMARTLIST_DEL_CURRENT(invalid_digests, cp);
      }
    } SMARTLIST_FOREACH_END(cp);
    /* Update requested[d] to 2 for the mds we asked for and got. Delete the
     * ones we never requested from the 'descriptors' smartlist.
     */
    SMARTLIST_FOREACH_BEGIN(descriptors, microdesc_t *, md) {
      if (digest256map_get(requested, (const uint8_t*)md->digest)) {
        digest256map_set(requested, (const uint8_t*)md->digest,
                         DIGEST_RECEIVED);
      } else {
        log_fn(LOG_PROTOCOL_WARN, LD_DIR, "Received non-requested microdesc");
        microdesc_free(md);
        SMARTLIST_DEL_CURRENT(descriptors, md);
      }
    } SMARTLIST_FOREACH_END(md);
    /* Remove the ones we got or the invalid ones from requested_digests256.
     */
    SMARTLIST_FOREACH_BEGIN(requested_digests256, uint8_t *, cp) {
      void *status = digest256map_get(requested, cp);
      if (status == DIGEST_RECEIVED || status == DIGEST_INVALID) {
        tor_free(cp);
        SMARTLIST_DEL_CURRENT(requested_digests256, cp);
      }
    } SMARTLIST_FOREACH_END(cp);
    digest256map_free(requested, NULL);
  }

  /* For every requested microdescriptor that was unparseable, mark it
   * as not to be retried. */
  if (smartlist_len(invalid_digests)) {
    networkstatus_t *ns =
      networkstatus_get_latest_consensus_by_flavor(FLAV_MICRODESC);
    if (ns) {
      SMARTLIST_FOREACH_BEGIN(invalid_digests, char *, d) {
        routerstatus_t *rs =
          router_get_mutable_consensus_status_by_descriptor_digest(ns, d);
        if (rs && tor_memeq(d, rs->descriptor_digest, DIGEST256_LEN)) {
          download_status_mark_impossible(&rs->dl_status);
        }
      } SMARTLIST_FOREACH_END(d);
    }
  }
  SMARTLIST_FOREACH(invalid_digests, uint8_t *, d, tor_free(d));
  smartlist_free(invalid_digests);

  added = microdescs_add_list_to_cache(cache, descriptors, where, no_save);
  smartlist_free(descriptors);
  return added;
}

/** As microdescs_add_to_cache, but takes a list of microdescriptors instead of
 * a string to decode.  Frees any members of <b>descriptors</b> that it does
 * not add. */
smartlist_t *
microdescs_add_list_to_cache(microdesc_cache_t *cache,
                             smartlist_t *descriptors, saved_location_t where,
                             int no_save)
{
  smartlist_t *added;
  open_file_t *open_file = NULL;
  int fd = -1;
  //  int n_added = 0;
  ssize_t size = 0;

  if (where == SAVED_NOWHERE && !no_save) {
    fd = start_writing_to_file(cache->journal_fname,
                               OPEN_FLAGS_APPEND|O_BINARY,
                               0600, &open_file);
    if (fd < 0) {
      log_warn(LD_DIR, "Couldn't append to journal in %s: %s",
               cache->journal_fname, strerror(errno));
    }
  }

  added = smartlist_new();
  SMARTLIST_FOREACH_BEGIN(descriptors, microdesc_t *, md) {
    microdesc_t *md2;
    md2 = HT_FIND(microdesc_map, &cache->map, md);
    if (md2) {
      /* We already had this one. */
      if (md2->last_listed < md->last_listed)
        md2->last_listed = md->last_listed;
      microdesc_free(md);
      if (where != SAVED_NOWHERE)
        cache->bytes_dropped += size;
      continue;
    }

    /* Okay, it's a new one. */
    if (fd >= 0) {
      size_t annotation_len;
      size = dump_microdescriptor(fd, md, &annotation_len);
      if (size < 0) {
        /* we already warned in dump_microdescriptor */
        abort_writing_to_file(open_file);
        fd = -1;
      } else {
        md->saved_location = SAVED_IN_JOURNAL;
        cache->journal_len += size;
      }
    } else {
      md->saved_location = where;
    }

    md->no_save = no_save;

    HT_INSERT(microdesc_map, &cache->map, md);
    md->held_in_map = 1;
    smartlist_add(added, md);
    ++cache->n_seen;
    cache->total_len_seen += md->bodylen;
  } SMARTLIST_FOREACH_END(md);

  if (fd >= 0) {
    if (finish_writing_to_file(open_file) < 0) {
      log_warn(LD_DIR, "Error appending to microdescriptor file: %s",
               strerror(errno));
      smartlist_clear(added);
      return added;
    }
  }

  {
    networkstatus_t *ns = networkstatus_get_latest_consensus();
    if (ns && ns->flavor == FLAV_MICRODESC)
      SMARTLIST_FOREACH(added, microdesc_t *, md, nodelist_add_microdesc(md));
  }

  if (smartlist_len(added))
    router_dir_info_changed();

  return added;
}

/** Remove every microdescriptor in <b>cache</b>. */
void
microdesc_cache_clear(microdesc_cache_t *cache)
{
  microdesc_t **entry, **next;

  for (entry = HT_START(microdesc_map, &cache->map); entry; entry = next) {
    microdesc_t *md = *entry;
    next = HT_NEXT_RMV(microdesc_map, &cache->map, entry);
    md->held_in_map = 0;
    microdesc_free(md);
  }
  HT_CLEAR(microdesc_map, &cache->map);
  if (cache->cache_content) {
    int res = tor_munmap_file(cache->cache_content);
    if (res != 0) {
      log_warn(LD_FS,
               "tor_munmap_file() failed clearing microdesc cache; "
               "we are probably about to leak memory.");
      /* TODO something smarter? */
    }
    cache->cache_content = NULL;
  }
  cache->total_len_seen = 0;
  cache->n_seen = 0;
  cache->bytes_dropped = 0;
}

static void
warn_if_nul_found(const char *inp, size_t len, int64_t offset,
                  const char *activity)
{
  const char *nul_found = memchr(inp, 0, len);
  if (BUG(nul_found)) {
    log_warn(LD_BUG, "Found unexpected NUL while %s, offset %"PRId64
             "at position %"TOR_PRIuSZ"/%"TOR_PRIuSZ".",
             activity, offset, (nul_found - inp), len);
    const char *start_excerpt_at, *eos = inp + len;
    if ((nul_found - inp) >= 16)
      start_excerpt_at = nul_found - 16;
    else
      start_excerpt_at = inp;
    size_t excerpt_len = MIN(32, eos - start_excerpt_at);
    char tmp[65];
    base16_encode(tmp, sizeof(tmp), start_excerpt_at, excerpt_len);
    log_warn(LD_BUG, "      surrounding string: %s", tmp);
  }
}

/** Reload the contents of <b>cache</b> from disk.  If it is empty, load it
 * for the first time.  Return 0 on success, -1 on failure. */
int
microdesc_cache_reload(microdesc_cache_t *cache)
{
  struct stat st;
  char *journal_content;
  smartlist_t *added;
  tor_mmap_t *mm;
  int total = 0;

  microdesc_cache_clear(cache);

  cache->is_loaded = 1;

  mm = cache->cache_content = tor_mmap_file(cache->cache_fname);
  if (mm) {
    warn_if_nul_found(mm->data, mm->size, 0, "scanning microdesc cache");
    added = microdescs_add_to_cache(cache, mm->data, mm->data+mm->size,
                                    SAVED_IN_CACHE, 0, -1, NULL);
    if (added) {
      total += smartlist_len(added);
      smartlist_free(added);
    }
  }

  journal_content = read_file_to_str(cache->journal_fname,
                                     RFTS_IGNORE_MISSING, &st);
  if (journal_content) {
    cache->journal_len = strlen(journal_content);
    warn_if_nul_found(journal_content, (size_t)st.st_size, 0,
                      "reading microdesc journal");
    added = microdescs_add_to_cache(cache, journal_content,
                                    journal_content+st.st_size,
                                    SAVED_IN_JOURNAL, 0, -1, NULL);
    if (added) {
      total += smartlist_len(added);
      smartlist_free(added);
    }
    tor_free(journal_content);
  }
  log_info(LD_DIR, "Reloaded microdescriptor cache. Found %d descriptors.",
           total);

  microdesc_cache_rebuild(cache, 0 /* don't force */);

  return 0;
}

/** By default, we remove any microdescriptors that have gone at least this
 * long without appearing in a current consensus. */
#define TOLERATE_MICRODESC_AGE (7*24*60*60)

/** Remove all microdescriptors from <b>cache</b> that haven't been listed for
 * a long time.  Does not rebuild the cache on disk.  If <b>cutoff</b> is
 * positive, specifically remove microdescriptors that have been unlisted
 * since <b>cutoff</b>.  If <b>force</b> is true, remove microdescriptors even
 * if we have no current live microdescriptor consensus.
 */
void
microdesc_cache_clean(microdesc_cache_t *cache, time_t cutoff, int force)
{
  microdesc_t **mdp, *victim;
  int dropped=0, kept=0;
  size_t bytes_dropped = 0;
  time_t now = time(NULL);

  /* If we don't know a reasonably live consensus, don't believe last_listed
   * values: we might be starting up after being down for a while. */
  if (! force &&
      ! networkstatus_get_reasonably_live_consensus(now, FLAV_MICRODESC))
      return;

  if (cutoff <= 0)
    cutoff = now - TOLERATE_MICRODESC_AGE;

  for (mdp = HT_START(microdesc_map, &cache->map); mdp != NULL; ) {
    const int is_old = (*mdp)->last_listed < cutoff;
    const unsigned held_by_nodes = (*mdp)->held_by_nodes;
    if (is_old && !held_by_nodes) {
      ++dropped;
      victim = *mdp;
      mdp = HT_NEXT_RMV(microdesc_map, &cache->map, mdp);
      victim->held_in_map = 0;
      bytes_dropped += victim->bodylen;
      microdesc_free(victim);
    } else {
      if (is_old) {
        /* It's old, but it has held_by_nodes set.  That's not okay. */
        /* Let's try to diagnose and fix #7164 . */
        smartlist_t *nodes = nodelist_find_nodes_with_microdesc(*mdp);
        const networkstatus_t *ns = networkstatus_get_latest_consensus();
        long networkstatus_age = -1;
        const int ht_badness = HT_REP_IS_BAD_(microdesc_map, &cache->map);
        if (ns) {
          networkstatus_age = now - ns->valid_after;
        }
        log_warn(LD_BUG, "Microdescriptor seemed very old "
                 "(last listed %d hours ago vs %d hour cutoff), but is still "
                 "marked as being held by %d node(s). I found %d node(s) "
                 "holding it. Current networkstatus is %ld hours old. "
                 "Hashtable badness is %d.",
                 (int)((now - (*mdp)->last_listed) / 3600),
                 (int)((now - cutoff) / 3600),
                 held_by_nodes,
                 smartlist_len(nodes),
                 networkstatus_age / 3600,
                 ht_badness);

        SMARTLIST_FOREACH_BEGIN(nodes, const node_t *, node) {
          const char *rs_match = "No RS";
          const char *rs_present = "";
          if (node->rs) {
            if (tor_memeq(node->rs->descriptor_digest,
                          (*mdp)->digest, DIGEST256_LEN)) {
              rs_match = "Microdesc digest in RS matches";
            } else {
              rs_match = "Microdesc digest in RS does not match";
            }
            if (ns) {
              /* This should be impossible, but let's see! */
              rs_present = " RS not present in networkstatus.";
              SMARTLIST_FOREACH(ns->routerstatus_list, routerstatus_t *,rs, {
                if (rs == node->rs) {
                  rs_present = " RS okay in networkstatus.";
                }
              });
            }
          }
          log_warn(LD_BUG, "  [%d]: ID=%s. md=%p, rs=%p, ri=%p. %s.%s",
                   node_sl_idx,
                   hex_str(node->identity, DIGEST_LEN),
                   node->md, node->rs, node->ri, rs_match, rs_present);
        } SMARTLIST_FOREACH_END(node);
        smartlist_free(nodes);
        (*mdp)->last_listed = now;
      }

      ++kept;
      mdp = HT_NEXT(microdesc_map, &cache->map, mdp);
    }
  }

  if (dropped) {
    log_info(LD_DIR, "Removed %d/%d microdescriptors as old.",
             dropped,dropped+kept);
    cache->bytes_dropped += bytes_dropped;
  }
}

static int
should_rebuild_md_cache(microdesc_cache_t *cache)
{
    const size_t old_len =
      cache->cache_content ? cache->cache_content->size : 0;
    const size_t journal_len = cache->journal_len;
    const size_t dropped = cache->bytes_dropped;

    if (journal_len < 16384)
      return 0; /* Don't bother, not enough has happened yet. */
    if (dropped > (journal_len + old_len) / 3)
      return 1; /* We could save 1/3 or more of the currently used space. */
    if (journal_len > old_len / 2)
      return 1; /* We should append to the regular file */

    return 0;
}

/**
 * Mark <b>md</b> as having no body, and release any storage previously held
 * by its body.
 */
static void
microdesc_wipe_body(microdesc_t *md)
{
  if (!md)
    return;

  if (md->saved_location != SAVED_IN_CACHE)
    tor_free(md->body);

  md->off = 0;
  md->saved_location = SAVED_NOWHERE;
  md->body = NULL;
  md->bodylen = 0;
  md->no_save = 1;
}

/** Regenerate the main cache file for <b>cache</b>, clear the journal file,
 * and update every microdesc_t in the cache with pointers to its new
 * location.  If <b>force</b> is true, do this unconditionally.  If
 * <b>force</b> is false, do it only if we expect to save space on disk. */
int
microdesc_cache_rebuild(microdesc_cache_t *cache, int force)
{
  open_file_t *open_file;
  int fd = -1, res;
  microdesc_t **mdp;
  smartlist_t *wrote;
  ssize_t size;
  off_t off = 0, off_real;
  int orig_size, new_size;

  if (cache == NULL) {
    cache = the_microdesc_cache;
    if (cache == NULL)
      return 0;
  }

  /* Remove dead descriptors */
  microdesc_cache_clean(cache, 0/*cutoff*/, 0/*force*/);

  if (!force && !should_rebuild_md_cache(cache))
    return 0;

  log_info(LD_DIR, "Rebuilding the microdescriptor cache...");

  orig_size = (int)(cache->cache_content ? cache->cache_content->size : 0);
  orig_size += (int)cache->journal_len;

  fd = start_writing_to_file(cache->cache_fname,
                             OPEN_FLAGS_REPLACE|O_BINARY,
                             0600, &open_file);
  if (fd < 0)
    return -1;

  wrote = smartlist_new();

  HT_FOREACH(mdp, microdesc_map, &cache->map) {
    microdesc_t *md = *mdp;
    size_t annotation_len;
    if (md->no_save || !md->body)
      continue;

    size = dump_microdescriptor(fd, md, &annotation_len);
    if (size < 0) {
      microdesc_wipe_body(md);

      /* rewind, in case it was a partial write. */
      tor_fd_setpos(fd, off);
      continue;
    }
    tor_assert(((size_t)size) == annotation_len + md->bodylen);
    md->off = off + annotation_len;
    off += size;
    off_real = tor_fd_getpos(fd);
    if (off_real != off) {
      log_warn(LD_BUG, "Discontinuity in position in microdescriptor cache."
               "By my count, I'm at %"PRId64
               ", but I should be at %"PRId64,
               (int64_t)(off), (int64_t)(off_real));
      if (off_real >= 0)
        off = off_real;
    }
    if (md->saved_location != SAVED_IN_CACHE) {
      tor_free(md->body);
      md->saved_location = SAVED_IN_CACHE;
    }
    smartlist_add(wrote, md);
  }

  /* We must do this unmap _before_ we call finish_writing_to_file(), or
   * windows will not actually replace the file. */
  if (cache->cache_content) {
    res = tor_munmap_file(cache->cache_content);
    if (res != 0) {
      log_warn(LD_FS,
               "Failed to unmap old microdescriptor cache while rebuilding");
    }
    cache->cache_content = NULL;
  }

  if (finish_writing_to_file(open_file) < 0) {
    log_warn(LD_DIR, "Error rebuilding microdescriptor cache: %s",
             strerror(errno));
    /* Okay. Let's prevent from making things worse elsewhere. */
    cache->cache_content = NULL;
    HT_FOREACH(mdp, microdesc_map, &cache->map) {
      microdesc_t *md = *mdp;
      if (md->saved_location == SAVED_IN_CACHE) {
        microdesc_wipe_body(md);
      }
    }
    smartlist_free(wrote);
    return -1;
  }

  cache->cache_content = tor_mmap_file(cache->cache_fname);

  if (!cache->cache_content && smartlist_len(wrote)) {
    log_err(LD_DIR, "Couldn't map file that we just wrote to %s!",
            cache->cache_fname);
    smartlist_free(wrote);
    return -1;
  }
  SMARTLIST_FOREACH_BEGIN(wrote, microdesc_t *, md) {
    tor_assert(md->saved_location == SAVED_IN_CACHE);
    md->body = (char*)cache->cache_content->data + md->off;
    if (PREDICT_UNLIKELY(
             md->bodylen < 9 || fast_memneq(md->body, "onion-key", 9) != 0)) {
      /* XXXX once bug 2022 is solved, we can kill this block and turn it
       * into just the tor_assert(fast_memeq) */
      off_t avail = cache->cache_content->size - md->off;
      char *bad_str;
      tor_assert(avail >= 0);
      bad_str = tor_strndup(md->body, MIN(128, (size_t)avail));
      log_err(LD_BUG, "After rebuilding microdesc cache, offsets seem wrong. "
              " At offset %d, I expected to find a microdescriptor starting "
              " with \"onion-key\".  Instead I got %s.",
              (int)md->off, escaped(bad_str));
      tor_free(bad_str);
      tor_assert(fast_memeq(md->body, "onion-key", 9));
    }
  } SMARTLIST_FOREACH_END(md);

  smartlist_free(wrote);

  write_str_to_file(cache->journal_fname, "", 1);
  cache->journal_len = 0;
  cache->bytes_dropped = 0;

  new_size = cache->cache_content ? (int)cache->cache_content->size : 0;
  log_info(LD_DIR, "Done rebuilding microdesc cache. "
           "Saved %d bytes; %d still used.",
           orig_size-new_size, new_size);

  return 0;
}

/** Make sure that the reference count of every microdescriptor in cache is
 * accurate. */
void
microdesc_check_counts(void)
{
  microdesc_t **mdp;
  if (!the_microdesc_cache)
    return;

  HT_FOREACH(mdp, microdesc_map, &the_microdesc_cache->map) {
    microdesc_t *md = *mdp;
    unsigned int found=0;
    const smartlist_t *nodes = nodelist_get_list();
    SMARTLIST_FOREACH(nodes, node_t *, node, {
        if (node->md == md) {
          ++found;
        }
      });
    tor_assert(found == md->held_by_nodes);
  }
}

/** Deallocate a single microdescriptor.  Note: the microdescriptor MUST have
 * previously been removed from the cache if it had ever been inserted. */
void
microdesc_free_(microdesc_t *md, const char *fname, int lineno)
{
  if (!md)
    return;

  /* Make sure that the microdesc was really removed from the appropriate data
     structures. */
  if (md->held_in_map) {
    microdesc_cache_t *cache = get_microdesc_cache_noload();
    microdesc_t *md2 = HT_FIND(microdesc_map, &cache->map, md);
    if (md2 == md) {
      log_warn(LD_BUG, "microdesc_free() called from %s:%d, but md was still "
               "in microdesc_map", fname, lineno);
      HT_REMOVE(microdesc_map, &cache->map, md);
    } else {
      log_warn(LD_BUG, "microdesc_free() called from %s:%d with held_in_map "
               "set, but microdesc was not in the map.", fname, lineno);
    }
    tor_fragile_assert();
  }
  if (md->held_by_nodes) {
    microdesc_cache_t *cache = get_microdesc_cache_noload();
    int found=0;
    const smartlist_t *nodes = nodelist_get_list();
    const int ht_badness = HT_REP_IS_BAD_(microdesc_map, &cache->map);
    SMARTLIST_FOREACH(nodes, node_t *, node, {
        if (node->md == md) {
          ++found;
          node->md = NULL;
        }
      });
    if (found) {
      log_warn(LD_BUG, "microdesc_free() called from %s:%d, but md was still "
               "referenced %d node(s); held_by_nodes == %u, ht_badness == %d",
               fname, lineno, found, md->held_by_nodes, ht_badness);
    } else {
      log_warn(LD_BUG, "microdesc_free() called from %s:%d with held_by_nodes "
               "set to %u, but md was not referenced by any nodes. "
               "ht_badness == %d",
               fname, lineno, md->held_by_nodes, ht_badness);
    }
    tor_fragile_assert();
  }
  //tor_assert(md->held_in_map == 0);
  //tor_assert(md->held_by_nodes == 0);

  tor_free(md->onion_curve25519_pkey);
  tor_free(md->ed25519_identity_pkey);
  if (md->body && md->saved_location != SAVED_IN_CACHE)
    tor_free(md->body);

  nodefamily_free(md->family);
  if (md->family_ids) {
    SMARTLIST_FOREACH(md->family_ids, char *, cp, tor_free(cp));
    smartlist_free(md->family_ids);
  }
  short_policy_free(md->exit_policy);
  short_policy_free(md->ipv6_exit_policy);

  tor_free(md);
}

/** Free all storage held in the microdesc.c module. */
void
microdesc_free_all(void)
{
  if (the_microdesc_cache) {
    microdesc_cache_clear(the_microdesc_cache);
    tor_free(the_microdesc_cache->cache_fname);
    tor_free(the_microdesc_cache->journal_fname);
    tor_free(the_microdesc_cache);
  }

  if (outdated_dirserver_list) {
    SMARTLIST_FOREACH(outdated_dirserver_list, char *, cp, tor_free(cp));
    smartlist_free(outdated_dirserver_list);
  }
}

/** If there is a microdescriptor in <b>cache</b> whose sha256 digest is
 * <b>d</b>, return it.  Otherwise return NULL. */
microdesc_t *
microdesc_cache_lookup_by_digest256(microdesc_cache_t *cache, const char *d)
{
  microdesc_t *md, search;
  if (!cache)
    cache = get_microdesc_cache();
  memcpy(search.digest, d, DIGEST256_LEN);
  md = HT_FIND(microdesc_map, &cache->map, &search);
  return md;
}

/** Return a smartlist of all the sha256 digest of the microdescriptors that
 * are listed in <b>ns</b> but not present in <b>cache</b>. Returns pointers
 * to internals of <b>ns</b>; you should not free the members of the resulting
 * smartlist.  Omit all microdescriptors whose digest appear in <b>skip</b>. */
smartlist_t *
microdesc_list_missing_digest256(networkstatus_t *ns, microdesc_cache_t *cache,
                                 int downloadable_only, digest256map_t *skip)
{
  smartlist_t *result = smartlist_new();
  time_t now = time(NULL);
  tor_assert(ns->flavor == FLAV_MICRODESC);
  SMARTLIST_FOREACH_BEGIN(ns->routerstatus_list, routerstatus_t *, rs) {
    if (microdesc_cache_lookup_by_digest256(cache, rs->descriptor_digest))
      continue;
    if (downloadable_only &&
        !download_status_is_ready(&rs->dl_status, now))
      continue;
    if (skip && digest256map_get(skip, (const uint8_t*)rs->descriptor_digest))
      continue;
    if (fast_mem_is_zero(rs->descriptor_digest, DIGEST256_LEN))
      continue;
    /* XXXX Also skip if we're a noncache and wouldn't use this router.
     * XXXX NM Microdesc
     */
    smartlist_add(result, rs->descriptor_digest);
  } SMARTLIST_FOREACH_END(rs);
  return result;
}

/** Launch download requests for microdescriptors as appropriate.
 *
 * Specifically, we should launch download requests if we are configured to
 * download mirodescriptors, and there are some microdescriptors listed in the
 * current microdesc consensus that we don't have, and either we never asked
 * for them, or we failed to download them but we're willing to retry.
 */
void
update_microdesc_downloads(time_t now)
{
  const or_options_t *options = get_options();
  networkstatus_t *consensus;
  smartlist_t *missing;
  digest256map_t *pending;

  if (should_delay_dir_fetches(options, NULL))
    return;
  if (dirclient_too_idle_to_fetch_descriptors(options, now))
    return;

  /* Give up if we don't have a reasonably live consensus. */
  consensus = networkstatus_get_reasonably_live_consensus(now, FLAV_MICRODESC);
  if (!consensus)
    return;

  if (!we_fetch_microdescriptors(options))
    return;

  pending = digest256map_new();
  list_pending_microdesc_downloads(pending);

  missing = microdesc_list_missing_digest256(consensus,
                                             get_microdesc_cache(),
                                             1,
                                             pending);
  digest256map_free(pending, NULL);

  launch_descriptor_downloads(DIR_PURPOSE_FETCH_MICRODESC,
                              missing, NULL, now);

  smartlist_free(missing);
}

/** For every microdescriptor listed in the current microdescriptor consensus,
 * update its last_listed field to be at least as recent as the publication
 * time of the current microdescriptor consensus.
 */
void
update_microdescs_from_networkstatus(time_t now)
{
  microdesc_cache_t *cache = get_microdesc_cache();
  microdesc_t *md;
  networkstatus_t *ns =
    networkstatus_get_reasonably_live_consensus(now, FLAV_MICRODESC);

  if (! ns)
    return;

  tor_assert(ns->flavor == FLAV_MICRODESC);

  SMARTLIST_FOREACH_BEGIN(ns->routerstatus_list, routerstatus_t *, rs) {
    md = microdesc_cache_lookup_by_digest256(cache, rs->descriptor_digest);
    if (md && ns->valid_after > md->last_listed)
      md->last_listed = ns->valid_after;
  } SMARTLIST_FOREACH_END(rs);
}

/** Return true iff we should prefer to use microdescriptors rather than
 * routerdescs for building circuits. */
int
we_use_microdescriptors_for_circuits(const or_options_t *options)
{
  if (options->UseMicrodescriptors == 0)
    return 0; /* the user explicitly picked no */
  return 1; /* yes and auto both mean yes */
}

/** Return true iff we should try to download microdescriptors at all. */
int
we_fetch_microdescriptors(const or_options_t *options)
{
  if (directory_caches_dir_info(options))
    return 1;
  if (options->FetchUselessDescriptors)
    return 1;
  return we_use_microdescriptors_for_circuits(options);
}

/** Return true iff we should try to download router descriptors at all. */
int
we_fetch_router_descriptors(const or_options_t *options)
{
  if (directory_caches_dir_info(options))
    return 1;
  if (options->FetchUselessDescriptors)
    return 1;
  return ! we_use_microdescriptors_for_circuits(options);
}

/** Return the consensus flavor we actually want to use to build circuits. */
MOCK_IMPL(int,
usable_consensus_flavor,(void))
{
  if (we_use_microdescriptors_for_circuits(get_options())) {
    return FLAV_MICRODESC;
  } else {
    return FLAV_NS;
  }
}
