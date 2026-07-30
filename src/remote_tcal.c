#include "remote_tcal.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>

#include "connection/esb.h"
#include "data_collect.h"
#include "esb_ota.h"
#include "globals.h"
#include "hid.h"
#include "receiver_ota.h"

LOG_MODULE_REGISTER(remote_tcal, LOG_LEVEL_INF);
BUILD_ASSERT(SK_ESB_EXT_ESCAPE == SK_REMOTE_TCAL_ESCAPE,
	     "ESB escape definitions must remain identical");

#define REMOTE_TCAL_CAPABILITY_FRESH_MS 2500U
#define REMOTE_TCAL_CAPABILITY_TIMEOUT_MS 5000U
#define REMOTE_TCAL_TRANSACTION_TIMEOUT_MS 5000U
#define REMOTE_TCAL_MANAGER_POLL_MS 20U
#define REMOTE_TCAL_START_RESULT_MIN_AGE_MS 1500U
#define REMOTE_TCAL_START_CONFIRM_COUNT 2U
#define REMOTE_TCAL_HID_ACK_RETRY_COUNT 25U
#define REMOTE_TCAL_HID_ACK_RETRY_MS 5U

#define RCV_HID_TYPE_CMD_ACK 251U
#define RCV_HID_TYPE_CMD 254U
/* HID opcode namespace is intentionally independent of the ESB escape. */
#define RCV_HID_OP_REMOTE_HEATED_TCAL 0xC8U

#define RCV_HID_ST_OK 0U
#define RCV_HID_ST_EINVAL 1U
#define RCV_HID_ST_EBUSY 3U
#define RCV_HID_ST_ENOENT 4U
#define RCV_HID_ST_ENOTSUP 5U
#define RCV_HID_ST_STARTED 7U
#define RCV_HID_ST_REMOTE_FAILED 8U

#if defined(CONFIG_SK_REMOTE_HEATED_TCAL_TEST) && CONFIG_SK_REMOTE_HEATED_TCAL_TEST
static uint8_t errno_to_hid_status(int err)
{
	switch (err) {
	case 0:
		return RCV_HID_ST_OK;
	case -EINVAL:
		return RCV_HID_ST_EINVAL;
	case -EBUSY:
		return RCV_HID_ST_EBUSY;
	case -ENOENT:
		return RCV_HID_ST_ENOENT;
	case -ENOTSUP:
	default:
		return RCV_HID_ST_ENOTSUP;
	}
}

static void init_ack(uint8_t ack[16], uint8_t sequence, uint8_t status)
{
	memset(ack, 0, 16);
	ack[0] = RCV_HID_TYPE_CMD_ACK;
	ack[1] = sequence;
	ack[2] = RCV_HID_OP_REMOTE_HEATED_TCAL;
	ack[3] = status;
	memset(&ack[8], 0xFF, 8);
}
#endif

#if defined(CONFIG_SK_REMOTE_HEATED_TCAL_TEST) && CONFIG_SK_REMOTE_HEATED_TCAL_TEST

struct remote_tcal_capability {
	volatile uint32_t last_ping_ms;
	volatile uint32_t generation;
	volatile uint32_t ping_serial;
	volatile uint8_t supported;
	volatile uint8_t status;
	volatile uint8_t last_ping_counter;
	volatile uint8_t ping_counter_valid;
	volatile uint8_t last_raw_ping_counter;
	volatile uint8_t raw_ping_counter_valid;
	volatile uint32_t last_raw_ping_ms;
};

/*
 * Command fields are published from thread context while local interrupts
 * are disabled, so RADIO observes a complete old or new slot without a lock.
 * RADIO records only that it attempted a command for a PING counter.  The
 * sequence-validated event path commits that attempt as execution evidence;
 * threads snapshot the evidence under irq_lock().
 */
struct remote_tcal_tx_slot {
	volatile uint32_t deadline_ms;
	volatile uint32_t session_generation;
	volatile uint32_t command_first_sent_ms;
	volatile uint32_t command_last_sent_serial;
	volatile uint16_t transaction_id;
	volatile int16_t target_centi_c;
	volatile uint8_t action;
	volatile uint8_t command_last_attempt_counter;
	volatile uint8_t command_confirm_count;
	volatile uint8_t command_attempt_valid;
	volatile uint8_t command_sent_valid;
	volatile uint8_t active;
};

struct remote_tcal_job {
	bool active;
	uint16_t transaction_id;
	uint16_t considered_mask;
	uint16_t reply_mask;
	uint32_t deadline_ms;
	uint8_t action;
	uint8_t status[MAX_TRACKERS];
	uint32_t session_generation[MAX_TRACKERS];
	remote_tcal_complete_cb_t callback;
	void *user_data;
};

struct remote_tcal_result_event {
	uint32_t now_ms;
	uint32_t session_generation;
	uint32_t ping_serial;
	uint8_t tracker_id;
	uint8_t ping[13];
};

K_MSGQ_DEFINE(remote_tcal_result_msgq,
	      sizeof(struct remote_tcal_result_event), 16, 4);

static struct remote_tcal_capability capabilities[MAX_TRACKERS];
static struct remote_tcal_tx_slot tx_slots[MAX_TRACKERS];
static struct remote_tcal_job current_job;
static K_MUTEX_DEFINE(remote_tcal_lock);
static uint16_t next_transaction;

static inline bool deadline_expired(uint32_t now, uint32_t deadline)
{
	return (int32_t)(now - deadline) >= 0;
}

static inline bool serial_after(uint32_t value, uint32_t previous)
{
	return (int32_t)(value - previous) > 0;
}

static uint32_t next_generation(uint32_t generation)
{
	generation++;
	return generation == 0U ? 1U : generation;
}

