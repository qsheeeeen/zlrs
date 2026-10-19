/*
 * Link layer: one instance per link, running the state machine and superframe slots (doc/07-protocol.md).
 *
 * Each superframe: zlrs_link_slot() hands out a transaction, the caller runs it and
 * zlrs_link_complete() closes it; zlrs_link_tick() handles timeouts and state changes. No Zephyr.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdalign.h>
#include <stdint.h>

#include "zlrs/link/radio.h"
#include "zlrs/proto/limits.h"
#include "zlrs/proto/crypto.h"
#include "zlrs/proto/fhss.h"
#include "zlrs/proto/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pairing handshake (doc/07-protocol.md). The two ends share no slot phase while pairing, so
 * a message is heard or missed by luck: every step repeats until the next one shows up. The
 * initiator sends this many PAIR_CFM before it calls the handshake done.
 */
#define ZLRS_PAIR_CFM_REPEATS    16U
/* Frames between two listen windows on the pairing channel while SEARCHING (see the DATA slot) */
#define ZLRS_PAIR_SCAN_FRAMES    4U
/*
 * Session confirm window. Same story as pairing: the two ends only share a slot phase once the
 * session is up. For this long after entering CONNECTED the initiator keeps listening on the sync
 * channel, where a responder that missed the confirm is still repeating its answer, and answers
 * each repeat with another confirm.
 */
#define ZLRS_SESS_CONFIRM_MS     500U
#define ZLRS_DATA_CONFIRM_FRAMES 16U

/* Superframe slots, see doc/07-protocol.md */
#define ZLRS_SLOT_DATA_A 0U /* first half of the data window: Leader -> Follower */
#define ZLRS_SLOT_DATA_B 1U /* second half of the data window: Follower -> Leader */

#define ZLRS_SLOT_COUNT 2U

#include "zlrs/link/state.h"

/* Completion status: the port layer folds the radio result into these two */
#define ZLRS_RADIO_OK   0
#define ZLRS_RADIO_FAIL 1

typedef struct {
	zlrs_role_t role;
	zlrs_fhss_domain_id_t domain;
	zlrs_profile_t profile;
	zlrs_mod_cfg_t mod;
	int8_t tx_power_dbm;
	int8_t pair_tx_power_dbm; /* Dedicated setpoint for every pairing transmission. */
	uint16_t rx_timeout_ms;   /* one receive window */
	/*
	 * Receive window while pairing or searching (0 = rx_timeout_ms). Acquisition happens before the
	 * two ends share a slot phase, so it needs a longer window than steady state: a short window
	 * catches the peer only by luck, and a handshake that depends on luck is what keeps two boards
	 * that are otherwise fine from ever pairing.
	 */
	uint16_t acq_timeout_ms;
	uint16_t sync_period_ms;  /* SYNC period while SEARCHING */
	uint16_t peer_timeout_ms; /* peer-loss timeout */
	uint16_t pair_period_ms;  /* PAIR_REQ repeat period */
	uint16_t pair_window_ms;  /* pairing window */
	uint16_t data_mtu;        /* data frame payload cap */
	uint8_t arq;              /* 1 = passthrough uses the ARQ profile, 0 = best-effort */
	/* ARQ retransmit timeout (ms). 0 = ZLRS_ARQ_TIMEOUT_MS, right for sub-ms FLRC frames */
	uint16_t arq_timeout_ms;
	uint16_t feedback_period_ms; /* empty DATA heartbeat while idle; 0 disables it */
	/*
	 * Per-frame hopping. 0 = stay on the sync channel (entry 0 of the hop table), which is how
	 * layer isolation looks at timing and the hop table separately, see doc/10-debug.md.
	 */
	uint8_t hopping;
	/*
	 * Periodic SYNC interval while connected, in frames. 0 = no periodic resync (the hop index
	 * is only aligned when the link comes up); >0 = a SYNC every this many frames lets the peer
	 * correct drift and notice a peer that changed sessions (reboot/re-pair) and re-handshake.
	 */
	uint16_t resync_every_n;
	bool automatic_band;
} zlrs_link_cfg_t;

typedef struct {
	/* True only after the entire packet has been accepted; false keeps its ACK unchanged. */
	bool (*on_data)(void *ctx, const uint8_t *data, uint16_t len);
	void (*on_state)(void *ctx, zlrs_link_state_t state);
	void *ctx;
} zlrs_link_events_t;

