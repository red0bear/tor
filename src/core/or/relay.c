/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2021, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file relay.c
 * \brief Handle relay cell encryption/decryption, plus packaging and
 *    receiving from circuits, plus queuing on circuits.
 *
 * This is a core modules that makes Tor work. It's responsible for
 * dealing with RELAY cells (the ones that travel more than one hop along a
 * circuit), by:
 *  <ul>
 *   <li>constructing relays cells,
 *   <li>encrypting relay cells,
 *   <li>decrypting relay cells,
 *   <li>demultiplexing relay cells as they arrive on a connection,
 *   <li>queueing relay cells for retransmission,
 *   <li>or handling relay cells that are for us to receive (as an exit or a
 *   client).
 *  </ul>
 *
 * RELAY cells are generated throughout the code at the client or relay side,
 * using relay_send_command_from_edge() or one of the functions like
 * connection_edge_send_command() that calls it.  Of particular interest is
 * connection_edge_package_raw_inbuf(), which takes information that has
 * arrived on an edge connection socket, and packages it as a RELAY_DATA cell
 * -- this is how information is actually sent across the Tor network.  The
 * cryptography for these functions is handled deep in
 * circuit_package_relay_cell(), which either adds a single layer of
 * encryption (if we're an exit), or multiple layers (if we're the origin of
 * the circuit).  After construction and encryption, the RELAY cells are
 * passed to append_cell_to_circuit_queue(), which queues them for
 * transmission and tells the circuitmux (see circuitmux.c) that the circuit
 * is waiting to send something.
 *
 * Incoming RELAY cells arrive at circuit_receive_relay_cell(), called from
 * command.c.  There they are decrypted and, if they are for us, are passed to
 * connection_edge_process_relay_cell(). If they're not for us, they're
 * re-queued for retransmission again with append_cell_to_circuit_queue().
 *
 * The connection_edge_process_relay_cell() function handles all the different
 * types of relay cells, launching requests or transmitting data as needed.
 **/

#include "lib/log/log.h"
#define RELAY_PRIVATE
#include "core/or/or.h"
#include "feature/client/addressmap.h"
#include "lib/err/backtrace.h"
#include "lib/buf/buffers.h"
#include "core/or/channel.h"
#include "feature/client/circpathbias.h"
#include "core/or/circuitbuild.h"
#include "core/or/circuitlist.h"
#include "core/or/circuituse.h"
#include "core/or/circuitpadding.h"
#include "core/or/extendinfo.h"
#include "lib/compress/compress.h"
#include "app/config/config.h"
#include "core/mainloop/connection.h"
#include "core/or/connection_edge.h"
#include "core/or/connection_or.h"
#include "feature/control/control_events.h"
#include "lib/crypt_ops/crypto_rand.h"
#include "lib/crypt_ops/crypto_util.h"
#include "feature/dircommon/directory.h"
#include "feature/relay/dns.h"
#include "feature/relay/circuitbuild_relay.h"
#include "feature/stats/geoip_stats.h"
#include "feature/hs/hs_cache.h"
#include "core/mainloop/mainloop.h"
#include "feature/nodelist/networkstatus.h"
#include "feature/nodelist/nodelist.h"
#include "core/or/onion.h"
#include "core/or/policies.h"
#include "core/or/reasons.h"
#include "core/or/relay.h"
#include "core/crypto/relay_crypto.h"
#include "feature/rend/rendcommon.h"
#include "feature/nodelist/describe.h"
#include "feature/nodelist/routerlist.h"
#include "core/or/scheduler.h"
#include "feature/hs/hs_metrics.h"
#include "feature/stats/rephist.h"
#include "core/or/relay_msg.h"

#include "core/or/cell_st.h"
#include "core/or/cell_queue_st.h"
#include "core/or/cpath_build_state_st.h"
#include "feature/dircommon/dir_connection_st.h"
#include "core/or/destroy_cell_queue_st.h"
#include "core/or/entry_connection_st.h"
#include "core/or/extend_info_st.h"
#include "core/or/or_circuit_st.h"
#include "core/or/origin_circuit_st.h"
#include "feature/nodelist/routerinfo_st.h"
#include "core/or/socks_request_st.h"
#include "core/or/sendme.h"
#include "core/or/congestion_control_common.h"
#include "core/or/congestion_control_flow.h"
#include "core/or/conflux.h"
#include "core/or/conflux_util.h"
#include "core/or/conflux_pool.h"
#include "core/or/relay_msg_st.h"

static edge_connection_t *relay_lookup_conn(circuit_t *circ,
                                            const relay_msg_t *msg,
                                            cell_direction_t cell_direction,
                                            crypt_path_t *layer_hint);

static void circuit_resume_edge_reading(circuit_t *circ,
                                        crypt_path_t *layer_hint);
static int circuit_resume_edge_reading_helper(edge_connection_t *conn,
                                              circuit_t *circ,
                                              crypt_path_t *layer_hint);
static int circuit_consider_stop_edge_reading(circuit_t *circ,
                                              crypt_path_t *layer_hint);
static int circuit_queue_streams_are_blocked(circuit_t *circ);
static void adjust_exit_policy_from_exitpolicy_failure(origin_circuit_t *circ,
                                                  entry_connection_t *conn,
                                                  node_t *node,
                                                  const tor_addr_t *addr);
static int connection_edge_process_ordered_relay_cell(const relay_msg_t *msg,
                                           circuit_t *circ,
                                           edge_connection_t *conn,
                                           crypt_path_t *layer_hint);
static void set_block_state_for_streams(circuit_t *circ,
                                        edge_connection_t *stream_list,
                                        int block, streamid_t stream_id);

/** Stats: how many relay cells have originated at this hop, or have
 * been relayed onward (not recognized at this hop)?
 */
uint64_t stats_n_relay_cells_relayed = 0;
/** Stats: how many relay cells have been delivered to streams at this
 * hop?
 */
uint64_t stats_n_relay_cells_delivered = 0;
/** Stats: how many circuits have we closed due to the cell queue limit being
 * reached (see append_cell_to_circuit_queue()) */
uint64_t stats_n_circ_max_cell_reached = 0;
uint64_t stats_n_circ_max_cell_outq_reached = 0;

/**
 * Update channel usage state based on the type of relay cell and
 * circuit properties.
 *
 * This is needed to determine if a client channel is being
 * used for application traffic, and if a relay channel is being
 * used for multihop circuits and application traffic. The decision
 * to pad in channelpadding.c depends upon this info (as well as
 * consensus parameters) to decide what channels to pad.
 */
static void
circuit_update_channel_usage(circuit_t *circ, cell_t *cell)
{
  if (CIRCUIT_IS_ORIGIN(circ)) {
    /*
     * The client state was first set much earlier in
     * circuit_send_next_onion_skin(), so we can start padding as early as
     * possible.
     *
     * However, if padding turns out to be expensive, we may want to not do
     * it until actual application traffic starts flowing (which is controlled
     * via consensus param nf_pad_before_usage).
     *
     * So: If we're an origin circuit and we've created a full length circuit,
     * then any CELL_RELAY cell means application data. Increase the usage
     * state of the channel to indicate this.
     *
     * We want to wait for CELL_RELAY specifically here, so we know that
     * the channel was definitely being used for data and not for extends.
     * By default, we pad as soon as a channel has been used for *any*
     * circuits, so this state is irrelevant to the padding decision in
     * the default case. However, if padding turns out to be expensive,
     * we would like the ability to avoid padding until we're absolutely
     * sure that a channel is used for enough application data to be worth
     * padding.
     *
     * (So it does not matter that CELL_RELAY_EARLY can actually contain
     * application data. This is only a load reducing option and that edge
     * case does not matter if we're desperately trying to reduce overhead
     * anyway. See also consensus parameter nf_pad_before_usage).
     */
    if (BUG(!circ->n_chan))
      return;

    if (circ->n_chan->channel_usage == CHANNEL_USED_FOR_FULL_CIRCS &&
        cell->command == CELL_RELAY) {
      circ->n_chan->channel_usage = CHANNEL_USED_FOR_USER_TRAFFIC;
    }
  } else {
    /* If we're a relay circuit, the question is more complicated. Basically:
     * we only want to pad connections that carry multihop (anonymous)
     * circuits.
     *
     * We assume we're more than one hop if either the previous hop
     * is not a client, or if the previous hop is a client and there's
     * a next hop. Then, circuit traffic starts at RELAY_EARLY, and
     * user application traffic starts when we see RELAY cells.
     */
    or_circuit_t *or_circ = TO_OR_CIRCUIT(circ);

    if (BUG(!or_circ->p_chan))
      return;

    if (!channel_is_client(or_circ->p_chan) ||
        (channel_is_client(or_circ->p_chan) && circ->n_chan)) {
      if (cell->command == CELL_RELAY_EARLY) {
        if (or_circ->p_chan->channel_usage < CHANNEL_USED_FOR_FULL_CIRCS) {
          or_circ->p_chan->channel_usage = CHANNEL_USED_FOR_FULL_CIRCS;
        }
      } else if (cell->command == CELL_RELAY) {
        or_circ->p_chan->channel_usage = CHANNEL_USED_FOR_USER_TRAFFIC;
      }
    }
  }
}

/** Receive a relay cell:
 *  - Crypt it (encrypt if headed toward the origin or if we <b>are</b> the
 *    origin; decrypt if we're headed toward the exit).
 *  - Check if recognized (if exitward).
 *  - If recognized and the digest checks out, then find if there's a stream
 *    that the cell is intended for, and deliver it to the right
 *    connection_edge.
 *  - If not recognized, then we need to relay it: append it to the appropriate
 *    cell_queue on <b>circ</b>.
 *
 * If a reason exists to close <b>circ</b>, circuit_mark_for_close()
 * is called in this function, so the caller doesn't have to do it.
 *
 * Return -<b>reason</b> on failure, else 0.
 */
int
circuit_receive_relay_cell(cell_t *cell, circuit_t *circ,
                           cell_direction_t cell_direction)
{
  channel_t *chan = NULL;
  crypt_path_t *layer_hint=NULL;
  char recognized=0;
  int reason;

  tor_assert(cell);
  tor_assert(circ);
  tor_assert(cell_direction == CELL_DIRECTION_OUT ||
             cell_direction == CELL_DIRECTION_IN);
  if (circ->marked_for_close)
    return 0;

  if (relay_decrypt_cell(circ, cell, cell_direction, &layer_hint, &recognized)
      < 0) {
    log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
           "relay crypt failed. Dropping connection.");
    reason = -END_CIRC_REASON_INTERNAL;
    goto error;
  }

  circuit_update_channel_usage(circ, cell);

  if (recognized) {
    edge_connection_t *conn = NULL;
    relay_cell_fmt_t format = circuit_get_relay_format(circ, layer_hint);

    relay_msg_t msg_buf;
    if (relay_msg_decode_cell_in_place(format, cell, &msg_buf) < 0) {
      log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
             "Received undecodable relay cell");
      return -END_CIRC_REASON_TORPROTOCOL;
    }
    const relay_msg_t *msg = &msg_buf;

    if (circ->purpose == CIRCUIT_PURPOSE_PATH_BIAS_TESTING) {
      if (pathbias_check_probe_response(circ, msg) == -1) {
        pathbias_count_valid_cells(circ, msg);
      }

      /* We need to drop this cell no matter what to avoid code that expects
       * a certain purpose (such as the hidserv code). */
      return 0;
    }

    conn = relay_lookup_conn(circ, msg, cell_direction, layer_hint);
    if (cell_direction == CELL_DIRECTION_OUT) {
      ++stats_n_relay_cells_delivered;
      log_debug(LD_OR,"Sending away from origin.");
      reason = connection_edge_process_relay_cell(msg, circ, conn, NULL);
      if (reason < 0) {
        log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
               "connection_edge_process_relay_cell (away from origin) "
               "failed.");
        goto error;
      }
    } else if (cell_direction == CELL_DIRECTION_IN) {
      ++stats_n_relay_cells_delivered;
      log_debug(LD_OR,"Sending to origin.");
      reason = connection_edge_process_relay_cell(msg, circ, conn,
                                                  layer_hint);
      if (reason < 0) {
        /* If a client is trying to connect to unknown hidden service port,
         * END_CIRC_AT_ORIGIN is sent back so we can then close the circuit.
         * Do not log warn as this is an expected behavior for a service. */
        if (reason != END_CIRC_AT_ORIGIN) {
          log_warn(LD_OR,
                   "connection_edge_process_relay_cell (at origin) failed.");
        }
        goto error;
      }
    }
    return 0;
  }

  /* not recognized. inform circpad and pass it on. */
  circpad_deliver_unrecognized_cell_events(circ, cell_direction);

  if (cell_direction == CELL_DIRECTION_OUT) {
    cell->circ_id = circ->n_circ_id; /* switch it */
    chan = circ->n_chan;
  } else if (! CIRCUIT_IS_ORIGIN(circ)) {
    cell->circ_id = TO_OR_CIRCUIT(circ)->p_circ_id; /* switch it */
    chan = TO_OR_CIRCUIT(circ)->p_chan;
  } else {
    log_fn(LOG_PROTOCOL_WARN, LD_OR,
           "Dropping unrecognized inbound cell on origin circuit.");
    /* If we see unrecognized cells on path bias testing circs,
     * it's bad mojo. Those circuits need to die.
     * XXX: Shouldn't they always die? */
    if (circ->purpose == CIRCUIT_PURPOSE_PATH_BIAS_TESTING) {
      TO_ORIGIN_CIRCUIT(circ)->path_state = PATH_STATE_USE_FAILED;
      reason = -END_CIRC_REASON_TORPROTOCOL;
      goto error;
    } else {
      return 0;
    }
  }

  if (!chan) {
    // XXXX Can this splice stuff be done more cleanly?
    if (! CIRCUIT_IS_ORIGIN(circ) &&
        TO_OR_CIRCUIT(circ)->rend_splice &&
        cell_direction == CELL_DIRECTION_OUT) {
      or_circuit_t *splice_ = TO_OR_CIRCUIT(circ)->rend_splice;
      tor_assert(circ->purpose == CIRCUIT_PURPOSE_REND_ESTABLISHED);
      tor_assert(splice_->base_.purpose == CIRCUIT_PURPOSE_REND_ESTABLISHED);
      cell->circ_id = splice_->p_circ_id;
      cell->command = CELL_RELAY; /* can't be relay_early anyway */
      if ((reason = circuit_receive_relay_cell(cell, TO_CIRCUIT(splice_),
                                               CELL_DIRECTION_IN)) < 0) {
        log_warn(LD_REND, "Error relaying cell across rendezvous; closing "
                 "circuits");
        goto error;
      }
      return 0;
    }
    if (BUG(CIRCUIT_IS_ORIGIN(circ))) {
      /* Should be impossible at this point. */
      reason = -END_CIRC_REASON_TORPROTOCOL;
      goto error;
    }
    or_circuit_t *or_circ = TO_OR_CIRCUIT(circ);
    if (++or_circ->n_cells_discarded_at_end == 1) {
      time_t seconds_open = approx_time() - circ->timestamp_created.tv_sec;
      log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
             "Didn't recognize a cell, but circ stops here! Closing circuit. "
             "It was created %ld seconds ago.", (long)seconds_open);
    }
    reason = -END_CIRC_REASON_TORPROTOCOL;
    goto error;
  }

  log_debug(LD_OR,"Passing on unrecognized cell.");

  ++stats_n_relay_cells_relayed; /* XXXX no longer quite accurate {cells}
                                  * we might kill the circ before we relay
                                  * the cells. */

  if (append_cell_to_circuit_queue(circ, chan, cell, cell_direction, 0) < 0) {
    reason = -END_CIRC_REASON_RESOURCELIMIT;
    goto error;
  }
  return 0;

error:
  circuit_mark_for_close(circ, -reason);
  return reason;
}

/** Package a relay cell from an edge:
 *  - Encrypt it to the right layer
 *  - Append it to the appropriate cell_queue on <b>circ</b>.
 *
 * Return 1 if the cell was successfully sent as in queued on the circuit.
 * Return 0 if the cell needs to be dropped as in ignored.
 * Return -1 on error for which the circuit should be marked for close. */