static void slot_write_locked(uint8_t tracker_id, bool active,
			      uint16_t transaction, uint8_t action,
			      int16_t target_centi_c, uint32_t deadline,
			      uint32_t session_generation)
{
	struct remote_tcal_tx_slot *slot = &tx_slots[tracker_id];

	slot->deadline_ms = deadline;
	slot->session_generation = session_generation;
	slot->command_first_sent_ms = 0U;
	slot->command_last_sent_serial = 0U;
	slot->transaction_id = transaction;
	slot->target_centi_c = target_centi_c;
	slot->action = action;
	slot->command_last_attempt_counter = 0U;
	slot->command_confirm_count = 0U;
	slot->command_attempt_valid = 0U;
	slot->command_sent_valid = 0U;
	__asm__ volatile("" ::: "memory");
	slot->active = active ? 1U : 0U;
}

static void slot_clear_locked(uint8_t tracker_id)
{
	tx_slots[tracker_id].active = 0;
	__asm__ volatile("" ::: "memory");
	tx_slots[tracker_id].deadline_ms = 0;
	tx_slots[tracker_id].session_generation = 0;
	tx_slots[tracker_id].command_first_sent_ms = 0;
	tx_slots[tracker_id].command_last_sent_serial = 0;
	tx_slots[tracker_id].transaction_id = 0;
	tx_slots[tracker_id].target_centi_c = 0;
	tx_slots[tracker_id].action = 0;
	tx_slots[tracker_id].command_last_attempt_counter = 0;
	tx_slots[tracker_id].command_confirm_count = 0;
	tx_slots[tracker_id].command_attempt_valid = 0;
	tx_slots[tracker_id].command_sent_valid = 0;
}

static void capability_reset_locked(uint8_t tracker_id)
{
	struct remote_tcal_capability *capability =
		&capabilities[tracker_id];
	uint32_t generation =
		next_generation(capability->generation);

	memset((void *)capability, 0, sizeof(*capability));
	capability->generation = generation;
	slot_clear_locked(tracker_id);
}

static void slot_clear(uint8_t tracker_id)
{
	unsigned int key = irq_lock();
	slot_clear_locked(tracker_id);
	irq_unlock(key);
}

static void clear_job_slots(uint16_t mask)
{
	for (uint8_t i = 0; i < MAX_TRACKERS; i++) {
		if ((mask & BIT(i)) != 0U) {
			slot_clear(i);
		}
	}
}

static uint16_t allocate_transaction(void)
{
	if (next_transaction == 0U) {
		next_transaction = (uint16_t)sys_rand32_get();
		if (next_transaction == 0U) {
			next_transaction = 1U;
		}
	}

	uint16_t allocated = next_transaction++;
	if (next_transaction == 0U) {
		next_transaction = 1U;
	}
	return allocated;
}

static void snapshot_capability(uint8_t tracker_id, uint32_t *last_ping,
				uint32_t *generation, bool *supported)
{
	unsigned int key = irq_lock();
	*last_ping = capabilities[tracker_id].last_ping_ms;
	*generation = capabilities[tracker_id].generation;
	*supported = capabilities[tracker_id].supported != 0U;
	irq_unlock(key);
}

static bool capability_age_eligible(uint32_t last_ping, uint32_t now,
				    uint8_t action)
{
	if (last_ping == 0U) {
		return false;
	}

	uint32_t age = now - last_ping;

	if (action == SK_REMOTE_TCAL_ACTION_START) {
		return age <= REMOTE_TCAL_CAPABILITY_FRESH_MS;
	}

	/*
	 * STOP/ABORT are safety actions.  Permit them for the rest of the
	 * capability cache lifetime, but never at or beyond the timeout.
	 */
	return age < REMOTE_TCAL_CAPABILITY_TIMEOUT_MS;
}

static bool capability_is_eligible(uint8_t tracker_id, uint32_t now,
				   uint8_t action, bool *online,
				   uint32_t *generation)
{
	uint32_t last_ping;
	bool supported;

	snapshot_capability(tracker_id, &last_ping, generation,
			    &supported);
	*online = capability_age_eligible(last_ping, now, action);
	return *online && supported;
}

static void invoke_completion(remote_tcal_complete_cb_t callback,
			      void *user_data,
			      const struct remote_tcal_outcome *outcome)
{
	if (callback != NULL) {
		callback(outcome, user_data);
	}
}

static void copy_outcome_locked(struct remote_tcal_outcome *outcome)
{
	outcome->transaction_id = current_job.transaction_id;
	outcome->considered_mask = current_job.considered_mask;
	outcome->reply_mask = current_job.reply_mask;
	outcome->action = current_job.action;
	memcpy(outcome->status, current_job.status, sizeof(outcome->status));
}

