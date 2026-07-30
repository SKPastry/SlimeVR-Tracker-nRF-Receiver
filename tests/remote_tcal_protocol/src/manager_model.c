#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "remote_tcal_protocol.h"

#define MODEL_TRACKER_COUNT 16U
#define MODEL_NO_RESPONSE 0xEU
#define MODEL_NOT_CONSIDERED 0xFU
#define START_CONFIRM_MIN_MS 1500U
#define START_CONFIRM_FRESH_MS 2000U
#define START_WAIT_MAX_MS 4000U
#define RECEIVER_DEADLINE_MS 5000U
#define CAPABILITY_START_FRESH_MS 2500U
#define CAPABILITY_TIMEOUT_MS 5000U
#define PING_DUPLICATE_SESSION_RESET_MS 250U

enum tracker_model_state {
	TRACKER_MODEL_IDLE,
	TRACKER_MODEL_WAIT_START,
	TRACKER_MODEL_SENSOR_EXEC,
	TRACKER_MODEL_RESULT_READY,
};

struct model_command {
	uint16_t transaction;
	int16_t target_centi_c;
	uint8_t action;
};

struct tracker_model {
	enum tracker_model_state state;
	struct model_command command;
	struct model_command completed_command;
	uint32_t first_ms;
	uint32_t last_ms;
	uint8_t matches;
	uint8_t result;
	uint8_t completed_result;
	uint8_t executions;
	bool completed_valid;
};

struct receiver_outcome_model {
	uint16_t considered_mask;
	uint16_t reply_mask;
	uint8_t status[MODEL_TRACKER_COUNT];
};

struct receiver_model {
	bool active;
	bool slot_active[MODEL_TRACKER_COUNT];
	bool saved_before_withdraw;
	bool ping_counter_valid[MODEL_TRACKER_COUNT];
	bool sent_valid[MODEL_TRACKER_COUNT];
	bool start_counter_valid[MODEL_TRACKER_COUNT];
	uint8_t ping_last_counter[MODEL_TRACKER_COUNT];
	uint8_t start_last_counter[MODEL_TRACKER_COUNT];
	uint8_t start_confirm_count[MODEL_TRACKER_COUNT];
	uint32_t ping_serial[MODEL_TRACKER_COUNT];
	uint32_t last_sent_serial[MODEL_TRACKER_COUNT];
	uint32_t start_first_sent_ms[MODEL_TRACKER_COUNT];
	uint16_t transaction;
	uint16_t considered_mask;
	uint16_t reply_mask;
	uint32_t deadline_ms;
	uint8_t action;
	uint8_t status[MODEL_TRACKER_COUNT];
};

struct hid_ack_gate_model {
	bool started_enqueued;
	bool final_ready;
	bool final_enqueued;
};

struct capability_model {
	uint32_t last_ping_ms;
	uint32_t generation;
	uint8_t supported;
	uint8_t status;
	bool slot_active;
};

struct command_collision_model {
	bool extension_slot;
	bool generic_command;
	bool generic_critical;
};

enum collision_pong_selection {
	COLLISION_PONG_NORMAL,
	COLLISION_PONG_EXTENSION,
	COLLISION_PONG_ORDINARY_LEGACY,
	COLLISION_PONG_CRITICAL_LEGACY,
};

struct causal_result_event {
	uint32_t generation;
	uint32_t serial;
};

struct causal_result_token {
	uint32_t generation;
	uint16_t transaction;
	uint8_t action;
	bool preliminary_valid;
};

struct causal_receiver_model {
	uint32_t capability_generation;
	uint32_t job_generation;
	uint32_t slot_generation;
	uint32_t ping_serial;
	uint32_t first_sent_ms;
	uint32_t last_sent_serial;
	uint16_t transaction;
	uint8_t action;
	uint8_t last_counter;
	uint8_t attempt_counter;
	uint8_t confirm_count;
	bool counter_valid;
	bool attempt_valid;
	bool sent_valid;
	bool slot_active;
	bool replied;
};

struct selection_snapshot_model {
	uint32_t generation;
	uint32_t last_ping_ms;
	bool supported;
};

struct hid_session_model {
	uint32_t current_generation;
	uint32_t context_generation;
	bool context_in_use;
	bool completion_pending;
	bool suppress_final;
	bool final_enqueued;
};

static bool deadline_expired(uint32_t now, uint32_t deadline)
{
	return (int32_t)(now - deadline) >= 0;
}

static bool serial_after(uint32_t value, uint32_t previous)
{
	return (int32_t)(value - previous) > 0;
}

static bool ping_counter_starts_new_session(
	bool initialized, uint8_t last_counter, uint8_t counter,
	uint64_t last_ping_ms, uint64_t now_ms)
{
	if (!initialized) {
		return false;
	}
	return counter < last_counter ||
	       (counter == last_counter && last_ping_ms > 0U &&
		now_ms - last_ping_ms > PING_DUPLICATE_SESSION_RESET_MS);
}

static bool data_sequence_starts_new_session(int sequence_result)
{
	return sequence_result == 3;
}

static bool command_equal(const struct model_command *left,
			  const struct model_command *right)
{
	return left->transaction == right->transaction &&
	       left->action == right->action &&
	       left->target_centi_c == right->target_centi_c;
}

static bool decode_command(const uint8_t pong[13],
			   struct model_command *command)
{
	if (pong[3] != SK_REMOTE_TCAL_MAGIC_0 ||
	    pong[4] != SK_REMOTE_TCAL_MAGIC_1 ||
	    pong[5] != SK_REMOTE_TCAL_VERSION ||
	    pong[7] != SK_REMOTE_TCAL_ESCAPE ||
	    pong[6] < SK_REMOTE_TCAL_ACTION_START ||
	    pong[6] > SK_REMOTE_TCAL_ACTION_ABORT) {
		return false;
	}

	command->transaction = sys_get_be16(&pong[8]);
	command->target_centi_c = (int16_t)sys_get_be16(&pong[10]);
	command->action = pong[6];
	return command->action == SK_REMOTE_TCAL_ACTION_START ||
	       command->target_centi_c == 0;
}

static void tracker_publish_result(struct tracker_model *model,
				   uint8_t result)
{
	model->result = result;
	model->completed_command = model->command;
	model->completed_result = result;
	model->completed_valid = true;
	model->state = TRACKER_MODEL_RESULT_READY;
}

static bool tracker_receive(struct tracker_model *model,
			    const uint8_t pong[13], uint32_t now_ms)
{
	struct model_command incoming;

	if (!decode_command(pong, &incoming)) {
		return false;
	}

	if (model->state == TRACKER_MODEL_IDLE) {
		if (model->completed_valid &&
		    incoming.transaction ==
			    model->completed_command.transaction) {
			model->command = incoming;
			if (command_equal(&incoming,
					  &model->completed_command)) {
				model->result = model->completed_result;
			} else {
				model->result =
					SK_REMOTE_TCAL_RESULT_INVALID;
			}
			model->state = TRACKER_MODEL_RESULT_READY;
			return true;
		}

		model->command = incoming;
		model->first_ms = now_ms;
		model->last_ms = now_ms;
		model->matches = 1U;
		model->state =
			incoming.action == SK_REMOTE_TCAL_ACTION_START
				? TRACKER_MODEL_WAIT_START
				: TRACKER_MODEL_SENSOR_EXEC;
		return true;
	}

	if (incoming.transaction == model->command.transaction) {
		if (!command_equal(&incoming, &model->command)) {
			model->command = incoming;
			tracker_publish_result(
				model, SK_REMOTE_TCAL_RESULT_INVALID);
		} else if (model->state == TRACKER_MODEL_WAIT_START) {
			model->matches++;
			model->last_ms = now_ms;
		}
		return true;
	}

	if (model->state == TRACKER_MODEL_WAIT_START &&
	    incoming.action != SK_REMOTE_TCAL_ACTION_START) {
		/* A safety action supersedes even an old, unpolled START. */
		model->command = incoming;
		model->first_ms = now_ms;
		model->last_ms = now_ms;
		model->matches = 1U;
		model->state = TRACKER_MODEL_SENSOR_EXEC;
		return true;
	}

	return false;
}