MOCK_IMPL(int,
circuit_package_relay_cell, (cell_t *cell, circuit_t *circ,
                           cell_direction_t cell_direction,
                           crypt_path_t *layer_hint, streamid_t on_stream,
                           const char *filename, int lineno))
{
  channel_t *chan; /* where to send the cell */

  if (circ->marked_for_close) {
    /* Circuit is marked; send nothing. */
    return 0;
  }

  if (cell_direction == CELL_DIRECTION_OUT) {
    chan = circ->n_chan;
    if (!chan) {
      log_warn(LD_BUG,"outgoing relay cell sent from %s:%d has n_chan==NULL."
               " Dropping. Circuit is in state %s (%d), and is "
               "%smarked for close. (%s:%d, %d)", filename, lineno,
               circuit_state_to_string(circ->state), circ->state,
               circ->marked_for_close ? "" : "not ",
               circ->marked_for_close_file?circ->marked_for_close_file:"",
               circ->marked_for_close, circ->marked_for_close_reason);
      if (CIRCUIT_IS_ORIGIN(circ)) {
        circuit_log_path(LOG_WARN, LD_BUG, TO_ORIGIN_CIRCUIT(circ));
      }
      log_backtrace(LOG_WARN,LD_BUG,"");
      return 0; /* just drop it */
    }
    if (!CIRCUIT_IS_ORIGIN(circ)) {
      log_warn(LD_BUG,"outgoing relay cell sent from %s:%d on non-origin "
               "circ. Dropping.", filename, lineno);
      log_backtrace(LOG_WARN,LD_BUG,"");
      return 0; /* just drop it */
    }

    relay_encrypt_cell_outbound(cell, TO_ORIGIN_CIRCUIT(circ), layer_hint);

    /* Update circ written totals for control port */
    origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
    ocirc->n_written_circ_bw = tor_add_u32_nowrap(ocirc->n_written_circ_bw,
                                                  CELL_PAYLOAD_SIZE);

  } else { /* incoming cell */
    if (CIRCUIT_IS_ORIGIN(circ)) {
      /* We should never package an _incoming_ cell from the circuit
       * origin; that means we messed up somewhere. */
      log_warn(LD_BUG,"incoming relay cell at origin circuit. Dropping.");
      assert_circuit_ok(circ);
      return 0; /* just drop it */
    }
    or_circuit_t *or_circ = TO_OR_CIRCUIT(circ);
    relay_encrypt_cell_inbound(cell, or_circ);
    chan = or_circ->p_chan;
  }
  ++stats_n_relay_cells_relayed;

  return append_cell_to_circuit_queue(circ, chan, cell,
                                      cell_direction, on_stream);
}

/** If cell's stream_id matches the stream_id of any conn that's
 * attached to circ, return that conn, else return NULL.
 */
static edge_connection_t *
relay_lookup_conn(circuit_t *circ, const relay_msg_t *msg,
                  cell_direction_t cell_direction, crypt_path_t *layer_hint)
{
  edge_connection_t *tmpconn;

  if (!msg->stream_id)
    return NULL;

  /* IN or OUT cells could have come from either direction, now
   * that we allow rendezvous *to* an OP.
   */
  if (CIRCUIT_IS_ORIGIN(circ)) {
    for (tmpconn = TO_ORIGIN_CIRCUIT(circ)->p_streams; tmpconn;
         tmpconn=tmpconn->next_stream) {
      if (msg->stream_id == tmpconn->stream_id &&
          !tmpconn->base_.marked_for_close &&
          edge_uses_cpath(tmpconn, layer_hint)) {
        log_debug(LD_APP,"found conn for stream %d.", msg->stream_id);
        return tmpconn;
      }
    }
  } else {
    for (tmpconn = TO_OR_CIRCUIT(circ)->n_streams; tmpconn;
         tmpconn=tmpconn->next_stream) {
      if (msg->stream_id == tmpconn->stream_id &&
          !tmpconn->base_.marked_for_close) {
        log_debug(LD_EXIT,"found conn for stream %d.", msg->stream_id);
        if (cell_direction == CELL_DIRECTION_OUT ||
            connection_edge_is_rendezvous_stream(tmpconn))
          return tmpconn;
      }
    }
    for (tmpconn = TO_OR_CIRCUIT(circ)->resolving_streams; tmpconn;
         tmpconn=tmpconn->next_stream) {
      if (msg->stream_id == tmpconn->stream_id &&
          !tmpconn->base_.marked_for_close) {
        log_debug(LD_EXIT,"found conn for stream %d.", msg->stream_id);
        return tmpconn;
      }
    }
  }
  return NULL; /* probably a begin relay cell */
}

#ifdef TOR_UNIT_TESTS
/** Pack the relay_header_t host-order structure <b>src</b> into
 * network-order in the buffer <b>dest</b>. See tor-spec.txt for details
 * about the wire format.
 */
void
relay_header_pack(uint8_t *dest, const relay_header_t *src)
{
  set_uint8(dest, src->command);
  set_uint16(dest+1, htons(src->recognized));
  set_uint16(dest+3, htons(src->stream_id));
  memcpy(dest+5, src->integrity, 4);
  set_uint16(dest+9, htons(src->length));
}

/** Unpack the network-order buffer <b>src</b> into a host-order
 * relay_header_t structure <b>dest</b>.
 */
void
relay_header_unpack(relay_header_t *dest, const uint8_t *src)
{
  dest->command = get_uint8(src);
  dest->recognized = ntohs(get_uint16(src+1));
  dest->stream_id = ntohs(get_uint16(src+3));
  memcpy(dest->integrity, src+5, 4);
  dest->length = ntohs(get_uint16(src+9));
}
#endif

/** Convert the relay <b>command</b> into a human-readable string. */
const char *
relay_command_to_string(uint8_t command)
{
  static char buf[64];
  switch (command) {
    case RELAY_COMMAND_BEGIN: return "BEGIN";
    case RELAY_COMMAND_DATA: return "DATA";
    case RELAY_COMMAND_END: return "END";
    case RELAY_COMMAND_CONNECTED: return "CONNECTED";
    case RELAY_COMMAND_SENDME: return "SENDME";
    case RELAY_COMMAND_EXTEND: return "EXTEND";
    case RELAY_COMMAND_EXTENDED: return "EXTENDED";
    case RELAY_COMMAND_TRUNCATE: return "TRUNCATE";
    case RELAY_COMMAND_TRUNCATED: return "TRUNCATED";
    case RELAY_COMMAND_DROP: return "DROP";
    case RELAY_COMMAND_RESOLVE: return "RESOLVE";
    case RELAY_COMMAND_RESOLVED: return "RESOLVED";
    case RELAY_COMMAND_BEGIN_DIR: return "BEGIN_DIR";
    case RELAY_COMMAND_ESTABLISH_INTRO: return "ESTABLISH_INTRO";
    case RELAY_COMMAND_ESTABLISH_RENDEZVOUS: return "ESTABLISH_RENDEZVOUS";
    case RELAY_COMMAND_INTRODUCE1: return "INTRODUCE1";
    case RELAY_COMMAND_INTRODUCE2: return "INTRODUCE2";
    case RELAY_COMMAND_RENDEZVOUS1: return "RENDEZVOUS1";
    case RELAY_COMMAND_RENDEZVOUS2: return "RENDEZVOUS2";
    case RELAY_COMMAND_INTRO_ESTABLISHED: return "INTRO_ESTABLISHED";
    case RELAY_COMMAND_RENDEZVOUS_ESTABLISHED:
      return "RENDEZVOUS_ESTABLISHED";
    case RELAY_COMMAND_INTRODUCE_ACK: return "INTRODUCE_ACK";
    case RELAY_COMMAND_EXTEND2: return "EXTEND2";
    case RELAY_COMMAND_EXTENDED2: return "EXTENDED2";
    case RELAY_COMMAND_PADDING_NEGOTIATE: return "PADDING_NEGOTIATE";
    case RELAY_COMMAND_PADDING_NEGOTIATED: return "PADDING_NEGOTIATED";
    case RELAY_COMMAND_CONFLUX_LINK: return "CONFLUX_LINK";
    case RELAY_COMMAND_CONFLUX_LINKED: return "CONFLUX_LINKED";
    case RELAY_COMMAND_CONFLUX_LINKED_ACK: return "CONFLUX_LINKED_ACK";
    case RELAY_COMMAND_CONFLUX_SWITCH: return "CONFLUX_SWITCH";
    default:
      tor_snprintf(buf, sizeof(buf), "Unrecognized relay command %u",
                   (unsigned)command);
      return buf;
  }
}

/** Make a relay cell out of <b>relay_command</b> and <b>payload</b>, and send
 * it onto the open circuit <b>circ</b>. <b>stream_id</b> is the ID on
 * <b>circ</b> for the stream that's sending the relay cell, or 0 if it's a
 * control cell.  <b>cpath_layer</b> is NULL for OR->OP cells, or the
 * destination hop for OP->OR cells.
 *
 * If you can't send the cell, mark the circuit for close and return -1. Else
 * return 0.
 */
MOCK_IMPL(int,
relay_send_command_from_edge_,(streamid_t stream_id, circuit_t *orig_circ,
                               uint8_t relay_command, const char *payload,
                               size_t payload_len, crypt_path_t *cpath_layer,
                               const char *filename, int lineno))
{
  cell_t cell;
  cell_direction_t cell_direction;
  circuit_t *circ = orig_circ;

  /* If conflux is enabled, decide which leg to send on, and use that */
  if (orig_circ->conflux && conflux_should_multiplex(relay_command)) {
    circ = conflux_decide_circ_for_send(orig_circ->conflux, orig_circ,
                                        relay_command);
    if (BUG(!circ)) {
      log_warn(LD_BUG, "No circuit to send for conflux for relay command %d, "
               "called from %s:%d", relay_command, filename, lineno);
      conflux_log_set(LOG_WARN, orig_circ->conflux,
                       CIRCUIT_IS_ORIGIN(orig_circ));
      circ = orig_circ;
    } else {
      /* Conflux circuits always send multiplexed relay commands to
       * to the last hop. (Non-multiplexed commands go on their
       * original circuit and hop). */
      cpath_layer = conflux_get_destination_hop(circ);
    }
  }

  /* XXXX NM Split this function into a separate versions per circuit type? */

  tor_assert(circ);

  size_t msg_body_len;
  {
    relay_cell_fmt_t cell_format = circuit_get_relay_format(circ, cpath_layer);
    relay_msg_t msg;
    if (payload_len >
        relay_cell_max_payload_size(cell_format, relay_command)) {
      // TODO CGO: Rate-limit this?
      log_warn(LD_BUG, "Tried to send a command %d of length %d in "
               "a v%d cell, from %s:%d",
               (int)relay_command, (int)payload_len, (int)cell_format,
               filename, lineno);
      circuit_mark_for_close(circ, END_CIRC_REASON_INTERNAL);
      return -1;
    }

    msg.command = relay_command;
    msg.stream_id = stream_id;
    msg.length = payload_len;
    msg.body = (const uint8_t *) payload;
    msg_body_len = msg.length;

    if (relay_msg_encode_cell(cell_format, &msg, &cell) < 0) {
      // We already called IF_BUG_ONCE in relay_msg_encode_cell.
      circuit_mark_for_close(circ, END_CIRC_REASON_INTERNAL);
      return -1;
    }
  }

  cell.command = CELL_RELAY;
  if (CIRCUIT_IS_ORIGIN(circ)) {
    tor_assert(cpath_layer);
    cell.circ_id = circ->n_circ_id;
    cell_direction = CELL_DIRECTION_OUT;
  } else {
    tor_assert(! cpath_layer);
    cell.circ_id = TO_OR_CIRCUIT(circ)->p_circ_id;
    cell_direction = CELL_DIRECTION_IN;
  }

  log_debug(LD_OR,"delivering %d cell %s.", relay_command,
            cell_direction == CELL_DIRECTION_OUT ? "forward" : "backward");

  /* Tell circpad we're sending a relay cell */
  circpad_deliver_sent_relay_cell_events(circ, relay_command);

  /* If we are sending an END cell and this circuit is used for a tunneled
   * directory request, advance its state. */
  if (relay_command == RELAY_COMMAND_END && circ->dirreq_id)
    geoip_change_dirreq_state(circ->dirreq_id, DIRREQ_TUNNELED,
                              DIRREQ_END_CELL_SENT);

  if (cell_direction == CELL_DIRECTION_OUT && circ->n_chan) {
    /* if we're using relaybandwidthrate, this conn wants priority */
    channel_timestamp_client(circ->n_chan);
  }

  if (cell_direction == CELL_DIRECTION_OUT) {
    origin_circuit_t *origin_circ = TO_ORIGIN_CIRCUIT(circ);
    if (origin_circ->remaining_relay_early_cells > 0 &&
        (relay_command == RELAY_COMMAND_EXTEND ||
         relay_command == RELAY_COMMAND_EXTEND2 ||
         cpath_layer != origin_circ->cpath)) {
      /* If we've got any relay_early cells left and (we're sending
       * an extend cell or we're not talking to the first hop), use
       * one of them.  Don't worry about the conn protocol version:
       * append_cell_to_circuit_queue will fix it up. */
      cell.command = CELL_RELAY_EARLY;
      /* If we're out of relay early cells, tell circpad */
      if (--origin_circ->remaining_relay_early_cells == 0)
        circpad_machine_event_circ_has_no_relay_early(origin_circ);
      log_debug(LD_OR, "Sending a RELAY_EARLY cell; %d remaining.",
                (int)origin_circ->remaining_relay_early_cells);
      /* Memorize the command that is sent as RELAY_EARLY cell; helps debug
       * task 878. */
      origin_circ->relay_early_commands[
          origin_circ->relay_early_cells_sent++] = relay_command;
    } else if (relay_command == RELAY_COMMAND_EXTEND ||
               relay_command == RELAY_COMMAND_EXTEND2) {
      /* If no RELAY_EARLY cells can be sent over this circuit, log which
       * commands have been sent as RELAY_EARLY cells before; helps debug
       * task 878. */
      smartlist_t *commands_list = smartlist_new();
      int i = 0;
      char *commands = NULL;
      for (; i < origin_circ->relay_early_cells_sent; i++)
        smartlist_add(commands_list, (char *)
            relay_command_to_string(origin_circ->relay_early_commands[i]));
      commands = smartlist_join_strings(commands_list, ",", 0, NULL);
      log_warn(LD_BUG, "Uh-oh.  We're sending a RELAY_COMMAND_EXTEND cell, "
               "but we have run out of RELAY_EARLY cells on that circuit. "
               "Commands sent before: %s", commands);
      tor_free(commands);
      smartlist_free(commands_list);
    }

    /* Let's assume we're well-behaved: Anything that we decide to send is
     * valid, delivered data. */
    circuit_sent_valid_data(origin_circ, msg_body_len);
  }

  int ret = circuit_package_relay_cell(&cell, circ, cell_direction,
                                       cpath_layer, stream_id, filename,
                                       lineno);
  if (ret < 0) {
    circuit_mark_for_close(circ, END_CIRC_REASON_INTERNAL);
    return -1;
  } else if (ret == 0) {
    /* This means we should drop the cell or that the circuit was already
     * marked for close. At this point in time, we do NOT close the circuit if
     * the cell is dropped. It is not the case with arti where each circuit
     * protocol violation will lead to closing the circuit. */
    return 0;
  }

  /* At this point, we are certain that the cell was queued on the circuit and
   * thus will be sent on the wire. */

  if (circ->conflux) {
    conflux_note_cell_sent(circ->conflux, circ, relay_command);
  }

  /* If applicable, note the cell digest for the SENDME version 1 purpose if
   * we need to. This call needs to be after the circuit_package_relay_cell()
   * because the cell digest is set within that function. */
  if (relay_command == RELAY_COMMAND_DATA) {
    sendme_record_cell_digest_on_circ(circ, cpath_layer);

    /* Handle the circuit-level SENDME package window. */
    if (sendme_note_circuit_data_packaged(circ, cpath_layer) < 0) {
      /* Package window has gone under 0. Protocol issue. */
      log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
             "Circuit package window is below 0. Closing circuit.");
      circuit_mark_for_close(circ, END_CIRC_REASON_TORPROTOCOL);
      return -1;
    }
  }

  return 0;
}

/** Make a relay cell out of <b>relay_command</b> and <b>payload</b>, and
 * send it onto the open circuit <b>circ</b>. <b>fromconn</b> is the stream
 * that's sending the relay cell, or NULL if it's a control cell.
 * <b>cpath_layer</b> is NULL for OR->OP cells, or the destination hop
 * for OP->OR cells.
 *
 * If you can't send the cell, mark the circuit for close and
 * return -1. Else return 0.
 */
int
connection_edge_send_command(edge_connection_t *fromconn,
                             uint8_t relay_command, const char *payload,
                             size_t payload_len)
{
  /* XXXX NM Split this function into a separate versions per circuit type? */
  circuit_t *circ;
  crypt_path_t *cpath_layer = fromconn->cpath_layer;
  tor_assert(fromconn);

  circ = fromconn->on_circuit;

  if (fromconn->base_.marked_for_close) {
    log_warn(LD_BUG,
             "called on conn that's already marked for close at %s:%d.",
             fromconn->base_.marked_for_close_file,
             fromconn->base_.marked_for_close);
    return 0;
  }

  if (!circ) {
    if (fromconn->base_.type == CONN_TYPE_AP) {
      log_info(LD_APP,"no circ. Closing conn.");
      connection_mark_unattached_ap(EDGE_TO_ENTRY_CONN(fromconn),
                                    END_STREAM_REASON_INTERNAL);
    } else {
      log_info(LD_EXIT,"no circ. Closing conn.");
      fromconn->edge_has_sent_end = 1; /* no circ to send to */
      fromconn->end_reason = END_STREAM_REASON_INTERNAL;
      connection_mark_for_close(TO_CONN(fromconn));
    }
    return -1;
  }

  if (circ->marked_for_close) {
    /* The circuit has been marked, but not freed yet. When it's freed, it
     * will mark this connection for close. */
    return -1;
  }

#ifdef MEASUREMENTS_21206
  /* Keep track of the number of RELAY_DATA cells sent for directory
   * connections. */
  connection_t *linked_conn = TO_CONN(fromconn)->linked_conn;

  if (linked_conn && linked_conn->type == CONN_TYPE_DIR) {
    ++(TO_DIR_CONN(linked_conn)->data_cells_sent);
  }
#endif /* defined(MEASUREMENTS_21206) */

  return relay_send_command_from_edge(fromconn->stream_id, circ,
                                      relay_command, payload,
                                      payload_len, cpath_layer);
}

/** How many times will I retry a stream that fails due to DNS
 * resolve failure or misc error?
 */
