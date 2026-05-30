#include "connection/esb.h"
#include "globals.h"
#include "hid.h"

#include <lvgl.h>
#include <lvgl_input_device.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(slimenrf_ui, LOG_LEVEL_INF);

#if !DT_HAS_CHOSEN(zephyr_display)
#error "CONFIG_SLIMENRF_DISPLAY_UI requires a zephyr,display chosen node"
#endif

#define UI_DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define UI_BACKLIGHT_NODE DT_ALIAS(lcd_backlight)

#define UI_THREAD_STACK_SIZE 4096
#define UI_COMMAND_STACK_SIZE 2048
#define UI_THREAD_PRIORITY 12
#define UI_COMMAND_PRIORITY 13

#define UI_TRACKER_ROWS 5
#define UI_IDLE_REFRESH_MS 500
#define UI_FAST_REFRESH_MS 100
#define UI_FAST_WINDOW_MS 2000
#define UI_LOOP_SLEEP_MS 20
#define UI_LONG_PRESS_MS 800

#if LV_FONT_MONTSERRAT_12
#define UI_FONT_SMALL (&lv_font_montserrat_12)
#else
#define UI_FONT_SMALL LV_FONT_DEFAULT
#endif

enum ui_key {
	UI_KEY_UP,
	UI_KEY_DOWN,
	UI_KEY_ENTER,
};

enum ui_view {
	UI_VIEW_MAIN,
	UI_VIEW_MENU,
	UI_VIEW_CONFIRM,
};

enum ui_command {
	UI_COMMAND_NONE,
	UI_COMMAND_PAIR_TOGGLE,
	UI_COMMAND_STATS_TOGGLE,
	UI_COMMAND_PING_ALL,
	UI_COMMAND_MEOW_ALL,
	UI_COMMAND_CALIBRATE_ALL,
};

struct ui_input_msg {
	enum ui_key key;
	bool pressed;
	int64_t time_ms;
};

struct ui_command_msg {
	enum ui_command command;
};

struct ui_menu_item {
	const char *label;
	enum ui_command command;
	bool confirm;
};

struct ui_model {
	enum ui_view view;
	enum ui_command confirm_command;
	uint8_t menu_index;
	uint8_t tracker_offset;
	bool enter_pressed;
	int64_t enter_press_time;
	int64_t fast_until;
	int64_t toast_until;
	bool dirty;
	char toast[32];
};

static const struct device *const display_dev = DEVICE_DT_GET(UI_DISPLAY_NODE);

#if DT_NODE_EXISTS(UI_BACKLIGHT_NODE)
static const struct pwm_dt_spec backlight = PWM_DT_SPEC_GET(UI_BACKLIGHT_NODE);
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(zephyr_lvgl_keypad_input)
static const struct device *const lvgl_keypad_dev =
	DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_lvgl_keypad_input));
#endif

static const struct ui_menu_item menu_items[] = {
	{ "Pair toggle", UI_COMMAND_PAIR_TOGGLE, false },
	{ "Stats toggle", UI_COMMAND_STATS_TOGGLE, false },
	{ "Ping all", UI_COMMAND_PING_ALL, true },
	{ "Meow all", UI_COMMAND_MEOW_ALL, true },
	{ "Cal all", UI_COMMAND_CALIBRATE_ALL, true },
	{ "Back", UI_COMMAND_NONE, false },
};

static struct ui_model ui = {
	.view = UI_VIEW_MAIN,
	.dirty = true,
};

static lv_obj_t *title_label;
static lv_obj_t *status_label;
static lv_obj_t *state_label;
static lv_obj_t *row_labels[UI_TRACKER_ROWS];
static lv_obj_t *footer_label;
static lv_group_t *ui_group;

K_MSGQ_DEFINE(ui_input_msgq, sizeof(struct ui_input_msg), 12, 4);
K_MSGQ_DEFINE(ui_command_msgq, sizeof(struct ui_command_msg), 4, 4);

static void ui_thread(void);
static void ui_command_thread(void);

K_THREAD_DEFINE(slimenrf_ui_thread_id, UI_THREAD_STACK_SIZE, ui_thread, NULL, NULL, NULL,
		UI_THREAD_PRIORITY, 0, 0);
K_THREAD_DEFINE(slimenrf_ui_command_thread_id, UI_COMMAND_STACK_SIZE, ui_command_thread, NULL, NULL,
		NULL, UI_COMMAND_PRIORITY, 0, 0);