static void tracker_poll(struct tracker_model *model, uint32_t now_ms)
{
	if (model->state != TRACKER_MODEL_WAIT_START) {
		return;
	}

	if ((uint32_t)(now_ms - model->first_ms) >= START_WAIT_MAX_MS) {
		tracker_publish_result(
			model, SK_REMOTE_TCAL_RESULT_TIMEOUT);
		return;
	}

	if (model->matches >= 2U &&
	    (uint32_t)(now_ms - model->first_ms) >=
		    START_CONFIRM_MIN_MS &&
	    (uint32_t)(now_ms - model->last_ms) <=
		    START_CONFIRM_FRESH_MS) {
		model->state = TRACKER_MODEL_SENSOR_EXEC;
	}
}

static void tracker_sensor_complete(struct tracker_model *model,
				    uint8_t result)
{
	zassert_equal(model->state, TRACKER_MODEL_SENSOR_EXEC);
	model->executions++;
	tracker_publish_result(model, result);
}

static uint8_t tracker_ping_flag(const struct tracker_model *model)
{
	return model->state == TRACKER_MODEL_RESULT_READY
		       ? SK_REMOTE_TCAL_ESCAPE
		       : 0U;
}

static void tracker_receive_normal_pong(struct tracker_model *model)
{
	if (model->state == TRACKER_MODEL_RESULT_READY) {
		model->state = TRACKER_MODEL_IDLE;
	}
}

static void receiver_begin(struct receiver_model *model,
			   uint16_t transaction, uint16_t considered_mask,
			   uint8_t action, uint32_t now_ms)
{
	memset(model, 0, sizeof(*model));
	model->active = true;
	model->transaction = transaction;
	model->considered_mask = considered_mask;
	model->deadline_ms = now_ms + RECEIVER_DEADLINE_MS;
	model->action = action;
	memset(model->status, MODEL_NOT_CONSIDERED,
	       sizeof(model->status));

	for (uint8_t i = 0; i < MODEL_TRACKER_COUNT; i++) {
		if ((considered_mask & (1U << i)) != 0U) {
			model->slot_active[i] = true;
			model->status[i] = MODEL_NO_RESPONSE;
		}
	}
}

static bool receiver_slot_can_send(struct receiver_model *model,
				   uint8_t tracker_id, uint32_t now_ms,
				   const uint8_t current_ping[13])
{
	if (tracker_id >= MODEL_TRACKER_COUNT ||
	    !model->slot_active[tracker_id] ||
	    deadline_expired(now_ms, model->deadline_ms)) {
		return false;
	}

	bool normal =
		sk_remote_tcal_normal_capability(current_ping, NULL);
	bool result =
		sk_remote_tcal_parse_result(current_ping,
					    model->transaction, NULL, NULL);
	bool distinct =
		!model->ping_counter_valid[tracker_id] ||
		model->ping_last_counter[tracker_id] != current_ping[2];

	if (distinct) {
		model->ping_serial[tracker_id]++;
		if (model->ping_serial[tracker_id] == 0U) {
			model->ping_serial[tracker_id] = 1U;
		}
		model->ping_last_counter[tracker_id] = current_ping[2];
		model->ping_counter_valid[tracker_id] = true;
	}

	if (normal && distinct) {
		model->last_sent_serial[tracker_id] =
			model->ping_serial[tracker_id];
		model->sent_valid[tracker_id] = true;
	}

	if (normal && distinct &&
	    model->action == SK_REMOTE_TCAL_ACTION_START) {
		uint8_t counter = current_ping[2];

		if (!model->start_counter_valid[tracker_id]) {
			model->start_counter_valid[tracker_id] = true;
			model->start_last_counter[tracker_id] = counter;
			model->start_confirm_count[tracker_id] = 1U;
			model->start_first_sent_ms[tracker_id] = now_ms;
		} else if (model->start_confirm_count[tracker_id] < 2U &&
			   model->start_last_counter[tracker_id] != counter) {
			model->start_last_counter[tracker_id] = counter;
			model->start_confirm_count[tracker_id]++;
		}
	}

	return normal || result;
}

static bool receiver_accept_result(struct receiver_model *model,
				   uint8_t tracker_id,
				   const uint8_t result_ping[13],
				   uint32_t captured_ms)
{
	uint8_t result;
	uint8_t status;

	if (tracker_id >= MODEL_TRACKER_COUNT) {
		return false;
	}

	/*
	 * Model the sequence-validated event path independently of whether the
	 * manager later accepts this result.  A duplicate result keeps the same
	 * serial, just like production.
	 */
	if (!model->ping_counter_valid[tracker_id] ||
	    model->ping_last_counter[tracker_id] != result_ping[2]) {
		model->ping_serial[tracker_id]++;
		if (model->ping_serial[tracker_id] == 0U) {
			model->ping_serial[tracker_id] = 1U;
		}
		model->ping_last_counter[tracker_id] = result_ping[2];
		model->ping_counter_valid[tracker_id] = true;
	}

	if (!model->active ||
	    (model->considered_mask & (1U << tracker_id)) == 0U ||
	    (model->reply_mask & (1U << tracker_id)) != 0U ||
	    deadline_expired(captured_ms, model->deadline_ms) ||
	    !sk_remote_tcal_parse_result(
		    result_ping, model->transaction, &result, &status)) {
		return false;
	}
	if (!model->sent_valid[tracker_id] ||
	    !serial_after(model->ping_serial[tracker_id],
			  model->last_sent_serial[tracker_id])) {
		return false;
	}
	if (model->action == SK_REMOTE_TCAL_ACTION_START &&
	    result == SK_REMOTE_TCAL_RESULT_OK &&
	    (!model->start_counter_valid[tracker_id] ||
	     model->start_confirm_count[tracker_id] < 2U ||
	     (uint32_t)(captured_ms -
			model->start_first_sent_ms[tracker_id]) <
		     START_CONFIRM_MIN_MS ||
	     (status & SK_REMOTE_TCAL_STATUS_ACTIVE) == 0U)) {
		return false;
	}
	if (model->action != SK_REMOTE_TCAL_ACTION_START &&
	    result == SK_REMOTE_TCAL_RESULT_OK &&
	    (status & SK_REMOTE_TCAL_STATUS_ACTIVE) != 0U) {
		return false;
	}

	/* Save the structured result before withdrawing the outgoing slot. */
	model->status[tracker_id] = result;
	model->reply_mask |= 1U << tracker_id;
	model->saved_before_withdraw = model->slot_active[tracker_id];
	model->slot_active[tracker_id] = false;
	if (model->reply_mask == model->considered_mask) {
		model->active = false;
	}
	return true;
}

static bool receiver_timeout(struct receiver_model *model, uint32_t now_ms,
			     struct receiver_outcome_model *outcome)
{
	if (!model->active ||
	    !deadline_expired(now_ms, model->deadline_ms)) {
		return false;
	}

	outcome->considered_mask = model->considered_mask;
	outcome->reply_mask = model->reply_mask;
	memcpy(outcome->status, model->status, sizeof(outcome->status));
	memset(model->slot_active, 0, sizeof(model->slot_active));
	model->active = false;
	return true;
}

