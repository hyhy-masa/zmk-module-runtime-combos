/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Runtime combo configuration — stores and persists combo settings.
 * The key event side lives in runtime_combos_engine.c.
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zmk/combos/runtime_combos.h>
#include <zmk/behavior.h>
#include <zmk/matrix.h>
#include <string.h>

#include "runtime_combos_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Settings subtree for the slot table. The pre-engine module (119de78) used
 * "rtc"; that firmware's loader takes any "rtc/c<N>" it finds, and "rtc2" does
 * not match it, so combos saved here are never read by an older firmware, and
 * combos saved by it are never read here.
 * Changing this key abandons every stored combo. It was acceptable once only
 * because pre-engine combos never took effect; a future layout change must
 * migrate instead of renaming again.
 */
#define SETTINGS_KEY "rtc2"
#define SETTINGS_NAME_BUF_LEN 16

/* save_slot() formats SETTINGS_KEY "/c%d" into SETTINGS_NAME_BUF_LEN bytes. */
BUILD_ASSERT(CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT <= 100,
	     "slot names are sized for at most two digits");
BUILD_ASSERT(sizeof(SETTINGS_KEY "/c99") <= SETTINGS_NAME_BUF_LEN,
	     "settings name of the last slot does not fit the name buffer");

struct runtime_combo_config zmk_runtime_combos_slots[CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT];

/* Not named settings_set/settings_commit: Zephyr declares a public
 * settings_commit(), and a static definition of the same name does not build. */
static int runtime_combos_settings_set(const char *name, size_t len, settings_read_cb read_cb,
				       void *cb_arg) {
	int index;
	if (sscanf(name, "c%d", &index) != 1) {
		return -ENOENT;
	}
	if (index < 0 || index >= CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT) {
		return -EINVAL;
	}
	if (len != sizeof(struct runtime_combo_config)) {
		LOG_ERR("Invalid settings size for combo %d", index);
		return -EINVAL;
	}
	int rc = read_cb(cb_arg, &zmk_runtime_combos_slots[index],
			 sizeof(struct runtime_combo_config));
	if (rc < 0) {
		return rc;
	}
	LOG_DBG("Loaded combo %d (id=%d, keys=%d)", index, zmk_runtime_combos_slots[index].combo_id,
		zmk_runtime_combos_slots[index].key_count);
	return 0;
}

/* Called once after every stored slot has been read back, so the engine builds
 * its lookup from a complete table rather than once per slot. */
static int runtime_combos_settings_commit(void) {
	zmk_runtime_combos_engine_refresh();
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(runtime_combos, SETTINGS_KEY, NULL, runtime_combos_settings_set,
			       runtime_combos_settings_commit,
			       NULL);

static int find_slot_by_id(uint32_t combo_id) {
	for (int i = 0; i < CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT; i++) {
		if (zmk_runtime_combos_slots[i].active &&
		    zmk_runtime_combos_slots[i].combo_id == combo_id) {
			return i;
		}
	}
	return -1;
}

static int find_free_slot(void) {
	for (int i = 0; i < CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT; i++) {
		if (!zmk_runtime_combos_slots[i].active) {
			return i;
		}
	}
	return -1;
}

static int save_slot(int index) {
	char key[SETTINGS_NAME_BUF_LEN];
	snprintf(key, sizeof(key), SETTINGS_KEY "/c%d", index);
	return settings_save_one(key, &zmk_runtime_combos_slots[index],
				 sizeof(struct runtime_combo_config));
}

/* Put a slot back after its flash write failed. Same locking as the write. */
static void restore_slot(int index, const struct runtime_combo_config *previous) {
	k_sched_lock();
	zmk_runtime_combos_slots[index] = *previous;
	zmk_runtime_combos_engine_refresh();
	k_sched_unlock();
}

int zmk_runtime_combos_get_all(struct runtime_combo_config *out, uint8_t max_count,
			       uint8_t *actual_count) {
	uint8_t count = 0;
	for (int i = 0; i < CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT && count < max_count; i++) {
		if (zmk_runtime_combos_slots[i].active) {
			out[count++] = zmk_runtime_combos_slots[i];
		}
	}
	if (actual_count) {
		*actual_count = count;
	}
	return 0;
}

int zmk_runtime_combos_set(const struct runtime_combo_config *combo) {
	if (combo->key_count < 2 || combo->key_count > ZMK_RUNTIME_COMBOS_MAX_KEYS) {
		return -EINVAL;
	}
	/* Refuse a position the keyboard does not have, instead of storing a combo
	 * that reports success and then never fires. */
	for (uint8_t i = 0; i < combo->key_count; i++) {
		if (combo->key_positions[i] >= ZMK_KEYMAP_LEN) {
			LOG_ERR("Combo id=%u names key position %u, out of %d", combo->combo_id,
				combo->key_positions[i], ZMK_KEYMAP_LEN);
			return -EINVAL;
		}
	}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
	/* Same idea for the behavior: an id this firmware lacks would be stored,
	 * reported as saved, and never fire. */
	if (zmk_behavior_find_behavior_name_from_local_id(combo->behavior_local_id) == NULL) {
		LOG_ERR("Combo id=%u names unknown behavior local id %u", combo->combo_id,
			combo->behavior_local_id);
		return -EINVAL;
	}
#endif

	int slot = find_slot_by_id(combo->combo_id);
	if (slot < 0) {
		slot = find_free_slot();
		if (slot < 0) {
			LOG_ERR("No free combo slots");
			return -ENOMEM;
		}
	}

	/*
	 * This runs on the Studio RPC thread, which sits at the lowest application
	 * priority; key events are raised from the system work queue and can
	 * preempt it mid-assignment. Keep the write and the engine's notification
	 * together so a key press never reads a half-written slot. The flash write
	 * below stays outside -- sleeping under a scheduler lock faults the kernel.
	 */
	struct runtime_combo_config previous = zmk_runtime_combos_slots[slot];

	k_sched_lock();
	zmk_runtime_combos_slots[slot] = *combo;
	zmk_runtime_combos_slots[slot].active = true;
	zmk_runtime_combos_engine_refresh();
	k_sched_unlock();

	int rc = save_slot(slot);
	if (rc != 0) {
		/* Keep RAM in step with flash: a combo the app was told failed must
		 * not keep firing until the next reboot. */
		LOG_ERR("Failed to save combo %d: %d", slot, rc);
		restore_slot(slot, &previous);
		return rc;
	}

	LOG_DBG("Set combo slot %d (id=%d)", slot, combo->combo_id);
	return rc;
}

int zmk_runtime_combos_delete(uint32_t combo_id) {
	int slot = find_slot_by_id(combo_id);
	if (slot < 0) {
		return -ENOENT;
	}

	/* Same reason as in _set: keep the clear and the notification together. */
	struct runtime_combo_config previous = zmk_runtime_combos_slots[slot];

	k_sched_lock();
	memset(&zmk_runtime_combos_slots[slot], 0, sizeof(struct runtime_combo_config));
	zmk_runtime_combos_engine_refresh();
	k_sched_unlock();

	int rc = save_slot(slot);
	if (rc != 0) {
		/* Same reason as in _set: undo the clear so RAM matches flash. */
		LOG_ERR("Failed to save deleted combo %d: %d", slot, rc);
		restore_slot(slot, &previous);
		return rc;
	}

	LOG_DBG("Deleted combo slot %d (id=%d)", slot, combo_id);
	return rc;
}