typedef struct {
	uint32_t tx_frames;
	uint32_t tx_bytes;
	uint32_t tx_data_frames;
	uint32_t tx_data_bytes; /* User bytes on air, including retransmissions. */
	uint32_t tx_full_data_frames;
	uint32_t tx_ack_only_frames;
	/* ACK coverage, staging delay and stop-and-wait pressure. */
	uint32_t tx_data_ack_frames;
	uint32_t tx_staged_stale_ack_frames;
	uint32_t tx_arq_wait_stalls;
	uint32_t tx_retry_age_ms_last;
	uint32_t tx_retry_age_ms_max;
	uint32_t tx_ack_delay_ms_last;
	uint32_t tx_ack_delay_ms_max;
	uint32_t tx_ack_delay_ms_sum;
	uint32_t tx_ack_entries;
	uint32_t rx_frames;
	uint32_t rx_bytes;
	/* Transactions per DATA direction (ZLRS_SLOT_*): sent, listened, heard a frame, failed */
	uint32_t slot_tx[ZLRS_SLOT_COUNT];
	uint32_t slot_rx[ZLRS_SLOT_COUNT];
	uint32_t slot_hit[ZLRS_SLOT_COUNT];
	uint32_t slot_fail[ZLRS_SLOT_COUNT];
	uint32_t rx_bad_tag;
	uint32_t rx_bad_frame;
	uint32_t rx_dup;
	uint32_t rx_out_of_order;   /* ARQ: drops caused by sequence-number jumps */
	uint32_t tx_retries;        /* ARQ: retransmissions */
	uint32_t tx_arq_retry_runs; /* ARQ: long retry runs (bytes are kept) */
	uint32_t rx_syncs;          /* SYNC frames received */
	uint32_t tx_syncs;          /* SYNC frames sent */
	uint32_t sess_resp_rx;      /* Leader: peer answered a SYNC */
	uint32_t sess_resp_tx;      /* Follower: answered a SYNC */
	uint32_t sess_cfm_rx;       /* Follower: session confirmed */
	uint32_t sess_cfm_tx;       /* Leader: confirmations sent */
	uint32_t hop_resync;        /* hop-index corrections received through SYNC */
	uint32_t hop_advances;      /* times this link moved its hop index by itself */
	/* Pairing handshake frames sent and received, per message type: this handshake loses frames */
	uint32_t pair_req_tx;
	uint32_t pair_resp_tx;
	uint32_t pair_cfm_tx;
	uint32_t pair_req_rx;
	uint32_t pair_resp_rx;
	uint32_t pair_cfm_rx;
	uint32_t pair_rejects; /* pairing frames that arrived with a foreign id or nonce */
	uint32_t radio_fail;
	int16_t last_rssi;
	int16_t rssi_min; /* weakest and strongest frame heard: the bench looks at the spread */
	int16_t rssi_max;
	int8_t last_snr; /* the radio stack cannot provide it under FLRC, see doc/04-radio.md */
	/* Uplink quality: frames really received in the last window vs. what the sequence predicts */
	uint8_t lq_percent;
	uint16_t lq_rx;
	uint16_t lq_expected;
	/* Passthrough backpressure: bytes dropped because the air-side queue was full */
	uint32_t data_drop_bytes;
} zlrs_link_stats_t;

/*
 * Bytes the link will hold for the air. ARQ keeps a frame in this queue until the peer
 * acknowledges it, so the queue has to cover that packet plus whatever the application
 * hands over in the meantime; at 512 a single 512-byte write filled it and everything after that
 * was dropped before it ever reached the air.
 */
#define ZLRS_LINK_TX_QUEUE_LEN 4096U

/* Stop-and-wait ARQ: one outstanding DATA packet in each direction. */
#define ZLRS_ARQ_TIMEOUT_MS  30U
/* Count long retry runs for diagnostics; continue retransmitting without resetting progress. */
#define ZLRS_ARQ_MAX_RETRIES 64U

/* A DATA packet encoded during RX. Sender state commits only when its TX slot runs. */
typedef struct {
	uint8_t buf[ZLRS_FRAME_MAX_LEN];
	uint8_t key[ZLRS_KEY_LEN];
	uint32_t frame_index;
	uint16_t size;
	uint32_t frame_seq;
	uint8_t data_seq;
	uint16_t data_len;
	uint16_t data_mtu;
	zlrs_profile_t profile;
	bool ack_bit;
	bool retry;
	bool valid;
} zlrs_prepared_data_t;