#define MAX_RESOLVE_FAILURES 3

/** Return 1 if reason is something that you should retry if you
 * get the end cell before you've connected; else return 0. */
static int
edge_reason_is_retriable(int reason)
{
  return reason == END_STREAM_REASON_HIBERNATING ||
         reason == END_STREAM_REASON_RESOURCELIMIT ||
         reason == END_STREAM_REASON_EXITPOLICY ||
         reason == END_STREAM_REASON_RESOLVEFAILED ||
         reason == END_STREAM_REASON_MISC ||
         reason == END_STREAM_REASON_NOROUTE;
}

/** Called when we receive an END cell on a stream that isn't open yet,
 * from the client side.
 * Arguments are as for connection_edge_process_relay_cell().
 */
static int
connection_ap_process_end_not_open(
    const relay_msg_t *msg, origin_circuit_t *circ,
    entry_connection_t *conn, crypt_path_t *layer_hint)
{
  node_t *exitrouter;
  int reason = get_uint8(msg->body);
  int control_reason;
  edge_connection_t *edge_conn = ENTRY_TO_EDGE_CONN(conn);
  (void) layer_hint; /* unused */

  if (msg->length > 0) {
    if (reason == END_STREAM_REASON_TORPROTOCOL ||
        reason == END_STREAM_REASON_DESTROY) {
      /* Both of these reasons could mean a failed tag
       * hit the exit and it complained. Do not probe.
       * Fail the circuit. */
      circ->path_state = PATH_STATE_USE_FAILED;
      return -END_CIRC_REASON_TORPROTOCOL;
    } else if (reason == END_STREAM_REASON_INTERNAL) {
      /* We can't infer success or failure, since older Tors report
       * ENETUNREACH as END_STREAM_REASON_INTERNAL. */
    } else {
      /* Path bias: If we get a valid reason code from the exit,
       * it wasn't due to tagging.
       *
       * We rely on recognized+digest being strong enough to make
       * tags unlikely to allow us to get tagged, yet 'recognized'
       * reason codes here. */
      pathbias_mark_use_success(circ);
    }
  }

  /* This end cell is now valid. */
  circuit_read_valid_data(circ, msg->length);

  if (msg->length == 0) {
    reason = END_STREAM_REASON_MISC;
  }

  control_reason = reason | END_STREAM_REASON_FLAG_REMOTE;

  if (edge_reason_is_retriable(reason) &&
      /* avoid retry if rend */
      !connection_edge_is_rendezvous_stream(edge_conn)) {
    const char *chosen_exit_digest =
      circ->build_state->chosen_exit->identity_digest;
    log_info(LD_APP,"Address '%s' refused due to '%s'. Considering retrying.",
             safe_str(conn->socks_request->address),
             stream_end_reason_to_string(reason));
    exitrouter = node_get_mutable_by_id(chosen_exit_digest);
    switch (reason) {
      case END_STREAM_REASON_EXITPOLICY: {
        tor_addr_t addr;
        tor_addr_make_unspec(&addr);
        if (msg->length >= 5) {
          int ttl = -1;
          tor_addr_make_unspec(&addr);
          if (msg->length == 5 || msg->length == 9) {
            tor_addr_from_ipv4n(&addr, get_uint32(msg->body + 1));
            if (msg->length == 9)
              ttl = (int)ntohl(get_uint32(msg->body + 5));
          } else if (msg->length == 17 || msg->length == 21) {
            tor_addr_from_ipv6_bytes(&addr, msg->body + 1);
            if (msg->length == 21)
              ttl = (int)ntohl(get_uint32(msg->body + 17));
          }
          if (tor_addr_is_null(&addr)) {
            log_info(LD_APP,"Address '%s' resolved to 0.0.0.0. Closing,",
                     safe_str(conn->socks_request->address));
            connection_mark_unattached_ap(conn, END_STREAM_REASON_TORPROTOCOL);
            return 0;
          }

          if ((tor_addr_family(&addr) == AF_INET &&
                                          !conn->entry_cfg.ipv4_traffic) ||
              (tor_addr_family(&addr) == AF_INET6 &&
                                          !conn->entry_cfg.ipv6_traffic)) {
            log_fn(LOG_PROTOCOL_WARN, LD_APP,
                   "Got an EXITPOLICY failure on a connection with a "
                   "mismatched family. Closing.");
            connection_mark_unattached_ap(conn, END_STREAM_REASON_TORPROTOCOL);
            return 0;
          }
          if (get_options()->ClientDNSRejectInternalAddresses &&
              tor_addr_is_internal(&addr, 0)) {
            log_info(LD_APP,"Address '%s' resolved to internal. Closing,",
                     safe_str(conn->socks_request->address));
            connection_mark_unattached_ap(conn, END_STREAM_REASON_TORPROTOCOL);
            return 0;
          }

          client_dns_set_addressmap(conn,
                                    conn->socks_request->address, &addr,
                                    conn->chosen_exit_name, ttl);

          {
            char new_addr[TOR_ADDR_BUF_LEN];
            tor_addr_to_str(new_addr, &addr, sizeof(new_addr), 1);
            if (strcmp(conn->socks_request->address, new_addr)) {
              strlcpy(conn->socks_request->address, new_addr,
                      sizeof(conn->socks_request->address));
              control_event_stream_status(conn, STREAM_EVENT_REMAP, 0);
            }
          }
        }
        /* check if the exit *ought* to have allowed it */

        adjust_exit_policy_from_exitpolicy_failure(circ,
                                                   conn,
                                                   exitrouter,
                                                   &addr);

        if (conn->chosen_exit_optional ||
            conn->chosen_exit_retries) {
          /* stop wanting a specific exit */
          conn->chosen_exit_optional = 0;
          /* A non-zero chosen_exit_retries can happen if we set a
           * TrackHostExits for this address under a port that the exit
           * relay allows, but then try the same address with a different
           * port that it doesn't allow to exit. We shouldn't unregister
           * the mapping, since it is probably still wanted on the
           * original port. But now we give away to the exit relay that
           * we probably have a TrackHostExits on it. So be it. */
          conn->chosen_exit_retries = 0;
          tor_free(conn->chosen_exit_name); /* clears it */
        }
        if (connection_ap_detach_retriable(conn, circ, control_reason) >= 0)
          return 0;
        /* else, conn will get closed below */
        break;
      }
      case END_STREAM_REASON_CONNECTREFUSED:
        if (!conn->chosen_exit_optional)
          break; /* break means it'll close, below */
        /* Else fall through: expire this circuit, clear the
         * chosen_exit_name field, and try again. */
        FALLTHROUGH;
      case END_STREAM_REASON_RESOLVEFAILED:
      case END_STREAM_REASON_TIMEOUT:
      case END_STREAM_REASON_MISC:
      case END_STREAM_REASON_NOROUTE:
        if (client_dns_incr_failures(conn->socks_request->address)
            < MAX_RESOLVE_FAILURES) {
          /* We haven't retried too many times; reattach the connection. */
          circuit_log_path(LOG_INFO,LD_APP,circ);
          /* Mark this circuit "unusable for new streams". */
          mark_circuit_unusable_for_new_conns(circ);

          if (conn->chosen_exit_optional) {
            /* stop wanting a specific exit */
            conn->chosen_exit_optional = 0;
            tor_free(conn->chosen_exit_name); /* clears it */
          }
          if (connection_ap_detach_retriable(conn, circ, control_reason) >= 0)
            return 0;
          /* else, conn will get closed below */
        } else {
          log_notice(LD_APP,
                     "Have tried resolving or connecting to address '%s' "
                     "at %d different places. Giving up.",
                     safe_str(conn->socks_request->address),
                     MAX_RESOLVE_FAILURES);
          /* clear the failures, so it will have a full try next time */
          client_dns_clear_failures(conn->socks_request->address);
        }
        break;
      case END_STREAM_REASON_HIBERNATING:
      case END_STREAM_REASON_RESOURCELIMIT:
        if (exitrouter) {
          policies_set_node_exitpolicy_to_reject_all(exitrouter);
        }
        if (conn->chosen_exit_optional) {
          /* stop wanting a specific exit */
          conn->chosen_exit_optional = 0;
          tor_free(conn->chosen_exit_name); /* clears it */
        }
        if (connection_ap_detach_retriable(conn, circ, control_reason) >= 0)
          return 0;
        /* else, will close below */
        break;
    } /* end switch */
    log_info(LD_APP,"Giving up on retrying; conn can't be handled.");
  }

  log_info(LD_APP,
           "Edge got end (%s) before we're connected. Marking for close.",
       stream_end_reason_to_string(msg->length > 0 ? reason : -1));
  circuit_log_path(LOG_INFO,LD_APP,circ);
  /* need to test because of detach_retriable */
  if (!ENTRY_TO_CONN(conn)->marked_for_close)
    connection_mark_unattached_ap(conn, control_reason);
  return 0;
}

/** Called when we have gotten an END_REASON_EXITPOLICY failure on <b>circ</b>
 * for <b>conn</b>, while attempting to connect via <b>node</b>.  If the node
 * told us which address it rejected, then <b>addr</b> is that address;
 * otherwise it is AF_UNSPEC.
 *
 * If we are sure the node should have allowed this address, mark the node as
 * having a reject *:* exit policy.  Otherwise, mark the circuit as unusable
 * for this particular address.
 **/
static void
adjust_exit_policy_from_exitpolicy_failure(origin_circuit_t *circ,
                                           entry_connection_t *conn,
                                           node_t *node,
                                           const tor_addr_t *addr)
{
  int make_reject_all = 0;
  const sa_family_t family = tor_addr_family(addr);

  if (node) {
    tor_addr_t tmp;
    int asked_for_family = tor_addr_parse(&tmp, conn->socks_request->address);
    if (family == AF_UNSPEC) {
      make_reject_all = 1;
    } else if (node_exit_policy_is_exact(node, family) &&
               asked_for_family != -1 && !conn->chosen_exit_name) {
      make_reject_all = 1;
    }

    if (make_reject_all) {
      log_info(LD_APP,
               "Exitrouter %s seems to be more restrictive than its exit "
               "policy. Not using this router as exit for now.",
               node_describe(node));
      policies_set_node_exitpolicy_to_reject_all(node);
    }
  }

  if (family != AF_UNSPEC)
    addr_policy_append_reject_addr(&circ->prepend_policy, addr);
}

/** Helper: change the socks_request-&gt;address field on conn to the
 * dotted-quad representation of <b>new_addr</b>,
 * and send an appropriate REMAP event. */
static void
remap_event_helper(entry_connection_t *conn, const tor_addr_t *new_addr)
{
  tor_addr_to_str(conn->socks_request->address, new_addr,
                  sizeof(conn->socks_request->address),
                  1);
  control_event_stream_status(conn, STREAM_EVENT_REMAP,
                              REMAP_STREAM_SOURCE_EXIT);
}

/** Extract the contents of a connected cell in <b>cell</b>, whose relay
 * header has already been parsed into <b>rh</b>. On success, set
 * <b>addr_out</b> to the address we're connected to, and <b>ttl_out</b> to
 * the ttl of that address, in seconds, and return 0.  On failure, return
 * -1.
 *
 * Note that the resulting address can be UNSPEC if the connected cell had no
 * address (as for a stream to an union service or a tunneled directory
 * connection), and that the ttl can be absent (in which case <b>ttl_out</b>
 * is set to -1). */
STATIC int
connected_cell_parse(const relay_msg_t *msg, tor_addr_t *addr_out,
                     int *ttl_out)
{
  uint32_t bytes;
  const uint8_t *payload = msg->body;

  tor_addr_make_unspec(addr_out);
  *ttl_out = -1;
  if (msg->length == 0)
    return 0;
  if (msg->length < 4)
    return -1;
  bytes = ntohl(get_uint32(payload));

  /* If bytes is 0, this is maybe a v6 address. Otherwise it's a v4 address */
  if (bytes != 0) {
    /* v4 address */
    tor_addr_from_ipv4h(addr_out, bytes);
    if (msg->length >= 8) {
      bytes = ntohl(get_uint32(payload + 4));
      if (bytes <= INT32_MAX)
        *ttl_out = bytes;
    }
  } else {
    if (msg->length < 25) /* 4 bytes of 0s, 1 addr, 16 ipv4, 4 ttl. */
      return -1;
    if (get_uint8(payload + 4) != 6)
      return -1;
    tor_addr_from_ipv6_bytes(addr_out, (payload + 5));
    bytes = ntohl(get_uint32(payload + 21));
    if (bytes <= INT32_MAX)
      *ttl_out = (int) bytes;
  }
  return 0;
}

/** Drop all storage held by <b>addr</b>. */
STATIC void
address_ttl_free_(address_ttl_t *addr)
{
  if (!addr)
    return;
  tor_free(addr->hostname);
  tor_free(addr);
}

/** Parse a resolved cell in <b>cell</b>, with parsed header in <b>rh</b>.
 * Return -1 on parse error.  On success, add one or more newly allocated
 * address_ttl_t to <b>addresses_out</b>; set *<b>errcode_out</b> to
 * one of 0, RESOLVED_TYPE_ERROR, or RESOLVED_TYPE_ERROR_TRANSIENT, and
 * return 0. */
STATIC int
resolved_cell_parse(const relay_msg_t *msg, smartlist_t *addresses_out,
                    int *errcode_out)
{
  const uint8_t *cp;
  uint8_t answer_type;
  size_t answer_len;
  address_ttl_t *addr;
  size_t remaining;
  int errcode = 0;
  smartlist_t *addrs;

  tor_assert(msg);
  tor_assert(addresses_out);
  tor_assert(errcode_out);

  *errcode_out = 0;

  if (msg->length > RELAY_PAYLOAD_SIZE_MAX)
    return -1;

  addrs = smartlist_new();

  cp = msg->body;

  remaining = msg->length;
  while (remaining) {
    const uint8_t *cp_orig = cp;
    if (remaining < 2)
      goto err;
    answer_type = *cp++;
    answer_len = *cp++;
    if (remaining < 2 + answer_len + 4) {
      goto err;
    }
    if (answer_type == RESOLVED_TYPE_IPV4) {
      if (answer_len != 4) {
        goto err;
      }
      addr = tor_malloc_zero(sizeof(*addr));
      tor_addr_from_ipv4n(&addr->addr, get_uint32(cp));
      cp += 4;
      addr->ttl = ntohl(get_uint32(cp));
      cp += 4;
      smartlist_add(addrs, addr);
    } else if (answer_type == RESOLVED_TYPE_IPV6) {
      if (answer_len != 16)
        goto err;
      addr = tor_malloc_zero(sizeof(*addr));
      tor_addr_from_ipv6_bytes(&addr->addr, cp);
      cp += 16;
      addr->ttl = ntohl(get_uint32(cp));
      cp += 4;
      smartlist_add(addrs, addr);
    } else if (answer_type == RESOLVED_TYPE_HOSTNAME) {
      if (answer_len == 0) {
        goto err;
      }
      addr = tor_malloc_zero(sizeof(*addr));
      addr->hostname = tor_memdup_nulterm(cp, answer_len);
      cp += answer_len;
      addr->ttl = ntohl(get_uint32(cp));
      cp += 4;
      smartlist_add(addrs, addr);
    } else if (answer_type == RESOLVED_TYPE_ERROR_TRANSIENT ||
               answer_type == RESOLVED_TYPE_ERROR) {
      errcode = answer_type;
      /* Ignore the error contents */
      cp += answer_len + 4;
    } else {
      cp += answer_len + 4;
    }
    tor_assert(((ssize_t)remaining) >= (cp - cp_orig));
    remaining -= (cp - cp_orig);
  }

  if (errcode && smartlist_len(addrs) == 0) {
    /* Report an error only if there were no results. */
    *errcode_out = errcode;
  }

  smartlist_add_all(addresses_out, addrs);
  smartlist_free(addrs);

  return 0;

 err:
  /* On parse error, don't report any results */
  SMARTLIST_FOREACH(addrs, address_ttl_t *, a, address_ttl_free(a));
  smartlist_free(addrs);
  return -1;
}

/** Helper for connection_edge_process_resolved_cell: given an error code,
 * an entry_connection, and a list of address_ttl_t *, report the best answer
 * to the entry_connection. */
