/*
 * Link layer: one instance per link, running the state machine and superframe slots (09-protocol 3, 4).
 *
 * Each superframe: zlrs_link_slot() hands out a transaction, radio.submit() runs it and
 * zlrs_link_complete() closes it; zlrs_link_tick() handles timeouts and state changes. No Zephyr.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/link/radio.h"
#include "zlrs/proto/config.h"
#include "zlrs/proto/crypto.h"
#include "zlrs/proto/fhss.h"
#include "zlrs/proto/frag.h"
#include "zlrs/proto/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Link kinds */
#define ZLRS_LINK_CONTROL 0U
#define ZLRS_LINK_DATA    1U

/*
 * Pairing handshake (doc/09-protocol.md 3.3). The two ends share no slot phase while pairing, so
 * a message is heard or missed by luck: every step repeats until the next one shows up. The
 * initiator sends this many PAIR_CFM before it calls the handshake done.
 */
#define ZLRS_PAIR_CFM_REPEATS 16U
/* Frames between two listen windows on the pairing channel while SEARCHING (see control_slot) */
#define ZLRS_PAIR_SCAN_FRAMES 4U
/*
 * Session confirm window. Same story as pairing: the two ends only share a slot phase once the
 * session is up. For this long after entering CONNECTED the initiator keeps listening on the sync
 * channel, where a responder that missed the confirm is still repeating its answer, and answers
 * each repeat with another confirm.
 */
#define ZLRS_SESS_CONFIRM_MS 500U

/* Superframe slots, see doc/09-protocol.md 4.1 */
#define ZLRS_SLOT_CTL_DOWN 0U /* TX -> RX: RC frame (the RX side is a receive window) */
#define ZLRS_SLOT_CTL_UP   1U /* RX -> TX: TLM frame (the TX side is a receive window) */
#define ZLRS_SLOT_DATA_A   2U /* first half of the data window: TX -> RX */
#define ZLRS_SLOT_DATA_B   3U /* second half of the data window: RX -> TX */

/* Link states, see doc/09-protocol.md 3.2 */
typedef enum {
	ZLRS_STATE_UNCONFIGURED = 0,
	ZLRS_STATE_UNPAIRED,
	ZLRS_STATE_PAIRING,
	ZLRS_STATE_SEARCHING,
	ZLRS_STATE_CONNECTED,
} zlrs_link_state_t;

/* Completion status: the port layer folds the radio result into these two */
#define ZLRS_RADIO_OK   0
#define ZLRS_RADIO_FAIL 1

typedef struct {
	uint8_t kind;   /* ZLRS_LINK_CONTROL / ZLRS_LINK_DATA */
	uint8_t role;   /* ZLRS_ROLE_TX / ZLRS_ROLE_RX */
	uint8_t domain; /* ZLRS_DOMAIN_*: the main hop table */
	/*
	 * Second table (sub-band mixing, doc/09-protocol.md 5.1): 0xFF = not configured, the link
	 * only hops the main table; otherwise each sub-band gets one and sub_mix splits the slots.
	 */
	uint8_t second_domain;
	uint8_t sync_slot; /* ZLRS_SYNC_SLOT_* */
	zlrs_profile_t profile;
	/*
	 * Modulation: an FLRC rate tier or LoRa SF/bandwidth/coding rate/preamble, see
	 * doc/07-architecture.md 3. LoRa sync words derive from the pairing key, and the link layer
	 * fills in mod.sync_byte itself (callers pass 0).
	 */
	zlrs_mod_cfg_t mod;
	int8_t tx_power_dbm;
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
	/*
	 * Per-frame hopping. 0 = stay on the sync channel (entry 0 of the hop table), which is how
	 * layer isolation looks at timing and the hop table separately, see doc/12-debug.md 3.
	 */
	uint8_t hopping;
	/*
	 * Session handshake. 0 = the pairing-level link_auth doubles as the session key: after a SYNC
	 * only a SESS_RESP is needed and the first data frame is the confirmation. Saves a derivation
	 * and a message, costs per-session key rotation: bench only, see doc/10-security.md
	 * 3.
	 */
	uint8_t handshake;
	/* Uplink telemetry slot on the control link. 0 = send nothing, open no receive window */
	uint8_t telemetry;
	/*
	 * Cadence thinning (1 = every frame): at high control rates the telemetry and data windows
	 * need not run every frame. Counted inside the link layer, so both ends stay in step.
	 */
	uint8_t tlm_every_n;
	uint8_t data_every_n;
	/*
	 * Sub-band mix (0..3 -> 0/1/2/4 of every 4 slots on sub-G): the initial value, which the TX
	 * advances at runtime through the SYNC table version, see doc/09-protocol.md 5.1.
	 */
	uint8_t mix;
	/*
	 * Periodic SYNC interval while connected, in frames. 0 = no periodic resync (the hop index
	 * is only aligned when the link comes up); >0 = a SYNC every this many frames lets the peer
	 * correct drift and notice a peer that changed sessions (reboot/re-pair) and re-handshake.
	 */
	uint8_t resync_every_n;
} zlrs_link_cfg_t;