typedef struct {
	zlrs_link_cfg_t cfg;
	bool (*random)(void *ctx, uint32_t *out);
	void *random_ctx;
	zlrs_link_events_t ev;
	zlrs_link_stats_t stats;

	/* configuration and keys */
	bool configured;
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t root_key[ZLRS_KEY_LEN];
	uint8_t auth_key[ZLRS_KEY_LEN];
	bool paired;
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	/*
	 * The two nonces exchanged while pairing (initiator first) feed the pair_key derivation and
	 * are stored with the pairing record: every pairing replaces them, so old captures can no
	 * longer reproduce a session. See doc/08-security.md#cryptographic-parameters.
	 */
	uint8_t pair_salt[ZLRS_PAIR_SALT_LEN];
	uint8_t pair_key[ZLRS_KEY_LEN];
	/*
	 * Per-frame tag keys are direction-split (sender -> receiver) with both dev_ids as context:
	 * out tags our frames (local id first), in verifies the peer's (peer id first).
	 * Each end derives both with no negotiation and no roles. One shared key would let our own
	 * frames pass verification when reflected, see doc/08-security.md#cryptographic-parameters.
	 */
	uint8_t link_auth_out[ZLRS_KEY_LEN];
	uint8_t link_auth_in[ZLRS_KEY_LEN];
	uint8_t sync_word[ZLRS_SYNC_WORD_LEN];
	uint8_t session_key_out[ZLRS_KEY_LEN];
	uint8_t session_key_in[ZLRS_KEY_LEN];
	bool session_valid;
	/*
	 * Session setup mirrors pairing (doc/07-protocol.md): the initiator offers nonce_leader, the
	 * responder answers with nonce_follower, both feed the KDF, and the last message carries nonce_follower
	 * to prove the peer derived the same key. Follower only adopts the session after that proof,
	 * so replaying an old SYNC yields no old session key.
	 */
	bool session_pending;         /* Leader waits for response; Follower waits for confirm */
	uint32_t sess_rx_seq;         /* Candidate handshake's receive baseline. */
	bool sess_nonce_valid;        /* A local nonce has been drawn before, including zero. */
	bool sess_resp_due;           /* Follower: SESS_RESP due in slot B */
	bool sess_cfm_due;            /* Leader: SESS_CFM due in slot A */
	bool sess_ack_due;            /* Received DATA: session-key authenticated empty ACK due */
	uint32_t sess_sync_frame;     /* Last DATA confirm's shared frame index. */
	uint32_t sess_nonce_leader;   /* Leader nonce */
	uint32_t sess_nonce_follower; /* Follower nonce */
	uint16_t sess_hop;            /* hop index of this session */
	uint32_t pair_nonce;          /* pairing: initiator nonce_a */
	uint32_t pair_nonce_b;        /* pairing: responder nonce_b */

	/* state and timing */
	zlrs_link_state_t state;
	uint32_t state_ms;
	uint32_t last_rx_ms;
	uint32_t accepted_rx_events;
	uint32_t last_rx_frame; /* Authenticated DATA hop epoch, for sparse-SYNC reacquisition. */
	uint32_t last_sync_ms;
	uint32_t last_pair_tx_ms;
	uint32_t
		last_pair_rx_ms; /* last PAIR_RESP: tells a repeated answer from a silent responder */
	bool pair_cfm_pending;   /* Leader got PAIR_RESP: PAIR_CFM due in slot A */
	uint8_t pair_cfm_sent;   /* PAIR_CFM of this handshake already sent */
	bool pair_wait_cfm;      /* Follower answered PAIR_RESP, waiting for PAIR_CFM */
	bool pair_request_valid; /* A responder request exists, including nonce_a == 0. */
	bool pair_nonce_valid;   /* A local pairing nonce exists independently of pending state. */
	uint32_t pair_rx_seq;    /* Pairing exchange's receive baseline. */
	uint32_t pair_tx_seq;    /* Pairing attempts, including delayed confirms after pairing. */

	/* sequence numbers and hopping */
	uint32_t tx_seq; /* Next attempt, starting at one; never wraps. */
	uint32_t last_rx_seq;
	uint32_t lq_last_seq;
	bool lq_started;
	uint32_t frame_index; /* DATA_A slots run */
	zlrs_fhss_t fhss;
	zlrs_band_plan_t band_plan;
	bool band_announced;
	bool radio_acquiring;
	bool radio_sync_seen;

	/* passthrough queue */
	uint8_t tx_queue[ZLRS_LINK_TX_QUEUE_LEN];
	uint16_t queue_head;
	uint16_t queue_count;
	uint16_t pending_data_len;

	/* ARQ sender: one packet retained until the peer ACK bit changes. */
	uint8_t arq_seq;
	uint16_t arq_len;
	uint32_t arq_first_sent_ms;
	/* Retransmissions preserve the data sequence and payload. */
	uint8_t arq_used;
	uint8_t arq_retries;
	uint32_t arq_sent_ms;
	uint32_t last_feedback_tx_ms;
	int16_t peer_rssi_dbm;
	uint8_t peer_lq_percent;
	uint32_t last_peer_feedback_ms;
	/* ARQ receiver: last accepted data sequence determines the ACK toggle. */
	uint8_t arq_rx_seq;
	bool arq_rx_started;
	bool arq_ack_pending;

	/* RX/TX buffers */
	/* Word alignment keeps the hardware SHA backend on its word-feed path. */
	alignas(uint32_t) uint8_t tx_buf[ZLRS_FRAME_MAX_LEN];
	alignas(uint32_t) uint8_t rx_buf[ZLRS_FRAME_MAX_LEN];
	zlrs_prepared_data_t prepared_data;

	/* transaction to submit next */
	zlrs_radio_req_t req;
	bool req_is_tx;
	bool req_pending;
	uint8_t slot; /* ZLRS_SLOT_*: the slot the pending transaction serves (stats and logs) */
	uint32_t slot_ms; /* Current slot time, used for the session-confirm slot B window. */
} zlrs_link_t;

