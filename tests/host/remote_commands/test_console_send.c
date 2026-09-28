#include <assert.h>
#include <stdio.h>
#include "rcv_cmd.h"
#include "console_send.h"

static unsigned requests;
static uint8_t last_target, last_flag;
uint8_t rcv_cmd_remote_flag(uint8_t target, uint8_t flag)
{
    assert(rcv_hid_opcode_is_pong_flag(flag));
    requests++;
    last_target = target;
    last_flag = flag;
    return RCV_HID_ST_QUEUED;
}
uint8_t rcv_cmd_tracker_channel_all(uint8_t channel) { assert(false); return 0; }
uint8_t rcv_cmd_tracker_channel_clear_all(void) { assert(false); return 0; }
uint8_t rcv_cmd_remote_sens_set(uint8_t target, float x, float y, float z) { assert(false); return 0; }
uint8_t rcv_cmd_remote_sens_auto(uint8_t target, uint8_t axis, uint16_t rev) { assert(false); return 0; }
uint8_t rcv_cmd_remote_test_on(uint8_t target, uint16_t tps) { assert(false); return 0; }
uint8_t rcv_cmd_remote_test_off(uint8_t target) { assert(false); return 0; }

int main(void)
{
    char *targets[] = {"0", "all"};
    for (unsigned i = 0; i < 2; i++) {
        requests = 0;
        console_handle_send(targets[i], "tcal", "heat", NULL, NULL);
        console_handle_send(targets[i], "tcal", "heat", "stop", NULL);
        console_handle_send(targets[i], "tcal", "heat", "status", NULL);
        console_handle_send(targets[i], "tcal", "heat", "44", NULL);
        console_handle_send(targets[i], "tcal", "heat", "start", "44");
        console_handle_send(targets[i], "tcal", "heat", "start", "nan");
        assert(requests == 0);
        console_handle_send(targets[i], "tcal", "heat", "start", NULL);
        assert(requests == 1 && last_target == (i ? RCV_HID_TARGET_ALL : 0));
        assert(last_flag == ESB_PONG_FLAG_TCAL_HEATED_START);
        console_handle_send(targets[i], "tcal", "auto", "on", NULL);
        assert(requests == 2 && last_flag == ESB_PONG_FLAG_TCAL_AUTO_ON);
        console_handle_send(targets[i], "tcal", "off", NULL, NULL);
        assert(requests == 3 && last_flag == ESB_PONG_FLAG_TCAL_OFF);
    }
    puts("console: heated start rejects unsupported controls/targets without queuing");
    return 0;
}