int remote_tcal_submit(uint8_t target, uint8_t action, int16_t target_centi_c,
		       remote_tcal_complete_cb_t callback, void *user_data,
		       uint16_t *transaction_id, uint16_t *considered_mask)
{
	if (action < SK_REMOTE_TCAL_ACTION_START ||
	    action > SK_REMOTE_TCAL_ACTION_ABORT ||
	    (action != SK_REMOTE_TCAL_ACTION_START && target_centi_c != 0) ||
	    (target != REMOTE_TCAL_TARGET_ALL && target >= MAX_TRACKERS)) {
		return -EINVAL;
	}

	uint32_t now = k_uptime_get_32();
	uint16_t selected = 0;
	uint16_t capability_selected = 0;
	uint16_t override_selected = 0;
	uint32_t selected_generation[MAX_TRACKERS] = {0};
	bool any_online = false;
	uint8_t count = MIN(stored_trackers, MAX_TRACKERS);

	if (target != REMOTE_TCAL_TARGET_ALL && target >= count) {
		return -ENOENT;
	}
	for (uint8_t i = 0; i < count; i++) {
		if (target != REMOTE_TCAL_TARGET_ALL && i != target) {
			continue;
		}
		bool online = false;
		if (capability_is_eligible(
			    i, now, action, &online, &selected_generation[i])) {
			selected |= BIT(i);
			capability_selected |= BIT(i);
		}
		any_online |= online;
	}

	if (action == SK_REMOTE_TCAL_ACTION_START) {
		if (receiver_ota_is_active() || esb_ota_relay_is_active() ||
		    data_collect_is_active()) {
			return -EBUSY;
		}
	}

	/*
	 * A safety STOP/ABORT is allowed to replace an extension START whose
	 * execution is not yet known.  The old requester keeps NO_RESPONSE for
	 * every member that did not already return a structured result.  The old
	 * job and every one of its slots are replaced while remote_tcal_lock is
	 * held; its callback runs only after the replacement is fully visible.
	 */
	struct remote_tcal_outcome replaced_outcome;
	remote_tcal_complete_cb_t replaced_callback = NULL;
	void *replaced_user_data = NULL;
	bool replacing_start = false;

	k_mutex_lock(&remote_tcal_lock, K_FOREVER);
	if (action != SK_REMOTE_TCAL_ACTION_START && current_job.active &&
	    current_job.action == SK_REMOTE_TCAL_ACTION_START) {
		uint16_t requested_mask =
			target == REMOTE_TCAL_TARGET_ALL
				? UINT16_MAX
				: BIT(target);
		if ((current_job.considered_mask & requested_mask) != 0U) {
			/*
			 * A partial safety request must cover the complete old START
			 * set, including Trackers whose capability just became stale.
			 */
			override_selected = current_job.considered_mask;
			selected |= override_selected;
			copy_outcome_locked(&replaced_outcome);
			replaced_callback = current_job.callback;
			replaced_user_data = current_job.user_data;
			replacing_start = true;
		}
	}

	if (selected == 0U) {
		k_mutex_unlock(&remote_tcal_lock);
		return any_online ? -ENOTSUP : -ENOENT;
	}
	if (current_job.active && !replacing_start) {
		k_mutex_unlock(&remote_tcal_lock);
		return -EBUSY;
	}
	uint16_t allocated_transaction = allocate_transaction();

	/*
	 * Generic command collision detection and extension slot publication
	 * share the same short IRQ critical section used by all generic command
	 * writers.  Thus START either sees the complete old generic state or is
	 * published before a generic writer checks the extension slot.
	 */
	uint32_t publish_now = k_uptime_get_32();
	unsigned int key = irq_lock();
	uint16_t validated = 0U;
	for (uint8_t i = 0; i < MAX_TRACKERS; i++) {
		uint16_t bit = BIT(i);
		if ((selected & bit) == 0U) {
			continue;
		}

		struct remote_tcal_capability *capability =
			&capabilities[i];
		bool fresh_selection =
			(capability_selected & bit) != 0U &&
			capability->generation ==
				selected_generation[i] &&
			capability->supported != 0U &&
			capability_age_eligible(
				capability->last_ping_ms, publish_now,
				action);
		bool same_session_override =
			(override_selected & bit) != 0U &&
			capability->generation ==
				current_job.session_generation[i] &&
			capability->supported != 0U &&
			capability_age_eligible(
				capability->last_ping_ms, publish_now,
				action);

		if (fresh_selection || same_session_override) {
			validated |= bit;
		}
	}
	selected = validated;
	if (selected == 0U) {
		irq_unlock(key);
		k_mutex_unlock(&remote_tcal_lock);
		return -ENOENT;
	}

	bool conflict =
		action == SK_REMOTE_TCAL_ACTION_START &&
		(receiver_ota_is_active() || esb_ota_relay_is_active() ||
		 data_collect_is_active());
	for (uint8_t i = 0; i < MAX_TRACKERS && !conflict; i++) {
		if ((selected & BIT(i)) == 0U) {
			continue;
		}
		/*
		 * START never overtakes an existing legacy command.  STOP/ABORT may
		 * serialize ahead of an ordinary command, but never ahead of a critical
		 * shutdown/reboot/DFU/scan/data-collection/OTA transition.
		 */
		if (esb_critical_remote_command_pending(i) ||
		    (action == SK_REMOTE_TCAL_ACTION_START &&
		     esb_remote_command_pending(i))) {
			conflict = true;
		}
	}
	if (conflict) {
		irq_unlock(key);
		k_mutex_unlock(&remote_tcal_lock);
		return -EBUSY;
	}

	if (replacing_start) {
		uint16_t old_mask = current_job.considered_mask;
		for (uint8_t i = 0; i < MAX_TRACKERS; i++) {
			if ((old_mask & BIT(i)) != 0U &&
			    (selected & BIT(i)) == 0U) {
				slot_clear_locked(i);
			}
		}
	}

	memset(&current_job, 0, sizeof(current_job));
	current_job.active = true;
	current_job.transaction_id = allocated_transaction;
	current_job.considered_mask = selected;
	current_job.deadline_ms =
		publish_now + REMOTE_TCAL_TRANSACTION_TIMEOUT_MS;
	current_job.action = action;
	current_job.callback = callback;
	current_job.user_data = user_data;
	memset(current_job.status, REMOTE_TCAL_NOT_CONSIDERED,
	       sizeof(current_job.status));
	for (uint8_t i = 0; i < MAX_TRACKERS; i++) {
		if ((selected & BIT(i)) != 0U) {
			current_job.status[i] = REMOTE_TCAL_NO_RESPONSE;
			current_job.session_generation[i] =
				capabilities[i].generation;
			slot_write_locked(i, true, current_job.transaction_id,
					  action, target_centi_c,
					  current_job.deadline_ms,
					  current_job.session_generation[i]);
		}
	}
	irq_unlock(key);

	if (transaction_id != NULL) {
		*transaction_id = current_job.transaction_id;
	}
	if (considered_mask != NULL) {
		*considered_mask = selected;
	}
	k_mutex_unlock(&remote_tcal_lock);

	if (replacing_start) {
		invoke_completion(replaced_callback, replaced_user_data,
				  &replaced_outcome);
	}
	LOG_INF("Remote heated T-Cal action %u tx=%u queued for mask 0x%04X",
		action, allocated_transaction, selected);
	return 0;
}