/* Storage layout above allows static allocation. Consumers use the functions below;
 * mutable protocol state belongs to core/link, not application service modules. */
typedef struct {
	zlrs_link_state_t state;
	bool configured;
	bool paired;
	bool session_valid;
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	int16_t peer_rssi_dbm;
	uint8_t peer_lq_percent;
	uint32_t last_peer_feedback_ms;
	uint32_t last_rx_ms;
	uint32_t accepted_rx_events;
	uint32_t frame_index;
	uint32_t frequency_hz;
	uint16_t hop_index;
	zlrs_band_plan_t band_plan;
	bool radio_acquiring;
	bool radio_sync_seen;
} zlrs_link_info_t;

/* Diagnostic values, without borrowed protocol buffers or key material. */
typedef struct {
	uint32_t frequency_hz;
	uint32_t frame_index;
	uint32_t last_rx_frame;
	uint32_t sess_nonce_leader;
	uint32_t sess_nonce_follower;
	bool sess_cfm_due;
	uint16_t hop_index;
	uint16_t queue_count;
	uint16_t arq_used;
	uint16_t arq_seq;
	uint16_t arq_rx_seq;
	bool arq_ack_pending;
	bool pair_wait_cfm;
	bool pair_cfm_pending;
	uint8_t pair_cfm_sent;
	uint32_t pair_nonce;
	uint32_t pair_nonce_b;
	uint32_t root_fingerprint;
	uint32_t pair_fingerprint;
	uint32_t sync_fingerprint;
} zlrs_link_diagnostics_t;

/* Read-only views are valid on the link owner thread; do not retain across async work. */
const zlrs_link_cfg_t *zlrs_link_get_config(const zlrs_link_t *link);
const zlrs_link_stats_t *zlrs_link_get_stats(const zlrs_link_t *link);
zlrs_link_state_t zlrs_link_get_state(const zlrs_link_t *link);
bool zlrs_link_is_busy(const zlrs_link_t *link);
zlrs_link_info_t zlrs_link_get_info(const zlrs_link_t *link);
zlrs_link_diagnostics_t zlrs_link_get_diagnostics(const zlrs_link_t *link);
bool zlrs_link_get_pairing(const zlrs_link_t *link, uint8_t peer_id[ZLRS_DEV_ID_LEN],
			   uint8_t pair_salt[ZLRS_PAIR_SALT_LEN]);

