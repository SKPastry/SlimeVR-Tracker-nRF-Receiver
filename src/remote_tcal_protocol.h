/*
 * Experimental SK remote heated T-Cal wire protocol.
 *
 * This header contains only wire constants and small pure helpers so the
 * codec can also be exercised by native_sim tests.
 */
#ifndef SLIMENRF_REMOTE_TCAL_PROTOCOL_H
#define SLIMENRF_REMOTE_TCAL_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#define SK_REMOTE_TCAL_MAGIC_0 'S'
#define SK_REMOTE_TCAL_MAGIC_1 'K'
#define SK_REMOTE_TCAL_VERSION 1U
#define SK_REMOTE_TCAL_ESCAPE 0xC8U
#define SK_REMOTE_TCAL_RESULT_MARKER 0xA0U
#define SK_REMOTE_TCAL_RESULT_MASK 0x0FU
#define SK_REMOTE_TCAL_DEFAULT_CENTI_C INT16_MIN

#define SK_REMOTE_TCAL_CAP_SUPPORTED (1U << 0)
#define SK_REMOTE_TCAL_STATUS_ACTIVE (1U << 1)
#define SK_REMOTE_TCAL_STATUS_SAMPLING (1U << 2)

enum sk_remote_tcal_action {
	SK_REMOTE_TCAL_ACTION_START = 1,
	SK_REMOTE_TCAL_ACTION_STOP = 2,
	SK_REMOTE_TCAL_ACTION_ABORT = 3,
};

enum sk_remote_tcal_result {
	SK_REMOTE_TCAL_RESULT_OK = 0,
	SK_REMOTE_TCAL_RESULT_INVALID = 1,
	SK_REMOTE_TCAL_RESULT_UNSUPPORTED = 2,
	SK_REMOTE_TCAL_RESULT_BUSY = 3,
	SK_REMOTE_TCAL_RESULT_POWER_REQUIRED = 4,
	SK_REMOTE_TCAL_RESULT_NOT_READY = 5,
	SK_REMOTE_TCAL_RESULT_TIMEOUT = 6,
	SK_REMOTE_TCAL_RESULT_HARDWARE_ERROR = 7,
	SK_REMOTE_TCAL_RESULT_NOT_ACTIVE = 8,
	SK_REMOTE_TCAL_RESULT_INTERNAL = 9,
	SK_REMOTE_TCAL_RESULT_CANCELED = 10,
	SK_REMOTE_TCAL_RESULT_MAX = SK_REMOTE_TCAL_RESULT_CANCELED,
};

static inline bool sk_remote_tcal_normal_capability(const uint8_t ping[13],
						     uint8_t *status)
{
	if (ping[7] != 0 || ping[8] != SK_REMOTE_TCAL_MAGIC_0 ||
	    ping[9] != SK_REMOTE_TCAL_MAGIC_1 ||
	    ping[10] != SK_REMOTE_TCAL_VERSION) {
		return false;
	}

	if (status != NULL) {
		*status = ping[11];
	}
	return (ping[11] & SK_REMOTE_TCAL_CAP_SUPPORTED) != 0;
}

static inline bool sk_remote_tcal_parse_result(const uint8_t ping[13],
						uint16_t expected_transaction,
						uint8_t *result,
						uint8_t *status)
{
	uint8_t encoded = ping[10];
	uint8_t decoded = encoded & SK_REMOTE_TCAL_RESULT_MASK;

	if (ping[7] != SK_REMOTE_TCAL_ESCAPE ||
	    sys_get_be16(&ping[8]) != expected_transaction ||
	    (encoded & 0xF0U) != SK_REMOTE_TCAL_RESULT_MARKER ||
	    decoded > SK_REMOTE_TCAL_RESULT_MAX) {
		return false;
	}

	if (result != NULL) {
		*result = decoded;
	}
	if (status != NULL) {
		*status = ping[11];
	}
	return true;
}

static inline void sk_remote_tcal_encode_pong(uint8_t pong[13],
					      uint16_t transaction,
					      uint8_t action,
					      int16_t target_centi_c)
{
	pong[3] = SK_REMOTE_TCAL_MAGIC_0;
	pong[4] = SK_REMOTE_TCAL_MAGIC_1;
	pong[5] = SK_REMOTE_TCAL_VERSION;
	pong[6] = action;
	pong[7] = SK_REMOTE_TCAL_ESCAPE;
	sys_put_be16(transaction, &pong[8]);
	sys_put_be16((uint16_t)target_centi_c, &pong[10]);
}

#endif