typedef struct {
	void (*on_rc)(void *ctx, const uint8_t *payload, uint16_t len);
	void (*on_tlm)(void *ctx, const uint8_t *payload, uint16_t len);
	void (*on_data)(void *ctx, const uint8_t *data, uint16_t len);
	void (*on_state)(void *ctx, uint8_t state);
	/* RX side: failsafe values (RC payload) from the TX during pairing; store and output them */
	void (*on_failsafe)(void *ctx, const uint8_t *payload, uint16_t len);
	void *ctx;
} zlrs_link_events_t;

typedef struct {
	uint32_t tx_frames;
	uint32_t tx_bytes;
	uint32_t rx_frames;
	uint32_t rx_bytes;
	/* Transactions per superframe slot (ZLRS_SLOT_*): sent, listened, heard a frame, failed */
	uint32_t slot_tx[4];
	uint32_t slot_rx[4];
	uint32_t slot_hit[4];
	uint32_t slot_fail[4];
	uint32_t rx_bad_tag;
	uint32_t rx_bad_frame;
	uint32_t rx_dup;
	uint32_t rx_out_of_order; /* ARQ: drops caused by sequence-number jumps */
	uint32_t tx_retries;      /* ARQ: retransmissions */
	uint32_t tx_arq_drop;     /* ARQ: runs restarted after the retries ran out (bytes are kept) */
	uint32_t rx_frag_drop;
	uint32_t rx_syncs;      /* SYNC frames received */
	uint32_t tx_syncs;      /* SYNC frames sent */
	uint32_t sess_resp_rx;  /* TX: peer answered a SYNC */
	uint32_t sess_resp_tx;  /* RX: answered a SYNC */
	uint32_t sess_cfm_rx;   /* RX: session confirmed */
	uint32_t sess_cfm_tx;   /* TX: confirmations sent */
	uint32_t mix_changes;   /* how often a new sub-band mix was adopted */
	uint32_t mix_fallbacks; /* fallbacks to the default mix after missing new SYNC versions */
	uint32_t hop_resync;    /* hop-index corrections received through SYNC */
	uint32_t hop_advances;  /* times this link moved its hop index by itself */
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
	int8_t last_snr; /* the radio stack cannot provide it under FLRC, see doc/06-radio.md */
	/* Uplink quality: frames really received in the last window vs. what the sequence predicts */
	uint8_t lq_percent;
	uint16_t lq_rx;
	uint16_t lq_expected;
	/* Passthrough backpressure: bytes dropped because the air-side queue was full */
	uint32_t data_drop_bytes;
} zlrs_link_stats_t;

/*
 * Bytes the link will hold for the air. ARQ keeps a frame in this queue until the peer
 * acknowledges it, so the queue has to cover the in-flight window plus whatever the application
 * hands over in the meantime; at 512 a single 512-byte write filled it and everything after that
 * was dropped before it ever reached the air.
 */
#define ZLRS_LINK_TX_QUEUE_LEN 4096U
#define ZLRS_LINK_TLM_MAX_LEN  32U

/*
 * ARQ profile (the data-link default, see doc/09-protocol.md 4.3): cumulative ACKs over a
 * fixed window, retransmit until acked or timed out. The window is a run of frames from the
 * head; bytes are only committed to the queue on ACK, so retransmits need no extra buffer.
 */
#define ZLRS_ARQ_WINDOW      4U
#define ZLRS_ARQ_TIMEOUT_MS  30U
/*
 * Retries before a run is given up and counted. Giving up is expensive: the head frame is thrown
 * away *and* the stream resyncs, so everything the receiver had buffered for that run goes too.
 * The air path is far from perfect (on the bench a frame gets through in maybe a third of the
 * attempts), so the budget has to cover long bad streaks; 64 tries is about two seconds at the
 * retry cadence, which is the most a stalled frame should ever cost before the resync path takes
 * over.
 */
#define ZLRS_ARQ_MAX_RETRIES 64U