static void
connection_ap_handshake_socks_got_resolved_cell(entry_connection_t *conn,
                                                int error_code,
                                                smartlist_t *results)
{
  address_ttl_t *addr_ipv4 = NULL;
  address_ttl_t *addr_ipv6 = NULL;
  address_ttl_t *addr_hostname = NULL;
  address_ttl_t *addr_best = NULL;

  /* If it's an error code, that's easy. */
  if (error_code) {
    tor_assert(error_code == RESOLVED_TYPE_ERROR ||
               error_code == RESOLVED_TYPE_ERROR_TRANSIENT);
    connection_ap_handshake_socks_resolved(conn,
                                           error_code,0,NULL,-1,-1);
    return;
  }

  /* Get the first answer of each type. */
  SMARTLIST_FOREACH_BEGIN(results, address_ttl_t *, addr) {
    if (addr->hostname) {
      if (!addr_hostname) {
        addr_hostname = addr;
      }
    } else if (tor_addr_family(&addr->addr) == AF_INET) {
      if (!addr_ipv4 && conn->entry_cfg.ipv4_traffic) {
        addr_ipv4 = addr;
      }
    } else if (tor_addr_family(&addr->addr) == AF_INET6) {
      if (!addr_ipv6 && conn->entry_cfg.ipv6_traffic) {
        addr_ipv6 = addr;
      }
    }
  } SMARTLIST_FOREACH_END(addr);

  /* Now figure out which type we wanted to deliver. */
  if (conn->socks_request->command == SOCKS_COMMAND_RESOLVE_PTR) {
    if (addr_hostname) {
      connection_ap_handshake_socks_resolved(conn,
                                             RESOLVED_TYPE_HOSTNAME,
                                             strlen(addr_hostname->hostname),
                                             (uint8_t*)addr_hostname->hostname,
                                             addr_hostname->ttl,-1);
    } else {
      connection_ap_handshake_socks_resolved(conn,
                                             RESOLVED_TYPE_ERROR,0,NULL,-1,-1);
    }
    return;
  }

  if (conn->entry_cfg.prefer_ipv6) {
    addr_best = addr_ipv6 ? addr_ipv6 : addr_ipv4;
  } else {
    addr_best = addr_ipv4 ? addr_ipv4 : addr_ipv6;
  }

  /* Now convert it to the ugly old interface */
  if (! addr_best) {
    connection_ap_handshake_socks_resolved(conn,
                                     RESOLVED_TYPE_NOERROR,0,NULL,-1,-1);
    return;
  }

  connection_ap_handshake_socks_resolved_addr(conn,
                                              &addr_best->addr,
                                              addr_best->ttl,
                                              -1);

  remap_event_helper(conn, &addr_best->addr);
}

/** Handle a RELAY_COMMAND_RESOLVED cell that we received on a non-open AP
 * stream. */
STATIC int
connection_edge_process_resolved_cell(edge_connection_t *conn,
                                      const relay_msg_t *msg)
{
  entry_connection_t *entry_conn = EDGE_TO_ENTRY_CONN(conn);
  smartlist_t *resolved_addresses = NULL;
  int errcode = 0;

  if (conn->base_.state != AP_CONN_STATE_RESOLVE_WAIT) {
    log_fn(LOG_PROTOCOL_WARN, LD_APP, "Got a 'resolved' cell while "
           "not in state resolve_wait. Dropping.");
    return 0;
  }
  tor_assert(SOCKS_COMMAND_IS_RESOLVE(entry_conn->socks_request->command));

  resolved_addresses = smartlist_new();
  if (resolved_cell_parse(msg, resolved_addresses, &errcode)) {
    log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
           "Dropping malformed 'resolved' cell");
    connection_mark_unattached_ap(entry_conn, END_STREAM_REASON_TORPROTOCOL);
    goto done;
  }

  if (get_options()->ClientDNSRejectInternalAddresses) {
    int orig_len = smartlist_len(resolved_addresses);
    SMARTLIST_FOREACH_BEGIN(resolved_addresses, address_ttl_t *, addr) {
      if (addr->hostname == NULL && tor_addr_is_internal(&addr->addr, 0)) {
        log_info(LD_APP, "Got a resolved cell with answer %s; dropping that "
                 "answer.",
                 safe_str_client(fmt_addr(&addr->addr)));
        address_ttl_free(addr);
        SMARTLIST_DEL_CURRENT(resolved_addresses, addr);
      }
    } SMARTLIST_FOREACH_END(addr);
    if (orig_len && smartlist_len(resolved_addresses) == 0) {
        log_info(LD_APP, "Got a resolved cell with only private addresses; "
                 "dropping it.");
      connection_ap_handshake_socks_resolved(entry_conn,
                                             RESOLVED_TYPE_ERROR_TRANSIENT,
                                             0, NULL, 0, TIME_MAX);
      connection_mark_unattached_ap(entry_conn,
                                    END_STREAM_REASON_TORPROTOCOL);
      goto done;
    }
  }

  /* This is valid data at this point. Count it */
  if (conn->on_circuit && CIRCUIT_IS_ORIGIN(conn->on_circuit)) {
    circuit_read_valid_data(TO_ORIGIN_CIRCUIT(conn->on_circuit),
                            msg->length);
  }

  connection_ap_handshake_socks_got_resolved_cell(entry_conn,
                                                  errcode,
                                                  resolved_addresses);

  connection_mark_unattached_ap(entry_conn,
                              END_STREAM_REASON_DONE |
                              END_STREAM_REASON_FLAG_ALREADY_SOCKS_REPLIED);

 done:
  SMARTLIST_FOREACH(resolved_addresses, address_ttl_t *, addr,
                    address_ttl_free(addr));
  smartlist_free(resolved_addresses);
  return 0;
}

/** An incoming relay cell has arrived from circuit <b>circ</b> to
 * stream <b>conn</b>.
 *
 * The arguments here are the same as in
 * connection_edge_process_relay_cell() below; this function is called
 * from there when <b>conn</b> is defined and not in an open state.
 */
static int
connection_edge_process_relay_cell_not_open(
    const relay_msg_t *msg, circuit_t *circ,
    edge_connection_t *conn, crypt_path_t *layer_hint)
{
  if (msg->command == RELAY_COMMAND_END) {
    if (CIRCUIT_IS_ORIGIN(circ) && conn->base_.type == CONN_TYPE_AP) {
      return connection_ap_process_end_not_open(msg,
                                                TO_ORIGIN_CIRCUIT(circ),
                                                EDGE_TO_ENTRY_CONN(conn),
                                                layer_hint);
    } else {
      /* we just got an 'end', don't need to send one */
      conn->edge_has_sent_end = 1;
      conn->end_reason = get_uint8(msg->body) | END_STREAM_REASON_FLAG_REMOTE;
      connection_mark_for_close(TO_CONN(conn));
      return 0;
    }
  }

  if (conn->base_.type == CONN_TYPE_AP &&
      msg->command == RELAY_COMMAND_CONNECTED) {
    tor_addr_t addr;
    int ttl;
    entry_connection_t *entry_conn = EDGE_TO_ENTRY_CONN(conn);
    tor_assert(CIRCUIT_IS_ORIGIN(circ));
    if (conn->base_.state != AP_CONN_STATE_CONNECT_WAIT) {
      log_fn(LOG_PROTOCOL_WARN, LD_APP,
             "Got 'connected' while not in state connect_wait. Dropping.");
      return 0;
    }
    CONNECTION_AP_EXPECT_NONPENDING(entry_conn);
    conn->base_.state = AP_CONN_STATE_OPEN;
    log_info(LD_APP,"'connected' received for circid %u streamid %d "
             "after %d seconds.",
             (unsigned)circ->n_circ_id,
             msg->stream_id,
             (int)(time(NULL) - conn->base_.timestamp_last_read_allowed));
    if (connected_cell_parse(msg, &addr, &ttl) < 0) {
      log_fn(LOG_PROTOCOL_WARN, LD_APP,
             "Got a badly formatted connected cell. Closing.");
      connection_edge_end(conn, END_STREAM_REASON_TORPROTOCOL);
      connection_mark_unattached_ap(entry_conn, END_STREAM_REASON_TORPROTOCOL);
      return 0;
    }
    if (tor_addr_family(&addr) != AF_UNSPEC) {
      /* The family is not UNSPEC: so we were given an address in the
       * connected cell. (This is normal, except for BEGINDIR and onion
       * service streams.) */
      const sa_family_t family = tor_addr_family(&addr);
      if (tor_addr_is_null(&addr) ||
          (get_options()->ClientDNSRejectInternalAddresses &&
           tor_addr_is_internal(&addr, 0))) {
        log_info(LD_APP, "...but it claims the IP address was %s. Closing.",
                 safe_str(fmt_addr(&addr)));
        connection_edge_end(conn, END_STREAM_REASON_TORPROTOCOL);
        connection_mark_unattached_ap(entry_conn,
                                      END_STREAM_REASON_TORPROTOCOL);
        return 0;
      }

      if ((family == AF_INET && ! entry_conn->entry_cfg.ipv4_traffic) ||
          (family == AF_INET6 && ! entry_conn->entry_cfg.ipv6_traffic)) {
        log_fn(LOG_PROTOCOL_WARN, LD_APP,
               "Got a connected cell to %s with unsupported address family."
               " Closing.", safe_str(fmt_addr(&addr)));
        connection_edge_end(conn, END_STREAM_REASON_TORPROTOCOL);
        connection_mark_unattached_ap(entry_conn,
                                      END_STREAM_REASON_TORPROTOCOL);
        return 0;
      }

      client_dns_set_addressmap(entry_conn,
                                entry_conn->socks_request->address, &addr,
                                entry_conn->chosen_exit_name, ttl);

      remap_event_helper(entry_conn, &addr);
    }
    circuit_log_path(LOG_INFO,LD_APP,TO_ORIGIN_CIRCUIT(circ));
    /* don't send a socks reply to transparent conns */
    tor_assert(entry_conn->socks_request != NULL);
    if (!entry_conn->socks_request->has_finished) {
      connection_ap_handshake_socks_reply(entry_conn, NULL, 0, 0);
    }

    /* Was it a linked dir conn? If so, a dir request just started to
     * fetch something; this could be a bootstrap status milestone. */
    log_debug(LD_APP, "considering");
    if (TO_CONN(conn)->linked_conn &&
        TO_CONN(conn)->linked_conn->type == CONN_TYPE_DIR) {
      connection_t *dirconn = TO_CONN(conn)->linked_conn;
      log_debug(LD_APP, "it is! %d", dirconn->purpose);
      switch (dirconn->purpose) {
        case DIR_PURPOSE_FETCH_CERTIFICATE:
          if (consensus_is_waiting_for_certs())
            control_event_bootstrap(BOOTSTRAP_STATUS_LOADING_KEYS, 0);
          break;
        case DIR_PURPOSE_FETCH_CONSENSUS:
          control_event_bootstrap(BOOTSTRAP_STATUS_LOADING_STATUS, 0);
          break;
        case DIR_PURPOSE_FETCH_SERVERDESC:
        case DIR_PURPOSE_FETCH_MICRODESC:
          if (TO_DIR_CONN(dirconn)->router_purpose == ROUTER_PURPOSE_GENERAL)
            control_event_boot_dir(BOOTSTRAP_STATUS_LOADING_DESCRIPTORS,
                                   count_loading_descriptors_progress());
          break;
      }
    }
    /* This is definitely a success, so forget about any pending data we
     * had sent. */
    if (entry_conn->pending_optimistic_data) {
      buf_free(entry_conn->pending_optimistic_data);
      entry_conn->pending_optimistic_data = NULL;
    }

    /* This is valid data at this point. Count it */
    circuit_read_valid_data(TO_ORIGIN_CIRCUIT(circ), msg->length);

    /* handle anything that might have queued */
    if (connection_edge_package_raw_inbuf(conn, 1, NULL) < 0) {
      /* (We already sent an end cell if possible) */
      connection_mark_for_close(TO_CONN(conn));
      return 0;
    }
    return 0;
  }
  if (conn->base_.type == CONN_TYPE_AP &&
      msg->command == RELAY_COMMAND_RESOLVED) {
    return connection_edge_process_resolved_cell(conn, msg);
  }

  log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
         "Got an unexpected relay command %d, in state %d (%s). Dropping.",
         msg->command, conn->base_.state,
         conn_state_to_string(conn->base_.type, conn->base_.state));
  return 0; /* for forward compatibility, don't kill the circuit */
//  connection_edge_end(conn, END_STREAM_REASON_TORPROTOCOL);
//  connection_mark_for_close(conn);
//  return -1;
}

/** Process a SENDME cell that arrived on <b>circ</b>. If it is a stream level
 * cell, it is destined for the given <b>conn</b>. If it is a circuit level
 * cell, it is destined for the <b>layer_hint</b>. The <b>domain</b> is the
 * logging domain that should be used.
 *
 * Return 0 if everything went well or a negative value representing a circuit
 * end reason on error for which the caller is responsible for closing it. */
static int
process_sendme_cell(const relay_msg_t *msg, circuit_t *circ,
                    edge_connection_t *conn, crypt_path_t *layer_hint,
                    int domain)
{
  int ret;

  tor_assert(msg);

  if (!msg->stream_id) {
    /* Circuit level SENDME cell. */
    ret = sendme_process_circuit_level(layer_hint, circ, msg->body,
                                       msg->length);
    if (ret < 0) {
      return ret;
    }
    /* Resume reading on any streams now that we've processed a valid
     * SENDME cell that updated our package window. */
    circuit_resume_edge_reading(circ, layer_hint);
    /* We are done, the rest of the code is for the stream level. */
    return 0;
  }

  /* No connection, might be half edge state. We are done if so. */
  if (!conn) {
    if (CIRCUIT_IS_ORIGIN(circ)) {
      origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
      if (connection_half_edge_is_valid_sendme(ocirc->half_streams,
                                               msg->stream_id)) {
        circuit_read_valid_data(ocirc, msg->length);
        log_info(domain, "Sendme cell on circ %u valid on half-closed "
                         "stream id %d",
                 ocirc->global_identifier, msg->stream_id);
      }
    }

    log_info(domain, "SENDME cell dropped, unknown stream (streamid %d).",
             msg->stream_id);
    return 0;
  }

  /* Stream level SENDME cell. */
  // TODO: Turn this off for cc_alg=1,2,3; use XON/XOFF instead
  ret = sendme_process_stream_level(conn, circ, msg->length);
  if (ret < 0) {
    /* Means we need to close the circuit with reason ret. */
    return ret;
  }

  /* We've now processed properly a SENDME cell, all windows have been
   * properly updated, we'll read on the edge connection to see if we can
   * get data out towards the end point (Exit or client) since we are now
   * allowed to deliver more cells. */

  if (circuit_queue_streams_are_blocked(circ)) {
    /* Still waiting for queue to flush; don't touch conn */
    return 0;
  }
  connection_start_reading(TO_CONN(conn));
  /* handle whatever might still be on the inbuf */
  if (connection_edge_package_raw_inbuf(conn, 1, NULL) < 0) {
    /* (We already sent an end cell if possible) */
    connection_mark_for_close(TO_CONN(conn));
    return 0;
  }
  return 0;
}

/** A helper for connection_edge_process_relay_cell(): Actually handles the
 *  cell that we received on the connection.
 *
 *  The arguments are the same as in the parent function
 *  connection_edge_process_relay_cell(), plus the relay header <b>rh</b> as
 *  unpacked by the parent function, and <b>optimistic_data</b> as set by the
 *  parent function.
 */
STATIC int
handle_relay_msg(const relay_msg_t *msg, circuit_t *circ,
                 edge_connection_t *conn, crypt_path_t *layer_hint,
                 int optimistic_data)
{
  unsigned domain = layer_hint?LD_APP:LD_EXIT;
  int reason;

  tor_assert(msg);

  /* First pass the cell to the circuit padding subsystem, in case it's a
   * padding cell or circuit that should be handled there. */
  if (circpad_check_received_cell(msg, circ, layer_hint) == 0) {
    log_debug(domain, "Cell handled as circuit padding");
    return 0;
  }

  /* Now handle all the other commands */
  switch (msg->command) {
    case RELAY_COMMAND_CONFLUX_LINK:
      conflux_process_link(circ, msg);
      return 0;
    case RELAY_COMMAND_CONFLUX_LINKED:
      conflux_process_linked(circ, layer_hint, msg);
      return 0;
    case RELAY_COMMAND_CONFLUX_LINKED_ACK:
      conflux_process_linked_ack(circ);
      return 0;
    case RELAY_COMMAND_CONFLUX_SWITCH:
      return conflux_process_switch_command(circ, layer_hint, msg);
    case RELAY_COMMAND_BEGIN:
    case RELAY_COMMAND_BEGIN_DIR:
      if (layer_hint &&
          circ->purpose != CIRCUIT_PURPOSE_S_REND_JOINED) {
        log_fn(LOG_PROTOCOL_WARN, LD_APP,
               "Relay begin request unsupported at AP. Dropping.");
        return 0;
      }
      if (circ->purpose == CIRCUIT_PURPOSE_S_REND_JOINED &&
          layer_hint != TO_ORIGIN_CIRCUIT(circ)->cpath->prev) {
        log_fn(LOG_PROTOCOL_WARN, LD_APP,
               "Relay begin request to Hidden Service "
               "from intermediary node. Dropping.");
        return 0;
      }
      if (conn) {
        log_fn(LOG_PROTOCOL_WARN, domain,
               "Begin cell for known stream. Dropping.");
        return 0;
      }
      if (msg->command == RELAY_COMMAND_BEGIN_DIR &&
          circ->purpose != CIRCUIT_PURPOSE_S_REND_JOINED) {
        /* Assign this circuit and its app-ward OR connection a unique ID,
         * so that we can measure download times. The local edge and dir
         * connection will be assigned the same ID when they are created
         * and linked. */
        static uint64_t next_id = 0;
        circ->dirreq_id = ++next_id;
        TO_OR_CIRCUIT(circ)->p_chan->dirreq_id = circ->dirreq_id;
      }
      return connection_exit_begin_conn(msg, circ);
    case RELAY_COMMAND_DATA:
      ++stats_n_data_cells_received;

      if (msg->stream_id == 0) {
        log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL, "Relay data cell with zero "
               "stream_id. Dropping.");
        return 0;
      } else if (!conn) {
        if (CIRCUIT_IS_ORIGIN(circ)) {
          origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
          if (connection_half_edge_is_valid_data(ocirc->half_streams,
                                                 msg->stream_id)) {
            circuit_read_valid_data(ocirc, msg->length);
            log_info(domain,
                     "data cell on circ %u valid on half-closed "
                     "stream id %d", ocirc->global_identifier, msg->stream_id);
          }
        }

        log_info(domain,"data cell dropped, unknown stream (streamid %d).",
                 msg->stream_id);
        return 0;
      }

      /* Update our stream-level deliver window that we just received a DATA
       * cell. Going below 0 means we have a protocol level error so the
       * stream and circuit are closed. */
      if (sendme_stream_data_received(conn) < 0) {
        log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
               "(relay data) conn deliver_window below 0. Killing.");
        connection_edge_end_close(conn, END_STREAM_REASON_TORPROTOCOL);
        return -END_CIRC_REASON_TORPROTOCOL;
      }
      /* Total all valid application bytes delivered */
      if (CIRCUIT_IS_ORIGIN(circ) && msg->length > 0) {
        circuit_read_valid_data(TO_ORIGIN_CIRCUIT(circ), msg->length);
      }

      /* For onion service connection, update the metrics. */
      if (conn->hs_ident) {
        hs_metrics_app_write_bytes(&conn->hs_ident->identity_pk,
                                   conn->hs_ident->orig_virtual_port,
                                   msg->length);
      }

      stats_n_data_bytes_received += msg->length;
      connection_buf_add((char*) msg->body, msg->length, TO_CONN(conn));