bool remote_tcal_fill_pong_isr(uint8_t tracker_id, const uint8_t ping[13],
			       uint32_t now_ms, uint8_t pong[13],
			       bool *force_normal)
{
	if (force_normal != NULL) {
		*force_normal = false;
	}
	if (tracker_id >= MAX_TRACKERS) {
		return false;
	}

	struct remote_tcal_capability *capability =
		&capabilities[tracker_id];
	bool counter_declined =
		capability->ping_counter_valid != 0U &&
		ping[2] < capability->last_ping_counter;
	bool stale_same_counter =
		capability->raw_ping_counter_valid != 0U &&
		ping[2] == capability->last_raw_ping_counter &&
		(uint32_t)(now_ms - capability->last_raw_ping_ms) >
			REMOTE_TCAL_PING_RETRY_WINDOW_MS;

	/*
	 * Detect the conservative no-boot-nonce boundary before constructing the
	 * ACK.  Otherwise a 255->0 wrap could receive the final START confirmation
	 * before event_handler invalidates the old session.
	 */
	if (counter_declined || stale_same_counter) {
		capability_reset_locked(tracker_id);
		capability = &capabilities[tracker_id];
		capability->last_raw_ping_counter = ping[2];
		capability->last_raw_ping_ms = now_ms;
		capability->raw_ping_counter_valid = 1U;
		if (force_normal != NULL &&
		    ping[7] == SK_REMOTE_TCAL_ESCAPE) {
			*force_normal = true;
		}
		return false;
	}
	capability->last_raw_ping_counter = ping[2];
	capability->last_raw_ping_ms = now_ms;
	capability->raw_ping_counter_valid = 1U;

	struct remote_tcal_tx_slot slot;
	slot.active = tx_slots[tracker_id].active;
	__asm__ volatile("" ::: "memory");
	slot.deadline_ms = tx_slots[tracker_id].deadline_ms;
	slot.session_generation =
		tx_slots[tracker_id].session_generation;
	slot.command_first_sent_ms =
		tx_slots[tracker_id].command_first_sent_ms;
	slot.command_last_sent_serial =
		tx_slots[tracker_id].command_last_sent_serial;
	slot.transaction_id = tx_slots[tracker_id].transaction_id;
	slot.target_centi_c = tx_slots[tracker_id].target_centi_c;
	slot.action = tx_slots[tracker_id].action;
	slot.command_last_attempt_counter =
		tx_slots[tracker_id].command_last_attempt_counter;
	slot.command_confirm_count =
		tx_slots[tracker_id].command_confirm_count;
	slot.command_attempt_valid =
		tx_slots[tracker_id].command_attempt_valid;
	slot.command_sent_valid =
		tx_slots[tracker_id].command_sent_valid;

	if (!slot.active || deadline_expired(now_ms, slot.deadline_ms)) {
		if (force_normal != NULL &&
		    ping[7] == SK_REMOTE_TCAL_ESCAPE) {
			/*
			 * A structured result remains latched at the Tracker until a
			 * NORMAL PONG arrives.  Once the matching slot is gone (saved,
			 * timed out, reset, or structurally mismatched), keep ordinary
			 * legacy flags from replacing that mandatory withdrawal
			 * handshake.  Critical legacy commands may still preempt this
			 * request at the caller.
			 */
			*force_normal = true;
		}
		return false;
	}

	/*
	 * A newly-started OTA/data collection session preempts only a pending
	 * START.  STOP/ABORT retain highest priority because they are safety
	 * actions.  Withdrawing the slot prevents any further START confirms;
	 * the host later receives NO_RESPONSE (outcome unknown).
	 */
	if (slot.action == SK_REMOTE_TCAL_ACTION_START &&
	    (receiver_ota_is_active() || esb_ota_relay_is_active() ||
	     data_collect_is_active())) {
		tx_slots[tracker_id].active = 0U;
		return false;
	}

	/*
	 * Never rely on a stale capability cache in the ISR.  The packet that
	 * solicits this PONG must itself either advertise capability or be the
	 * structured result for the active transaction.
	 */
	uint8_t ignored;
	bool current_capability =
		sk_remote_tcal_normal_capability(ping, &ignored);
	bool current_result =
		sk_remote_tcal_parse_result(ping, slot.transaction_id, NULL, NULL);
	if (!current_capability && !current_result) {
		if (ping[7] == ESB_PONG_FLAG_NORMAL) {
			capability->supported = 0U;
		} else if (force_normal != NULL &&
			   ping[7] == SK_REMOTE_TCAL_ESCAPE) {
			*force_normal = true;
		}
		return false;
	}

	if (current_capability) {
		struct remote_tcal_tx_slot *published =
			&tx_slots[tracker_id];

		/*
		 * This is only an attempted transmission.  It becomes result
		 * evidence after the event path accepts this PING's sequence.
		 */
		published->command_last_attempt_counter = ping[2];
		__asm__ volatile("" ::: "memory");
		published->command_attempt_valid = 1U;
	}

	sk_remote_tcal_encode_pong(pong, slot.transaction_id, slot.action,
				   slot.target_centi_c);
	return true;
}