static bool ui_key_from_input(uint16_t code, enum ui_key *key)
{
	switch (code) {
	case INPUT_KEY_UP:
		*key = UI_KEY_UP;
		return true;
	case INPUT_KEY_DOWN:
		*key = UI_KEY_DOWN;
		return true;
	case INPUT_KEY_ENTER:
		*key = UI_KEY_ENTER;
		return true;
	default:
		return false;
	}
}

static void ui_input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	enum ui_key key;

	if (evt->type != INPUT_EV_KEY || !ui_key_from_input(evt->code, &key)) {
		return;
	}

	struct ui_input_msg msg = {
		.key = key,
		.pressed = evt->value != 0,
		.time_ms = k_uptime_get(),
	};

	(void)k_msgq_put(&ui_input_msgq, &msg, K_NO_WAIT);
}

INPUT_CALLBACK_DEFINE(NULL, ui_input_cb, NULL);

static const char *ui_command_name(enum ui_command command)
{
	switch (command) {
	case UI_COMMAND_PAIR_TOGGLE:
		return "PAIR";
	case UI_COMMAND_STATS_TOGGLE:
		return "STATS";
	case UI_COMMAND_PING_ALL:
		return "PING";
	case UI_COMMAND_MEOW_ALL:
		return "MEOW";
	case UI_COMMAND_CALIBRATE_ALL:
		return "CAL";
	default:
		return "CMD";
	}
}

static void ui_mark_fast(void)
{
	ui.fast_until = k_uptime_get() + UI_FAST_WINDOW_MS;
	ui.dirty = true;
}

static void ui_toast(const char *text)
{
	(void)snprintk(ui.toast, sizeof(ui.toast), "%s", text);
	ui.toast_until = k_uptime_get() + UI_FAST_WINDOW_MS;
	ui_mark_fast();
}

static void ui_queue_command(enum ui_command command)
{
	struct ui_command_msg msg = {
		.command = command,
	};
	char toast[32];

	if (k_msgq_put(&ui_command_msgq, &msg, K_NO_WAIT) == 0) {
		(void)snprintk(toast, sizeof(toast), "Queued %s", ui_command_name(command));
		ui_toast(toast);
	} else {
		ui_toast("Command busy");
	}
}

static void ui_set_text(lv_obj_t *label, const char *text, lv_color_t color)
{
	lv_label_set_text(label, text);
	lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
}

static lv_obj_t *ui_create_label(int16_t x, int16_t y, int16_t w, const lv_font_t *font,
				 lv_color_t color)
{
	lv_obj_t *label = lv_label_create(lv_screen_active());

	lv_obj_set_pos(label, x, y);
	lv_obj_set_width(label, w);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
	lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
	lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN);
	lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

	return label;
}

static void ui_format_age(char *buf, size_t len, uint32_t ms)
{
	if (ms == UINT32_MAX) {
		(void)snprintk(buf, len, "--");
	} else if (ms < 1000) {
		(void)snprintk(buf, len, "now");
	} else if (ms < 60000) {
		(void)snprintk(buf, len, "%us", ms / 1000);
	} else {
		(void)snprintk(buf, len, "%um", ms / 60000);
	}
}

static void ui_format_rssi(char *buf, size_t len, int8_t rssi)
{
	if (rssi <= 0) {
		(void)snprintk(buf, len, "R--");
	} else {
		(void)snprintk(buf, len, "R-%u", (uint8_t)rssi);
	}
}

static void ui_format_channel(char *buf, size_t len, uint8_t channel)
{
	if (channel == 0xFF || channel == 0) {
		(void)snprintk(buf, len, "DEF");
	} else {
		(void)snprintk(buf, len, "%u", channel);
	}
}

static uint8_t ui_max_tracker_offset(size_t tracker_count)
{
	if (tracker_count <= UI_TRACKER_ROWS) {
		return 0;
	}

	return (uint8_t)(tracker_count - UI_TRACKER_ROWS);
}