#ifdef MEASUREMENTS_21206
      /* Count number of RELAY_DATA cells received on a linked directory
       * connection. */
      connection_t *linked_conn = TO_CONN(conn)->linked_conn;

      if (linked_conn && linked_conn->type == CONN_TYPE_DIR) {
        ++(TO_DIR_CONN(linked_conn)->data_cells_received);
      }
#endif /* defined(MEASUREMENTS_21206) */

      if (!optimistic_data) {
        /* Only send a SENDME if we're not getting optimistic data; otherwise
         * a SENDME could arrive before the CONNECTED.
         */
        sendme_connection_edge_consider_sending(conn);
      }

      return 0;
    case RELAY_COMMAND_XOFF:
      if (!conn) {
        if (CIRCUIT_IS_ORIGIN(circ)) {
          origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
          if (relay_crypt_from_last_hop(ocirc, layer_hint) &&
              connection_half_edge_is_valid_data(ocirc->half_streams,
                                                 msg->stream_id)) {
            circuit_read_valid_data(ocirc, msg->length);
          }
        }
        return 0;
      }

      if (circuit_process_stream_xoff(conn, layer_hint)) {
        if (CIRCUIT_IS_ORIGIN(circ)) {
          circuit_read_valid_data(TO_ORIGIN_CIRCUIT(circ), msg->length);
        }
      }
      return 0;
    case RELAY_COMMAND_XON:
      if (!conn) {
        if (CIRCUIT_IS_ORIGIN(circ)) {
          origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
          if (relay_crypt_from_last_hop(ocirc, layer_hint) &&
              connection_half_edge_is_valid_data(ocirc->half_streams,
                                                 msg->stream_id)) {
            circuit_read_valid_data(ocirc, msg->length);
          }
        }
        return 0;
      }

      if (circuit_process_stream_xon(conn, layer_hint, msg)) {
        if (CIRCUIT_IS_ORIGIN(circ)) {
          circuit_read_valid_data(TO_ORIGIN_CIRCUIT(circ), msg->length);
        }
      }
      return 0;
    case RELAY_COMMAND_END:
      reason = msg->length > 0 ? get_uint8(msg->body) : END_STREAM_REASON_MISC;
      if (!conn) {
        if (CIRCUIT_IS_ORIGIN(circ)) {
          origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
          if (relay_crypt_from_last_hop(ocirc, layer_hint) &&
              connection_half_edge_is_valid_end(ocirc->half_streams,
                                                msg->stream_id)) {

            circuit_read_valid_data(ocirc, msg->length);
            log_info(domain,
                     "end cell (%s) on circ %u valid on half-closed "
                     "stream id %d",
                     stream_end_reason_to_string(reason),
                     ocirc->global_identifier, msg->stream_id);
            return 0;
          }
        }
        log_info(domain,"end cell (%s) dropped, unknown stream.",
                 stream_end_reason_to_string(reason));
        return 0;
      }
/* XXX add to this log_fn the exit node's nickname? */
      log_info(domain,TOR_SOCKET_T_FORMAT": end cell (%s) for stream %d. "
               "Removing stream.",
               conn->base_.s,
               stream_end_reason_to_string(reason),
               conn->stream_id);
      if (conn->base_.type == CONN_TYPE_AP) {
        entry_connection_t *entry_conn = EDGE_TO_ENTRY_CONN(conn);
        if (entry_conn->socks_request &&
            !entry_conn->socks_request->has_finished)
          log_warn(LD_BUG,
                   "open stream hasn't sent socks answer yet? Closing.");
      }
      /* We just *got* an end; no reason to send one. */
      conn->edge_has_sent_end = 1;
      if (!conn->end_reason)
        conn->end_reason = reason | END_STREAM_REASON_FLAG_REMOTE;
      if (!conn->base_.marked_for_close) {
        /* only mark it if not already marked. it's possible to
         * get the 'end' right around when the client hangs up on us. */
        connection_mark_and_flush(TO_CONN(conn));

        /* Total all valid application bytes delivered */
        if (CIRCUIT_IS_ORIGIN(circ)) {
          circuit_read_valid_data(TO_ORIGIN_CIRCUIT(circ), msg->length);
        }
      }
      return 0;
    case RELAY_COMMAND_EXTEND:
    case RELAY_COMMAND_EXTEND2: {
      static uint64_t total_n_extend=0, total_nonearly=0;
      total_n_extend++;
      if (msg->stream_id) {
        log_fn(LOG_PROTOCOL_WARN, domain,
               "'extend' cell received for non-zero stream. Dropping.");
        return 0;
      }
      if (!msg->is_relay_early &&
          !networkstatus_get_param(NULL,"AllowNonearlyExtend",0,0,1)) {
#define EARLY_WARNING_INTERVAL 3600
        static ratelim_t early_warning_limit =
          RATELIM_INIT(EARLY_WARNING_INTERVAL);
        char *m;
        if (!msg->is_relay_early) {
          ++total_nonearly;
          if ((m = rate_limit_log(&early_warning_limit, approx_time()))) {
            double percentage = ((double)total_nonearly)/total_n_extend;
            percentage *= 100;
            log_fn(LOG_PROTOCOL_WARN, domain, "EXTEND cell received, "
                   "but not via RELAY_EARLY. Dropping.%s", m);
            log_fn(LOG_PROTOCOL_WARN, domain, "  (We have dropped %.02f%% of "
                   "all EXTEND cells for this reason)", percentage);
            tor_free(m);
          }
        } else {
          log_fn(LOG_WARN, domain,
                 "EXTEND cell received, in a cell with type %d! Dropping.",
                 msg->command);
        }
        return 0;
      }
      return circuit_extend(msg, circ);
    }
    case RELAY_COMMAND_EXTENDED:
    case RELAY_COMMAND_EXTENDED2:
      if (!layer_hint) {
        log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
               "'extended' unsupported at non-origin. Dropping.");
        return 0;
      }
      log_debug(domain,"Got an extended cell! Yay.");
      {
        extended_cell_t extended_cell;
        if (extended_cell_parse(&extended_cell, msg->command,
                                msg->body, msg->length) < 0) {
          log_warn(LD_PROTOCOL,
                   "Can't parse EXTENDED cell; killing circuit.");
          return -END_CIRC_REASON_TORPROTOCOL;
        }
        if ((reason = circuit_finish_handshake(TO_ORIGIN_CIRCUIT(circ),
                                         &extended_cell.created_cell)) < 0) {
          circuit_mark_for_close(circ, -reason);
          return 0; /* We don't want to cause a warning, so we mark the circuit
                     * here. */
        }
      }
      if ((reason=circuit_send_next_onion_skin(TO_ORIGIN_CIRCUIT(circ)))<0) {
        log_info(domain,"circuit_send_next_onion_skin() failed.");
        return reason;
      }
      /* Total all valid bytes delivered. */
      if (CIRCUIT_IS_ORIGIN(circ)) {
        circuit_read_valid_data(TO_ORIGIN_CIRCUIT(circ), msg->length);
      }
      return 0;
    case RELAY_COMMAND_TRUNCATE:
      if (layer_hint) {
        log_fn(LOG_PROTOCOL_WARN, LD_APP,
               "'truncate' unsupported at origin. Dropping.");
        return 0;
      }
      if (circ->n_hop) {
        if (circ->n_chan)
          log_warn(LD_BUG, "n_chan and n_hop set on the same circuit!");
        extend_info_free(circ->n_hop);
        circ->n_hop = NULL;
        tor_free(circ->n_chan_create_cell);
        circuit_set_state(circ, CIRCUIT_STATE_OPEN);
      }
      if (circ->n_chan) {
        uint8_t trunc_reason = get_uint8(msg->body);
        circuit_synchronize_written_or_bandwidth(circ, CIRCUIT_N_CHAN);
        circuit_clear_cell_queue(circ, circ->n_chan);
        channel_send_destroy(circ->n_circ_id, circ->n_chan,
                             trunc_reason);
        circuit_set_n_circid_chan(circ, 0, NULL);
      }
      log_debug(LD_EXIT, "Processed 'truncate', replying.");
      {
        char payload[1];
        payload[0] = (char)END_CIRC_REASON_REQUESTED;
        relay_send_command_from_edge(0, circ, RELAY_COMMAND_TRUNCATED,
                                     payload, sizeof(payload), NULL);
      }
      return 0;
    case RELAY_COMMAND_TRUNCATED:
      if (!layer_hint) {
        log_fn(LOG_PROTOCOL_WARN, LD_EXIT,
               "'truncated' unsupported at non-origin. Dropping.");
        return 0;
      }

      /* Count the truncated as valid, for completeness. The
       * circuit is being torn down anyway, though.  */
      if (CIRCUIT_IS_ORIGIN(circ)) {
        circuit_read_valid_data(TO_ORIGIN_CIRCUIT(circ), msg->length);
      }
      circuit_truncated(TO_ORIGIN_CIRCUIT(circ), get_uint8(msg->body));
      return 0;
    case RELAY_COMMAND_CONNECTED:
      if (conn) {
        log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
               "'connected' unsupported while open. Closing circ.");
        return -END_CIRC_REASON_TORPROTOCOL;
      }

      if (CIRCUIT_IS_ORIGIN(circ)) {
        origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
        if (connection_half_edge_is_valid_connected(ocirc->half_streams,
                                                    msg->stream_id)) {
          circuit_read_valid_data(ocirc, msg->length);
          log_info(domain,
                   "connected cell on circ %u valid on half-closed "
                   "stream id %d", ocirc->global_identifier, msg->stream_id);
          return 0;
        }
      }

      log_info(domain,
               "'connected' received on circid %u for streamid %d, "
               "no conn attached anymore. Ignoring.",
               (unsigned)circ->n_circ_id, msg->stream_id);
      return 0;
    case RELAY_COMMAND_SENDME:
      return process_sendme_cell(msg, circ, conn, layer_hint, domain);
    case RELAY_COMMAND_RESOLVE:
      if (layer_hint) {
        log_fn(LOG_PROTOCOL_WARN, LD_APP,
               "resolve request unsupported at AP; dropping.");
        return 0;
      } else if (conn) {
        log_fn(LOG_PROTOCOL_WARN, domain,
               "resolve request for known stream; dropping.");
        return 0;
      } else if (circ->purpose != CIRCUIT_PURPOSE_OR) {
        log_fn(LOG_PROTOCOL_WARN, domain,
               "resolve request on circ with purpose %d; dropping",
               circ->purpose);
        return 0;
      }
      return connection_exit_begin_resolve(msg, TO_OR_CIRCUIT(circ));
    case RELAY_COMMAND_RESOLVED:
      if (conn) {
        log_fn(LOG_PROTOCOL_WARN, domain,
               "'resolved' unsupported while open. Closing circ.");
        return -END_CIRC_REASON_TORPROTOCOL;
      }

      if (CIRCUIT_IS_ORIGIN(circ)) {
        origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
        if (relay_crypt_from_last_hop(ocirc, layer_hint) &&
            connection_half_edge_is_valid_resolved(ocirc->half_streams,
                                                   msg->stream_id)) {
          circuit_read_valid_data(ocirc, msg->length);
          log_info(domain,
                   "resolved cell on circ %u valid on half-closed "
                   "stream id %d", ocirc->global_identifier, msg->stream_id);
          return 0;
        }
      }

      log_info(domain,
               "'resolved' received, no conn attached anymore. Ignoring.");
      return 0;
    case RELAY_COMMAND_ESTABLISH_INTRO:
    case RELAY_COMMAND_ESTABLISH_RENDEZVOUS:
    case RELAY_COMMAND_INTRODUCE1:
    case RELAY_COMMAND_INTRODUCE2:
    case RELAY_COMMAND_INTRODUCE_ACK:
    case RELAY_COMMAND_RENDEZVOUS1:
    case RELAY_COMMAND_RENDEZVOUS2:
    case RELAY_COMMAND_INTRO_ESTABLISHED:
    case RELAY_COMMAND_RENDEZVOUS_ESTABLISHED:
      rend_process_relay_cell(circ, layer_hint,
                              msg->command, msg->length, msg->body);
      return 0;
  }
  log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
         "Received unknown relay command %d. Perhaps the other side is using "
         "a newer version of Tor? Dropping.",
         msg->command);
  return 0; /* for forward compatibility, don't kill the circuit */
}

/** An incoming relay cell has arrived on circuit <b>circ</b>. If
 * <b>conn</b> is NULL this is a control cell, else <b>cell</b> is
 * destined for <b>conn</b>.
 *
 * If <b>layer_hint</b> is defined, then we're the origin of the
 * circuit, and it specifies the hop that packaged <b>cell</b>.
 *
 * Return -reason if you want to warn and tear down the circuit, else 0.
 */
STATIC int
connection_edge_process_relay_cell(const relay_msg_t *msg, circuit_t *circ,
                                   edge_connection_t *conn,
                                   crypt_path_t *layer_hint)
{
  static int num_seen=0;
  unsigned domain = layer_hint?LD_APP:LD_EXIT;

  tor_assert(msg);
  tor_assert(circ);

//  log_fn(LOG_DEBUG,"command %d stream %d", rh.command, rh.stream_id);
  num_seen++;
  log_debug(domain, "Now seen %d relay cells here (command %d, stream %d).",
            num_seen, msg->command, msg->stream_id);

  if (msg->stream_id == 0) {
    switch (msg->command) {
      case RELAY_COMMAND_BEGIN:
      case RELAY_COMMAND_CONNECTED:
      case RELAY_COMMAND_END:
      case RELAY_COMMAND_RESOLVE:
      case RELAY_COMMAND_RESOLVED:
      case RELAY_COMMAND_BEGIN_DIR:
        log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL, "Relay command %u with zero "
               "stream_id. Dropping.", msg->command);
        return 0;
      default:
        ;
    }
  }

  /* Regardless of conflux or not, we always decide to send a SENDME
   * for RELAY_DATA immediately
   */
  if (msg->command == RELAY_COMMAND_DATA) {
    /* Update our circuit-level deliver window that we received a DATA cell.
     * If the deliver window goes below 0, we end the circuit and stream due
     * to a protocol failure. */
    if (sendme_circuit_data_received(circ, layer_hint) < 0) {
      log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
             "(relay data) circ deliver_window below 0. Killing.");
      connection_edge_end_close(conn, END_STREAM_REASON_TORPROTOCOL);
      return -END_CIRC_REASON_TORPROTOCOL;
    }

    /* Consider sending a circuit-level SENDME cell. */
    sendme_circuit_consider_sending(circ, layer_hint);

    /* Continue on to process the data cell via conflux or not */
  }

  /* Conflux handling: If conflux is disabled, or the relay command is not
   * multiplexed across circuits, then process it immediately.
   *
   * Otherwise, we need to process the relay cell against our conflux
   * queues, and if doing so results in ordered cells to deliver, we
   * dequeue and process those in-order until there are no more.
   */
  if (!circ->conflux || !conflux_should_multiplex(msg->command)) {
    return connection_edge_process_ordered_relay_cell(msg, circ, conn,
                                                      layer_hint);
  } else {
    // If conflux says this cell is in-order, then begin processing
    // cells from queue until there are none. Otherwise, we do nothing
    // until further cells arrive.
    if (conflux_process_relay_msg(circ->conflux, circ, layer_hint,
                                  (relay_msg_t *) msg)) {
      conflux_msg_t *c_msg = NULL;

      /* First, process this cell */
      int ret = connection_edge_process_ordered_relay_cell(
                 msg, circ, conn, layer_hint);
      if (ret < 0) {
        return ret;
      }

      /* Now, check queue for more */
      while ((c_msg = conflux_dequeue_relay_msg(circ->conflux))) {
        conn = relay_lookup_conn(circ, c_msg->msg, CELL_DIRECTION_OUT,
                                 layer_hint);
        ret = connection_edge_process_ordered_relay_cell(c_msg->msg, circ,
                                                         conn,
                                                         layer_hint);
        if (ret < 0) {
          /* Negative return value is a fatal error. Return early and tear down
           * circuit */
          conflux_relay_msg_free(c_msg);
          return ret;
        }
        conflux_relay_msg_free(c_msg);
      }
    }
  }

  return 0;
}

