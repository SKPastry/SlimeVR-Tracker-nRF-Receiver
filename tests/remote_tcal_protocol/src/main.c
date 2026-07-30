#include <zephyr/sys/crc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "remote_tcal_protocol.h"

struct decoded_command {
	uint16_t transaction;
	int16_t target_centi_c;
	uint8_t action;
};

static bool decode_command(const uint8_t pong[13],
			   struct decoded_command *command)
{
	if (pong[3] != SK_REMOTE_TCAL_MAGIC_0 ||
	    pong[4] != SK_REMOTE_TCAL_MAGIC_1 ||
	    pong[5] != SK_REMOTE_TCAL_VERSION ||
	    pong[7] != SK_REMOTE_TCAL_ESCAPE ||
	    pong[6] < SK_REMOTE_TCAL_ACTION_START ||
	    pong[6] > SK_REMOTE_TCAL_ACTION_ABORT) {
		return false;
	}

	int16_t target = (int16_t)sys_get_be16(&pong[10]);
	if (pong[6] != SK_REMOTE_TCAL_ACTION_START && target != 0) {
		return false;
	}

	if (command != NULL) {
		command->transaction = sys_get_be16(&pong[8]);
		command->target_centi_c = target;
		command->action = pong[6];
	}
	return true;
}

ZTEST(remote_tcal_protocol, test_normal_capability_golden_and_rejection)
{
	uint8_t ping[13] = {
		[0] = 0xF0,
		[1] = 0x01,
		[2] = 0x02,
		[3] = 0x11,
		[4] = 0x22,
		[5] = 0x33,
		[6] = 0x44,
		[7] = 0,
		[8] = 'S',
		[9] = 'K',
		[10] = 1,
		[11] = SK_REMOTE_TCAL_CAP_SUPPORTED |
		       SK_REMOTE_TCAL_STATUS_ACTIVE |
		       SK_REMOTE_TCAL_STATUS_SAMPLING | (17U << 3),
	};
	uint8_t status = 0;
	const uint8_t expected_extension[] = {
		0, 'S', 'K', 1,
		SK_REMOTE_TCAL_CAP_SUPPORTED |
			SK_REMOTE_TCAL_STATUS_ACTIVE |
			SK_REMOTE_TCAL_STATUS_SAMPLING | (17U << 3),
	};

	zassert_mem_equal(&ping[7], expected_extension,
			  sizeof(expected_extension));
	zassert_true(sk_remote_tcal_normal_capability(ping, &status));
	zassert_equal(status, ping[11]);

	ping[11] &= ~SK_REMOTE_TCAL_CAP_SUPPORTED;
	zassert_false(sk_remote_tcal_normal_capability(ping, &status));
	zassert_equal(status, ping[11]);
	ping[11] |= SK_REMOTE_TCAL_CAP_SUPPORTED;

	ping[10] = 2;
	zassert_false(sk_remote_tcal_normal_capability(ping, NULL));
	ping[10] = 1;
	ping[8] = 0;
	zassert_false(sk_remote_tcal_normal_capability(ping, NULL));
	ping[8] = 'S';
	ping[9] = 0;
	zassert_false(sk_remote_tcal_normal_capability(ping, NULL));
	ping[9] = 'K';
	ping[7] = SK_REMOTE_TCAL_ESCAPE;
	zassert_false(sk_remote_tcal_normal_capability(ping, NULL));
}

ZTEST(remote_tcal_protocol, test_command_golden_packet_and_endianness)
{
	uint8_t pong[13] = {0xF1, 3, 9};
	struct decoded_command decoded;

	sk_remote_tcal_encode_pong(
		pong, 0x1234, SK_REMOTE_TCAL_ACTION_START, 2345);
	const uint8_t expected[] = {
		'S', 'K', 1, SK_REMOTE_TCAL_ACTION_START,
		0xC8, 0x12, 0x34, 0x09, 0x29,
	};
	zassert_mem_equal(&pong[3], expected, sizeof(expected));
	zassert_true(decode_command(pong, &decoded));
	zassert_equal(decoded.transaction, 0x1234);
	zassert_equal(decoded.action, SK_REMOTE_TCAL_ACTION_START);
	zassert_equal(decoded.target_centi_c, 2345);

	sk_remote_tcal_encode_pong(
		pong, 0xFFFF, SK_REMOTE_TCAL_ACTION_START,
		SK_REMOTE_TCAL_DEFAULT_CENTI_C);
	zassert_equal(sys_get_be16(&pong[8]), 0xFFFF);
	zassert_equal(pong[10], 0x80);
	zassert_equal(pong[11], 0x00);
	zassert_true(decode_command(pong, &decoded));
	zassert_equal(decoded.target_centi_c,
		      SK_REMOTE_TCAL_DEFAULT_CENTI_C);

	sk_remote_tcal_encode_pong(
		pong, 1, SK_REMOTE_TCAL_ACTION_START, -1234);
	zassert_equal(pong[10], 0xFB);
	zassert_equal(pong[11], 0x2E);
	zassert_true(decode_command(pong, &decoded));
	zassert_equal(decoded.target_centi_c, -1234);
}