static void ui_render_main(void)
{
	struct esb_receiver_snapshot receiver = {0};
	struct esb_tracker_snapshot trackers[MAX_TRACKERS];
	char line[80];
	char channel[8];
	size_t tracker_count;

	esb_get_receiver_snapshot(&receiver);
	tracker_count = esb_get_tracker_snapshots(trackers, ARRAY_SIZE(trackers));
	ui.tracker_offset = MIN(ui.tracker_offset, ui_max_tracker_offset(tracker_count));
	ui_format_channel(channel, sizeof(channel), receiver.rf_channel);

	(void)snprintk(line, sizeof(line), "EWT73 RX  USB:%s  RF:%s",
		       hid_usb_is_configured() ? "ON" : "OFF", channel);
	ui_set_text(title_label, line, lv_color_hex(0xf5f7fa));

	(void)snprintk(line, sizeof(line), "TK:%u/%u  TPS:%u  PAIR:%s  STAT:%s",
		       receiver.active_count, receiver.paired_count, receiver.total_tps,
		       receiver.pairing ? "ON" : "OFF",
		       receiver.stats_detailed ? "ON" : "OFF");
	ui_set_text(status_label, line, receiver.pairing ? lv_color_hex(0x9adf6f) :
							 lv_color_hex(0xb6c2cc));

	if (ui.toast_until > k_uptime_get()) {
		ui_set_text(state_label, ui.toast, lv_color_hex(0xffd166));
	} else if (tracker_count == 0) {
		ui_set_text(state_label, "No paired tk", lv_color_hex(0xffd166));
	} else {
		(void)snprintk(line, sizeof(line), "List %u-%u/%u",
			       ui.tracker_offset + 1,
			       (uint8_t)MIN((size_t)ui.tracker_offset + UI_TRACKER_ROWS,
					     tracker_count),
			       (uint8_t)tracker_count);
		ui_set_text(state_label, line, lv_color_hex(0x75d5ff));
	}

	for (uint8_t row = 0; row < UI_TRACKER_ROWS; row++) {
		size_t index = ui.tracker_offset + row;

		if (index >= tracker_count) {
			ui_set_text(row_labels[row], "", lv_color_hex(0xb6c2cc));
			continue;
		}

		char age[8];
		char rssi[8];
		const struct esb_tracker_snapshot *tracker = &trackers[index];

		ui_format_age(age, sizeof(age), tracker->last_seen_ms);
		ui_format_rssi(rssi, sizeof(rssi), tracker->rssi);

		(void)snprintk(line, sizeof(line), "%02u %06X %s T%3u %s %s",
			       tracker->id,
			       (uint32_t)(tracker->address & 0xFFFFFFu),
			       tracker->active ? "ACT" : "OFF",
			       tracker->tps,
			       rssi,
			       age);
		ui_set_text(row_labels[row], line,
			    tracker->active ? lv_color_hex(0xf5f7fa) : lv_color_hex(0x7a8591));
	}

	ui_set_text(footer_label, "SW4/SW3 Scroll    SW1 Menu", lv_color_hex(0x9aa6b2));
}

static void ui_render_menu(void)
{
	struct esb_receiver_snapshot receiver = {0};
	char line[80];
	uint8_t menu_start = 0;

	esb_get_receiver_snapshot(&receiver);
	if (ui.menu_index >= UI_TRACKER_ROWS) {
		menu_start = ui.menu_index - UI_TRACKER_ROWS + 1;
	}

	ui_set_text(title_label, "Menu", lv_color_hex(0xf5f7fa));
	(void)snprintk(line, sizeof(line), "PAIR:%s  STAT:%s  ACTIVE:%u",
		       receiver.pairing ? "ON" : "OFF",
		       receiver.stats_detailed ? "ON" : "OFF",
		       receiver.active_count);
	ui_set_text(status_label, line, lv_color_hex(0xb6c2cc));

	if (ui.toast_until > k_uptime_get()) {
		ui_set_text(state_label, ui.toast, lv_color_hex(0xffd166));
	} else {
		ui_set_text(state_label, "Safe commands only", lv_color_hex(0x75d5ff));
	}

	for (uint8_t row = 0; row < UI_TRACKER_ROWS; row++) {
		uint8_t item_index = menu_start + row;

		if (item_index >= ARRAY_SIZE(menu_items)) {
			ui_set_text(row_labels[row], "", lv_color_hex(0xb6c2cc));
			continue;
		}

		const struct ui_menu_item *item = &menu_items[item_index];
		const char *label = item->label;

		if (item->command == UI_COMMAND_PAIR_TOGGLE) {
			label = receiver.pairing ? "Pair off" : "Pair on";
		} else if (item->command == UI_COMMAND_STATS_TOGGLE) {
			label = receiver.stats_detailed ? "Stats off" : "Stats on";
		}

		(void)snprintk(line, sizeof(line), "%c %s",
			       ui.menu_index == item_index ? '>' : ' ', label);
		ui_set_text(row_labels[row], line,
			    ui.menu_index == item_index ? lv_color_hex(0xffd166) :
							  lv_color_hex(0xf5f7fa));
	}

	ui_set_text(footer_label, "SW1 Select    Hold SW1 Back", lv_color_hex(0x9aa6b2));
}