/**
 * Helper function to process a relay cell that is in the proper order
 * for processing right now. */
static int
connection_edge_process_ordered_relay_cell(const relay_msg_t *msg,
                                           circuit_t *circ,
                                           edge_connection_t *conn,
                                           crypt_path_t *layer_hint)
{
  int optimistic_data = 0; /* Set to 1 if we receive data on a stream
                            * that's in the EXIT_CONN_STATE_RESOLVING
                            * or EXIT_CONN_STATE_CONNECTING states. */

  /* Tell circpad that we've received a recognized cell */
  circpad_deliver_recognized_relay_cell_events(circ, msg->command, layer_hint);

  /* either conn is NULL, in which case we've got a control cell, or else
   * conn points to the recognized stream. */
  if (conn && !connection_state_is_open(TO_CONN(conn))) {
    if (conn->base_.type == CONN_TYPE_EXIT &&
        (conn->base_.state == EXIT_CONN_STATE_CONNECTING ||
         conn->base_.state == EXIT_CONN_STATE_RESOLVING) &&
        msg->command == RELAY_COMMAND_DATA) {
      /* Allow DATA cells to be delivered to an exit node in state
       * EXIT_CONN_STATE_CONNECTING or EXIT_CONN_STATE_RESOLVING.
       * This speeds up HTTP, for example. */
      optimistic_data = 1;
    } else if (msg->stream_id == 0 && msg->command == RELAY_COMMAND_DATA) {
      log_warn(LD_BUG, "Somehow I had a connection that matched a "
               "data cell with stream ID 0.");
    } else {
      return connection_edge_process_relay_cell_not_open(
               msg, circ, conn, layer_hint);
    }
  }

  return handle_relay_msg(msg, circ, conn, layer_hint, optimistic_data);
}

/** How many relay_data cells have we built, ever? */
uint64_t stats_n_data_cells_packaged = 0;
/** How many bytes of data have we put in relay_data cells have we built,
 * ever? This would be RELAY_PAYLOAD_SIZE*stats_n_data_cells_packaged if
 * every relay cell we ever sent were completely full of data. */
uint64_t stats_n_data_bytes_packaged = 0;
/** How many relay_data cells have we received, ever? */
uint64_t stats_n_data_cells_received = 0;
/** How many bytes of data have we received relay_data cells, ever? This would
 * be RELAY_PAYLOAD_SIZE*stats_n_data_cells_packaged if every relay cell we
 * ever received were completely full of data. */
uint64_t stats_n_data_bytes_received = 0;

/**
 * Called when initializing a circuit, or when we have reached the end of the
 * window in which we need to send some randomness so that incoming sendme
 * cells will be unpredictable.  Resets the flags and picks a new window.
 */
void
circuit_reset_sendme_randomness(circuit_t *circ)
{
  circ->have_sent_sufficiently_random_cell = 0;
  // XXX: do we need to change this check for congestion control?
  circ->send_randomness_after_n_cells = CIRCWINDOW_INCREMENT / 2 +
    crypto_fast_rng_get_uint(get_thread_fast_rng(), CIRCWINDOW_INCREMENT / 2);
}

/**
 * Helper. Return the number of bytes that should be put into a cell from a
 * given edge connection on which <b>n_available</b> bytes are available.
 */
STATIC size_t
connection_edge_get_inbuf_bytes_to_package(size_t n_available,
                                           int package_partial,
                                           circuit_t *on_circuit,
                                           crypt_path_t *cpath)
{
  if (!n_available)
    return 0;

  /* Do we need to force this payload to have space for randomness? */
  const bool force_random_bytes =
    (on_circuit->send_randomness_after_n_cells == 0) &&
    (! on_circuit->have_sent_sufficiently_random_cell);

  relay_cell_fmt_t cell_format = circuit_get_relay_format(on_circuit, cpath);
  size_t target_length =
    relay_cell_max_payload_size(cell_format, RELAY_COMMAND_DATA);

#define RELAY_CELL_PADDING_GAP 4

  /* Any relay data payload containing fewer than this many real bytes is
   * considered to have enough randomness to. */
  size_t target_length_with_random = target_length -
                                     RELAY_CELL_PADDING_GAP - 16;
  if (force_random_bytes) {
    target_length = target_length_with_random;
  }

  /* Decide how many bytes we will actually put into this cell. */
  size_t package_length;
  if (n_available >= target_length) { /* A full payload is available. */
    package_length = target_length;
  } else { /* not a full payload available */
    if (package_partial)
      package_length = n_available; /* just take whatever's available now */
    else
      return 0; /* nothing to do until we have a full payload */
  }

  /* If we reach this point, we will be definitely sending the cell. */
  tor_assert_nonfatal(package_length > 0);

  if (package_length <= target_length_with_random) {
    /* This cell will have enough randomness in the padding to make a future
     * sendme cell unpredictable. */
    on_circuit->have_sent_sufficiently_random_cell = 1;
  }

  if (on_circuit->send_randomness_after_n_cells == 0) {
    /* Either this cell, or some previous cell, had enough padding to
     * ensure sendme unpredictability. */
    tor_assert_nonfatal(on_circuit->have_sent_sufficiently_random_cell);
    /* Pick a new interval in which we need to send randomness. */
    circuit_reset_sendme_randomness(on_circuit);
  }

  --on_circuit->send_randomness_after_n_cells;

  return package_length;
}

/** If <b>conn</b> has an entire relay payload of bytes on its inbuf (or
 * <b>package_partial</b> is true), and the appropriate package windows aren't
 * empty, grab a cell and send it down the circuit.
 *
 * If *<b>max_cells</b> is given, package no more than max_cells.  Decrement
 * *<b>max_cells</b> by the number of cells packaged.
 *
 * Return -1 (and send a RELAY_COMMAND_END cell if necessary) if conn should
 * be marked for close, else return 0.
 */
int
connection_edge_package_raw_inbuf(edge_connection_t *conn, int package_partial,
                                  int *max_cells)
{
  size_t bytes_to_process, length;
  char payload[CELL_PAYLOAD_SIZE];
  circuit_t *circ;
  const unsigned domain = conn->base_.type == CONN_TYPE_AP ? LD_APP : LD_EXIT;
  int sending_from_optimistic = 0;
  entry_connection_t *entry_conn =
    conn->base_.type == CONN_TYPE_AP ? EDGE_TO_ENTRY_CONN(conn) : NULL;
  const int sending_optimistically =
    entry_conn &&
    conn->base_.type == CONN_TYPE_AP &&
    conn->base_.state != AP_CONN_STATE_OPEN;
  crypt_path_t *cpath_layer = conn->cpath_layer;

  tor_assert(conn);

  if (BUG(conn->base_.marked_for_close)) {
    log_warn(LD_BUG,
             "called on conn that's already marked for close at %s:%d.",
             conn->base_.marked_for_close_file, conn->base_.marked_for_close);
    return 0;
  }

  if (max_cells && *max_cells <= 0)
    return 0;

 repeat_connection_edge_package_raw_inbuf:

  circ = circuit_get_by_edge_conn(conn);
  if (!circ) {
    log_info(domain,"conn has no circuit! Closing.");
    conn->end_reason = END_STREAM_REASON_CANT_ATTACH;
    return -1;
  }

  if (circuit_consider_stop_edge_reading(circ, cpath_layer))
    return 0;

  if (conn->package_window <= 0) {
    log_info(domain,"called with package_window %d. Skipping.",
             conn->package_window);
    connection_stop_reading(TO_CONN(conn));
    return 0;
  }

  sending_from_optimistic = entry_conn &&
    entry_conn->sending_optimistic_data != NULL;

  if (PREDICT_UNLIKELY(sending_from_optimistic)) {
    bytes_to_process = buf_datalen(entry_conn->sending_optimistic_data);
    if (PREDICT_UNLIKELY(!bytes_to_process)) {
      log_warn(LD_BUG, "sending_optimistic_data was non-NULL but empty");
      bytes_to_process = connection_get_inbuf_len(TO_CONN(conn));
      sending_from_optimistic = 0;
    }
  } else {
    bytes_to_process = connection_get_inbuf_len(TO_CONN(conn));
  }

  length = connection_edge_get_inbuf_bytes_to_package(bytes_to_process,
                                                      package_partial, circ,
                                                      cpath_layer);
  if (!length)
    return 0;

  /* If we reach this point, we will definitely be packaging bytes into
   * a cell. */

  stats_n_data_bytes_packaged += length;
  stats_n_data_cells_packaged += 1;

  if (PREDICT_UNLIKELY(sending_from_optimistic)) {
    /* XXXX We could be more efficient here by sometimes packing
     * previously-sent optimistic data in the same cell with data
     * from the inbuf. */
    buf_get_bytes(entry_conn->sending_optimistic_data, payload, length);
    if (!buf_datalen(entry_conn->sending_optimistic_data)) {
        buf_free(entry_conn->sending_optimistic_data);
        entry_conn->sending_optimistic_data = NULL;
    }
  } else {
    connection_buf_get_bytes(payload, length, TO_CONN(conn));
  }

  log_debug(domain,TOR_SOCKET_T_FORMAT": Packaging %d bytes (%d waiting).",
            conn->base_.s,
            (int)length, (int)connection_get_inbuf_len(TO_CONN(conn)));

  if (sending_optimistically && !sending_from_optimistic) {
    /* This is new optimistic data; remember it in case we need to detach and
       retry */
    if (!entry_conn->pending_optimistic_data)
      entry_conn->pending_optimistic_data = buf_new();
    buf_add(entry_conn->pending_optimistic_data, payload, length);
  }

  /* Send a data cell. This handles the circuit package window. */
  if (connection_edge_send_command(conn, RELAY_COMMAND_DATA,
                                   payload, length) < 0 ) {
    /* circuit got marked for close, don't continue, don't need to mark conn */
    return 0;
  }

  /* Handle the stream-level SENDME package window. */
  if (sendme_note_stream_data_packaged(conn, length) < 0) {
    connection_stop_reading(TO_CONN(conn));
    log_debug(domain,"conn->package_window reached 0.");
    circuit_consider_stop_edge_reading(circ, cpath_layer);
    return 0; /* don't process the inbuf any more */
  }
  log_debug(domain,"conn->package_window is now %d",conn->package_window);

  if (max_cells) {
    *max_cells -= 1;
    if (*max_cells <= 0)
      return 0;
  }

  /* handle more if there's more, or return 0 if there isn't */
  goto repeat_connection_edge_package_raw_inbuf;
}

/** The circuit <b>circ</b> has received a circuit-level sendme
 * (on hop <b>layer_hint</b>, if we're the OP). Go through all the
 * attached streams and let them resume reading and packaging, if
 * their stream windows allow it.
 */
static void
circuit_resume_edge_reading(circuit_t *circ, crypt_path_t *layer_hint)
{
  if (circuit_queue_streams_are_blocked(circ)) {
    log_debug(layer_hint?LD_APP:LD_EXIT,"Too big queue, no resuming");
    return;
  }

  /* If we have a conflux negotiated, and it still can't send on
   * any circuit, then do not resume sending. */
  if (circ->conflux && !conflux_can_send(circ->conflux)) {
    log_debug(layer_hint?LD_APP:LD_EXIT,
              "Conflux can't send, not resuming edges");
    return;
  }

  log_debug(layer_hint?LD_APP:LD_EXIT,"resuming");

  if (CIRCUIT_IS_ORIGIN(circ))
    circuit_resume_edge_reading_helper(TO_ORIGIN_CIRCUIT(circ)->p_streams,
                                       circ, layer_hint);
  else
    circuit_resume_edge_reading_helper(TO_OR_CIRCUIT(circ)->n_streams,
                                       circ, layer_hint);
}

/** A helper function for circuit_resume_edge_reading() above.
 * The arguments are the same, except that <b>conn</b> is the head
 * of a linked list of edge streams that should each be considered.
 */
static int
circuit_resume_edge_reading_helper(edge_connection_t *first_conn,
                                   circuit_t *circ,
                                   crypt_path_t *layer_hint)
{
  edge_connection_t *conn;
  int n_packaging_streams, n_streams_left;
  int packaged_this_round;
  int cells_on_queue;
  int cells_per_conn;
  edge_connection_t *chosen_stream = NULL;
  int max_to_package;

  if (first_conn == NULL) {
    /* Don't bother to try to do the rest of this if there are no connections
     * to resume. */
    return 0;
  }

  /* Once we used to start listening on the streams in the order they
   * appeared in the linked list.  That leads to starvation on the
   * streams that appeared later on the list, since the first streams
   * would always get to read first.  Instead, we just pick a random
   * stream on the list, and enable reading for streams starting at that
   * point (and wrapping around as if the list were circular).  It would
   * probably be better to actually remember which streams we've
   * serviced in the past, but this is simple and effective. */

  /* Select a stream uniformly at random from the linked list.  We
   * don't need cryptographic randomness here. */
  {
    int num_streams = 0;
    for (conn = first_conn; conn; conn = conn->next_stream) {
      num_streams++;

      if (crypto_fast_rng_one_in_n(get_thread_fast_rng(), num_streams)) {
        chosen_stream = conn;
      }
      /* Invariant: chosen_stream has been chosen uniformly at random from
       * among the first num_streams streams on first_conn.
       *
       * (Note that we iterate over every stream on the circuit, so that after
       * we've considered the first stream, we've chosen it with P=1; and
       * after we consider the second stream, we've switched to it with P=1/2
       * and stayed with the first stream with P=1/2; and after we've
       * considered the third stream, we've switched to it with P=1/3 and
       * remained with one of the first two streams with P=(2/3), giving each
       * one P=(1/2)(2/3) )=(1/3).) */
    }
  }

  /* Count how many non-marked streams there are that have anything on
   * their inbuf, and enable reading on all of the connections. */
  n_packaging_streams = 0;
  /* Activate reading starting from the chosen stream */
  for (conn=chosen_stream; conn; conn = conn->next_stream) {
    /* Start reading for the streams starting from here */
    if (conn->base_.marked_for_close || conn->package_window <= 0)
      continue;

    if (edge_uses_cpath(conn, layer_hint)) {
      if (!conn->xoff_received) {
        connection_start_reading(TO_CONN(conn));
      }

      if (connection_get_inbuf_len(TO_CONN(conn)) > 0)
        ++n_packaging_streams;
    }
  }
  /* Go back and do the ones we skipped, circular-style */
  for (conn = first_conn; conn != chosen_stream; conn = conn->next_stream) {
    if (conn->base_.marked_for_close || conn->package_window <= 0)
      continue;

    if (edge_uses_cpath(conn, layer_hint)) {
      if (!conn->xoff_received) {
        connection_start_reading(TO_CONN(conn));
      }

      if (connection_get_inbuf_len(TO_CONN(conn)) > 0)
        ++n_packaging_streams;
    }
  }

  if (n_packaging_streams == 0) /* avoid divide-by-zero */
    return 0;

 again:

  /* If we're using conflux, the circuit we decide to send on may change
   * after we're sending. Get it again, and re-check package windows
   * for it */
  if (circ->conflux) {
    if (circuit_consider_stop_edge_reading(circ, layer_hint))
      return -1;

    circ = conflux_decide_next_circ(circ->conflux);

    /* Get the destination layer hint for this circuit */
    layer_hint = conflux_get_destination_hop(circ);
  }

  /* How many cells do we have space for?  It will be the minimum of
   * the number needed to exhaust the package window, and the minimum
   * needed to fill the cell queue. */
  max_to_package = congestion_control_get_package_window(circ, layer_hint);
  if (CIRCUIT_IS_ORIGIN(circ)) {
    cells_on_queue = circ->n_chan_cells.n;
  } else {
    or_circuit_t *or_circ = TO_OR_CIRCUIT(circ);
    cells_on_queue = or_circ->p_chan_cells.n;
  }
  if (cell_queue_highwatermark() - cells_on_queue < max_to_package)
    max_to_package = cell_queue_highwatermark() - cells_on_queue;

  cells_per_conn = CEIL_DIV(max_to_package, n_packaging_streams);

  packaged_this_round = 0;
  n_streams_left = 0;

  /* Iterate over all connections.  Package up to cells_per_conn cells on
   * each.  Update packaged_this_round with the total number of cells
   * packaged, and n_streams_left with the number that still have data to
   * package.
   */
  for (conn=first_conn; conn; conn=conn->next_stream) {
    if (conn->base_.marked_for_close || conn->package_window <= 0)
      continue;
    if (edge_uses_cpath(conn, layer_hint)) {
      int n = cells_per_conn, r;
      /* handle whatever might still be on the inbuf */
      r = connection_edge_package_raw_inbuf(conn, 1, &n);

      /* Note how many we packaged */
      packaged_this_round += (cells_per_conn-n);

      if (r<0) {
        /* Problem while packaging. (We already sent an end cell if
         * possible) */
        connection_mark_for_close(TO_CONN(conn));
        continue;
      }

      /* If there's still data to read, we'll be coming back to this stream. */
      if (connection_get_inbuf_len(TO_CONN(conn)))
          ++n_streams_left;

      /* If the circuit won't accept any more data, return without looking
       * at any more of the streams. Any connections that should be stopped
       * have already been stopped by connection_edge_package_raw_inbuf. */
      if (circuit_consider_stop_edge_reading(circ, layer_hint))
        return -1;
      /* XXXX should we also stop immediately if we fill up the cell queue?
       * Probably. */
    }
  }

  /* If we made progress, and we are willing to package more, and there are
   * any streams left that want to package stuff... try again!
   */
  if (packaged_this_round && packaged_this_round < max_to_package &&
      n_streams_left) {
    n_packaging_streams = n_streams_left;
    goto again;
  }

  return 0;
}