static uint16_t receiver_override_start(
	struct receiver_model *old_job, uint16_t requested_mask,
	struct receiver_outcome_model *old_outcome,
	uint16_t replacement_transaction, uint8_t replacement_action,
	bool *callback_saw_replacement)
{
	if (!old_job->active ||
	    old_job->action != SK_REMOTE_TCAL_ACTION_START ||
	    (old_job->considered_mask & requested_mask) == 0U) {
		return requested_mask;
	}

	uint16_t expanded = requested_mask | old_job->considered_mask;
	old_outcome->considered_mask = old_job->considered_mask;
	old_outcome->reply_mask = old_job->reply_mask;
	memcpy(old_outcome->status, old_job->status,
	       sizeof(old_outcome->status));
	/*
	 * Model the production critical transaction: overwrite the old slots
	 * directly with the replacement, then release the lock and callback.
	 * There is no observable slot-inactive interval.
	 */
	receiver_begin(old_job, replacement_transaction, expanded,
		       replacement_action, 500U);
	*callback_saw_replacement =
		old_job->active &&
		old_job->considered_mask == expanded;
	return expanded;
}

static bool collision_publish_start(
	struct command_collision_model *model)
{
	/* One simulated IRQ critical section: check generic, then publish. */
	if (model->generic_command) {
		return false;
	}
	model->extension_slot = true;
	return true;
}

static bool collision_publish_generic(
	struct command_collision_model *model)
{
	/* The inverse ordering uses the same simulated critical section. */
	if (model->extension_slot) {
		return false;
	}
	model->generic_command = true;
	model->generic_critical = false;
	return true;
}

static bool collision_publish_critical_generic(
	struct command_collision_model *model)
{
	/*
	 * One simulated IRQ critical section: withdraw the extension first, then
	 * publish the critical legacy flag without an observable empty interval.
	 */
	model->extension_slot = false;
	model->generic_command = true;
	model->generic_critical = true;
	return true;
}

static bool collision_publish_safety_extension(
	struct command_collision_model *model)
{
	/* Safety extensions may pass ordinary legacy work, never critical work. */
	if (model->generic_critical) {
		return false;
	}
	model->extension_slot = true;
	return true;
}

static enum collision_pong_selection collision_select_pong(
	const struct command_collision_model *model, bool result_ping)
{
	if (model->extension_slot) {
		return COLLISION_PONG_EXTENSION;
	}
	/*
	 * A latched result needs NORMAL before ordinary legacy work can reach the
	 * Tracker. Critical lifecycle commands retain their safety preemption.
	 */
	if (result_ping && !model->generic_critical) {
		return COLLISION_PONG_NORMAL;
	}
	if (model->generic_command) {
		return model->generic_critical
			       ? COLLISION_PONG_CRITICAL_LEGACY
			       : COLLISION_PONG_ORDINARY_LEGACY;
	}
	return COLLISION_PONG_NORMAL;
}

static void causal_receiver_begin(
	struct causal_receiver_model *model, uint16_t transaction,
	uint8_t action, uint32_t generation)
{
	memset(model, 0, sizeof(*model));
	model->capability_generation = generation;
	model->job_generation = generation;
	model->slot_generation = generation;
	model->transaction = transaction;
	model->action = action;
	model->slot_active = true;
}

static void causal_attempt_normal(
	struct causal_receiver_model *model, uint8_t counter)
{
	model->attempt_counter = counter;
	model->attempt_valid = true;
}

static struct causal_result_event causal_process_validated_ping(
	struct causal_receiver_model *model, uint8_t counter,
	bool normal_capability, uint32_t now_ms)
{
	bool distinct =
		!model->counter_valid ||
		model->last_counter != counter;
	if (distinct) {
		model->ping_serial++;
		if (model->ping_serial == 0U) {
			model->ping_serial = 1U;
		}
		model->last_counter = counter;
		model->counter_valid = true;
	}

	if (normal_capability && distinct && model->slot_active &&
	    model->attempt_valid &&
	    model->attempt_counter == counter) {
		if (!model->sent_valid) {
			model->first_sent_ms = now_ms;
			model->confirm_count = 1U;
		} else if (serial_after(
				   model->ping_serial,
				   model->last_sent_serial)) {
			model->confirm_count++;
		}
		model->last_sent_serial = model->ping_serial;
		model->sent_valid = true;
	}

	return (struct causal_result_event) {
		.generation = model->capability_generation,
		.serial = model->ping_serial,
	};
}

static struct causal_result_token causal_precheck_result(
	const struct causal_receiver_model *model,
	const struct causal_result_event *event,
	const uint8_t result_ping[13], uint32_t now_ms)
{
	struct causal_result_token token = {
		.generation = event->generation,
		.transaction = model->transaction,
		.action = model->action,
	};
	uint8_t result;
	uint8_t status;

	if (!model->slot_active ||
	    event->generation != model->job_generation ||
	    model->slot_generation != model->job_generation ||
	    !sk_remote_tcal_parse_result(
		    result_ping, model->transaction, &result, &status)) {
		return token;
	}

	if (!model->sent_valid ||
	    !serial_after(event->serial, model->last_sent_serial)) {
		return token;
	}

	if (model->action == SK_REMOTE_TCAL_ACTION_START &&
	    result == SK_REMOTE_TCAL_RESULT_OK) {
		if (model->confirm_count < 2U ||
		    (uint32_t)(now_ms - model->first_sent_ms) <
			    START_CONFIRM_MIN_MS ||
		    (status & SK_REMOTE_TCAL_STATUS_ACTIVE) == 0U) {
			return token;
		}
	} else if (model->action != SK_REMOTE_TCAL_ACTION_START) {
		if (result == SK_REMOTE_TCAL_RESULT_OK &&
		    (status & SK_REMOTE_TCAL_STATUS_ACTIVE) != 0U) {
			return token;
		}
	}

	token.preliminary_valid = true;
	return token;
}

static bool causal_commit_result(
	struct causal_receiver_model *model,
	const struct causal_result_token *token)
{
	if (!token->preliminary_valid ||
	    model->capability_generation != token->generation ||
	    model->slot_generation != token->generation ||
	    model->transaction != token->transaction ||
	    model->action != token->action ||
	    !model->slot_active) {
		return false;
	}

	model->replied = true;
	model->slot_active = false;
	return true;
}

static void causal_reset_tracker(
	struct causal_receiver_model *model)
{
	model->capability_generation++;
	if (model->capability_generation == 0U) {
		model->capability_generation = 1U;
	}
	model->slot_active = false;
	model->sent_valid = false;
}

static struct selection_snapshot_model selection_snapshot(
	const struct capability_model *capability)
{
	return (struct selection_snapshot_model) {
		.generation = capability->generation,
		.last_ping_ms = capability->last_ping_ms,
		.supported = capability->supported != 0U,
	};
}

static bool capability_age_eligible_model(uint32_t last_ping_ms,
					  uint32_t now_ms,
					  uint8_t action)
{
	if (last_ping_ms == 0U) {
		return false;
	}

	uint32_t age = now_ms - last_ping_ms;

	return action == SK_REMOTE_TCAL_ACTION_START
		       ? age <= CAPABILITY_START_FRESH_MS
		       : age < CAPABILITY_TIMEOUT_MS;
}

static bool selection_publish_if_same_generation_eligible(
	struct capability_model *capability,
	const struct selection_snapshot_model *snapshot,
	uint32_t now_ms, uint8_t action)
{
	if (!snapshot->supported ||
	    capability->generation != snapshot->generation ||
	    capability->supported == 0U ||
	    !capability_age_eligible_model(
		    capability->last_ping_ms, now_ms, action)) {
		return false;
	}

	capability->slot_active = true;
	return true;
}