static void ui_render_confirm(void)
{
	char line[80];

	ui_set_text(title_label, "Confirm", lv_color_hex(0xffd166));
	(void)snprintk(line, sizeof(line), "%s all active tk", ui_command_name(ui.confirm_command));
	ui_set_text(status_label, line, lv_color_hex(0xf5f7fa));
	ui_set_text(state_label, "Hold SW1 800ms to run", lv_color_hex(0x75d5ff));

	for (uint8_t row = 0; row < UI_TRACKER_ROWS; row++) {
		ui_set_text(row_labels[row], "", lv_color_hex(0xb6c2cc));
	}

	ui_set_text(row_labels[1], "UP/DOWN cancels", lv_color_hex(0xb6c2cc));
	ui_set_text(row_labels[2], "No shutdown/reboot/DFU", lv_color_hex(0x9adf6f));
	ui_set_text(footer_label, "SW4/SW3 Cancel", lv_color_hex(0x9aa6b2));
}

static void ui_render(void)
{
	switch (ui.view) {
	case UI_VIEW_MENU:
		ui_render_menu();
		break;
	case UI_VIEW_CONFIRM:
		ui_render_confirm();
		break;
	default:
		ui_render_main();
		break;
	}

	ui.dirty = false;
}

static void ui_cancel_confirm(void)
{
	ui.view = UI_VIEW_MENU;
	ui.confirm_command = UI_COMMAND_NONE;
	ui_toast("Canceled");
}

static void ui_handle_up_down(enum ui_key key)
{
	if (ui.view == UI_VIEW_MAIN) {
		if (key == UI_KEY_UP && ui.tracker_offset > 0) {
			ui.tracker_offset--;
		} else if (key == UI_KEY_DOWN) {
			struct esb_tracker_snapshot trackers[MAX_TRACKERS];
			size_t count = esb_get_tracker_snapshots(trackers, ARRAY_SIZE(trackers));
			uint8_t max_offset = ui_max_tracker_offset(count);

			if (ui.tracker_offset < max_offset) {
				ui.tracker_offset++;
			}
		}
		ui_mark_fast();
		return;
	}

	if (ui.view == UI_VIEW_MENU) {
		if (key == UI_KEY_UP) {
			ui.menu_index = ui.menu_index == 0 ? ARRAY_SIZE(menu_items) - 1 : ui.menu_index - 1;
		} else {
			ui.menu_index = (ui.menu_index + 1) % ARRAY_SIZE(menu_items);
		}
		ui_mark_fast();
		return;
	}

	if (ui.view == UI_VIEW_CONFIRM) {
		ui_cancel_confirm();
	}
}

static void ui_handle_enter_release(int64_t duration_ms)
{
	if (ui.view == UI_VIEW_MAIN) {
		ui.view = UI_VIEW_MENU;
		ui.menu_index = 0;
		ui_mark_fast();
		return;
	}

	if (ui.view == UI_VIEW_MENU) {
		if (duration_ms >= UI_LONG_PRESS_MS) {
			ui.view = UI_VIEW_MAIN;
			ui_mark_fast();
			return;
		}

		const struct ui_menu_item *item = &menu_items[ui.menu_index];

		if (item->command == UI_COMMAND_NONE) {
			ui.view = UI_VIEW_MAIN;
			ui_mark_fast();
		} else if (item->confirm) {
			ui.confirm_command = item->command;
			ui.view = UI_VIEW_CONFIRM;
			ui_mark_fast();
		} else {
			ui_queue_command(item->command);
		}
		return;
	}

	if (ui.view == UI_VIEW_CONFIRM) {
		if (duration_ms >= UI_LONG_PRESS_MS) {
			ui_queue_command(ui.confirm_command);
			ui.confirm_command = UI_COMMAND_NONE;
			ui.view = UI_VIEW_MENU;
		} else {
			ui_toast("Hold longer");
		}
	}
}

