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

#define SETTINGS_KEY "rtc"

struct runtime_combo_config zmk_runtime_combos_slots[CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT];

static int settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
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
static int settings_commit(void) {
	zmk_runtime_combos_engine_refresh();
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(runtime_combos, SETTINGS_KEY, NULL, settings_set, settings_commit,
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
	char key[16];
	snprintf(key, sizeof(key), SETTINGS_KEY "/c%d", index);
	return settings_save_one(key, &zmk_runtime_combos_slots[index],
				 sizeof(struct runtime_combo_config));
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
	k_sched_lock();
	zmk_runtime_combos_slots[slot] = *combo;
	zmk_runtime_combos_slots[slot].active = true;
	zmk_runtime_combos_engine_refresh();
	k_sched_unlock();

	int rc = save_slot(slot);
	if (rc != 0) {
		LOG_ERR("Failed to save combo %d: %d", slot, rc);
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
	k_sched_lock();
	memset(&zmk_runtime_combos_slots[slot], 0, sizeof(struct runtime_combo_config));
	zmk_runtime_combos_engine_refresh();
	k_sched_unlock();

	int rc = save_slot(slot);
	if (rc != 0) {
		LOG_ERR("Failed to save deleted combo %d: %d", slot, rc);
	}

	LOG_DBG("Deleted combo slot %d (id=%d)", slot, combo_id);
	return rc;
}