static void hid_session_acquire(struct hid_session_model *model)
{
	model->context_generation = model->current_generation;
	model->context_in_use = true;
	model->suppress_final = false;
}

static void hid_session_disconnect(struct hid_session_model *model)
{
	model->current_generation++;
	if (model->current_generation == 0U) {
		model->current_generation = 1U;
	}
}

static void hid_session_finish_started(
	struct hid_session_model *model, bool enqueue_succeeded)
{
	if (!enqueue_succeeded ||
	    model->context_generation != model->current_generation) {
		model->suppress_final = true;
	}
	if (model->completion_pending) {
		model->context_in_use = false;
		model->completion_pending = false;
		if (!model->suppress_final) {
			model->final_enqueued = true;
		}
	}
}

static void hid_session_complete(struct hid_session_model *model)
{
	if (!model->context_in_use) {
		return;
	}
	if (model->suppress_final ||
	    model->context_generation != model->current_generation) {
		model->context_in_use = false;
		return;
	}

	model->completion_pending = true;
}

static bool control_ack_snapshot_can_commit(
	uint32_t snapshot_generation, uint32_t current_generation,
	uint8_t snapshot_read, uint8_t current_read)
{
	return snapshot_generation == current_generation &&
	       snapshot_read == current_read;
}

static int hid_request_disposition(size_t len, uint8_t type,
				   uint8_t opcode)
{
	if (len < 3U || type != 254U || opcode != 0xC8U) {
		return -1;
	}
	return len < 7U ? 1 : 0;
}

static void capability_apply_result_status(
	struct capability_model *capability, uint8_t status)
{
	/* A result updates status only, never NORMAL capability freshness. */
	capability->status = status;
}

static void hid_final_result_ready(struct hid_ack_gate_model *gate)
{
	gate->final_ready = true;
	gate->final_enqueued = gate->started_enqueued;
}

static void hid_started_enqueue_succeeded(struct hid_ack_gate_model *gate)
{
	gate->started_enqueued = true;
	if (gate->final_ready) {
		gate->final_enqueued = true;
	}
}

static bool capability_clear_if_still_stale(
	struct capability_model *capability, uint32_t stale_snapshot)
{
	if (capability->last_ping_ms != stale_snapshot) {
		return false;
	}

	capability->last_ping_ms = 0U;
	capability->supported = 0U;
	capability->status = 0U;
	capability->generation++;
	capability->slot_active = false;
	return true;
}

static uint16_t allocate_transaction(uint16_t *next_transaction)
{
	uint16_t allocated = *next_transaction;

	(*next_transaction)++;
	if (*next_transaction == 0U) {
		*next_transaction = 1U;
	}
	return allocated;
}

static void make_capability_ping(uint8_t ping[13])
{
	memset(ping, 0, 13);
	ping[8] = SK_REMOTE_TCAL_MAGIC_0;
	ping[9] = SK_REMOTE_TCAL_MAGIC_1;
	ping[10] = SK_REMOTE_TCAL_VERSION;
	ping[11] = SK_REMOTE_TCAL_CAP_SUPPORTED;
}

static void make_result_ping(uint8_t ping[13], uint16_t transaction,
			     uint8_t result)
{
	memset(ping, 0, 13);
	ping[7] = SK_REMOTE_TCAL_ESCAPE;
	sys_put_be16(transaction, &ping[8]);
	ping[10] = SK_REMOTE_TCAL_RESULT_MARKER | result;
	ping[11] = SK_REMOTE_TCAL_CAP_SUPPORTED;
}

static void make_result_ping_with_status(uint8_t ping[13],
					 uint16_t transaction,
					 uint8_t result,
					 uint8_t status)
{
	make_result_ping(ping, transaction, result);
	ping[11] = status;
}

ZTEST(remote_tcal_manager_model,
      test_start_needs_two_fresh_confirms_and_no_early_result)
{
	struct tracker_model model = {0};
	uint8_t pong[13] = {0};

	sk_remote_tcal_encode_pong(
		pong, 0x1234, SK_REMOTE_TCAL_ACTION_START,
		SK_REMOTE_TCAL_DEFAULT_CENTI_C);
	zassert_true(tracker_receive(&model, pong, 1000U));
	zassert_equal(model.state, TRACKER_MODEL_WAIT_START);
	zassert_equal(tracker_ping_flag(&model), 0);

	zassert_true(tracker_receive(&model, pong, 1100U));
	tracker_poll(&model, 2499U);
	zassert_equal(model.state, TRACKER_MODEL_WAIT_START);
	zassert_equal(tracker_ping_flag(&model), 0);

	tracker_poll(&model, 2500U);
	zassert_equal(model.state, TRACKER_MODEL_SENSOR_EXEC);
	zassert_equal(tracker_ping_flag(&model), 0,
		      "execution must finish before a result PING");

	tracker_sensor_complete(&model, SK_REMOTE_TCAL_RESULT_OK);
	zassert_equal(tracker_ping_flag(&model), SK_REMOTE_TCAL_ESCAPE);
	zassert_equal(model.executions, 1);

	/* Result remains ready until NORMAL PONG completes the handshake. */
	zassert_true(tracker_receive(&model, pong, 2600U));
	zassert_equal(model.executions, 1);
	zassert_equal(tracker_ping_flag(&model), SK_REMOTE_TCAL_ESCAPE);
	tracker_receive_normal_pong(&model);
	zassert_equal(model.state, TRACKER_MODEL_IDLE);

	/* A late retry of the completed payload repeats, never re-executes. */
	zassert_true(tracker_receive(&model, pong, 2700U));
	zassert_equal(model.state, TRACKER_MODEL_RESULT_READY);
	zassert_equal(model.result, SK_REMOTE_TCAL_RESULT_OK);
	zassert_equal(model.executions, 1);
}

ZTEST(remote_tcal_manager_model,
      test_start_timeout_and_transaction_payload_mismatch)
{
	struct tracker_model model = {0};
	uint8_t pong[13] = {0};

	sk_remote_tcal_encode_pong(
		pong, 0x4567, SK_REMOTE_TCAL_ACTION_START, 2500);
	zassert_true(tracker_receive(&model, pong, 100U));
	tracker_poll(&model, 4100U);
	zassert_equal(model.state, TRACKER_MODEL_RESULT_READY);
	zassert_equal(model.result, SK_REMOTE_TCAL_RESULT_TIMEOUT);
	zassert_equal(model.executions, 0);

	tracker_receive_normal_pong(&model);
	zassert_true(tracker_receive(&model, pong, 5000U));
	zassert_equal(model.result, SK_REMOTE_TCAL_RESULT_TIMEOUT,
		      "timed-out START must not start later");
	zassert_equal(model.executions, 0);

	tracker_receive_normal_pong(&model);
	sk_remote_tcal_encode_pong(
		pong, 0x4567, SK_REMOTE_TCAL_ACTION_START, 2600);
	zassert_true(tracker_receive(&model, pong, 5100U));
	zassert_equal(model.result, SK_REMOTE_TCAL_RESULT_INVALID);
	zassert_equal(model.executions, 0);
}

ZTEST(remote_tcal_manager_model,
      test_stale_start_is_preempted_by_one_safety_command)
{
	struct tracker_model model = {0};
	uint8_t pong[13] = {0};

	sk_remote_tcal_encode_pong(
		pong, 10, SK_REMOTE_TCAL_ACTION_START, 2500);
	zassert_true(tracker_receive(&model, pong, 1000U));

	/* No poll ran at the old START deadline; ABORT still supersedes it. */
	sk_remote_tcal_encode_pong(
		pong, 11, SK_REMOTE_TCAL_ACTION_ABORT, 0);
	zassert_true(tracker_receive(&model, pong, 5101U));
	zassert_equal(model.state, TRACKER_MODEL_SENSOR_EXEC);
	zassert_equal(tracker_ping_flag(&model), 0);
	tracker_sensor_complete(
		&model, SK_REMOTE_TCAL_RESULT_NOT_ACTIVE);
	zassert_equal(model.executions, 1);
	zassert_equal(model.result, SK_REMOTE_TCAL_RESULT_NOT_ACTIVE);
	zassert_equal(tracker_ping_flag(&model), SK_REMOTE_TCAL_ESCAPE);
}