static void process_result_event(
	const struct remote_tcal_result_event *event)
{
	struct remote_tcal_outcome outcome;
	remote_tcal_complete_cb_t callback = NULL;
	void *user_data = NULL;
	uint8_t tracker_id = event->tracker_id;

	k_mutex_lock(&remote_tcal_lock, K_FOREVER);
	if (!current_job.active ||
	    (current_job.considered_mask & BIT(tracker_id)) == 0U) {
		k_mutex_unlock(&remote_tcal_lock);
		return;
	}
	/*
	 * The manager drains queued radio events before checking its wall clock.
	 * Validate the capture timestamp too, otherwise a result received after
	 * the 5-second transaction window could win that ordering race.
	 */
	if (deadline_expired(event->now_ms, current_job.deadline_ms)) {
		k_mutex_unlock(&remote_tcal_lock);
		return;
	}

	uint8_t result;
	uint8_t status;
	if (!sk_remote_tcal_parse_result(
		    event->ping, current_job.transaction_id, &result, &status)) {
		k_mutex_unlock(&remote_tcal_lock);
		return;
	}

	uint32_t first_sent_ms;
	uint32_t last_sent_serial;
	uint32_t slot_generation;
	uint16_t slot_transaction;
	uint8_t slot_action;
	uint8_t confirm_count;
	uint8_t sent_valid;
	uint8_t slot_active;
	unsigned int key = irq_lock();

	slot_active = tx_slots[tracker_id].active;
	first_sent_ms =
		tx_slots[tracker_id].command_first_sent_ms;
	last_sent_serial =
		tx_slots[tracker_id].command_last_sent_serial;
	slot_generation =
		tx_slots[tracker_id].session_generation;
	slot_transaction =
		tx_slots[tracker_id].transaction_id;
	slot_action = tx_slots[tracker_id].action;
	confirm_count =
		tx_slots[tracker_id].command_confirm_count;
	sent_valid =
		tx_slots[tracker_id].command_sent_valid;
	irq_unlock(key);

	/*
	 * Results belong to both a transaction and a Tracker session.  A reset,
	 * removal, timeout, or re-pair changes generation and invalidates queued
	 * events even if the 16-bit transaction happens to collide.
	 */
	if (event->session_generation !=
		    current_job.session_generation[tracker_id] ||
	    slot_active == 0U ||
	    slot_generation !=
		    current_job.session_generation[tracker_id] ||
	    slot_transaction != current_job.transaction_id ||
	    slot_action != current_job.action) {
		k_mutex_unlock(&remote_tcal_lock);
		return;
	}

	/*
	 * Every structured result needs causal proof that this receiver sent the
	 * current task on a sequence-validated NORMAL PING and that the result came
	 * on a later distinct PING.  This rejects cached/colliding non-OK START
	 * results as well as premature OK results.
	 */
	if (sent_valid == 0U ||
	    !serial_after(event->ping_serial, last_sent_serial)) {
		k_mutex_unlock(&remote_tcal_lock);
		return;
	}

	if (current_job.action == SK_REMOTE_TCAL_ACTION_START &&
	    result == SK_REMOTE_TCAL_RESULT_OK) {
		/*
		 * A 16-bit transaction can collide after reboot or wrap.  Accept
		 * START OK only after this receiver has sent the current payload
		 * for two distinct NORMAL PING counters over the Tracker's minimum
		 * confirmation window, and the result snapshot says it is active.
		 */
		if (confirm_count < REMOTE_TCAL_START_CONFIRM_COUNT ||
		    (uint32_t)(event->now_ms - first_sent_ms) <
			    REMOTE_TCAL_START_RESULT_MIN_AGE_MS ||
		    (status & SK_REMOTE_TCAL_STATUS_ACTIVE) == 0U) {
			k_mutex_unlock(&remote_tcal_lock);
			return;
		}
	} else if (current_job.action != SK_REMOTE_TCAL_ACTION_START &&
		   result == SK_REMOTE_TCAL_RESULT_OK &&
		   (status & SK_REMOTE_TCAL_STATUS_ACTIVE) != 0U) {
		/*
		 * Safety OK additionally requires the post-action snapshot to be
		 * inactive; real structured errors are still reported even if the
		 * Tracker remains active.
		 */
		k_mutex_unlock(&remote_tcal_lock);
		return;
	}

	/*
	 * Reset/remove/timeout can run outside remote_tcal_lock.  Revalidate the
	 * session and slot while interrupts are disabled, then save status and
	 * withdraw the slot in that same critical section.  This prevents an old
	 * queued event from updating or clearing a newly-reset identity.
	 */
	key = irq_lock();
	if (capabilities[tracker_id].generation !=
		    current_job.session_generation[tracker_id] ||
	    tx_slots[tracker_id].active == 0U ||
	    tx_slots[tracker_id].session_generation !=
		    current_job.session_generation[tracker_id] ||
	    tx_slots[tracker_id].transaction_id !=
		    current_job.transaction_id ||
	    tx_slots[tracker_id].action != current_job.action) {
		irq_unlock(key);
		k_mutex_unlock(&remote_tcal_lock);
		return;
	}
	current_job.status[tracker_id] = result;
	current_job.reply_mask |= BIT(tracker_id);
	capabilities[tracker_id].status = status;
	slot_clear_locked(tracker_id);
	irq_unlock(key);

	if (current_job.reply_mask == current_job.considered_mask) {
		copy_outcome_locked(&outcome);
		callback = current_job.callback;
		user_data = current_job.user_data;
		memset(&current_job, 0, sizeof(current_job));
	}
	k_mutex_unlock(&remote_tcal_lock);

	invoke_completion(callback, user_data, &outcome);
}