/** Check if the package window for <b>circ</b> is empty (at
 * hop <b>layer_hint</b> if it's defined).
 *
 * If yes, tell edge streams to stop reading and return 1.
 * Else return 0.
 */
static int
circuit_consider_stop_edge_reading(circuit_t *circ, crypt_path_t *layer_hint)
{
  edge_connection_t *conn = NULL;
  unsigned domain = layer_hint ? LD_APP : LD_EXIT;

  if (!layer_hint) {
    or_circuit_t *or_circ = TO_OR_CIRCUIT(circ);
    log_debug(domain,"considering circ->package_window %d",
              circ->package_window);
    if (circuit_get_package_window(circ, layer_hint) <= 0) {
      log_debug(domain,"yes, not-at-origin. stopped.");
      for (conn = or_circ->n_streams; conn; conn=conn->next_stream)
        connection_stop_reading(TO_CONN(conn));
      return 1;
    }
    return 0;
  }
  /* else, layer hint is defined, use it */
  log_debug(domain,"considering layer_hint->package_window %d",
            layer_hint->package_window);
  if (circuit_get_package_window(circ, layer_hint) <= 0) {
    log_debug(domain,"yes, at-origin. stopped.");
    for (conn = TO_ORIGIN_CIRCUIT(circ)->p_streams; conn;
         conn=conn->next_stream) {
      if (edge_uses_cpath(conn, layer_hint))
        connection_stop_reading(TO_CONN(conn));
    }
    return 1;
  }
  return 0;
}

/** The total number of cells we have allocated. */
static size_t total_cells_allocated = 0;

/** Release storage held by <b>cell</b>. */
static inline void
packed_cell_free_unchecked(packed_cell_t *cell)
{
  --total_cells_allocated;
  tor_free(cell);
}

/** Allocate and return a new packed_cell_t. */
STATIC packed_cell_t *
packed_cell_new(void)
{
  ++total_cells_allocated;
  return tor_malloc_zero(sizeof(packed_cell_t));
}

/** Return a packed cell used outside by channel_t lower layer */
void
packed_cell_free_(packed_cell_t *cell)
{
  if (!cell)
    return;
  packed_cell_free_unchecked(cell);
}

/** Log current statistics for cell pool allocation at log level
 * <b>severity</b>. */
void
dump_cell_pool_usage(int severity)
{
  int n_circs = 0;
  int n_cells = 0;
  SMARTLIST_FOREACH_BEGIN(circuit_get_global_list(), circuit_t *, c) {
    n_cells += c->n_chan_cells.n;
    if (!CIRCUIT_IS_ORIGIN(c))
      n_cells += TO_OR_CIRCUIT(c)->p_chan_cells.n;
    ++n_circs;
  }
  SMARTLIST_FOREACH_END(c);
  tor_log(severity, LD_MM,
          "%d cells allocated on %d circuits. %d cells leaked.",
          n_cells, n_circs, (int)total_cells_allocated - n_cells);
}

/** Allocate a new copy of packed <b>cell</b>. */
static inline packed_cell_t *
packed_cell_copy(const cell_t *cell, int wide_circ_ids)
{
  packed_cell_t *c = packed_cell_new();
  cell_pack(c, cell, wide_circ_ids);
  return c;
}

/** Append <b>cell</b> to the end of <b>queue</b>. */
void
cell_queue_append(cell_queue_t *queue, packed_cell_t *cell)
{
  TOR_SIMPLEQ_INSERT_TAIL(&queue->head, cell, next);
  ++queue->n;
}

/** Append a newly allocated copy of <b>cell</b> to the end of the
 * <b>exitward</b> (or app-ward) <b>queue</b> of <b>circ</b>.  If
 * <b>use_stats</b> is true, record statistics about the cell.
 */
void
cell_queue_append_packed_copy(circuit_t *circ, cell_queue_t *queue,
                              int exitward, const cell_t *cell,
                              int wide_circ_ids, int use_stats)
{
  packed_cell_t *copy = packed_cell_copy(cell, wide_circ_ids);
  (void)circ;
  (void)exitward;
  (void)use_stats;

  copy->inserted_timestamp = monotime_coarse_get_stamp();

  cell_queue_append(queue, copy);
}

/** Initialize <b>queue</b> as an empty cell queue. */
void
cell_queue_init(cell_queue_t *queue)
{
  memset(queue, 0, sizeof(cell_queue_t));
  TOR_SIMPLEQ_INIT(&queue->head);
}

/** Remove and free every cell in <b>queue</b>. */
void
cell_queue_clear(cell_queue_t *queue)
{
  packed_cell_t *cell;
  while ((cell = TOR_SIMPLEQ_FIRST(&queue->head))) {
    TOR_SIMPLEQ_REMOVE_HEAD(&queue->head, next);
    packed_cell_free_unchecked(cell);
  }
  TOR_SIMPLEQ_INIT(&queue->head);
  queue->n = 0;
}

/** Extract and return the cell at the head of <b>queue</b>; return NULL if
 * <b>queue</b> is empty. */
STATIC packed_cell_t *
cell_queue_pop(cell_queue_t *queue)
{
  packed_cell_t *cell = TOR_SIMPLEQ_FIRST(&queue->head);
  if (!cell)
    return NULL;
  TOR_SIMPLEQ_REMOVE_HEAD(&queue->head, next);
  --queue->n;
  return cell;
}

/** Initialize <b>queue</b> as an empty cell queue. */
void
destroy_cell_queue_init(destroy_cell_queue_t *queue)
{
  memset(queue, 0, sizeof(destroy_cell_queue_t));
  TOR_SIMPLEQ_INIT(&queue->head);
}

/** Remove and free every cell in <b>queue</b>. */
void
destroy_cell_queue_clear(destroy_cell_queue_t *queue)
{
  destroy_cell_t *cell;
  while ((cell = TOR_SIMPLEQ_FIRST(&queue->head))) {
    TOR_SIMPLEQ_REMOVE_HEAD(&queue->head, next);
    tor_free(cell);
  }
  TOR_SIMPLEQ_INIT(&queue->head);
  queue->n = 0;
}

/** Extract and return the cell at the head of <b>queue</b>; return NULL if
 * <b>queue</b> is empty. */
STATIC destroy_cell_t *
destroy_cell_queue_pop(destroy_cell_queue_t *queue)
{
  destroy_cell_t *cell = TOR_SIMPLEQ_FIRST(&queue->head);
  if (!cell)
    return NULL;
  TOR_SIMPLEQ_REMOVE_HEAD(&queue->head, next);
  --queue->n;
  return cell;
}

/** Append a destroy cell for <b>circid</b> to <b>queue</b>. */
void
destroy_cell_queue_append(destroy_cell_queue_t *queue,
                          circid_t circid,
                          uint8_t reason)
{
  destroy_cell_t *cell = tor_malloc_zero(sizeof(destroy_cell_t));
  cell->circid = circid;
  cell->reason = reason;
  /* Not yet used, but will be required for OOM handling. */
  cell->inserted_timestamp = monotime_coarse_get_stamp();

  TOR_SIMPLEQ_INSERT_TAIL(&queue->head, cell, next);
  ++queue->n;
}

/** Convert a destroy_cell_t to a newly allocated cell_t. Frees its input. */
static packed_cell_t *
destroy_cell_to_packed_cell(destroy_cell_t *inp, int wide_circ_ids)
{
  packed_cell_t *packed = packed_cell_new();
  cell_t cell;
  memset(&cell, 0, sizeof(cell));
  cell.circ_id = inp->circid;
  cell.command = CELL_DESTROY;
  cell.payload[0] = inp->reason;
  cell_pack(packed, &cell, wide_circ_ids);

  tor_free(inp);
  return packed;
}

/** Return the total number of bytes used for each packed_cell in a queue.
 * Approximate. */
size_t
packed_cell_mem_cost(void)
{
  return sizeof(packed_cell_t);
}

/* DOCDOC */
size_t
cell_queues_get_total_allocation(void)
{
  return total_cells_allocated * packed_cell_mem_cost();
}

/** How long after we've been low on memory should we try to conserve it? */
#define MEMORY_PRESSURE_INTERVAL (30*60)

/** The time at which we were last low on memory. */
static time_t last_time_under_memory_pressure = 0;

/** Statistics on how many bytes were removed by the OOM per type. */
uint64_t oom_stats_n_bytes_removed_dns = 0;
uint64_t oom_stats_n_bytes_removed_cell = 0;
uint64_t oom_stats_n_bytes_removed_geoip = 0;
uint64_t oom_stats_n_bytes_removed_hsdir = 0;

/** Check whether we've got too much space used for cells.  If so,
 * call the OOM handler and return 1.  Otherwise, return 0. */
STATIC int
cell_queues_check_size(void)
{
  size_t removed = 0;
  time_t now = time(NULL);
  size_t alloc = cell_queues_get_total_allocation();
  alloc += half_streams_get_total_allocation();
  alloc += buf_get_total_allocation();
  alloc += tor_compress_get_total_allocation();
  const size_t hs_cache_total = hs_cache_get_total_allocation();
  alloc += hs_cache_total;
  const size_t geoip_client_cache_total =
    geoip_client_cache_total_allocation();
  alloc += geoip_client_cache_total;
  const size_t dns_cache_total = dns_cache_total_allocation();
  alloc += dns_cache_total;
  const size_t conflux_total = conflux_get_total_bytes_allocation();
  alloc += conflux_total;
  if (alloc >= get_options()->MaxMemInQueues_low_threshold) {
    last_time_under_memory_pressure = approx_time();
    if (alloc >= get_options()->MaxMemInQueues) {
      /* Note this overload down */
      rep_hist_note_overload(OVERLOAD_GENERAL);

      /* If we're spending over 20% of the memory limit on hidden service
       * descriptors, free them until we're down to 10%. Do the same for geoip
       * client cache. */
      if (hs_cache_total > get_options()->MaxMemInQueues / 5) {
        const size_t bytes_to_remove =
          hs_cache_total - (size_t)(get_options()->MaxMemInQueues / 10);
        removed = hs_cache_handle_oom(bytes_to_remove);
        oom_stats_n_bytes_removed_hsdir += removed;
        alloc -= removed;
      }
      if (geoip_client_cache_total > get_options()->MaxMemInQueues / 5) {
        const size_t bytes_to_remove =
          geoip_client_cache_total -
          (size_t)(get_options()->MaxMemInQueues / 10);
        removed = geoip_client_cache_handle_oom(now, bytes_to_remove);
        oom_stats_n_bytes_removed_geoip += removed;
        alloc -= removed;
      }
      if (dns_cache_total > get_options()->MaxMemInQueues / 5) {
        const size_t bytes_to_remove =
          dns_cache_total - (size_t)(get_options()->MaxMemInQueues / 10);
        removed = dns_cache_handle_oom(now, bytes_to_remove);
        oom_stats_n_bytes_removed_dns += removed;
        alloc -= removed;
      }
      /* Like onion service above, try to go down to 10% if we are above 20% */
      if (conflux_total > get_options()->MaxMemInQueues / 5) {
        const size_t bytes_to_remove =
          conflux_total - (size_t)(get_options()->MaxMemInQueues / 10);
        removed = conflux_handle_oom(bytes_to_remove);
        oom_stats_n_bytes_removed_cell += removed;
        alloc -= removed;
      }
      removed = circuits_handle_oom(alloc);
      oom_stats_n_bytes_removed_cell += removed;
      return 1;
    }
  }
  return 0;
}

/** Return true if we've been under memory pressure in the last
 * MEMORY_PRESSURE_INTERVAL seconds. */
int
have_been_under_memory_pressure(void)
{
  return last_time_under_memory_pressure + MEMORY_PRESSURE_INTERVAL
    < approx_time();
}

/**
 * Update the number of cells available on the circuit's n_chan or p_chan's
 * circuit mux.
 */
void
update_circuit_on_cmux_(circuit_t *circ, cell_direction_t direction,
                        const char *file, int lineno)
{
  channel_t *chan = NULL;
  or_circuit_t *or_circ = NULL;
  circuitmux_t *cmux = NULL;

  tor_assert(circ);

  /* Okay, get the channel */
  if (direction == CELL_DIRECTION_OUT) {
    chan = circ->n_chan;
  } else {
    or_circ = TO_OR_CIRCUIT(circ);
    chan = or_circ->p_chan;
  }

  tor_assert(chan);
  tor_assert(chan->cmux);

  /* Now get the cmux */
  cmux = chan->cmux;

  /* Cmux sanity check */
  if (! circuitmux_is_circuit_attached(cmux, circ)) {
    log_warn(LD_BUG, "called on non-attached circuit from %s:%d",
             file, lineno);
    return;
  }
  tor_assert(circuitmux_attached_circuit_direction(cmux, circ) == direction);

  /* Update the number of cells we have for the circuit mux */
  if (direction == CELL_DIRECTION_OUT) {
    circuitmux_set_num_cells(cmux, circ, circ->n_chan_cells.n);
  } else {
    circuitmux_set_num_cells(cmux, circ, or_circ->p_chan_cells.n);
  }
}

/** Remove all circuits from the cmux on <b>chan</b>.
 *
 * If <b>circuits_out</b> is non-NULL, add all detached circuits to
 * <b>circuits_out</b>.
 **/
void
channel_unlink_all_circuits(channel_t *chan, smartlist_t *circuits_out)
{
  tor_assert(chan);
  tor_assert(chan->cmux);

  circuitmux_detach_all_circuits(chan->cmux, circuits_out);
  chan->num_n_circuits = 0;
  chan->num_p_circuits = 0;
}

/**
 * Called when a circuit becomes blocked or unblocked due to the channel
 * cell queue.
 *
 * Block (if <b>block</b> is true) or unblock (if <b>block</b> is false)
 * every edge connection that is using <b>circ</b> to write to <b>chan</b>,
 * and start or stop reading as appropriate.
 */
static void
set_circuit_blocked_on_chan(circuit_t *circ, channel_t *chan, int block)
{
  edge_connection_t *edge = NULL;
  if (circ->n_chan == chan) {
    circ->circuit_blocked_on_n_chan = block;
    if (CIRCUIT_IS_ORIGIN(circ))
      edge = TO_ORIGIN_CIRCUIT(circ)->p_streams;
  } else {
    circ->circuit_blocked_on_p_chan = block;
    tor_assert(!CIRCUIT_IS_ORIGIN(circ));
    edge = TO_OR_CIRCUIT(circ)->n_streams;
  }

  set_block_state_for_streams(circ, edge, block, 0);
}

/**
 * Helper function to block or unblock streams in a stream list.
 *
 * If <b>stream_id</b> is 0, apply the <b>block</b> state to all streams
 * in the stream list. If it is non-zero, only apply to that specific stream.
 */
static void
set_block_state_for_streams(circuit_t *circ, edge_connection_t *stream_list,
                            int block, streamid_t stream_id)
{
  /* If we have a conflux object, we need to examine its status before
   * blocking and unblocking streams. */
  if (circ->conflux) {
    bool can_send = conflux_can_send(circ->conflux);

    if (block && can_send) {
      /* Don't actually block streams, since conflux can send*/
      return;
    } else if (!block && !can_send) {
      /* Don't actually unblock streams, since conflux still can't send */
      return;
    }
  }

  for (edge_connection_t *edge = stream_list; edge; edge = edge->next_stream) {
    connection_t *conn = TO_CONN(edge);
    if (stream_id && edge->stream_id != stream_id)
      continue;

    if (!conn->read_event || edge->xoff_received ||
        conn->marked_for_close) {
      /* This connection should not start or stop reading. */
      continue;
    }

    if (block) {
      if (connection_is_reading(conn))
        connection_stop_reading(conn);
    } else {
      /* Is this right? */
      if (!connection_is_reading(conn))
        connection_start_reading(conn);
    }
  }
}

/** Extract the command from a packed cell. */
uint8_t
packed_cell_get_command(const packed_cell_t *cell, int wide_circ_ids)
{
  if (wide_circ_ids) {
    return get_uint8(cell->body+4);
  } else {
    return get_uint8(cell->body+2);
  }
}

/** Extract the circuit ID from a packed cell. */
circid_t
packed_cell_get_circid(const packed_cell_t *cell, int wide_circ_ids)
{
  if (wide_circ_ids) {
    return ntohl(get_uint32(cell->body));
  } else {
    return ntohs(get_uint16(cell->body));
  }
}

/** Pull as many cells as possible (but no more than <b>max</b>) from the
 * queue of the first active circuit on <b>chan</b>, and write them to
 * <b>chan</b>-&gt;outbuf.  Return the number of cells written.  Advance
 * the active circuit pointer to the next active circuit in the ring. */