ZTEST(remote_tcal_manager_model,
      test_receiver_saves_result_before_withdrawing_command)
{
	struct receiver_model model;
	uint8_t ping[13];

	receiver_begin(&model, 0xBEEF, 1U << 3,
		       SK_REMOTE_TCAL_ACTION_START, 100U);
	make_capability_ping(ping);
	zassert_true(receiver_slot_can_send(&model, 3, 200U, ping));

	make_result_ping(
		ping, 0xBEEF, SK_REMOTE_TCAL_RESULT_POWER_REQUIRED);
	ping[2] = 1U;
	zassert_true(receiver_slot_can_send(&model, 3, 300U, ping));
	zassert_true(receiver_accept_result(&model, 3, ping, 300U));
	zassert_true(model.saved_before_withdraw);
	zassert_equal(model.reply_mask, 1U << 3);
	zassert_equal(model.status[3],
		      SK_REMOTE_TCAL_RESULT_POWER_REQUIRED);
	zassert_false(model.slot_active[3]);
	zassert_false(model.active);
	zassert_false(receiver_accept_result(&model, 3, ping, 301U),
		      "duplicate structured result must be ignored");
	zassert_false(receiver_slot_can_send(&model, 3, 301U, ping),
		      "next result PING must receive NORMAL after save");
}

ZTEST(remote_tcal_manager_model,
      test_receiver_requires_current_capability_and_enforces_deadline)
{
	struct receiver_model model;
	struct receiver_outcome_model outcome = {0};
	uint8_t ping[13];

	receiver_begin(&model, 7, (1U << 1) | (1U << 4),
		       SK_REMOTE_TCAL_ACTION_START, 100U);
	make_capability_ping(ping);
	zassert_true(receiver_slot_can_send(&model, 1, 5099U, ping));

	/* A NORMAL PING without the current marker clears eligibility. */
	memset(ping, 0, sizeof(ping));
	zassert_false(receiver_slot_can_send(&model, 1, 5099U, ping));

	make_capability_ping(ping);
	zassert_false(receiver_slot_can_send(&model, 1, 5100U, ping),
		      "ISR deadline is exact, independent of cleanup delay");
	zassert_true(receiver_timeout(&model, 5100U, &outcome));
	zassert_equal(outcome.considered_mask, (1U << 1) | (1U << 4));
	zassert_equal(outcome.reply_mask, 0);
	zassert_equal(outcome.status[1], MODEL_NO_RESPONSE);
	zassert_equal(outcome.status[4], MODEL_NO_RESPONSE);
	zassert_equal(outcome.status[0], MODEL_NOT_CONSIDERED);
}

ZTEST(remote_tcal_manager_model,
      test_receiver_deadline_is_wrap_safe)
{
	struct receiver_model model;
	uint8_t ping[13];

	receiver_begin(&model, 8, 1U, SK_REMOTE_TCAL_ACTION_START,
		       UINT32_MAX - 100U);
	make_capability_ping(ping);
	zassert_true(receiver_slot_can_send(
		&model, 0, UINT32_MAX, ping));
	zassert_true(receiver_slot_can_send(&model, 0, 4898U, ping));
	zassert_false(receiver_slot_can_send(&model, 0, 4899U, ping));
}

ZTEST(remote_tcal_manager_model,
      test_safety_override_expands_old_start_set_without_false_canceled)
{
	struct receiver_model old_job;
	struct receiver_outcome_model old_outcome = {0};
	bool callback_saw_replacement = false;

	receiver_begin(&old_job, 20, (1U << 1) | (1U << 4),
		       SK_REMOTE_TCAL_ACTION_START, 100U);
	old_job.reply_mask = 1U << 1;
	old_job.status[1] = SK_REMOTE_TCAL_RESULT_OK;
	old_job.slot_active[1] = false;

	/*
	 * The old START is already radio-stale, but cleanup has not run. A
	 * partial STOP/ABORT must cover every member whose START is uncertain.
	 */
	uint16_t expanded = receiver_override_start(
		&old_job, 1U << 1, &old_outcome, 21U,
		SK_REMOTE_TCAL_ACTION_ABORT,
		&callback_saw_replacement);
	zassert_equal(expanded, (1U << 1) | (1U << 4));
	zassert_equal(old_outcome.considered_mask,
		      (1U << 1) | (1U << 4));
	zassert_equal(old_outcome.reply_mask, 1U << 1);
	zassert_equal(old_outcome.status[1], SK_REMOTE_TCAL_RESULT_OK);
	zassert_equal(old_outcome.status[4], MODEL_NO_RESPONSE,
		      "unknown START outcome is not CANCELED");
	zassert_true(old_job.active);
	zassert_equal(old_job.transaction, 21U);
	zassert_equal(old_job.action, SK_REMOTE_TCAL_ACTION_ABORT);
	zassert_true(old_job.slot_active[1]);
	zassert_true(old_job.slot_active[4]);
	zassert_true(callback_saw_replacement,
		     "old callback runs only after replacement publication");
}

ZTEST(remote_tcal_manager_model,
      test_result_capture_time_controls_deadline_acceptance)
{
	struct receiver_model model;
	uint8_t ping[13];

	receiver_begin(&model, 0x7788, 1U << 2,
		       SK_REMOTE_TCAL_ACTION_START, 100U);
	make_capability_ping(ping);
	ping[2] = 1U;
	zassert_true(receiver_slot_can_send(&model, 2, 100U, ping));
	ping[2] = 2U;
	zassert_true(receiver_slot_can_send(&model, 2, 1600U, ping));
	make_result_ping_with_status(
		ping, 0x7788, SK_REMOTE_TCAL_RESULT_OK,
		SK_REMOTE_TCAL_CAP_SUPPORTED |
			SK_REMOTE_TCAL_STATUS_ACTIVE);
	zassert_false(receiver_accept_result(&model, 2, ping, 5100U),
		      "result captured at the exact deadline is late");
	zassert_true(model.slot_active[2]);
	zassert_equal(model.reply_mask, 0);

	/*
	 * Manager processing may be delayed beyond 5 s; the ISR capture time,
	 * rather than processing time, decides whether the result was timely.
	 */
	zassert_true(receiver_accept_result(&model, 2, ping, 5099U));
	zassert_equal(model.reply_mask, 1U << 2);
}

