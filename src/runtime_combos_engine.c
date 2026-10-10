/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Key event engine for runtime combos.
 *
 * Structure follows ZMK's static combo implementation (app/src/combo.c): the
 * same candidate filtering, capture/re-raise of position events, timeout task
 * and slow-release semantics. The difference is where the combo list comes
 * from -- a RAM slot table written over the Studio RPC instead of a const
 * array generated from the devicetree.
 *
 * With no combo configured, the listener still forwards keycode events to
 * store_last_tapped and applies a pending refresh, then returns BUBBLE.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <stdint.h>
#include <string.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/virtual_key_position.h>
#include <zmk/combos.h>
#include <zmk/input_listeners.h>

#include "runtime_combos_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* One 32-bit word carries the candidate set, so the slot table has to fit. */
BUILD_ASSERT(CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT <= 32,
	     "Runtime combo slots must fit in a 32 bit candidate mask");

/*
 * Virtual key position used when invoking a combo's behavior.
 *
 * Upstream hands out positions in this order: real keys, sensors, static
 * combos, then input processors. The input processor range grows with
 * (listeners x processors) and has no published upper bound, so we skip a
 * headroom of 16 processors per listener before taking ours. A behavior only
 * uses this value to tell one invocation from another, so a gap costs nothing.
 *
 * On a board with no input listeners the headroom is 0 and our first position
 * equals the static combo range. That range is empty on such a board, and no
 * input processor positions exist either, so nothing collides.
 */
#define RUNTIME_COMBOS_VKP_HEADROOM (ZMK_INPUT_LISTENERS_LEN * 16)
#define RUNTIME_COMBO_VKP(index)                                                                   \
	(ZMK_VIRTUAL_KEY_POSITION_COMBO(ZMK_COMBOS_LEN) + RUNTIME_COMBOS_VKP_HEADROOM + (index))

struct active_combo {
	int8_t slot;
	struct zmk_behavior_binding binding;
	/* Filled with the combo's keys on press; entries are removed as they are
	 * released. Empty means every key is up. */
	uint8_t key_positions_pressed_count;
	struct zmk_position_state_changed_event key_positions_pressed[ZMK_RUNTIME_COMBOS_MAX_KEYS];
};

/* bit i set = slot i listens on this key position */
static uint32_t combo_lookup[ZMK_KEYMAP_LEN];
/* slots still possible given the keys held so far */
static uint32_t candidates;
/* set once any slot is active, so the common "no combos" path stays cheap */
static bool any_combo_active;

static uint8_t pressed_keys_count;
static struct zmk_position_state_changed_event pressed_keys[ZMK_RUNTIME_COMBOS_MAX_KEYS];

/* the last candidate found completely pressed, -1 when there is none */
static int8_t fully_pressed_combo = -1;

static struct active_combo active_combos[CONFIG_ZMK_RUNTIME_COMBOS_MAX_PRESSED];
static uint8_t active_combo_count;

static struct k_work_delayable timeout_task;
static int64_t timeout_task_timeout_at;
/* True while combo_timeout_handler runs. See stop_timeout_task(). */
static bool in_timeout_handler;

/* when a non-modifier key was last tapped, and when a combo last fired */
static int64_t last_tapped_timestamp = INT32_MIN;
static int64_t last_combo_timestamp = INT32_MIN;

/* Set by zmk_runtime_combos_engine_refresh(), consumed at the top of the key
 * event listener. The RPC thread must not walk the tables below while a key is
 * being processed, so it only raises this flag and the rebuild happens in the
 * event context. */
static atomic_t refresh_pending = ATOMIC_INIT(1);

static int cleanup(void);
static void apply_pending_refresh(void);
static void deactivate_combo(int active_combo_index);

/* ------------------------------------------------------------------ */
/* Slot helpers                                                        */
/* ------------------------------------------------------------------ */

static inline const struct runtime_combo_config *slot_cfg(int slot) {
	return &zmk_runtime_combos_slots[slot];
}

static bool slot_is_usable(const struct runtime_combo_config *cfg) {
	if (!cfg->active) {
		return false;
	}
	if (cfg->key_count < 2 || cfg->key_count > ZMK_RUNTIME_COMBOS_MAX_KEYS) {
		return false;
	}
	/* The RPC writes 0 when the request carried no binding at all. */
	if (cfg->behavior_local_id == 0) {
		return false;
	}
	/* A stored id this firmware does not know would capture and delay the
	 * keys on every press and then fire nothing; let them pass through. */
	if (zmk_behavior_find_behavior_name_from_local_id(cfg->behavior_local_id) == NULL) {
		return false;
	}
	for (uint8_t i = 0; i < cfg->key_count; i++) {
		if (cfg->key_positions[i] >= ZMK_KEYMAP_LEN) {
			return false;
		}
	}
	return true;
}

