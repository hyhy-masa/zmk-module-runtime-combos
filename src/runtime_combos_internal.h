/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Shared between the settings/RPC side (runtime_combos.c) and the key event
 * engine (runtime_combos_engine.c).
 */

#pragma once

#include <zephyr/sys/util.h>
#include <zmk/combos/runtime_combos.h>

/* Slot table. Owned by runtime_combos.c; read by the engine. */
extern struct runtime_combo_config zmk_runtime_combos_slots[CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT];

/**
 * @brief Rebuild the key position lookup from the slot table.
 *
 * Called after any change to the slots (settings load, set, delete). Any combo
 * press in flight is abandoned first, because the slot it refers to may have
 * just been rewritten. No-op when the engine is compiled out.
 */
#if IS_ENABLED(CONFIG_ZMK_RUNTIME_COMBOS_ENGINE)
void zmk_runtime_combos_engine_refresh(void);
#else
static inline void zmk_runtime_combos_engine_refresh(void) {}
#endif