void remote_tcal_process_ping(uint8_t tracker_id, const uint8_t ping[13],
			      uint32_t now_ms)
{
	if (tracker_id >= MAX_TRACKERS) {
		return;
	}

	/*
	 * The caller invokes this only after accepting the Tracker PING sequence.
	 * Duplicate counters retain the same serial, while every accepted,
	 * distinct counter advances it.  This gives result validation a
	 * wrap-safe ordering token that raw queued packets cannot forge.
	 */
	struct remote_tcal_capability *capability =
		&capabilities[tracker_id];
	bool distinct_counter =
		capability->ping_counter_valid == 0U ||
		capability->last_ping_counter != ping[2];
	if (distinct_counter) {
		capability->ping_serial =
			next_generation(capability->ping_serial);
		capability->last_ping_counter = ping[2];
		capability->ping_counter_valid = 1U;
	}
	uint32_t generation = capability->generation;
	uint32_t ping_serial = capability->ping_serial;

	if (ping[7] == ESB_PONG_FLAG_NORMAL) {
		uint8_t status = 0;
		bool supported =
			sk_remote_tcal_normal_capability(ping, &status);
		capability->last_ping_ms = now_ms;
		capability->status = supported ? status : 0U;
		__asm__ volatile("" ::: "memory");
		capability->supported = supported ? 1U : 0U;
		if (!supported) {
			tx_slots[tracker_id].active = 0U;
			return;
		}

		struct remote_tcal_tx_slot *slot =
			&tx_slots[tracker_id];
		if (distinct_counter && slot->active != 0U &&
		    slot->session_generation == generation &&
		    slot->command_attempt_valid != 0U &&
		    slot->command_last_attempt_counter == ping[2]) {
			if (slot->command_sent_valid == 0U) {
				slot->command_first_sent_ms = now_ms;
				slot->command_confirm_count = 1U;
			} else if (serial_after(
					   ping_serial,
					   slot->command_last_sent_serial) &&
				   slot->command_confirm_count < UINT8_MAX) {
				slot->command_confirm_count++;
			}
			slot->command_last_sent_serial = ping_serial;
			__asm__ volatile("" ::: "memory");
			slot->command_sent_valid = 1U;
		}
		return;
	}

	if (ping[7] == SK_REMOTE_TCAL_ESCAPE) {
		struct remote_tcal_result_event event = {
			.now_ms = now_ms,
			.session_generation = generation,
			.ping_serial = ping_serial,
			.tracker_id = tracker_id,
		};
		memcpy(event.ping, ping, sizeof(event.ping));
		/*
		 * On overflow leave the slot active.  The Tracker will repeat its
		 * RESULT PING and a later copy can be accepted.
		 */
		(void)k_msgq_put(&remote_tcal_result_msgq, &event, K_NO_WAIT);
	}
}

void remote_tcal_reset_tracker(uint8_t tracker_id)
{
	if (tracker_id >= MAX_TRACKERS) {
		return;
	}

	unsigned int key = irq_lock();
	capability_reset_locked(tracker_id);
	irq_unlock(key);
}

void remote_tcal_reset_all(void)
{
	for (uint8_t i = 0; i < MAX_TRACKERS; i++) {
		remote_tcal_reset_tracker(i);
	}
}

bool remote_tcal_tracker_pending(uint8_t tracker_id)
{
	return tracker_id < MAX_TRACKERS && tx_slots[tracker_id].active != 0U;
}

bool remote_tcal_prepare_legacy_command_locked(uint8_t tracker_id,
					       bool critical)
{
	if (tracker_id >= MAX_TRACKERS) {
		return false;
	}
	if (tx_slots[tracker_id].active == 0U) {
		return true;
	}
	if (!critical) {
		return false;
	}

	/*
	 * The caller publishes the critical legacy flag before releasing the same
	 * IRQ critical section.  The job deliberately remains outstanding with
	 * NO_RESPONSE for this member because execution of the old task is unknown.
	 */
	slot_clear_locked(tracker_id);
	return true;
}

static void remote_tcal_manager_thread(void)
{
	while (true) {
		k_msleep(REMOTE_TCAL_MANAGER_POLL_MS);
		uint32_t now = k_uptime_get_32();
		struct remote_tcal_result_event event;
		while (k_msgq_get(&remote_tcal_result_msgq, &event,
				  K_NO_WAIT) == 0) {
			process_result_event(&event);
		}

		for (uint8_t i = 0; i < MAX_TRACKERS; i++) {
			uint32_t last = capabilities[i].last_ping_ms;
			if (last != 0U &&
			    deadline_expired(
				    now,
				    last + REMOTE_TCAL_CAPABILITY_TIMEOUT_MS)) {
				unsigned int key = irq_lock();
				if (capabilities[i].last_ping_ms == last) {
					/*
					 * Timeout is a session boundary. Increment
					 * generation and clear the slot in the same
					 * critical section, unless a fresh PING won
					 * the stale-snapshot race.
					 */
					capability_reset_locked(i);
				}
				irq_unlock(key);
			}
		}

		struct remote_tcal_outcome outcome;
		remote_tcal_complete_cb_t callback = NULL;
		void *user_data = NULL;

		k_mutex_lock(&remote_tcal_lock, K_FOREVER);
		if (current_job.active &&
		    deadline_expired(now, current_job.deadline_ms)) {
			copy_outcome_locked(&outcome);
			callback = current_job.callback;
			user_data = current_job.user_data;
			clear_job_slots(current_job.considered_mask);
			memset(&current_job, 0, sizeof(current_job));
		}
		k_mutex_unlock(&remote_tcal_lock);

		invoke_completion(callback, user_data, &outcome);
	}
}

K_THREAD_DEFINE(remote_tcal_manager, 1024, remote_tcal_manager_thread,
		NULL, NULL, NULL, 6, 0, 0);

const char *remote_tcal_result_name(uint8_t result)
{
	static const char *const names[] = {
		"OK", "INVALID", "UNSUPPORTED", "BUSY", "POWER_REQUIRED",
		"NOT_READY", "TIMEOUT", "HARDWARE_ERROR", "NOT_ACTIVE",
		"INTERNAL", "CANCELED",
	};

	if (result <= SK_REMOTE_TCAL_RESULT_MAX) {
		return names[result];
	}
	if (result == REMOTE_TCAL_NO_RESPONSE) {
		return "NO_RESPONSE";
	}
	return "NOT_CONSIDERED";
}

