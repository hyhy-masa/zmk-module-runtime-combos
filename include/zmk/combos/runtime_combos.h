#pragma once

#include <zmk/behavior.h>
#include <stdint.h>

#define ZMK_RUNTIME_COMBOS_MAX_KEYS 4

struct runtime_combo_config {
    uint32_t combo_id;
    uint32_t key_positions[ZMK_RUNTIME_COMBOS_MAX_KEYS];
    uint8_t key_count;
    uint32_t timeout_ms;
    zmk_behavior_local_id_t behavior_local_id;
    uint32_t param1;
    uint32_t param2;
    uint32_t layer_mask;
    /* Suppress the combo when a key was tapped less than this long ago, so
     * ordinary typing does not trigger it. 0 means "use the Kconfig default". */
    uint16_t require_prior_idle_ms;
    bool slow_release;
    bool active;
};

int zmk_runtime_combos_get_all(struct runtime_combo_config *out, uint8_t max_count,
                               uint8_t *actual_count);

int zmk_runtime_combos_set(const struct runtime_combo_config *combo);

int zmk_runtime_combos_delete(uint32_t combo_id);