/* Resolve the stored behavior id into a binding we can invoke. Returns false
 * when the id names no behavior in this firmware, which is the case for a
 * config written by an app build that knows a behavior we do not have. */
static bool resolve_binding(const struct runtime_combo_config *cfg,
			    struct zmk_behavior_binding *out) {
	const char *name = zmk_behavior_find_behavior_name_from_local_id(cfg->behavior_local_id);
	if (name == NULL) {
		LOG_WRN("Combo id=%u names unknown behavior local id %u", cfg->combo_id,
			cfg->behavior_local_id);
		return false;
	}

	*out = (struct zmk_behavior_binding){
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
		.local_id = cfg->behavior_local_id,
#endif
		.behavior_dev = name,
		.param1 = cfg->param1,
		.param2 = cfg->param2,
	};
	return true;
}

/*
 * Ordinary typing rolls one key into the next, and two keys that happen to sit
 * on a combo would fire it. Upstream guards this with require-prior-idle-ms:
 * a combo is only a candidate when nothing was tapped in the window before it.
 * A stored 0 means the app never sent a value, so the Kconfig default applies
 * rather than leaving the guard off.
 */
static inline uint16_t effective_prior_idle_ms(const struct runtime_combo_config *cfg) {
	return cfg->require_prior_idle_ms ? cfg->require_prior_idle_ms
					  : CONFIG_ZMK_RUNTIME_COMBOS_DEFAULT_PRIOR_IDLE_MS;
}

static void store_last_tapped(int64_t timestamp) {
	if (timestamp > last_combo_timestamp) {
		last_tapped_timestamp = timestamp;
	}
}

static bool is_quick_tap(const struct runtime_combo_config *cfg, int64_t timestamp) {
	return (last_tapped_timestamp + effective_prior_idle_ms(cfg)) > timestamp;
}

static bool combo_active_on_layer(const struct runtime_combo_config *cfg, uint8_t layer) {
	if (!cfg->layer_mask) {
		return true;
	}
	return cfg->layer_mask & BIT(layer);
}

/* ------------------------------------------------------------------ */
/* Candidate handling                                                  */
/* ------------------------------------------------------------------ */

static int setup_candidates_for_first_keypress(uint32_t position, int64_t timestamp) {
	int count = 0;
	uint8_t highest_active_layer = zmk_keymap_highest_layer_active();
	uint32_t listening = combo_lookup[position];

	candidates = 0;
	for (int i = 0; i < CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT; i++) {
		if (!(listening & BIT(i))) {
			continue;
		}
		if (!combo_active_on_layer(slot_cfg(i), highest_active_layer)) {
			continue;
		}
		if (is_quick_tap(slot_cfg(i), timestamp)) {
			continue;
		}
		candidates |= BIT(i);
		count++;
	}

	return count;
}

static inline int count_bits(uint32_t mask) { return __builtin_popcount(mask); }

static int filter_candidates(uint32_t position) {
	candidates &= combo_lookup[position];
	return count_bits(candidates);
}

static int filter_timed_out_candidates(int64_t timestamp) {
	if (pressed_keys_count == 0) {
		return count_bits(candidates);
	}

	for (int i = 0; i < CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT; i++) {
		if (!(candidates & BIT(i))) {
			continue;
		}
		if (pressed_keys[0].data.timestamp + slot_cfg(i)->timeout_ms <= timestamp) {
			candidates &= ~BIT(i);
		}
	}

	return count_bits(candidates);
}

static int64_t first_candidate_timeout(void) {
	if (pressed_keys_count == 0) {
		return INT64_MAX;
	}

	int64_t first_timeout = INT64_MAX;
	for (int i = 0; i < CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT; i++) {
		if (candidates & BIT(i)) {
			first_timeout = MIN(first_timeout, (int64_t)slot_cfg(i)->timeout_ms);
		}
	}

	if (first_timeout == INT64_MAX) {
		return INT64_MAX;
	}

	return pressed_keys[0].data.timestamp + first_timeout;
}

