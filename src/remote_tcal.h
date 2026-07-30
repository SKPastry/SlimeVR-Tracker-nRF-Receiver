#ifndef SLIMENRF_REMOTE_TCAL_H
#define SLIMENRF_REMOTE_TCAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "remote_tcal_protocol.h"

#define REMOTE_TCAL_TARGET_ALL 0xFFU
#define REMOTE_TCAL_NO_RESPONSE 0x0EU
#define REMOTE_TCAL_NOT_CONSIDERED 0x0FU
#define REMOTE_TCAL_PING_RETRY_WINDOW_MS 250U

struct remote_tcal_outcome {
	uint16_t transaction_id;
	uint16_t considered_mask;
	uint16_t reply_mask;
	uint8_t action;
	uint8_t status[16];
};

typedef void (*remote_tcal_complete_cb_t)(
	const struct remote_tcal_outcome *outcome, void *user_data);

int remote_tcal_submit(uint8_t target, uint8_t action, int16_t target_centi_c,
		       remote_tcal_complete_cb_t callback, void *user_data,
		       uint16_t *transaction_id, uint16_t *considered_mask);

bool remote_tcal_fill_pong_isr(uint8_t tracker_id, const uint8_t ping[13],
			       uint32_t now_ms, uint8_t pong[13],
			       bool *force_normal);
void remote_tcal_process_ping(uint8_t tracker_id, const uint8_t ping[13],
			      uint32_t now_ms);
void remote_tcal_reset_tracker(uint8_t tracker_id);
void remote_tcal_reset_all(void);
bool remote_tcal_tracker_pending(uint8_t tracker_id);
/*
 * Called with local IRQs locked by a legacy command publisher.  Critical
 * commands atomically withdraw an extension slot; ordinary commands are
 * rejected while the slot is active.
 */
bool remote_tcal_prepare_legacy_command_locked(uint8_t tracker_id,
					       bool critical);

void remote_tcal_hid_handle_report(const uint8_t *report, size_t len,
				   uint32_t session_generation);
const char *remote_tcal_result_name(uint8_t result);

#endif