ZTEST(remote_tcal_manager_model,
      test_old_start_ok_needs_distinct_confirms_age_and_active)
{
	struct receiver_model model;
	uint8_t ping[13];

	receiver_begin(&model, 0x1234, 1U << 0,
		       SK_REMOTE_TCAL_ACTION_START, 100U);
	make_capability_ping(ping);
	ping[2] = 9U;
	zassert_true(receiver_slot_can_send(&model, 0, 200U, ping));

	/* A retry of the same normal PING counter is not a second confirm. */
	zassert_true(receiver_slot_can_send(&model, 0, 1600U, ping));
	zassert_equal(model.start_confirm_count[0], 1U);
	make_result_ping_with_status(
		ping, 0x1234, SK_REMOTE_TCAL_RESULT_OK,
		SK_REMOTE_TCAL_CAP_SUPPORTED |
			SK_REMOTE_TCAL_STATUS_ACTIVE);
	zassert_false(receiver_accept_result(&model, 0, ping, 1699U));

	/* A distinct counter supplies the second receiver-side confirmation. */
	make_capability_ping(ping);
	ping[2] = 10U;
	zassert_true(receiver_slot_can_send(&model, 0, 1699U, ping));
	zassert_equal(model.start_confirm_count[0], 2U);

	/* Exact age is still insufficient if the returned snapshot is inactive. */
	make_result_ping(ping, 0x1234, SK_REMOTE_TCAL_RESULT_OK);
	zassert_false(receiver_accept_result(&model, 0, ping, 1700U));

	/* Active alone cannot move the minimum-age boundary one tick early. */
	make_result_ping_with_status(
		ping, 0x1234, SK_REMOTE_TCAL_RESULT_OK,
		SK_REMOTE_TCAL_CAP_SUPPORTED |
			SK_REMOTE_TCAL_STATUS_ACTIVE);
	zassert_false(receiver_accept_result(&model, 0, ping, 1699U));
	zassert_true(receiver_accept_result(&model, 0, ping, 1700U));
}

ZTEST(remote_tcal_manager_model,
      test_safety_result_requires_inactive_snapshot)
{
	struct receiver_model model;
	uint8_t ping[13];

	receiver_begin(&model, 0x3344, 1U << 5,
		       SK_REMOTE_TCAL_ACTION_ABORT, 100U);
	make_capability_ping(ping);
	ping[2] = 1U;
	zassert_true(receiver_slot_can_send(&model, 5, 150U, ping));
	make_result_ping_with_status(
		ping, 0x3344, SK_REMOTE_TCAL_RESULT_OK,
		SK_REMOTE_TCAL_CAP_SUPPORTED |
			SK_REMOTE_TCAL_STATUS_ACTIVE);
	ping[2] = 2U;
	zassert_false(receiver_accept_result(&model, 5, ping, 200U),
		      "cached safety result cannot succeed while active");
	zassert_equal(model.reply_mask, 0U);

	make_result_ping(ping, 0x3344,
			 SK_REMOTE_TCAL_RESULT_OK);
	zassert_true(receiver_accept_result(&model, 5, ping, 201U));
	zassert_equal(model.reply_mask, 1U << 5);
}

ZTEST(remote_tcal_manager_model,
      test_old_receiver_model_requires_send_and_later_ping_for_non_ok)
{
	struct receiver_model model;
	uint8_t ping[13];

	receiver_begin(&model, 0x3345, 1U << 5,
		       SK_REMOTE_TCAL_ACTION_ABORT, 100U);
	make_result_ping(
		ping, 0x3345, SK_REMOTE_TCAL_RESULT_HARDWARE_ERROR);
	ping[2] = 1U;
	zassert_false(receiver_accept_result(&model, 5, ping, 150U),
		      "a cached structured error is not execution evidence");

	make_capability_ping(ping);
	ping[2] = 2U;
	zassert_true(receiver_slot_can_send(&model, 5, 160U, ping));

	make_result_ping(
		ping, 0x3345, SK_REMOTE_TCAL_RESULT_HARDWARE_ERROR);
	ping[2] = 2U;
	zassert_false(receiver_accept_result(&model, 5, ping, 161U),
		      "the triggering PING cannot also be the result");

	ping[2] = 3U;
	zassert_true(receiver_accept_result(&model, 5, ping, 162U));
	zassert_equal(model.reply_mask, 1U << 5);
}

ZTEST(remote_tcal_manager_model,
      test_safety_result_requires_validated_send_and_later_counter)
{
	struct causal_receiver_model model;
	uint8_t ping[13];

	causal_receiver_begin(
		&model, 0x5566, SK_REMOTE_TCAL_ACTION_STOP, 7U);
	make_result_ping(
		ping, 0x5566, SK_REMOTE_TCAL_RESULT_OK);

	/* A radio attempt on a packet rejected by sequence checks is no proof. */
	causal_attempt_normal(&model, 9U);
	struct causal_result_event event =
		causal_process_validated_ping(
			&model, 10U, false, 200U);
	struct causal_result_token token =
		causal_precheck_result(
			&model, &event, ping, 200U);
	zassert_false(token.preliminary_valid);

	/* Commit a real send on counter 11. */
	causal_attempt_normal(&model, 11U);
	(void)causal_process_validated_ping(
		&model, 11U, true, 300U);
	zassert_true(model.sent_valid);

	/* A cached result carrying that same trigger counter is still too early. */
	event = causal_process_validated_ping(
		&model, 11U, false, 301U);
	token = causal_precheck_result(
		&model, &event, ping, 301U);
	zassert_false(token.preliminary_valid);

	/* The next strictly accepted counter supplies causal ordering. */
	event = causal_process_validated_ping(
		&model, 12U, false, 302U);
	token = causal_precheck_result(
		&model, &event, ping, 302U);
	zassert_true(token.preliminary_valid);
	zassert_true(causal_commit_result(&model, &token));
}

ZTEST(remote_tcal_manager_model,
      test_start_non_ok_result_still_requires_causal_send)
{
	struct causal_receiver_model model;
	uint8_t ping[13];

	causal_receiver_begin(
		&model, 0x5567, SK_REMOTE_TCAL_ACTION_START, 7U);
	make_result_ping(
		ping, 0x5567, SK_REMOTE_TCAL_RESULT_INVALID);

	struct causal_result_event event =
		causal_process_validated_ping(
			&model, 1U, false, 90U);
	struct causal_result_token token =
		causal_precheck_result(
			&model, &event, ping, 90U);
	zassert_false(token.preliminary_valid,
		      "cached START error before any send is not a reply");

	causal_attempt_normal(&model, 2U);
	(void)causal_process_validated_ping(
		&model, 2U, true, 100U);
	event = causal_process_validated_ping(
		&model, 2U, false, 101U);
	token = causal_precheck_result(
		&model, &event, ping, 101U);
	zassert_false(token.preliminary_valid,
		      "same-counter result is not later than the send");

	event = causal_process_validated_ping(
		&model, 3U, false, 102U);
	token = causal_precheck_result(
		&model, &event, ping, 102U);
	zassert_true(token.preliminary_valid,
		     "non-OK needs causality but not the OK-only 2x/1500ms gate");
}

ZTEST(remote_tcal_manager_model,
      test_counter_reboot_detection_is_conservative_safe_failure)
{
	/* Numeric decline includes natural wrap: cancel rather than cross boots. */
	zassert_true(ping_counter_starts_new_session(
		true, 250U, 0U, 100U, 101U));
	zassert_true(ping_counter_starts_new_session(
		true, 255U, 0U, 100U, 101U));
	zassert_false(ping_counter_starts_new_session(
		true, 0U, 0U, 100U, 350U),
		"normal same-counter radio retry remains in this session");
	zassert_true(ping_counter_starts_new_session(
		true, 0U, 0U, 100U, 351U),
		"old 0 -> rebooted 0 is caught after the retry window");
	zassert_false(ping_counter_starts_new_session(
		true, 0U, 1U, 100U, 101U));
	zassert_true(data_sequence_starts_new_session(3));
	zassert_false(data_sequence_starts_new_session(1));

	struct causal_receiver_model model;
	causal_receiver_begin(
		&model, 0x5568, SK_REMOTE_TCAL_ACTION_START, 4U);
	causal_attempt_normal(&model, 7U);
	(void)causal_process_validated_ping(
		&model, 7U, true, 100U);
	causal_attempt_normal(&model, 7U);
	(void)causal_process_validated_ping(
		&model, 7U, true, 110U);
	zassert_equal(model.confirm_count, 1U,
		      "a normal retransmit is never a second START confirmation");

	causal_reset_tracker(&model);
	zassert_equal(model.capability_generation, 5U);
	zassert_false(model.slot_active,
		      "a conservative session boundary safely cancels the task");
}