#else

int remote_tcal_submit(uint8_t target, uint8_t action, int16_t target_centi_c,
		       remote_tcal_complete_cb_t callback, void *user_data,
		       uint16_t *transaction_id, uint16_t *considered_mask)
{
	ARG_UNUSED(target);
	ARG_UNUSED(action);
	ARG_UNUSED(target_centi_c);
	ARG_UNUSED(callback);
	ARG_UNUSED(user_data);
	ARG_UNUSED(transaction_id);
	ARG_UNUSED(considered_mask);
	return -ENOTSUP;
}

bool remote_tcal_fill_pong_isr(uint8_t tracker_id, const uint8_t ping[13],
			       uint32_t now_ms, uint8_t pong[13],
			       bool *force_normal)
{
	ARG_UNUSED(tracker_id);
	ARG_UNUSED(ping);
	ARG_UNUSED(now_ms);
	ARG_UNUSED(pong);
	if (force_normal != NULL) {
		*force_normal = false;
	}
	return false;
}

void remote_tcal_process_ping(uint8_t tracker_id, const uint8_t ping[13],
			      uint32_t now_ms)
{
	ARG_UNUSED(tracker_id);
	ARG_UNUSED(ping);
	ARG_UNUSED(now_ms);
}

void remote_tcal_reset_tracker(uint8_t tracker_id)
{
	ARG_UNUSED(tracker_id);
}

void remote_tcal_reset_all(void)
{
}

bool remote_tcal_tracker_pending(uint8_t tracker_id)
{
	ARG_UNUSED(tracker_id);
	return false;
}

bool remote_tcal_prepare_legacy_command_locked(uint8_t tracker_id,
					       bool critical)
{
	ARG_UNUSED(tracker_id);
	ARG_UNUSED(critical);
	return true;
}

const char *remote_tcal_result_name(uint8_t result)
{
	ARG_UNUSED(result);
	return "UNSUPPORTED";
}

#endif

#if defined(CONFIG_SK_REMOTE_HEATED_TCAL_TEST) && CONFIG_SK_REMOTE_HEATED_TCAL_TEST

struct hid_completion_context {
	struct k_spinlock lock;
	struct remote_tcal_outcome pending_outcome;
	uint32_t session_generation;
	uint8_t sequence;
	bool in_use;
	bool started_enqueued;
	bool completion_pending;
	bool suppress_final;
};

#define HID_COMPLETION_CONTEXT_COUNT 2U
static struct hid_completion_context
	hid_contexts[HID_COMPLETION_CONTEXT_COUNT];

static void put_status_nibbles(uint8_t ack[16],
			       const struct remote_tcal_outcome *outcome)
{
	for (uint8_t tracker = 0; tracker < MAX_TRACKERS; tracker++) {
		uint8_t value = outcome->status[tracker] & 0x0FU;
		uint8_t *packed = &ack[8 + tracker / 2U];
		if ((tracker & 1U) == 0U) {
			*packed = (*packed & 0xF0U) | value;
		} else {
			*packed = (*packed & 0x0FU) | (value << 4);
		}
	}
}

static int enqueue_hid_ack_with_retry(
	const uint8_t ack[16], uint32_t session_generation)
{
	int err = -ENOSPC;

	for (uint8_t attempt = 0U;
	     attempt < REMOTE_TCAL_HID_ACK_RETRY_COUNT; attempt++) {
		err = hid_control_ack_enqueue(
			ack, session_generation);
		if (err != -ENOSPC) {
			break;
		}
		k_msleep(REMOTE_TCAL_HID_ACK_RETRY_MS);
	}

	if (err != 0 && err != -ENOTCONN &&
	    err != -ESTALE) {
		LOG_ERR("Unable to enqueue remote T-Cal HID ACK: %d", err);
	}
	return err;
}

static void enqueue_hid_final_ack(
	const struct remote_tcal_outcome *outcome, uint8_t sequence,
	uint32_t session_generation)
{
	uint8_t ack[16];
	bool all_ok = outcome->reply_mask == outcome->considered_mask;

	for (uint8_t i = 0; i < MAX_TRACKERS; i++) {
		if ((outcome->considered_mask & BIT(i)) != 0U &&
		    outcome->status[i] != SK_REMOTE_TCAL_RESULT_OK) {
			all_ok = false;
		}
	}

	init_ack(ack, sequence,
		 all_ok ? RCV_HID_ST_OK : RCV_HID_ST_REMOTE_FAILED);
	sys_put_le16(outcome->considered_mask, &ack[4]);
	sys_put_le16(outcome->reply_mask, &ack[6]);
	put_status_nibbles(ack, outcome);
	(void)enqueue_hid_ack_with_retry(
		ack, session_generation);
}

static struct hid_completion_context *hid_context_acquire(
	uint8_t sequence, uint32_t session_generation)
{
	for (size_t i = 0; i < ARRAY_SIZE(hid_contexts); i++) {
		struct hid_completion_context *context = &hid_contexts[i];
		k_spinlock_key_t key = k_spin_lock(&context->lock);
			if (!context->in_use) {
				context->sequence = sequence;
				context->session_generation =
					session_generation;
				context->started_enqueued = false;
				context->completion_pending = false;
				context->suppress_final = false;
			context->in_use = true;
			k_spin_unlock(&context->lock, key);
			return context;
		}
		k_spin_unlock(&context->lock, key);
	}

	return NULL;
}

static void hid_context_release(struct hid_completion_context *context)
{
	k_spinlock_key_t key = k_spin_lock(&context->lock);
	context->in_use = false;
	context->started_enqueued = false;
	context->completion_pending = false;
	context->suppress_final = false;
	context->session_generation = 0U;
	k_spin_unlock(&context->lock, key);
}