typedef struct {
	zlrs_link_cfg_t cfg;
	zlrs_radio_ops_t radio;
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
	 * longer reproduce a session. See doc/10-security.md 3.
	 */
	uint8_t pair_salt[ZLRS_PAIR_SALT_LEN];
	uint8_t pair_key[ZLRS_KEY_LEN];
	/*
	 * Per-frame tag keys are direction-split (sender -> receiver) with both dev_ids as context:
	 * out tags our frames (local id first), in verifies the peer's (peer id first).
	 * Each end derives both with no negotiation and no roles. One shared key would let our own
	 * frames pass verification when reflected, see doc/10-security.md 3.
	 */
	uint8_t link_auth_out[ZLRS_KEY_LEN];
	uint8_t link_auth_in[ZLRS_KEY_LEN];
	uint8_t sync_word[ZLRS_SYNC_WORD_LEN];
	/* LoRa sync words are one byte plus a private bit, see doc/09-protocol.md 5.3 */
	uint8_t lora_sync;
	uint8_t session_key_out[ZLRS_KEY_LEN];
	uint8_t session_key_in[ZLRS_KEY_LEN];
	bool session_valid;
	/*
	 * Session setup mirrors pairing (doc/09-protocol.md 3.5): the initiator offers nonce_t, the
	 * responder answers with nonce_r, both feed the KDF, and the last message carries nonce_r
	 * to prove the peer derived the same key. The RX only adopts the session after that proof,
	 * so replaying an old SYNC yields no old session key.
	 */
	bool session_pending;  /* RX: answered a SYNC, waiting for the peer's confirm */
	bool sess_resp_due;    /* RX: SESS_RESP due in the uplink slot */
	bool sess_cfm_due;     /* TX: SESS_CFM due in the downlink slot */
	uint32_t sess_nonce_t; /* initiator (TX) nonce */
	uint32_t sess_nonce_r; /* responder (RX) nonce */
	uint16_t sess_hop;     /* hop index of this session */
	uint32_t pair_nonce;   /* pairing: initiator nonce_a */
	uint32_t pair_nonce_b; /* pairing: responder nonce_b */

	/* state and timing */
	uint8_t state;
	uint32_t state_ms;
	uint32_t last_rx_ms;
	uint32_t last_sync_ms;
	uint32_t last_pair_tx_ms;
	uint32_t last_pair_rx_ms; /* last PAIR_RESP: tells a repeated answer from a silent responder */
	bool pair_cfm_pending;    /* TX got PAIR_RESP: PAIR_CFM due in the downlink slots */
	uint8_t pair_cfm_sent;    /* PAIR_CFM of this handshake already sent */
	bool pair_wait_cfm;       /* RX answered PAIR_RESP, waiting for PAIR_CFM */
	/*
	 * Simplified handshake (cfg.handshake = 0): the RX enters CONNECTED on a SYNC but keeps
	 * answering SESS_RESP until the peer's first frame arrives: it may still be SEARCHING.
	 */
	bool announce;

	/* sequence numbers and hopping */
	uint16_t tx_seq;
	uint16_t last_rx_seq;
	uint16_t lq_last_seq;
	uint16_t frame_index; /* main slots run (CTL_DOWN on control, DATA_A on data) */
	zlrs_fhss_t fhss;
	/* Sub-band mix: current level, table version, default level, last-version timestamp */
	uint8_t sub_mix;
	uint8_t sub_mix_version;
	uint8_t sub_mix_default;
	uint32_t sub_mix_seen_ms;

	/* payloads */
	uint8_t rc_payload[ZLRS_RC_PAYLOAD_LEN];
	uint16_t rc_len;
	uint8_t tlm_payload[ZLRS_LINK_TLM_MAX_LEN];
	uint16_t tlm_len;
	/* Failsafe values: the TX side ships its own, the RX keeps the last ones received */
	uint8_t failsafe[ZLRS_RC_PAYLOAD_LEN];
	uint16_t failsafe_len;

	/* passthrough queue */
	uint8_t tx_queue[ZLRS_LINK_TX_QUEUE_LEN];
	uint16_t queue_head;
	uint16_t queue_count;
	uint16_t pending_data_len;

	/* ARQ sender: lengths of the frames in flight, head frame at [0] */
	uint16_t arq_seq;
	uint16_t arq_len[ZLRS_ARQ_WINDOW];
	/* MORE flag of each window frame: a retransmit has to reproduce the frame, run boundary included */
	uint8_t arq_more[ZLRS_ARQ_WINDOW];
	uint8_t arq_used;
	uint8_t arq_retries;
	uint32_t arq_sent_ms;
	/* Bytes already queued into the run that is being sent (0 = the next frame starts a new one) */
	uint16_t arq_run_bytes;
	/* ARQ receiver: highest in-order sequence number plus a pending cumulative ACK */
	uint16_t arq_rx_seq;
	bool arq_rx_started;
	bool arq_ack_pending;
	/* ARQ resync: retries ran out, restart and wait for the peer to confirm its reset */
	bool arq_resync;

	/* RX/TX buffers and reassembly */
	uint8_t tx_buf[ZLRS_FRAME_MAX_LEN];
	uint8_t rx_buf[ZLRS_FRAME_MAX_LEN];
	zlrs_reasm_t reasm;
	uint8_t reasm_buf[ZLRS_REASM_BUF_LEN];

	/* transaction to submit next */
	zlrs_radio_req_t req;
	bool req_is_tx;
	bool req_pending;
	uint8_t slot; /* ZLRS_SLOT_*: the slot the pending transaction serves (stats and logs) */
} zlrs_link_t;

void zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg, const zlrs_radio_ops_t *radio,
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

/*
 * Recompute the pairing-level keys from the record the link holds now (ids, salt, root key).
 * Bench entry: two ends that show the same record but different keys can be told apart by
 * re-deriving on both and comparing again.
 */
void zlrs_link_rederive(zlrs_link_t *link);

/* The exact bytes the pairing KDF is fed with: both ids (sorted) then the salt (sorted) */
#define ZLRS_PAIR_CTX_LEN (2U * ZLRS_DEV_ID_LEN + ZLRS_PAIR_SALT_LEN)
void zlrs_link_pair_ctx(const zlrs_link_t *link, uint8_t out[ZLRS_PAIR_CTX_LEN]);

/*
 * Open one receive window on this link's own channel, outside the superframe's own slot. A build
 * with one link has slots the other one does not use, and listening there is free air time for the
 * side that is only receiving: on the bench it takes the receive duty from about 40% to nearly
 * always, which is what a weak peer needs.
 */
const zlrs_radio_req_t *zlrs_link_listen(zlrs_link_t *link);

/*
 * Bench-only raw radio access, no state machine and no ARQ in the way:
 * blast() sends one complete frame right now, listen_raw() opens one window of the given length.
 * They answer "does the radio hear the peer at all" independently of slots, phases and retries.
 */
const zlrs_radio_req_t *zlrs_link_blast(zlrs_link_t *link, const uint8_t *data, uint16_t len);
const zlrs_radio_req_t *zlrs_link_listen_raw(zlrs_link_t *link, uint16_t timeout_ms);

/* Change modulation at runtime (rate adaptation); effective with the next transaction */
void zlrs_link_set_mod(zlrs_link_t *link, const zlrs_mod_cfg_t *mod);

/*
 * Set the sub-band mix: on the TX the next SYNC carries it (table version +1); on the RX it
 * only moves the default (fallback) level, the TX's SYNC still drives the current one.
 */
void zlrs_link_set_mix(zlrs_link_t *link, uint8_t mix);

/* Change TX power at runtime (dynamic power); effective with the next transaction */
void zlrs_link_set_power(zlrs_link_t *link, int8_t tx_power_dbm);

/*
 * Restore pairing from the stored record (no re-pairing after a reboot). pair_salt has to
 * match the peer's copy: the two exchanged nonces. Keys are recomputed, never stored.
 */
void zlrs_link_set_peer(zlrs_link_t *link, const uint8_t peer_id[ZLRS_DEV_ID_LEN],
			const uint8_t pair_salt[ZLRS_PAIR_SALT_LEN], uint32_t now_ms);

/* Button: a long press starts pairing (re-pairing takes this path too, the new record wins) */
void zlrs_link_press(zlrs_link_t *link, uint32_t now_ms);

/* TX side sets the failsafe values shipped during pairing (RC payload, 09-protocol 3.4) */
void zlrs_link_set_failsafe(zlrs_link_t *link, const uint8_t *payload, uint16_t len);

/* Superframe slot: return the transaction to submit, or NULL when there is none */
const zlrs_radio_req_t *zlrs_link_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms);

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

/* Control link payloads: the TX side sets RC, the RX side sets TLM */
void zlrs_link_set_rc(zlrs_link_t *link, const uint8_t *channels, uint16_t len);
void zlrs_link_set_tlm(zlrs_link_t *link, const uint8_t *payload, uint16_t len);

#ifdef __cplusplus
}
#endif