/*
 * Cancelling the work item from inside its own handler flags it CANCELING
 * until the handler returns, and Zephyr then refuses an immediate resubmit
 * (-EBUSY). A press re-raised by the handler whose deadline has already
 * passed needs exactly that resubmit, so it would sit captured until the next
 * key. Inside the handler, clearing the deadline is enough: any firing still
 * queued returns early on timeout_task_timeout_at == 0 (or before the
 * deadline), so keep that early return if the handler changes.
 * Assumes the listener and this handler run on the same thread (the system
 * work queue); the Studio RPC thread only raises refresh_pending.
 */
static void stop_timeout_task(void) {
	timeout_task_timeout_at = 0;
	if (!in_timeout_handler) {
		k_work_cancel_delayable(&timeout_task);
	}
}

static void update_timeout_task(void) {
	int64_t first_timeout = first_candidate_timeout();
	if (timeout_task_timeout_at == first_timeout) {
		return;
	}
	if (first_timeout == INT64_MAX) {
		stop_timeout_task();
		return;
	}
	/* Per-combo timeouts differ, so the next deadline can move either way.
	 * k_work_schedule() leaves a pending item untouched, and the handler then
	 * fires at the old time and returns early; reschedule replaces it. */
	int64_t delay_ms = MAX(first_timeout - k_uptime_get(), 0);
	if (k_work_reschedule(&timeout_task, K_MSEC(delay_ms)) >= 0) {
		timeout_task_timeout_at = first_timeout;
	}
}

static bool candidate_is_completely_pressed(const struct runtime_combo_config *cfg) {
	/* filter_candidates guarantees every pressed key belongs to this combo,
	 * so matching counts means all of its keys are down. */
	return cfg->key_count == pressed_keys_count;
}

/* ------------------------------------------------------------------ */
/* Pressed key buffer                                                  */
/* ------------------------------------------------------------------ */

static int capture_pressed_key(const struct zmk_position_state_changed *ev) {
	if (pressed_keys_count == ZMK_RUNTIME_COMBOS_MAX_KEYS) {
		return ZMK_EV_EVENT_BUBBLE;
	}

	pressed_keys[pressed_keys_count++] = copy_raised_zmk_position_state_changed(ev);
	return ZMK_EV_EVENT_CAPTURED;
}

static int release_pressed_keys(void) {
	uint8_t count = pressed_keys_count;
	pressed_keys_count = 0;
	for (int i = 0; i < count; i++) {
		struct zmk_position_state_changed_event *ev = &pressed_keys[i];
		if (i == 0) {
			ZMK_EVENT_RELEASE(*ev);
		} else {
			/* Re-raise so overlapping combos see the later presses again. */
			ZMK_EVENT_RAISE(*ev);
		}
	}

	return count;
}

/* ------------------------------------------------------------------ */
/* Activation                                                          */
/* ------------------------------------------------------------------ */

static int press_combo_behavior(struct active_combo *active, int64_t timestamp) {
	struct zmk_behavior_binding_event event = {
		.position = RUNTIME_COMBO_VKP(active->slot),
		.timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
		.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
	};

	last_combo_timestamp = timestamp;

	return zmk_behavior_invoke_binding(&active->binding, event, true);
}

static int release_combo_behavior(struct active_combo *active, int64_t timestamp) {
	struct zmk_behavior_binding_event event = {
		.position = RUNTIME_COMBO_VKP(active->slot),
		.timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
		.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
	};

	return zmk_behavior_invoke_binding(&active->binding, event, false);
}

static struct active_combo *store_active_combo(int slot) {
	if (active_combo_count == CONFIG_ZMK_RUNTIME_COMBOS_MAX_PRESSED) {
		LOG_ERR("Unable to store combo; already %d active", active_combo_count);
		return NULL;
	}

	struct active_combo *active = &active_combos[active_combo_count++];
	*active = (struct active_combo){0};
	active->slot = slot;
	return active;
}

static void move_pressed_keys_to_active_combo(struct active_combo *active, uint8_t combo_length) {
	for (uint8_t i = 0; i < combo_length; i++) {
		active->key_positions_pressed[i] = pressed_keys[i];
	}
	active->key_positions_pressed_count = combo_length;

	/* Keep any key that was not part of this combo. */
	for (uint8_t i = 0; i + combo_length < pressed_keys_count; i++) {
		pressed_keys[i] = pressed_keys[i + combo_length];
	}

	pressed_keys_count -= combo_length;
}