MOCK_IMPL(int,
channel_flush_from_first_active_circuit, (channel_t *chan, int max))
{
  circuitmux_t *cmux = NULL;
  int n_flushed = 0;
  cell_queue_t *queue;
  destroy_cell_queue_t *destroy_queue=NULL;
  circuit_t *circ;
  or_circuit_t *or_circ;
  int circ_blocked;
  packed_cell_t *cell;

  /* Get the cmux */
  tor_assert(chan);
  tor_assert(chan->cmux);
  cmux = chan->cmux;

  /* Main loop: pick a circuit, send a cell, update the cmux */
  while (n_flushed < max) {
    circ = circuitmux_get_first_active_circuit(cmux, &destroy_queue);
    if (destroy_queue) {
      destroy_cell_t *dcell;
      /* this code is duplicated from some of the logic below. Ugly! XXXX */
      /* If we are given a destroy_queue here, then it is required to be
       * nonempty... */
      tor_assert(destroy_queue->n > 0);
      dcell = destroy_cell_queue_pop(destroy_queue);
      /* ...and pop() will always yield a cell from a nonempty queue. */
      tor_assert(dcell);
      /* frees dcell */
      cell = destroy_cell_to_packed_cell(dcell, chan->wide_circ_ids);
      /* Send the DESTROY cell. It is very unlikely that this fails but just
       * in case, get rid of the channel. */
      if (channel_write_packed_cell(chan, cell) < 0) {
        /* The cell has been freed. */
        channel_mark_for_close(chan);
        continue;
      }
      /* Update the cmux destroy counter */
      circuitmux_notify_xmit_destroy(cmux);
      cell = NULL;
      ++n_flushed;
      continue;
    }
    /* If it returns NULL, no cells left to send */
    if (!circ) break;

    if (circ->n_chan == chan) {
      queue = &circ->n_chan_cells;
      circ_blocked = circ->circuit_blocked_on_n_chan;
    } else {
      or_circ = TO_OR_CIRCUIT(circ);
      tor_assert(or_circ->p_chan == chan);
      queue = &TO_OR_CIRCUIT(circ)->p_chan_cells;
      circ_blocked = circ->circuit_blocked_on_p_chan;
    }

    /* Circuitmux told us this was active, so it should have cells.
     *
     * Note: In terms of logic and coherence, this should never happen but the
     * cmux dragon is powerful. Reason is that when the OOM is triggered, when
     * cleaning up circuits, we mark them for close and then clear their cell
     * queues. And so, we can have a circuit considered active by the cmux
     * dragon but without cells. The cmux subsystem is only notified of this
     * when the circuit is freed which leaves a tiny window between close and
     * free to end up here.
     *
     * We are accepting this as an "ok" race else the changes are likely non
     * trivial to make the mark for close to set the num cells to 0 and change
     * the free functions to detach the circuit conditionally without creating
     * a chain effect of madness.
     *
     * The lesson here is arti will prevail and leave the cmux dragon alone. */
    if (queue->n == 0) {
      circuitmux_set_num_cells(cmux, circ, 0);
      if (! circ->marked_for_close)
        circuit_mark_for_close(circ, END_CIRC_REASON_INTERNAL);
      continue;
    }

    tor_assert(queue->n > 0);

    /*
     * Get just one cell here; once we've sent it, that can change the circuit
     * selection, so we have to loop around for another even if this circuit
     * has more than one.
     */
    cell = cell_queue_pop(queue);

    /* Calculate the exact time that this cell has spent in the queue. */
    if (get_options()->CellStatistics ||
        get_options()->TestingEnableCellStatsEvent) {
      uint32_t timestamp_now = monotime_coarse_get_stamp();
      uint32_t msec_waiting =
        (uint32_t) monotime_coarse_stamp_units_to_approx_msec(
                         timestamp_now - cell->inserted_timestamp);

      if (get_options()->CellStatistics && !CIRCUIT_IS_ORIGIN(circ)) {
        or_circ = TO_OR_CIRCUIT(circ);
        or_circ->total_cell_waiting_time += msec_waiting;
        or_circ->processed_cells++;
      }

      if (get_options()->TestingEnableCellStatsEvent) {
        uint8_t command = packed_cell_get_command(cell, chan->wide_circ_ids);

        testing_cell_stats_entry_t *ent =
          tor_malloc_zero(sizeof(testing_cell_stats_entry_t));
        ent->command = command;
        ent->waiting_time = msec_waiting / 10;
        ent->removed = 1;
        if (circ->n_chan == chan)
          ent->exitward = 1;
        if (!circ->testing_cell_stats)
          circ->testing_cell_stats = smartlist_new();
        smartlist_add(circ->testing_cell_stats, ent);
      }
    }

    /* If we just flushed our queue and this circuit is used for a
     * tunneled directory request, possibly advance its state. */
    if (queue->n == 0 && chan->dirreq_id)
      geoip_change_dirreq_state(chan->dirreq_id,
                                DIRREQ_TUNNELED,
                                DIRREQ_CIRC_QUEUE_FLUSHED);

    /* Now send the cell. It is very unlikely that this fails but just in
     * case, get rid of the channel. */
    if (channel_write_packed_cell(chan, cell) < 0) {
      /* The cell has been freed at this point. */
      channel_mark_for_close(chan);
      continue;
    }
    cell = NULL;

    /*
     * Don't packed_cell_free_unchecked(cell) here because the channel will
     * do so when it gets out of the channel queue (probably already did, in
     * which case that was an immediate double-free bug).
     */

    /* Update the counter */
    ++n_flushed;

    /*
     * Now update the cmux; tell it we've just sent a cell, and how many
     * we have left.
     */
    circuitmux_notify_xmit_cells(cmux, circ, 1);
    circuitmux_set_num_cells(cmux, circ, queue->n);
    if (queue->n == 0)
      log_debug(LD_GENERAL, "Made a circuit inactive.");

    /* Is the cell queue low enough to unblock all the streams that are waiting
     * to write to this circuit? */
    if (circ_blocked && queue->n <= cell_queue_lowwatermark())
      set_circuit_blocked_on_chan(circ, chan, 0); /* unblock streams */

    /* If n_flushed < max still, loop around and pick another circuit */
  }

  /* Okay, we're done sending now */
  return n_flushed;
}

/* Minimum value is the maximum circuit window size.
 *
 * This value is set to a lower bound we believe is reasonable with congestion
 * control and basic network running parameters.
 *
 * SENDME cells makes it that we can control how many cells can be inflight on
 * a circuit from end to end. This logic makes it that on any circuit cell
 * queue, we have a maximum of cells possible.
 *
 * Because the Tor protocol allows for a client to exit at any hop in a
 * circuit and a circuit can be of a maximum of 8 hops, so in theory the
 * normal worst case will be the circuit window start value times the maximum
 * number of hops (8). Having more cells then that means something is wrong.
 *
 * However, because padding cells aren't counted in the package window, we set
 * the maximum size to a reasonably large size for which we expect that we'll
 * never reach in theory. And if we ever do because of future changes, we'll
 * be able to control it with a consensus parameter.
 *
 * XXX: Unfortunately, END cells aren't accounted for in the circuit window
 * which means that for instance if a client opens 8001 streams, the 8001
 * following END cells will queue up in the circuit which will get closed if
 * the max limit is 8000. Which is sad because it is allowed by the Tor
 * protocol. But, we need an upper bound on circuit queue in order to avoid
 * DoS memory pressure so the default size is a middle ground between not
 * having any limit and having a very restricted one. This is why we can also
 * control it through a consensus parameter. */
#define RELAY_CIRC_CELL_QUEUE_SIZE_MIN 50
/* We can't have a consensus parameter above this value. */
#define RELAY_CIRC_CELL_QUEUE_SIZE_MAX INT32_MAX
/* Default value is set to a large value so we can handle padding cells
 * properly which aren't accounted for in the SENDME window. Default is 2500
 * allowed cells in the queue resulting in ~1MB. */
#define RELAY_CIRC_CELL_QUEUE_SIZE_DEFAULT \
  (50 * RELAY_CIRC_CELL_QUEUE_SIZE_MIN)

/* The maximum number of cells a circuit queue can contain. This is updated at
 * every new consensus and controlled by a parameter. */
static int32_t max_circuit_cell_queue_size =
  RELAY_CIRC_CELL_QUEUE_SIZE_DEFAULT;
/** Maximum number of cell on an outbound circuit queue. This is updated at
 * every new consensus and controlled by a parameter. This default is incorrect
 * and won't be used at all except in unit tests. */
static int32_t max_circuit_cell_queue_size_out =
  RELAY_CIRC_CELL_QUEUE_SIZE_DEFAULT;

/** Return consensus parameter "circ_max_cell_queue_size". The given ns can be
 * NULL. */
static uint32_t
get_param_max_circuit_cell_queue_size(const networkstatus_t *ns)
{
  return networkstatus_get_param(ns, "circ_max_cell_queue_size",
                                 RELAY_CIRC_CELL_QUEUE_SIZE_DEFAULT,
                                 RELAY_CIRC_CELL_QUEUE_SIZE_MIN,
                                 RELAY_CIRC_CELL_QUEUE_SIZE_MAX);
}

/** Return consensus parameter "circ_max_cell_queue_size_out". The given ns can
 * be NULL. */
static uint32_t
get_param_max_circuit_cell_queue_size_out(const networkstatus_t *ns)
{
  return networkstatus_get_param(ns, "circ_max_cell_queue_size_out",
                                 get_param_max_circuit_cell_queue_size(ns),
                                 RELAY_CIRC_CELL_QUEUE_SIZE_MIN,
                                 RELAY_CIRC_CELL_QUEUE_SIZE_MAX);
}

/* Called when the consensus has changed. At this stage, the global consensus
 * object has NOT been updated. It is called from
 * notify_before_networkstatus_changes(). */
void
relay_consensus_has_changed(const networkstatus_t *ns)
{
  tor_assert(ns);

  /* Update the circuit max cell queue size from the consensus. */
  max_circuit_cell_queue_size =
    get_param_max_circuit_cell_queue_size(ns);
  max_circuit_cell_queue_size_out =
    get_param_max_circuit_cell_queue_size_out(ns);
}

/** Add <b>cell</b> to the queue of <b>circ</b> writing to <b>chan</b>
 * transmitting in <b>direction</b>.
 *
 * The given <b>cell</b> is copied onto the circuit queue so the caller must
 * cleanup the memory.
 *
 * This function is part of the fast path.
 *
 * Return 1 if the cell was successfully sent.
 * Return 0 if the cell can not be sent. The caller MUST NOT close the circuit.
 * Return -1 indicating an error and that the caller should mark the circuit
 * for close. */
int
append_cell_to_circuit_queue(circuit_t *circ, channel_t *chan,
                             cell_t *cell, cell_direction_t direction,
                             streamid_t fromstream)
{
  or_circuit_t *orcirc = NULL;
  edge_connection_t *stream_list = NULL;
  cell_queue_t *queue;
  int32_t max_queue_size;
  int circ_blocked;
  int exitward;
  if (circ->marked_for_close) {
    return 0;
  }

  exitward = (direction == CELL_DIRECTION_OUT);
  if (exitward) {
    queue = &circ->n_chan_cells;
    circ_blocked = circ->circuit_blocked_on_n_chan;
    max_queue_size = max_circuit_cell_queue_size_out;
    if (CIRCUIT_IS_ORIGIN(circ))
      stream_list = TO_ORIGIN_CIRCUIT(circ)->p_streams;
  } else {
    orcirc = TO_OR_CIRCUIT(circ);
    queue = &orcirc->p_chan_cells;
    circ_blocked = circ->circuit_blocked_on_p_chan;
    max_queue_size = max_circuit_cell_queue_size;
    stream_list = TO_OR_CIRCUIT(circ)->n_streams;
  }

  if (PREDICT_UNLIKELY(queue->n >= max_queue_size)) {
    /* This DoS defense only applies at the Guard as in the p_chan is likely
     * a client IP attacking the network. */
    if (exitward && CIRCUIT_IS_ORCIRC(circ)) {
      stats_n_circ_max_cell_outq_reached++;
      dos_note_circ_max_outq(CONST_TO_OR_CIRCUIT(circ)->p_chan);
    }

    log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
           "%s circuit has %d cells in its queue, maximum allowed is %d. "
           "Closing circuit for safety reasons.",
           (exitward) ? "Outbound" : "Inbound", queue->n,
           max_queue_size);
    stats_n_circ_max_cell_reached++;
    return -1;
  }

  /* Very important that we copy to the circuit queue because all calls to
   * this function use the stack for the cell memory. */
  cell_queue_append_packed_copy(circ, queue, exitward, cell,
                                chan->wide_circ_ids, 1);

  /* Check and run the OOM if needed. */
  if (PREDICT_UNLIKELY(cell_queues_check_size())) {
    /* We ran the OOM handler which might have closed this circuit. */
    if (circ->marked_for_close) {
      return 0;
    }
  }

  /* If we have too many cells on the circuit, note that it should
   * be blocked from new cells. */
  if (!circ_blocked && queue->n >= cell_queue_highwatermark())
    set_circuit_blocked_on_chan(circ, chan, 1);

  if (circ_blocked && fromstream) {
    /* This edge connection is apparently not blocked; this can happen for
     * new streams on a blocked circuit, for their CONNECTED response.
     * block it now, unless we have conflux. */
    set_block_state_for_streams(circ, stream_list, 1, fromstream);
  }

  update_circuit_on_cmux(circ, direction);
  if (queue->n == 1) {
    /* This was the first cell added to the queue.  We just made this
     * circuit active. */
    log_debug(LD_GENERAL, "Made a circuit active.");
  }

  /* New way: mark this as having waiting cells for the scheduler */
  scheduler_channel_has_waiting_cells(chan);
  return 1;
}

/** Append an encoded value of <b>addr</b> to <b>payload_out</b>, which must
 * have at least 18 bytes of free space.  The encoding is, as specified in
 * tor-spec.txt:
 *   RESOLVED_TYPE_IPV4 or RESOLVED_TYPE_IPV6  [1 byte]
 *   LENGTH                                    [1 byte]
 *   ADDRESS                                   [length bytes]
 * Return the number of bytes added, or -1 on error */
int
append_address_to_payload(uint8_t *payload_out, const tor_addr_t *addr)
{
  uint32_t a;
  switch (tor_addr_family(addr)) {
  case AF_INET:
    payload_out[0] = RESOLVED_TYPE_IPV4;
    payload_out[1] = 4;
    a = tor_addr_to_ipv4n(addr);
    memcpy(payload_out+2, &a, 4);
    return 6;
  case AF_INET6:
    payload_out[0] = RESOLVED_TYPE_IPV6;
    payload_out[1] = 16;
    memcpy(payload_out+2, tor_addr_to_in6_addr8(addr), 16);
    return 18;
  case AF_UNSPEC:
  default:
    return -1;
  }
}

/** Given <b>payload_len</b> bytes at <b>payload</b>, starting with an address
 * encoded as by append_address_to_payload(), try to decode the address into
 * *<b>addr_out</b>.  Return the next byte in the payload after the address on
 * success, or NULL on failure. */
const uint8_t *
decode_address_from_payload(tor_addr_t *addr_out, const uint8_t *payload,
                            int payload_len)
{
  if (payload_len < 2)
    return NULL;
  if (payload_len < 2+payload[1])
    return NULL;

  switch (payload[0]) {
  case RESOLVED_TYPE_IPV4:
    if (payload[1] != 4)
      return NULL;
    tor_addr_from_ipv4n(addr_out, get_uint32(payload+2));
    break;
  case RESOLVED_TYPE_IPV6:
    if (payload[1] != 16)
      return NULL;
    tor_addr_from_ipv6_bytes(addr_out, (payload+2));
    break;
  default:
    tor_addr_make_unspec(addr_out);
    break;
  }
  return payload + 2 + payload[1];
}

/** Remove all the cells queued on <b>circ</b> for <b>chan</b>. */
void
circuit_clear_cell_queue(circuit_t *circ, channel_t *chan)
{
  cell_queue_t *queue;
  cell_direction_t direction;

  if (circ->n_chan == chan) {
    queue = &circ->n_chan_cells;
    direction = CELL_DIRECTION_OUT;
  } else {
    or_circuit_t *orcirc = TO_OR_CIRCUIT(circ);
    tor_assert(orcirc->p_chan == chan);
    queue = &orcirc->p_chan_cells;
    direction = CELL_DIRECTION_IN;
  }

  /* Clear the queue */
  cell_queue_clear(queue);

  /* Update the cell counter in the cmux */
  if (chan->cmux && circuitmux_is_circuit_attached(chan->cmux, circ))
    update_circuit_on_cmux(circ, direction);
}

/** Return 1 if we shouldn't restart reading on this circuit, even if
 * we get a SENDME.  Else return 0.
*/
static int
circuit_queue_streams_are_blocked(circuit_t *circ)
{
  if (CIRCUIT_IS_ORIGIN(circ)) {
    return circ->circuit_blocked_on_n_chan;
  } else {
    return circ->circuit_blocked_on_p_chan;
  }
}

/** Return the format to use.
 *
 * NULL can be passed but not for both. */
relay_cell_fmt_t
circuit_get_relay_format(const circuit_t *circ, const crypt_path_t *cpath)
{
  if (circ && CIRCUIT_IS_ORCIRC(circ)) {
    return CONST_TO_OR_CIRCUIT(circ)->relay_cell_format;
  } else if (cpath) {
    return cpath->relay_cell_format;
  } else {
    /* We end up here when both params are NULL, which is not allowed, or when
     * only an origin circuit is given (which again is not allowed). */
    tor_assert_unreached();
  }
}

/**
 * Return the maximum relay payload that can be sent to the chosen
 * point, with the specified command.
 */
size_t
circuit_max_relay_payload(const circuit_t *circ, const crypt_path_t *cpath,
                              uint8_t relay_command)
{
  relay_cell_fmt_t fmt = circuit_get_relay_format(circ, cpath);
  return relay_cell_max_payload_size(fmt, relay_command);
}