ZTEST(remote_tcal_protocol, test_command_magic_version_and_shape_rejection)
{
	uint8_t pong[13] = {0};
	sk_remote_tcal_encode_pong(
		pong, 0x1234, SK_REMOTE_TCAL_ACTION_STOP, 0);
	zassert_true(decode_command(pong, NULL));

	pong[3] = 0;
	zassert_false(decode_command(pong, NULL));
	pong[3] = SK_REMOTE_TCAL_MAGIC_0;
	pong[4] = 0;
	zassert_false(decode_command(pong, NULL));
	pong[4] = SK_REMOTE_TCAL_MAGIC_1;
	pong[5] = SK_REMOTE_TCAL_VERSION + 1U;
	zassert_false(decode_command(pong, NULL));
	pong[5] = SK_REMOTE_TCAL_VERSION;
	pong[7] = 0;
	zassert_false(decode_command(pong, NULL));
	pong[7] = SK_REMOTE_TCAL_ESCAPE;
	pong[6] = 0;
	zassert_false(decode_command(pong, NULL));
	pong[6] = SK_REMOTE_TCAL_ACTION_ABORT + 1U;
	zassert_false(decode_command(pong, NULL));

	pong[6] = SK_REMOTE_TCAL_ACTION_STOP;
	pong[10] = 0;
	pong[11] = 1;
	zassert_false(decode_command(pong, NULL));
}

ZTEST(remote_tcal_protocol, test_structured_result_golden_and_rejection)
{
	uint8_t ping[13] = {
		[3] = 0xDE,
		[4] = 0xAD,
		[5] = 0xBE,
		[6] = 0xEF,
		[7] = 0xC8,
		[8] = 0xBE,
		[9] = 0xEF,
		[10] = 0xA0 | SK_REMOTE_TCAL_RESULT_BUSY,
		[11] = SK_REMOTE_TCAL_CAP_SUPPORTED |
		       SK_REMOTE_TCAL_STATUS_ACTIVE,
	};
	uint8_t result = 0;
	uint8_t status = 0;
	const uint8_t expected[] = {
		0xDE, 0xAD, 0xBE, 0xEF, 0xC8, 0xBE, 0xEF,
		0xA0 | SK_REMOTE_TCAL_RESULT_BUSY,
		SK_REMOTE_TCAL_CAP_SUPPORTED |
			SK_REMOTE_TCAL_STATUS_ACTIVE,
	};

	zassert_mem_equal(&ping[3], expected, sizeof(expected));
	zassert_true(sk_remote_tcal_parse_result(
		ping, 0xBEEF, &result, &status));
	zassert_equal(result, SK_REMOTE_TCAL_RESULT_BUSY);
	zassert_equal(status, ping[11]);
	zassert_false(sk_remote_tcal_parse_result(
		ping, 0xBEF0, NULL, NULL));

	ping[10] = 0xC8; /* legacy unknown-flag echo, not an A-result */
	zassert_false(sk_remote_tcal_parse_result(
		ping, 0xBEEF, NULL, NULL));
	ping[10] = 0xAB; /* result nibble outside the fixed range */
	zassert_false(sk_remote_tcal_parse_result(
		ping, 0xBEEF, NULL, NULL));
	ping[10] = 0xB0 | SK_REMOTE_TCAL_RESULT_OK;
	zassert_false(sk_remote_tcal_parse_result(
		ping, 0xBEEF, NULL, NULL));
	ping[10] = 0xA0 | SK_REMOTE_TCAL_RESULT_OK;
	ping[7] = 0;
	zassert_false(sk_remote_tcal_parse_result(
		ping, 0xBEEF, NULL, NULL));
}

ZTEST(remote_tcal_protocol, test_crc_covers_bytes_zero_through_eleven)
{
	uint8_t pong[13] = {0xF1, 3, 9};
	const uint8_t expected[13] = {
		0xF1, 0x03, 0x09, 'S', 'K', 1,
		SK_REMOTE_TCAL_ACTION_STOP, 0xC8,
		0x12, 0x34, 0x00, 0x00, 0x53,
	};

	sk_remote_tcal_encode_pong(
		pong, 0x1234, SK_REMOTE_TCAL_ACTION_STOP, 0);
	pong[12] = crc8_ccitt(0x07, pong, 12);
	zassert_mem_equal(pong, expected, sizeof(expected));

	uint8_t original_crc = pong[12];
	pong[11] ^= 1U;
	zassert_not_equal(crc8_ccitt(0x07, pong, 12), original_crc);
	pong[11] ^= 1U;
	pong[12] ^= 0xFFU;
	zassert_equal(crc8_ccitt(0x07, pong, 12), original_crc,
		      "byte 12 must not feed the CRC over bytes 0..11");
}

ZTEST_SUITE(remote_tcal_protocol, NULL, NULL, NULL, NULL, NULL);