/* Returns how many captured presses were re-raised because the combo could
 * not fire, so the caller can order the triggering release after them. */
static int activate_combo(int slot) {
	const struct runtime_combo_config *cfg = slot_cfg(slot);

	struct zmk_behavior_binding binding;
	if (!slot_is_usable(cfg) || !resolve_binding(cfg, &binding)) {
		return release_pressed_keys();
	}

	struct active_combo *active = store_active_combo(slot);
	if (active == NULL) {
		return release_pressed_keys();
	}

	active->binding = binding;

	uint8_t combo_length = MIN(pressed_keys_count, cfg->key_count);
	if (combo_length == 0) {
		/* No path reaches this today, but firing a behavior we could never
		 * release would leave it held forever, so refuse rather than trust. */
		deactivate_combo(active_combo_count - 1);
		return 0;
	}
	move_pressed_keys_to_active_combo(active, combo_length);
	press_combo_behavior(active, active->key_positions_pressed[0].data.timestamp);
	return 0;
}

static void deactivate_combo(int active_combo_index) {
	active_combo_count--;
	if (active_combo_index != active_combo_count) {
		active_combos[active_combo_index] = active_combos[active_combo_count];
	}
	active_combos[active_combo_count] = (struct active_combo){0};
}

/* Returns true when the released position belonged to an active combo. */
static bool release_combo_key(uint32_t position, int64_t timestamp) {
	for (int i = 0; i < active_combo_count; i++) {
		struct active_combo *active = &active_combos[i];
		const struct runtime_combo_config *cfg = slot_cfg(active->slot);

		bool key_released = false;
		bool all_keys_pressed = active->key_positions_pressed_count == cfg->key_count;
		bool all_keys_released = true;

		for (uint8_t j = 0; j < active->key_positions_pressed_count; j++) {
			if (key_released) {
				active->key_positions_pressed[j - 1] = active->key_positions_pressed[j];
				all_keys_released = false;
			} else if (active->key_positions_pressed[j].data.position != position) {
				all_keys_released = false;
			} else {
				key_released = true;
			}
		}

		if (!key_released) {
			continue;
		}

		active->key_positions_pressed_count--;
		if ((cfg->slow_release && all_keys_released) ||
		    (!cfg->slow_release && all_keys_pressed)) {
			release_combo_behavior(active, timestamp);
		}
		if (all_keys_released) {
			deactivate_combo(i);
		}
		return true;
	}

	return false;
}

static int cleanup(void) {
	stop_timeout_task();
	candidates = 0;
	int released = 0;
	if (fully_pressed_combo >= 0) {
		int slot = fully_pressed_combo;
		fully_pressed_combo = -1;
		released = activate_combo(slot);
	}
	return released + release_pressed_keys();
}

/* ------------------------------------------------------------------ */
/* Event listener                                                      */
/* ------------------------------------------------------------------ */

static int position_state_down(struct zmk_position_state_changed *data) {
	int num_candidates;

	if (!pressed_keys_count) {
		num_candidates = setup_candidates_for_first_keypress(data->position, data->timestamp);
		if (num_candidates == 0) {
			return ZMK_EV_EVENT_BUBBLE;
		}
	} else {
		filter_timed_out_candidates(data->timestamp);
		num_candidates = filter_candidates(data->position);
	}

	int ret = capture_pressed_key(data);
	update_timeout_task();

	if (num_candidates == 0) {
		cleanup();
		return ret;
	}

	/*
	 * Upstream stops at the first candidate because its combo array is sorted
	 * shortest-first, so the first one it meets is the one to fire. Our slots
	 * are in registration order, so we keep looking until we find one that is
	 * completely pressed. Every completely pressed candidate has the same key
	 * count by definition, so taking the first such match matches upstream.
	 */
	for (int i = 0; i < CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT; i++) {
		if (!(candidates & BIT(i))) {
			continue;
		}
		if (!candidate_is_completely_pressed(slot_cfg(i))) {
			continue;
		}
		fully_pressed_combo = i;
		if (num_candidates == 1) {
			cleanup();
		}
		break;
	}

	return ret;
}

static int position_state_up(struct zmk_position_state_changed *data) {
	int released_keys = cleanup();
	if (release_combo_key(data->position, data->timestamp)) {
		return ZMK_EV_EVENT_HANDLED;
	}
	if (released_keys > 1) {
		/* The re-raised presses have to be followed by this release, or a
		 * hold-tap downstream sees them in the wrong order. */
		struct zmk_position_state_changed_event dupe_ev =
			copy_raised_zmk_position_state_changed(data);
		ZMK_EVENT_RAISE(dupe_ev);
		return ZMK_EV_EVENT_CAPTURED;
	}
	return ZMK_EV_EVENT_BUBBLE;
}

