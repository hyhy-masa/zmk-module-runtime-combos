/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Runtime combo configuration — stores and persists combo settings.
 * TODO: Hook into ZMK's combo event processing to activate runtime combos.
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zmk/combos/runtime_combos.h>
#include <zmk/behavior.h>
#include <string.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define SETTINGS_KEY "rtc"

static struct runtime_combo_config combos[CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT];

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
	int rc = read_cb(cb_arg, &combos[index], sizeof(struct runtime_combo_config));
	if (rc < 0) {
		return rc;
	}
	LOG_DBG("Loaded combo %d (id=%d, keys=%d)", index, combos[index].combo_id,
		combos[index].key_count);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(runtime_combos, SETTINGS_KEY, NULL, settings_set, NULL, NULL);

static int find_slot_by_id(uint32_t combo_id) {
	for (int i = 0; i < CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT; i++) {
		if (combos[i].active && combos[i].combo_id == combo_id) {
			return i;
		}
	}
	return -1;
}

static int find_free_slot(void) {
	for (int i = 0; i < CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT; i++) {
		if (!combos[i].active) {
			return i;
		}
	}
	return -1;
}

static int save_slot(int index) {
	char key[16];
	snprintf(key, sizeof(key), SETTINGS_KEY "/c%d", index);
	return settings_save_one(key, &combos[index], sizeof(struct runtime_combo_config));
}

int zmk_runtime_combos_get_all(struct runtime_combo_config *out, uint8_t max_count,
			       uint8_t *actual_count) {
	uint8_t count = 0;
	for (int i = 0; i < CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT && count < max_count; i++) {
		if (combos[i].active) {
			out[count++] = combos[i];
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

	int slot = find_slot_by_id(combo->combo_id);
	if (slot < 0) {
		slot = find_free_slot();
		if (slot < 0) {
			LOG_ERR("No free combo slots");
			return -ENOMEM;
		}
	}

	combos[slot] = *combo;
	combos[slot].active = true;

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

	memset(&combos[slot], 0, sizeof(struct runtime_combo_config));

	int rc = save_slot(slot);
	if (rc != 0) {
		LOG_ERR("Failed to save deleted combo %d: %d", slot, rc);
	}

	LOG_DBG("Deleted combo slot %d (id=%d)", slot, combo_id);
	return rc;
}