static void ui_handle_input(const struct ui_input_msg *msg)
{
	if (msg->key == UI_KEY_ENTER) {
		if (msg->pressed) {
			ui.enter_pressed = true;
			ui.enter_press_time = msg->time_ms;
			ui_mark_fast();
		} else if (ui.enter_pressed) {
			int64_t duration = msg->time_ms - ui.enter_press_time;

			ui.enter_pressed = false;
			ui_handle_enter_release(duration);
		}
		return;
	}

	if (msg->pressed) {
		ui_handle_up_down(msg->key);
	}
}

static void ui_process_input(void)
{
	struct ui_input_msg msg;

	while (k_msgq_get(&ui_input_msgq, &msg, K_NO_WAIT) == 0) {
		ui_handle_input(&msg);
	}
}

static void ui_init_display(void)
{
	lv_obj_t *screen = lv_screen_active();

	lv_obj_set_style_bg_color(screen, lv_color_hex(0x101315), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

	title_label = ui_create_label(4, 1, 232, LV_FONT_DEFAULT, lv_color_hex(0xf5f7fa));
	status_label = ui_create_label(4, 18, 232, UI_FONT_SMALL, lv_color_hex(0xb6c2cc));
	state_label = ui_create_label(4, 34, 232, UI_FONT_SMALL, lv_color_hex(0x75d5ff));

	for (uint8_t i = 0; i < UI_TRACKER_ROWS; i++) {
		row_labels[i] = ui_create_label(4, 50 + (i * 14), 232, UI_FONT_SMALL,
						lv_color_hex(0xf5f7fa));
	}

	footer_label = ui_create_label(4, 122, 232, UI_FONT_SMALL, lv_color_hex(0x9aa6b2));

	ui_group = lv_group_create();
	if (ui_group != NULL) {
#if DT_HAS_COMPAT_STATUS_OKAY(zephyr_lvgl_keypad_input)
		lv_indev_t *indev = lvgl_input_get_indev(lvgl_keypad_dev);

		if (indev != NULL) {
			lv_indev_set_group(indev, ui_group);
		}
#endif
	}

	ui_render();
	lv_timer_handler();
	display_blanking_off(display_dev);
}

static void ui_init_backlight(void)
{
#if DT_NODE_EXISTS(UI_BACKLIGHT_NODE)
	if (!pwm_is_ready_dt(&backlight)) {
		LOG_WRN("LCD backlight PWM is not ready");
		return;
	}

	int err = pwm_set_dt(&backlight, backlight.period, backlight.period);

	if (err) {
		LOG_WRN("LCD backlight PWM set failed: %d", err);
	}
#endif
}

static void ui_thread(void)
{
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device is not ready");
		return;
	}

	ui_init_backlight();
	ui_init_display();
	LOG_INF("EWT73 ST7789 status UI started");

	int64_t last_render = 0;

	while (1) {
		int64_t now = k_uptime_get();
		int64_t refresh_ms = now < ui.fast_until ? UI_FAST_REFRESH_MS : UI_IDLE_REFRESH_MS;

		ui_process_input();

		if (ui.dirty || now - last_render >= refresh_ms) {
			ui_render();
			last_render = now;
		}

		lv_timer_handler();
		k_sleep(K_MSEC(UI_LOOP_SLEEP_MS));
	}
}

static void ui_command_thread(void)
{
	struct ui_command_msg msg;

	while (1) {
		k_msgq_get(&ui_command_msgq, &msg, K_FOREVER);

		switch (msg.command) {
		case UI_COMMAND_PAIR_TOGGLE: {
			struct esb_receiver_snapshot receiver = {0};

			esb_get_receiver_snapshot(&receiver);
			if (receiver.pairing) {
				esb_finish_pair();
			} else {
				esb_start_pairing();
			}
			break;
		}
		case UI_COMMAND_STATS_TOGGLE:
			(void)esb_toggle_stats_detailed();
			break;
		case UI_COMMAND_PING_ALL:
			esb_send_remote_command_all(ESB_PONG_FLAG_PING);
			break;
		case UI_COMMAND_MEOW_ALL:
			esb_send_remote_command_all(ESB_PONG_FLAG_MEOW);
			break;
		case UI_COMMAND_CALIBRATE_ALL:
			esb_send_remote_command_all(ESB_PONG_FLAG_CALIBRATE);
			break;
		default:
			break;
		}

		ui_mark_fast();
	}
}