ZTEST(remote_tcal_manager_model,
      test_safety_structured_error_is_kept_while_active)
{
	struct causal_receiver_model model;
	uint8_t ping[13];

	causal_receiver_begin(
		&model, 0x6677, SK_REMOTE_TCAL_ACTION_ABORT, 3U);
	causal_attempt_normal(&model, 1U);
	(void)causal_process_validated_ping(
		&model, 1U, true, 100U);
	struct causal_result_event event =
		causal_process_validated_ping(
			&model, 2U, false, 101U);
	make_result_ping_with_status(
		ping, 0x6677,
		SK_REMOTE_TCAL_RESULT_HARDWARE_ERROR,
		SK_REMOTE_TCAL_CAP_SUPPORTED |
			SK_REMOTE_TCAL_STATUS_ACTIVE);
	struct causal_result_token token =
		causal_precheck_result(
			&model, &event, ping, 101U);
	zassert_true(token.preliminary_valid,
		     "real non-OK result belongs in reply_mask");
	zassert_true(causal_commit_result(&model, &token));
}

ZTEST(remote_tcal_manager_model,
      test_reset_between_result_precheck_and_commit_is_rejected)
{
	struct causal_receiver_model model;
	uint8_t ping[13];

	causal_receiver_begin(
		&model, 0x7789, SK_REMOTE_TCAL_ACTION_STOP, 9U);
	causal_attempt_normal(&model, 20U);
	(void)causal_process_validated_ping(
		&model, 20U, true, 100U);
	struct causal_result_event event =
		causal_process_validated_ping(
			&model, 21U, false, 101U);
	make_result_ping(
		ping, 0x7789, SK_REMOTE_TCAL_RESULT_OK);
	struct causal_result_token token =
		causal_precheck_result(
			&model, &event, ping, 101U);
	zassert_true(token.preliminary_valid);

	causal_reset_tracker(&model);
	zassert_false(causal_commit_result(&model, &token),
		      "final IRQ revalidation rejects old queued event");
	zassert_false(model.replied);
}

ZTEST(remote_tcal_manager_model,
      test_critical_legacy_withdrawal_rejects_queued_result_at_commit)
{
	struct causal_receiver_model model;
	struct command_collision_model collision = {
		.extension_slot = true,
	};
	uint8_t ping[13];

	causal_receiver_begin(
		&model, 0x778A, SK_REMOTE_TCAL_ACTION_ABORT, 9U);
	causal_attempt_normal(&model, 30U);
	(void)causal_process_validated_ping(
		&model, 30U, true, 100U);
	struct causal_result_event event =
		causal_process_validated_ping(
			&model, 31U, false, 101U);
	make_result_ping(
		ping, 0x778A, SK_REMOTE_TCAL_RESULT_OK);
	struct causal_result_token token =
		causal_precheck_result(
			&model, &event, ping, 101U);
	zassert_true(token.preliminary_valid);

	/* Critical legacy publication clears the slot without changing its ID. */
	zassert_true(collision_publish_critical_generic(&collision));
	model.slot_active = collision.extension_slot;
	zassert_false(causal_commit_result(&model, &token));
	zassert_false(model.replied);
}

ZTEST(remote_tcal_manager_model,
      test_generation_revalidation_prevents_slot_revival)
{
	struct capability_model capability = {
		.last_ping_ms = 100U,
		.generation = 4U,
		.supported = 1U,
	};
	struct selection_snapshot_model old_snapshot =
		selection_snapshot(&capability);

	/* Reset and ID reuse publish a fresh capability in a new generation. */
	capability.generation = 5U;
	capability.last_ping_ms = 200U;
	capability.supported = 1U;
	capability.slot_active = false;
	zassert_false(selection_publish_if_same_generation_eligible(
		&capability, &old_snapshot, 200U,
		SK_REMOTE_TCAL_ACTION_START));
	zassert_false(capability.slot_active,
		      "stale submit snapshot cannot revive a cleared slot");

	struct selection_snapshot_model new_snapshot =
		selection_snapshot(&capability);
	zassert_true(selection_publish_if_same_generation_eligible(
		&capability, &new_snapshot, 200U,
		SK_REMOTE_TCAL_ACTION_START));
	zassert_true(capability.slot_active);
}

ZTEST(remote_tcal_manager_model,
      test_start_and_safety_use_distinct_capability_age_windows)
{
	struct capability_model capability = {
		.last_ping_ms = 100U,
		.generation = 4U,
		.supported = 1U,
	};
	struct selection_snapshot_model snapshot =
		selection_snapshot(&capability);

	zassert_true(selection_publish_if_same_generation_eligible(
		&capability, &snapshot, 2600U,
		SK_REMOTE_TCAL_ACTION_START),
		"START includes the exact 2500 ms freshness boundary");
	capability.slot_active = false;
	zassert_false(selection_publish_if_same_generation_eligible(
		&capability, &snapshot, 2601U,
		SK_REMOTE_TCAL_ACTION_START));
	zassert_true(selection_publish_if_same_generation_eligible(
		&capability, &snapshot, 2601U,
		SK_REMOTE_TCAL_ACTION_STOP),
		"STOP remains eligible at age 2501 ms");
	capability.slot_active = false;
	zassert_true(selection_publish_if_same_generation_eligible(
		&capability, &snapshot, 5099U,
		SK_REMOTE_TCAL_ACTION_ABORT),
		"ABORT remains eligible through age 4999 ms");
	capability.slot_active = false;
	zassert_false(selection_publish_if_same_generation_eligible(
		&capability, &snapshot, 5100U,
		SK_REMOTE_TCAL_ACTION_ABORT),
		"the 5000 ms capability timeout is exclusive");

	/* A newer PING in the same generation may refresh final publication. */
	capability.last_ping_ms = 200U;
	zassert_true(selection_publish_if_same_generation_eligible(
		&capability, &snapshot, 2601U,
		SK_REMOTE_TCAL_ACTION_START));

	capability.last_ping_ms = 0U;
	zassert_false(selection_publish_if_same_generation_eligible(
		&capability, &snapshot, 2601U,
		SK_REMOTE_TCAL_ACTION_ABORT));
	capability.last_ping_ms = 2600U;
	capability.supported = 0U;
	zassert_false(selection_publish_if_same_generation_eligible(
		&capability, &snapshot, 2601U,
		SK_REMOTE_TCAL_ACTION_ABORT));
}

ZTEST(remote_tcal_manager_model,
      test_usb_generation_drops_old_completion_and_failed_started_final)
{
	struct hid_session_model model = {
		.current_generation = 1U,
	};

	hid_session_acquire(&model);
	hid_session_disconnect(&model);
	hid_session_complete(&model);
	zassert_false(model.context_in_use);
	zassert_false(model.final_enqueued,
		      "old completion cannot enter a new USB session");

	hid_session_acquire(&model);
	hid_session_complete(&model);
	zassert_true(model.completion_pending);
	hid_session_finish_started(&model, false);
	zassert_false(model.context_in_use);
	zassert_false(model.final_enqueued,
		      "final is suppressed when STARTED enqueue failed");

	hid_session_acquire(&model);
	hid_session_finish_started(&model, false);
	zassert_true(model.context_in_use,
		     "callback pointer stays reserved until completion");
	hid_session_complete(&model);
	zassert_false(model.context_in_use);
	zassert_false(model.final_enqueued);
}