static void combo_timeout_handler(struct k_work *item) {
	if (timeout_task_timeout_at == 0 || k_uptime_get() < timeout_task_timeout_at) {
		/* cancelled or rescheduled */
		return;
	}
	in_timeout_handler = true;
	if (filter_timed_out_candidates(timeout_task_timeout_at) == 0) {
		cleanup();
	}
	update_timeout_task();
	in_timeout_handler = false;
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
	struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
	if (ev->state && !is_mod(ev->usage_page, ev->keycode)) {
		store_last_tapped(ev->timestamp);
	}
	return ZMK_EV_EVENT_BUBBLE;
}

static int runtime_combos_listener(const zmk_event_t *eh) {
	struct zmk_position_state_changed *data = as_zmk_position_state_changed(eh);
	if (data == NULL) {
		if (as_zmk_keycode_state_changed(eh) != NULL) {
			return keycode_state_changed_listener(eh);
		}
		return ZMK_EV_EVENT_BUBBLE;
	}

	/* Pick up any table change the RPC thread asked for, in this context. */
	apply_pending_refresh();

	/* Nothing configured: leave the event stream exactly as it was. */
	if (!any_combo_active && active_combo_count == 0 && pressed_keys_count == 0) {
		return ZMK_EV_EVENT_BUBBLE;
	}

	if (data->position >= ZMK_KEYMAP_LEN) {
		/* Sensors and other virtual positions never take part in a combo. */
		return ZMK_EV_EVENT_BUBBLE;
	}

	if (data->state) {
		return position_state_down(data);
	}

	return position_state_up(data);
}

ZMK_LISTENER(runtime_combos, runtime_combos_listener);
ZMK_SUBSCRIPTION(runtime_combos, zmk_position_state_changed);
ZMK_SUBSCRIPTION(runtime_combos, zmk_keycode_state_changed);

/* ------------------------------------------------------------------ */
/* Lookup maintenance                                                  */
/* ------------------------------------------------------------------ */

static void rebuild_lookup(void) {
	/* A slot being rewritten under a held combo would leave us releasing a
	 * behavior we no longer have the config for, so drop everything first. */
	for (int i = active_combo_count - 1; i >= 0; i--) {
		release_combo_behavior(&active_combos[i], k_uptime_get());
		deactivate_combo(i);
	}
	stop_timeout_task();
	candidates = 0;
	fully_pressed_combo = -1;

	memset(combo_lookup, 0, sizeof(combo_lookup));
	any_combo_active = false;

	for (int i = 0; i < CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT; i++) {
		const struct runtime_combo_config *cfg = slot_cfg(i);
		if (!slot_is_usable(cfg)) {
			continue;
		}
		for (uint8_t k = 0; k < cfg->key_count; k++) {
			combo_lookup[cfg->key_positions[k]] |= BIT(i);
		}
		any_combo_active = true;
	}

	/*
	 * Hand back any press we had captured but not yet turned into a combo.
	 * Dropping the buffer instead would swallow the key down while its key up
	 * still reaches the keymap, which strands whatever it was bound to -- a
	 * layer that never comes back down, a modifier that stays held.
	 * release_pressed_keys() clears the count first, and the caller has
	 * already cleared refresh_pending, so the re-raised events cannot land
	 * back here.
	 */
	release_pressed_keys();

	LOG_DBG("Runtime combo lookup rebuilt (active=%d)", any_combo_active);
}

void zmk_runtime_combos_engine_refresh(void) { atomic_set(&refresh_pending, 1); }

static void apply_pending_refresh(void) {
	if (atomic_cas(&refresh_pending, 1, 0)) {
		rebuild_lookup();
	}
}

static int runtime_combos_engine_init(void) {
	k_work_init_delayable(&timeout_task, combo_timeout_handler);
	/*
	 * Leave the flag raised rather than building the table here. Settings may
	 * be loaded before or after this init runs; clearing the flag afterwards
	 * could swallow the one settings_commit raised in between, and then no key
	 * press would ever see the stored combos.
	 */
	atomic_set(&refresh_pending, 1);
	return 0;
}

SYS_INIT(runtime_combos_engine_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
