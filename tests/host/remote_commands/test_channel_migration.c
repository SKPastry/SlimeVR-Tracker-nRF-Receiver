#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define MAX_TRACKERS 16
#define BIT(i) (1u << (i))
#define K_FOREVER 0
#define LOG_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define TPS_CALCULATION_INTERVAL_MS 1000
#define ESB_RF_CHANNEL_DEFAULT 255
#define ESB_RF_CHANNEL_ZERO_ENC 128
#define ESB_PONG_FLAG_NORMAL 0
#define ESB_PONG_FLAG_SET_CHANNEL 10
#define ESB_PONG_FLAG_CLEAR_CHANNEL 11
#define ESB_PONG_FLAG_DATA_COLLECT_ON 34
#define ESB_PONG_FLAG_TEST_MODE_ON 31
#define ESB_PONG_FLAG_TEST_MODE_OFF 32
#define ESB_PONG_FLAG_DATA_COLLECT_BATCH_ON 52
#define RF_CHANNEL 1
#define RADIO_RF_CHANNEL 22
#define SYS_STATUS_CONNECTION_ERROR 1
#define CONFIG_DATA_COLLECT 1
#define RCV_HID_ST_OK 0
#define RCV_HID_ST_EINVAL 1
#define RCV_HID_ST_ENOENT 2
#define RCV_HID_ST_EBUSY 3
#define ESB_PONG_FLAG_DATA_COLLECT_OFF 35
#define ESB_PONG_FLAG_DATA_COLLECT_BATCH_OFF 53
typedef int atomic_t;
typedef void (*esb_channel_change_done_cb_t)(bool);
typedef void (*esb_remote_confirm_cb_t)(uint8_t, uint8_t);
static int atomic_get(atomic_t *p) { return *p; }
static void atomic_set(atomic_t *p, int v) { *p = v; }
static bool atomic_cas(atomic_t *p, int old, int value) {
    if (*p != old) return false;
    *p = value;
    return true;
}
static int atomic_or(atomic_t *p, int v) { int old = *p; *p |= v; return old; }
static int tracker_store_lock;
static void k_mutex_lock(int *m, int timeout) { assert(!*m); *m = 1; }
static void k_mutex_unlock(int *m) { assert(*m); *m = 0; }
static int64_t now;
static int64_t k_uptime_get(void) { return now; }
static bool ota, self_ota, collecting, batch, abort_pending[MAX_TRACKERS];
static bool esb_ota_relay_is_active(void) { return ota; }
static bool receiver_ota_is_active(void) { return self_ota; }
static bool data_collect_is_active(void) { return collecting; }
static bool data_collect_batch_is_active(void) { return batch; }
static bool esb_ota_relay_abort_pending(uint8_t id) { return abort_pending[id]; }
static uint8_t collecting_id;
static uint32_t collecting_mask;
static void data_collect_start(uint8_t id) { collecting = true; collecting_id = id; }
static void data_collect_stop(void) { collecting = false; }
static uint8_t data_collect_get_target_id(void) { return collecting_id; }
static void data_collect_batch_start(uint32_t mask, uint16_t rate) { batch = true; collecting_mask = mask; }
static void data_collect_batch_stop(void) { batch = false; }
static bool data_collect_batch_is_target(uint8_t id) { return (collecting_mask & BIT(id)) != 0; }
static uint8_t stored_trackers, receiver_rf_channel, physical_channel, durable_channel;
static uint64_t stored_tracker_addr[MAX_TRACKERS];
static uint8_t tracker_remote_command[MAX_TRACKERS], pending_cmd_arg[MAX_TRACKERS];
static bool last_raw_valid[MAX_TRACKERS];
static void raw_arq_reset(void) {}
static bool rx_running, fault_status, auto_enabled;
static bool esb_pairing;
static uint32_t auto_generation;
static uint32_t auto_loss_reset_epoch;
static bool sequence_initialized[MAX_TRACKERS];
static uint8_t last_packet_sequence[MAX_TRACKERS];
static atomic_t tdma_rx_discontinuity_pending;
static bool stale_fifo;
static int esb_flush_rx(void) { assert(!rx_running); stale_fifo = false; return 0; }
static bool auto_channel_decision_valid(uint32_t generation) {
    return auto_enabled && auto_generation == generation;
}
static unsigned int irq_lock(void) { return 0; }
static void irq_unlock(unsigned int key) {}
static uint16_t tracker_test_on_tps[MAX_TRACKERS];
static atomic_t test_all_state_valid, test_all_enabled, test_all_confirmed_mask;
static uint16_t test_all_wire_tps(void) { return 0; }
static int fail_stop, fail_set, fail_start, fail_store;
static int esb_stop_rx(void) {
    if (fail_stop) { fail_stop--; return -EIO; }
    rx_running = false; return 0;
}
static int esb_set_rf_channel(uint8_t c) {
    assert(!rx_running);
    if (fail_set) { fail_set--; return -EIO; }
    physical_channel = c; return 0;
}
static int esb_start_rx(void) {
    assert(!stale_fifo);
    if (fail_start) { fail_start--; return -EIO; }
    rx_running = true; return 0;
}
static int sys_write_checked(uint16_t id, const void *data, size_t len) {
    assert(id == RF_CHANNEL && len == 1 && rx_running);
    if (fail_store) { fail_store--; return -EIO; }
    durable_channel = *(const uint8_t *)data;
    return 0;
}
static void set_status(int status, bool value) { fault_status = value; }
static void auto_channel_set_enabled(bool value) { auto_enabled = value; }
static unsigned auto_completed;
static uint32_t completed_generation;
static uint8_t completed_old_home, completed_new_home;
static bool auto_succeeded;
static void auto_channel_migration_complete(uint32_t generation, uint8_t old_home,
                                            uint8_t new_home, bool success) {
    auto_completed++;
    completed_generation = generation;
    completed_old_home = old_home;
    completed_new_home = new_home;
    auto_succeeded = success;
    if (success) assert(rx_running && physical_channel == new_home &&
                        (durable_channel == 128 ? 0 : durable_channel) == new_home);
}
struct payload { uint8_t data[13]; };
/* PRODUCTION BODIES */
static unsigned completed;
static bool succeeded;
static void done(bool success) {
    assert(esb_channel_is_busy());
    assert(esb_set_all_trackers_channel(7) == (esb_radio_fault ? -EIO : -EBUSY));
    completed++;
    succeeded = success;
}
static void reset(unsigned count) {
    radio_operation_busy = channel_change_pending = 0;
    tracker_store_lock = 0;
    stored_trackers = count;
    memset(stored_tracker_addr, 0, sizeof(stored_tracker_addr));
    memset(tracker_remote_command, 0, sizeof(tracker_remote_command));
    memset(abort_pending, 0, sizeof(abort_pending));
    for (unsigned i = 0; i < count; i++) stored_tracker_addr[i] = 100 + i;
    receiver_rf_channel = physical_channel = durable_channel = 22;
    rx_running = auto_enabled = true;
    fault_status = ota = self_ota = collecting = batch = false;
    fail_stop = fail_set = fail_start = fail_store = 0;
    now = 100;
    completed = 0;
    auto_completed = 0;
    esb_radio_fault = esb_pairing = false;
    succeeded = false;
    channel_change_done_cb = done;
}
static void deadline(void) { now = channel_change_timeout; channel_change_poll(); }
static void assert_wire(uint8_t id, uint8_t flag, uint8_t channel) {
    struct payload p = {0};
    assert(pong(id, &p) == flag);
    if (flag == ESB_PONG_FLAG_SET_CHANNEL) {
        assert(p.data[8] == 0 && p.data[9] == 0 && p.data[10] == 0 && p.data[11] == channel);
    }
}
int main(void) {
    /* Production channel_apply discards queued old-home packets before
     * resetting admission, including storage-error rollback. */
    for (unsigned rollback = 0; rollback < 2; ++rollback) {
        reset(2);
        memset(sequence_initialized, 0, sizeof(sequence_initialized));
        memset(tracker_stats, 0, sizeof(tracker_stats));
        assert(check_packet_sequence(0, 100) == 0);
        assert(check_packet_sequence(1, 255) == 0);
        assert(esb_set_all_trackers_channel(26) == 0);
        stale_fifo = true;
        fail_store = rollback;
        deadline();
        assert(!stale_fifo && rx_running);
        assert(physical_channel == (rollback ? 22 : 26));
        assert(check_packet_sequence(0, 64) == 0);
        assert(check_packet_sequence(0, 64) == 4);
        assert(check_packet_sequence(0, 63) == 2);
        assert(check_packet_sequence(1, 220) == 0);
        assert(check_packet_sequence(1, 220) == 4);
        assert(tracker_stats[0].total_received == 2);
        assert(tracker_stats[0].total_gaps == 0 && tracker_stats[0].restart_events == 0);
        assert(tracker_stats[1].total_received == 2);
    }
    /* No live traffic, partial traffic, all traffic: delivery is not a barrier. */
    for (unsigned count = 0; count <= 3; count++) {
        for (unsigned online = 0; online <= count; online++) {
            reset(count);
            assert(esb_set_all_trackers_channel(100) == 0);
            assert(!auto_enabled);
            for (unsigned i = 0; i < online; i++) assert_wire(i, 10, 100);
            now = channel_change_timeout - 1;
            channel_change_poll();
            assert(!completed && physical_channel == 22);
            deadline();
            assert(completed == 1 && succeeded && physical_channel == 100 && durable_channel == 100);
            assert(auto_completed == 0);
            assert(!esb_channel_is_busy() && !channel_change_pending && rx_running);
            for (unsigned i = 0; i < count; i++) assert_wire(i, 0, 0);
            channel_change_poll();
            assert(completed == 1);
        }
    }
    /* Generic pending commands are neither consumed nor zero-channel corrupted. */
    reset(3);
    esb_send_remote_command_arg(0, 4, 77);
    assert(esb_set_all_trackers_channel(31) == 0);
    assert(esb_set_all_trackers_channel(32) == -EBUSY);
    assert(esb_set_receiver_channel(32) == -EBUSY);
    assert(esb_clear_receiver_channel() == -EBUSY);
    esb_send_remote_command_arg(1, 7, 0);
    esb_send_remote_command_arg(0, 10, 0); /* Reject raw channel opcode injection. */
    assert_wire(0, 10, 31);
    assert_wire(1, 10, 31);
    assert(tracker_remote_command[0] == 4 && pending_cmd_arg[0] == 77);
    echo(0, ESB_PONG_FLAG_SET_CHANNEL); /* No echo can end the announcement. */
    channel_change_poll();
    assert(completed == 0 && physical_channel == 22);
    deadline();
    assert_wire(0, 4, 0);
    assert_wire(1, 7, 0);
    /* Repeated destination/new destination/clear do not reuse prior completion. */
    assert(esb_set_all_trackers_channel(31) == 0);
    assert_wire(0, 10, 31);
    channel_change_poll();
    assert(completed == 1);
    deadline();
    assert(completed == 2 && succeeded);
    assert(esb_set_all_trackers_channel(0) == 0);
    echo(0, ESB_PONG_FLAG_SET_CHANNEL); /* Late echo from the old transaction. */
    assert_wire(0, 10, 0);
    assert_wire(2, 10, 0);
    deadline();
    assert(physical_channel == 0 && durable_channel == 128);
    assert(esb_clear_all_trackers_channel() == 0);
    assert_wire(2, 11, 0);
    deadline();
    assert(physical_channel == RADIO_RF_CHANNEL && durable_channel == 255);
    /* Snapshot doesn't grow/shrink with stored count while announcing. */
    reset(2);
    assert(esb_set_all_trackers_channel(55) == 0);
    stored_trackers = 3;
    stored_tracker_addr[2] = 200;
    assert_wire(2, 0, 0);
    stored_trackers = 0;
    deadline();
    assert(succeeded && physical_channel == 55);
    /* First failed stop, tuning, start or persistence never reports success. */
    for (unsigned failure = 0; failure < 4; failure++) {
        reset(2);
        assert(esb_set_all_trackers_channel(1) == 0);
        if (failure == 0) fail_stop = 1;
        if (failure == 1) fail_set = 1;
        if (failure == 2) fail_start = 1;
        if (failure == 3) fail_store = 1;
        deadline();
        assert(completed == 1 && !succeeded && !esb_channel_is_busy());
        assert(physical_channel == 22 && durable_channel == 22 && receiver_rf_channel == 22 && rx_running);
        assert_wire(0, 0, 0);
    }
    reset(1);
    assert(esb_set_all_trackers_channel(1) == 0);
    fail_set = 2;
    deadline();
    assert(!succeeded && fault_status && !rx_running);
    /* All incompatible transfer/owner states exclude admission. */
    reset(1);
    bool *busy[] = {&ota, &self_ota, &collecting, &batch, &abort_pending[0]};
    for (unsigned i = 0; i < sizeof(busy) / sizeof(*busy); i++) {
        *busy[i] = true;
        assert(esb_set_all_trackers_channel(3) == -EBUSY);
        assert(esb_set_receiver_channel(3) == -EBUSY);
        *busy[i] = false;
    }
    assert(esb_radio_operation_begin() == 0);
    assert(esb_set_all_trackers_channel(3) == -EBUSY);
    esb_radio_operation_end();
    assert(esb_set_all_trackers_channel(101) == -EINVAL);
    auto_enabled = false;
    assert(esb_auto_channel_request(88, 22, auto_generation) == -EBUSY);
    auto_enabled = true;
    assert(esb_auto_channel_request(88, 22, auto_generation + 1) == -EBUSY);
    assert(esb_auto_channel_request(88, 23, auto_generation) == -EBUSY);
    esb_pairing = true;
    assert(esb_auto_channel_request(88, 22, auto_generation) == -EBUSY);
    esb_pairing = false;
    for (unsigned ch = 0; ch <= 100; ch++) {
        if ((ch & 1) || ch > 88)
            assert(esb_auto_channel_request(ch, 22, auto_generation) == -EINVAL);
    }
    /* 90 is even but outside the automatic destination range; 88 is the highest. */
    assert(esb_auto_channel_request(90, 22, auto_generation) == -EINVAL);
    assert(esb_auto_channel_request(89, 22, auto_generation) == -EINVAL);
    assert(esb_auto_channel_request(88, 22, auto_generation) == 0 && auto_enabled);
    assert(auto_completed == 0);
    deadline();
    assert(succeeded && esb_get_receiver_channel() == 88);
    assert(auto_completed == 1 && auto_succeeded);
    assert(completed_generation == auto_generation && completed_old_home == 22 && completed_new_home == 88);
    assert(esb_auto_channel_request(0, 88, auto_generation) == 0);
    deadline();
    assert(succeeded && durable_channel == 128);
    assert(esb_set_receiver_channel(99) == 0);
    assert(esb_get_receiver_channel() == 99);
    assert(esb_set_receiver_channel(0) == 0 && !auto_enabled);
    assert(esb_get_receiver_channel() == 0 && durable_channel == 128);
    assert(esb_clear_receiver_channel() == 0 && durable_channel == 255);
    /* No-op automatic requests never count as useful moves. */
    reset(1);
    assert(esb_auto_channel_request(22, 22, auto_generation) == 0);
    deadline();
    assert(succeeded && auto_completed == 1 && !auto_succeeded);
    /* Failure (including failed rollback) publishes failure with original tag. */
    for (unsigned failure = 0; failure < 5; ++failure) {
        reset(1);
        assert(esb_auto_channel_request(88, 22, auto_generation) == 0);
        if (failure == 0) fail_stop = 1;
        if (failure == 1) fail_set = 1;
        if (failure == 2) fail_start = 1;
        if (failure == 3) fail_store = 1;
        if (failure == 4) fail_set = 2;
        deadline();
        assert(auto_completed == 1 && !auto_succeeded && !succeeded);
        assert(completed_old_home == 22 && completed_new_home == 22);
        assert(esb_radio_fault == (failure == 4));
        assert(esb_channel_is_busy() == (failure == 4));
        if (failure == 4) assert(esb_radio_operation_begin() == -EIO);
    }
    /* Cancellation during an accepted announcement keeps the original tag;
     * the worker can reject it rather than associating it with the new intent. */
    reset(1);
    uint32_t original_generation = auto_generation;
    assert(esb_auto_channel_request(88, 22, original_generation) == 0);
    auto_channel_set_enabled(false);
    auto_generation++;
    deadline();
    assert(auto_completed == 1 && completed_generation == original_generation);
    assert(esb_set_all_trackers_channel(40) == 0);
    deadline();
    assert(auto_completed == 1 && physical_channel == 40);
    reset(3);
    assert(rcv_cmd_collect_start(0) == RCV_HID_ST_OK && collecting && !batch);
    assert(auto_loss_reset_epoch != 0);
    assert(esb_set_all_trackers_channel(33) == -EBUSY);
    ota = true;
    assert(rcv_cmd_collect_start(1) == RCV_HID_ST_EBUSY);
    assert(collecting && collecting_id == 0); /* failed replacement preserves old session */
    ota = false;
    assert(rcv_cmd_collect_batch_start(50) == RCV_HID_ST_OK && !collecting && batch);
    assert(esb_set_receiver_channel(33) == -EBUSY);
    assert(rcv_cmd_collect_batch_stop() == RCV_HID_ST_OK && !batch);
    assert(esb_set_all_trackers_channel(33) == 0);
    assert(rcv_cmd_collect_start(0) == RCV_HID_ST_EBUSY && !collecting);
    assert(rcv_cmd_collect_batch_start(50) == RCV_HID_ST_EBUSY && !batch);
    deadline();
    /* Whole raw/batch sessions without advancing time invalidate evidence. */
    uint32_t epoch = auto_loss_reset_epoch;
    assert(rcv_cmd_collect_start(0) == RCV_HID_ST_OK);
    assert(rcv_cmd_collect_stop() == RCV_HID_ST_OK);
    assert(!collecting && auto_loss_reset_epoch != epoch);
    epoch = auto_loss_reset_epoch;
    assert(rcv_cmd_collect_batch_start(50) == RCV_HID_ST_OK);
    assert(rcv_cmd_collect_batch_stop() == RCV_HID_ST_OK);
    assert(!batch && auto_loss_reset_epoch != epoch);
    puts("PASS receiver channel migration: offline/partial/all, fixed deadline, pending command isolation, snapshot, repeat/clear/zero, RF/storage rollback and exclusion");
    return 0;
}