ZTEST(remote_tcal_manager_model,
      test_control_ack_snapshot_cannot_submit_or_consume_new_usb_session)
{
	zassert_true(control_ack_snapshot_can_commit(
		5U, 5U, 2U, 2U));
	zassert_false(control_ack_snapshot_can_commit(
		5U, 6U, 2U, 2U),
		"disconnect invalidates both pre-submit and commit checks");
	zassert_false(control_ack_snapshot_can_commit(
		5U, 5U, 2U, 0U),
		"queue reset/read movement invalidates an old snapshot");
}

ZTEST(remote_tcal_manager_model,
      test_short_recognized_hid_command_is_einval)
{
	zassert_equal(hid_request_disposition(2U, 254U, 0xC8U), -1);
	zassert_equal(hid_request_disposition(3U, 254U, 0xC8U), 1);
	zassert_equal(hid_request_disposition(6U, 254U, 0xC8U), 1);
	zassert_equal(hid_request_disposition(7U, 254U, 0xC8U), 0);
}

ZTEST(remote_tcal_manager_model,
      test_hid_request_uses_contiguous_seven_byte_layout)
{
	const uint8_t request[7] = {
		254U, 0x42U, 0xC8U, 0xFFU,
		SK_REMOTE_TCAL_ACTION_START, 0x29U, 0x09U,
	};

	zassert_equal(request[0], 254U);
	zassert_equal(request[1], 0x42U);
	zassert_equal(request[2], 0xC8U);
	zassert_equal(request[3], 0xFFU);
	zassert_equal(request[4], SK_REMOTE_TCAL_ACTION_START);
	zassert_equal((int16_t)sys_get_le16(&request[5]), 2345);
}

ZTEST(remote_tcal_manager_model,
      test_generic_and_start_publications_are_mutually_exclusive)
{
	struct command_collision_model generic_first = {0};
	struct command_collision_model start_first = {0};

	zassert_true(collision_publish_generic(&generic_first));
	zassert_false(collision_publish_start(&generic_first));
	zassert_true(generic_first.generic_command);
	zassert_false(generic_first.extension_slot);

	zassert_true(collision_publish_start(&start_first));
	zassert_false(collision_publish_generic(&start_first));
	zassert_true(start_first.extension_slot);
	zassert_false(start_first.generic_command);
}

ZTEST(remote_tcal_manager_model,
      test_critical_legacy_preempts_extension_but_ordinary_does_not)
{
	struct command_collision_model ordinary = {
		.extension_slot = true,
	};
	struct command_collision_model critical = {
		.extension_slot = true,
	};
	struct command_collision_model ordinary_first = {0};

	zassert_false(collision_publish_generic(&ordinary));
	zassert_true(ordinary.extension_slot);
	zassert_false(ordinary.generic_command);

	zassert_true(collision_publish_critical_generic(&critical));
	zassert_false(critical.extension_slot);
	zassert_true(critical.generic_command);
	zassert_true(critical.generic_critical);
	zassert_false(collision_publish_start(&critical));
	zassert_false(collision_publish_safety_extension(&critical),
		      "critical legacy flag cannot be revived over by STOP/ABORT");

	zassert_true(collision_publish_generic(&ordinary_first));
	zassert_true(collision_publish_safety_extension(&ordinary_first),
		     "STOP/ABORT may serialize ahead of ordinary legacy work");
	zassert_true(ordinary_first.generic_command);
	zassert_true(ordinary_first.extension_slot);
}

ZTEST(remote_tcal_manager_model,
      test_result_withdrawal_precedes_queued_ordinary_legacy)
{
	struct command_collision_model ordinary_first = {0};

	zassert_true(collision_publish_generic(&ordinary_first));
	zassert_true(collision_publish_safety_extension(&ordinary_first));
	zassert_equal(
		collision_select_pong(&ordinary_first, true),
		COLLISION_PONG_EXTENSION,
		"the matching result is saved before command withdrawal");

	/* Model process_result_event() saving the result and clearing its slot. */
	ordinary_first.extension_slot = false;
	zassert_equal(
		collision_select_pong(&ordinary_first, true),
		COLLISION_PONG_NORMAL,
		"NORMAL must finish RESULT_READY before ordinary legacy is sent");
	zassert_equal(
		collision_select_pong(&ordinary_first, true),
		COLLISION_PONG_NORMAL,
		"a lost NORMAL is retried for every repeated result PING");
	zassert_equal(
		collision_select_pong(&ordinary_first, false),
		COLLISION_PONG_ORDINARY_LEGACY,
		"the queued ordinary flag is preserved for the following PING");

	struct command_collision_model critical = {
		.generic_command = true,
		.generic_critical = true,
	};
	zassert_equal(
		collision_select_pong(&critical, true),
		COLLISION_PONG_CRITICAL_LEGACY,
		"critical lifecycle commands still preempt a latched result");
}

ZTEST(remote_tcal_manager_model,
      test_result_status_does_not_refresh_normal_capability)
{
	struct capability_model capability = {
		.last_ping_ms = 100U,
		.generation = 6U,
		.supported = 1U,
		.status = SK_REMOTE_TCAL_CAP_SUPPORTED,
	};

	capability_apply_result_status(
		&capability,
		SK_REMOTE_TCAL_CAP_SUPPORTED |
			SK_REMOTE_TCAL_STATUS_ACTIVE);
	zassert_equal(capability.last_ping_ms, 100U);
	zassert_true(capability.supported);
	zassert_true((capability.status &
		      SK_REMOTE_TCAL_STATUS_ACTIVE) != 0U);
}

ZTEST(remote_tcal_manager_model,
      test_hid_final_ack_waits_until_started_is_enqueued)
{
	struct hid_ack_gate_model gate = {0};

	hid_final_result_ready(&gate);
	zassert_true(gate.final_ready);
	zassert_false(gate.final_enqueued,
		      "a fast completion cannot overtake STARTED");
	hid_started_enqueue_succeeded(&gate);
	zassert_true(gate.started_enqueued);
	zassert_true(gate.final_enqueued);
}

ZTEST(remote_tcal_manager_model,
      test_capability_timeout_does_not_clear_a_newer_ping)
{
	struct capability_model capability = {
		.last_ping_ms = 100U,
		.generation = 6U,
		.supported = 1U,
		.status = SK_REMOTE_TCAL_CAP_SUPPORTED |
			  SK_REMOTE_TCAL_STATUS_ACTIVE,
		.slot_active = true,
	};
	uint32_t stale_snapshot = capability.last_ping_ms;

	/* A newer ISR publication wins over delayed timeout cleanup. */
	capability.last_ping_ms = 200U;
	zassert_false(capability_clear_if_still_stale(
		&capability, stale_snapshot));
	zassert_equal(capability.last_ping_ms, 200U);
	zassert_true(capability.supported);
	zassert_true(capability.slot_active);

	zassert_true(capability_clear_if_still_stale(&capability, 200U));
	zassert_equal(capability.last_ping_ms, 0);
	zassert_equal(capability.generation, 7U);
	zassert_false(capability.supported);
	zassert_false(capability.slot_active);
}

ZTEST(remote_tcal_manager_model,
      test_transaction_wrap_skips_zero_and_stays_big_endian)
{
	uint16_t next = 0xFFFE;
	uint8_t pong[13] = {0};
	const uint16_t expected[] = {0xFFFE, 0xFFFF, 1};

	for (size_t i = 0; i < ARRAY_SIZE(expected); i++) {
		uint16_t transaction = allocate_transaction(&next);
		zassert_equal(transaction, expected[i]);
		sk_remote_tcal_encode_pong(
			pong, transaction,
			SK_REMOTE_TCAL_ACTION_ABORT, 0);
		zassert_equal(sys_get_be16(&pong[8]), expected[i]);
	}
	zassert_equal(next, 2);
}

ZTEST_SUITE(remote_tcal_manager_model, NULL, NULL, NULL, NULL, NULL);