static void hid_context_finish_started(
	struct hid_completion_context *context,
	bool started_enqueued)
{
	struct remote_tcal_outcome pending_outcome;
	bool completion_pending;
	uint8_t sequence;
	uint32_t session_generation;
	uint32_t current_generation =
		hid_control_session_generation();

	k_spinlock_key_t key = k_spin_lock(&context->lock);
	context->started_enqueued = started_enqueued;
	completion_pending = context->completion_pending;
	sequence = context->sequence;
	session_generation = context->session_generation;
	bool current_session =
		context->in_use &&
		session_generation == current_generation;
	if (!started_enqueued || !current_session) {
		context->suppress_final = true;
	}
	if (completion_pending || context->suppress_final) {
		/*
		 * If the wireless callback has not happened yet, an invalidated
		 * context remains reserved until that callback releases its
		 * pointer.  This prevents a new USB session from reusing it.
		 */
		if (completion_pending) {
			context->in_use = false;
			pending_outcome = context->pending_outcome;
		}
		context->completion_pending = false;
	}
	k_spin_unlock(&context->lock, key);

	if (completion_pending && current_session &&
	    started_enqueued) {
		enqueue_hid_final_ack(
			&pending_outcome, sequence,
			session_generation);
	}
}

static void hid_remote_tcal_complete(
	const struct remote_tcal_outcome *outcome, void *user_data)
{
	struct hid_completion_context *context = user_data;
	uint8_t sequence;
	uint32_t session_generation;
	uint32_t current_generation =
		hid_control_session_generation();

	k_spinlock_key_t key = k_spin_lock(&context->lock);
	session_generation = context->session_generation;
	if (!context->in_use ||
	    session_generation != current_generation ||
	    context->suppress_final) {
		context->in_use = false;
		context->completion_pending = false;
		k_spin_unlock(&context->lock, key);
		return;
	}
	if (!context->started_enqueued) {
		context->pending_outcome = *outcome;
		context->completion_pending = true;
		k_spin_unlock(&context->lock, key);
		return;
	}
	sequence = context->sequence;
	context->in_use = false;
	k_spin_unlock(&context->lock, key);

	enqueue_hid_final_ack(
		outcome, sequence, session_generation);
}

void remote_tcal_hid_handle_report(
	const uint8_t *report, size_t len,
	uint32_t session_generation)
{
	if (report == NULL || len < 3U ||
	    report[0] != RCV_HID_TYPE_CMD ||
	    report[2] != RCV_HID_OP_REMOTE_HEATED_TCAL ||
	    session_generation !=
		    hid_control_session_generation()) {
		return;
	}

	uint8_t sequence = report[1];
	uint8_t ack[16];
	if (len < 7U) {
		init_ack(ack, sequence, RCV_HID_ST_EINVAL);
		(void)enqueue_hid_ack_with_retry(
			ack, session_generation);
		return;
	}
	if ((report[3] != REMOTE_TCAL_TARGET_ALL &&
	     report[3] >= MAX_TRACKERS) ||
	    report[4] < SK_REMOTE_TCAL_ACTION_START ||
	    report[4] > SK_REMOTE_TCAL_ACTION_ABORT) {
		init_ack(ack, sequence, RCV_HID_ST_EINVAL);
		(void)enqueue_hid_ack_with_retry(
			ack, session_generation);
		return;
	}

	int16_t target_centi_c = (int16_t)sys_get_le16(&report[5]);
	if (report[4] != SK_REMOTE_TCAL_ACTION_START &&
	    target_centi_c != 0) {
		init_ack(ack, sequence, RCV_HID_ST_EINVAL);
		(void)enqueue_hid_ack_with_retry(
			ack, session_generation);
		return;
	}

	/*
	 * A second context lets STOP/ABORT replace an active HID START without
	 * overwriting the old callback's host sequence.
	 */
	struct hid_completion_context *context =
		hid_context_acquire(
			sequence, session_generation);
	if (context == NULL) {
		init_ack(ack, sequence, RCV_HID_ST_EBUSY);
		(void)enqueue_hid_ack_with_retry(
			ack, session_generation);
		return;
	}
	uint16_t considered;
	int err = remote_tcal_submit(
		report[3], report[4], target_centi_c,
		hid_remote_tcal_complete, context,
		NULL, &considered);
	if (err != 0) {
		hid_context_release(context);
		init_ack(ack, sequence, errno_to_hid_status(err));
		(void)enqueue_hid_ack_with_retry(
			ack, session_generation);
		return;
	}

	init_ack(ack, sequence, RCV_HID_ST_STARTED);
	sys_put_le16(considered, &ack[4]);
	sys_put_le16(0U, &ack[6]);
	for (uint8_t i = 0; i < MAX_TRACKERS; i++) {
		uint8_t value = (considered & BIT(i)) != 0U
					? REMOTE_TCAL_NO_RESPONSE
					: REMOTE_TCAL_NOT_CONSIDERED;
		uint8_t *packed = &ack[8 + i / 2U];
		if ((i & 1U) == 0U) {
			*packed = (*packed & 0xF0U) | value;
		} else {
			*packed = (*packed & 0x0FU) | (value << 4);
		}
	}
	int started_err = enqueue_hid_ack_with_retry(
		ack, session_generation);
	/*
	 * If completion raced submission, release it only after STARTED is in
	 * the priority FIFO so the host always observes the required ordering.
	 * If STARTED could not be enqueued, suppress this session's final ACK;
	 * a final without STARTED would be ambiguous to the host.
	 */
	hid_context_finish_started(context, started_err == 0);
}

#else

void remote_tcal_hid_handle_report(
	const uint8_t *report, size_t len,
	uint32_t session_generation)
{
	ARG_UNUSED(report);
	ARG_UNUSED(len);
	ARG_UNUSED(session_generation);
}

#endif