/* Only nonce generation is injected; the caller executes each returned radio request. */
void zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
		    bool (*random)(void *ctx, uint32_t *out), void *random_ctx,
		    const zlrs_link_events_t *events);

/* The link only joins the air once a root key has been written */
void zlrs_link_set_root_key(zlrs_link_t *link, const uint8_t root_key[ZLRS_KEY_LEN],
			    uint32_t now_ms);
void zlrs_link_set_dev_id(zlrs_link_t *link, const uint8_t dev_id[ZLRS_DEV_ID_LEN]);

/*
 * Room left in the air-side queue. Callers that own a byte source (a UART, a CDC) should take no
 * more than this: zlrs_link_write() drops the tail it cannot take, and dropped input is gone.
 */
uint16_t zlrs_link_tx_space(const zlrs_link_t *link);

/* The exact bytes the pairing KDF is fed with: both ids (sorted) then the salt (sorted) */
#define ZLRS_PAIR_CTX_LEN (2U * ZLRS_DEV_ID_LEN + ZLRS_PAIR_SALT_LEN)
void zlrs_link_pair_ctx(const zlrs_link_t *link, uint8_t out[ZLRS_PAIR_CTX_LEN]);

/* Change TX power at runtime (dynamic power); effective with the next transaction */
void zlrs_link_set_power(zlrs_link_t *link, int8_t tx_power_dbm);

/* Safe between radio transactions. Retune without resetting session/ARQ/queues. */
bool zlrs_link_reconfigure_radio(zlrs_link_t *link, const zlrs_link_cfg_t *cfg);
bool zlrs_link_plan_band(zlrs_link_t *link, bool target_subg, uint32_t switch_frame);
bool zlrs_link_plan_radio(zlrs_link_t *link, bool target_subg, zlrs_phy_t target_phy,
			  uint32_t switch_frame);
void zlrs_link_acquire_radio(zlrs_link_t *link, uint32_t now_ms);
/* RF verifies physical phase after a valid SYNC/confirm, before reverse DATA proof. */
bool zlrs_link_radio_captured(zlrs_link_t *link);

/*
 * Restore pairing from the stored record (no re-pairing after a reboot). pair_salt has to
 * match the peer's copy: the two exchanged nonces. Keys are recomputed, never stored.
 */
void zlrs_link_set_peer(zlrs_link_t *link, const uint8_t peer_id[ZLRS_DEV_ID_LEN],
			const uint8_t pair_salt[ZLRS_PAIR_SALT_LEN], uint32_t now_ms);

/* Button: a long press starts pairing (re-pairing takes this path too, the new record wins) */
void zlrs_link_press(zlrs_link_t *link, uint32_t now_ms);

/* Superframe slot: return the transaction to submit, or NULL when there is none */
const zlrs_radio_req_t *zlrs_link_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms);

/* Encode new ARQ DATA for the next assigned TX slot without advancing sender/hop state.
 * Safe while RX is pending. Returned bytes stay immutable through the resulting TX request;
 * NULL means that handshake, recovery or backpressure requires the normal path. */
const uint8_t *zlrs_link_prepare_data(zlrs_link_t *link, uint8_t slot, uint32_t now_ms,
				      uint16_t *size);

/* After stopping radio work, discard staged encoding without losing queued/reliable data. */
void zlrs_link_discard_prepared_data(zlrs_link_t *link);

/* Advance a missed slot without building a packet, consuming data or changing session state. */
void zlrs_link_skip_slot(zlrs_link_t *link, uint8_t slot);

/* radio transaction finished */
void zlrs_link_complete(zlrs_link_t *link, int status, uint16_t rx_len, int16_t rssi, int8_t snr,
			uint32_t now_ms);

/*
 * Inject a frame directly, equivalent to "the radio received len bytes". The normal path
 * through zlrs_link_complete() ends up here too; bypass receivers and host tests use it.
 */
void zlrs_link_rx(zlrs_link_t *link, const uint8_t *buf, uint16_t len, int16_t rssi, int8_t snr,
		  uint32_t now_ms);

/* Timeouts and state transitions, called once per superframe */
void zlrs_link_tick(zlrs_link_t *link, uint32_t now_ms);

/* Queue passthrough data (RX and TX), returns the number of bytes written */
uint16_t zlrs_link_write(zlrs_link_t *link, const uint8_t *data, uint16_t len);

bool zlrs_link_cancel_pair(zlrs_link_t *link, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
