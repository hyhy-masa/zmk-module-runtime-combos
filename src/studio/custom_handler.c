/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <pb_decode.h>
#include <pb_encode.h>
#include <cormoran/combos/custom.pb.h>
#include <zmk/studio/custom.h>
#include <zmk/combos/runtime_combos.h>
#include <zmk/behavior.h>

#include <zephyr/logging/log.h>
#include <string.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta combos_meta = {
	ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("http://localhost:5173"),
	.security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran_combos, &combos_meta, combos_rpc_handle_request);
ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran_combos, cormoran_combos_Response);

static int handle_set_combo(const cormoran_combos_SetComboRequest *req,
			    cormoran_combos_Response *resp);
static int handle_delete_combo(const cormoran_combos_DeleteComboRequest *req,
			       cormoran_combos_Response *resp);
static int handle_get_all_combos(const cormoran_combos_GetAllCombosRequest *req,
				 cormoran_combos_Response *resp);

static bool combos_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
				      pb_callback_t *encode_response) {
	cormoran_combos_Response *resp =
		ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran_combos, encode_response);

	cormoran_combos_Request req = cormoran_combos_Request_init_zero;

	pb_istream_t req_stream =
		pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
	if (!pb_decode(&req_stream, cormoran_combos_Request_fields, &req)) {
		LOG_WRN("Failed to decode combos request: %s", PB_GET_ERROR(&req_stream));
		cormoran_combos_ErrorResponse err = cormoran_combos_ErrorResponse_init_zero;
		snprintf(err.message, sizeof(err.message), "Failed to decode request");
		resp->which_response_type = cormoran_combos_Response_error_tag;
		resp->response_type.error = err;
		return true;
	}

	int rc = 0;
	switch (req.which_request_type) {
	case cormoran_combos_Request_set_combo_tag:
		rc = handle_set_combo(&req.request_type.set_combo, resp);
		break;
	case cormoran_combos_Request_delete_combo_tag:
		rc = handle_delete_combo(&req.request_type.delete_combo, resp);
		break;
	case cormoran_combos_Request_get_all_combos_tag:
		rc = handle_get_all_combos(&req.request_type.get_all_combos, resp);
		break;
	default:
		LOG_WRN("Unsupported combos request type: %d", req.which_request_type);
		rc = -1;
	}

	if (rc != 0) {
		cormoran_combos_ErrorResponse err = cormoran_combos_ErrorResponse_init_zero;
		snprintf(err.message, sizeof(err.message), "Failed to process request");
		resp->which_response_type = cormoran_combos_Response_error_tag;
		resp->response_type.error = err;
	}
	return true;
}

static int handle_set_combo(const cormoran_combos_SetComboRequest *req,
			    cormoran_combos_Response *resp) {
	const cormoran_combos_ComboConfig *cfg = &req->combo;
	LOG_DBG("Set combo: id=%d keys=%d timeout=%d", cfg->combo_id, cfg->key_positions_count,
		cfg->timeout_ms);

	struct runtime_combo_config combo = {
		.combo_id = cfg->combo_id,
		.key_count = cfg->key_positions_count,
		.timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 50,
		.behavior_local_id = cfg->has_binding ? cfg->binding.behavior_id : 0,
		.param1 = cfg->has_binding ? cfg->binding.param1 : 0,
		.param2 = cfg->has_binding ? cfg->binding.param2 : 0,
		.layer_mask = cfg->layer_mask,
		.slow_release = cfg->slow_release,
		.active = true,
	};

	for (uint8_t i = 0; i < combo.key_count && i < ZMK_RUNTIME_COMBOS_MAX_KEYS; i++) {
		combo.key_positions[i] = cfg->key_positions[i];
	}

	int rc = zmk_runtime_combos_set(&combo);

	cormoran_combos_SetComboResponse result = cormoran_combos_SetComboResponse_init_zero;
	result.success = (rc == 0);
	resp->which_response_type = cormoran_combos_Response_set_combo_tag;
	resp->response_type.set_combo = result;
	return rc;
}

static int handle_delete_combo(const cormoran_combos_DeleteComboRequest *req,
			       cormoran_combos_Response *resp) {
	LOG_DBG("Delete combo: id=%d", req->combo_id);

	int rc = zmk_runtime_combos_delete(req->combo_id);

	cormoran_combos_DeleteComboResponse result = cormoran_combos_DeleteComboResponse_init_zero;
	result.success = (rc == 0);
	resp->which_response_type = cormoran_combos_Response_delete_combo_tag;
	resp->response_type.delete_combo = result;
	return rc;
}

static int handle_get_all_combos(const cormoran_combos_GetAllCombosRequest *req,
				 cormoran_combos_Response *resp) {
	LOG_DBG("Get all combos");

	struct runtime_combo_config all[CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT];
	uint8_t count;
	int rc = zmk_runtime_combos_get_all(all, CONFIG_ZMK_RUNTIME_COMBOS_MAX_COUNT, &count);
	if (rc != 0) {
		return rc;
	}

	cormoran_combos_GetAllCombosResponse result =
		cormoran_combos_GetAllCombosResponse_init_zero;
	result.combos_count = count;

	for (uint8_t i = 0; i < count; i++) {
		result.combos[i].combo_id = all[i].combo_id;
		result.combos[i].key_positions_count = all[i].key_count;
		for (uint8_t j = 0; j < all[i].key_count; j++) {
			result.combos[i].key_positions[j] = all[i].key_positions[j];
		}
		result.combos[i].timeout_ms = all[i].timeout_ms;
		result.combos[i].has_binding = true;
		result.combos[i].binding.behavior_id = all[i].behavior_local_id;
		result.combos[i].binding.param1 = all[i].param1;
		result.combos[i].binding.param2 = all[i].param2;
		result.combos[i].layer_mask = all[i].layer_mask;
		result.combos[i].slow_release = all[i].slow_release;
	}

	resp->which_response_type = cormoran_combos_Response_get_all_combos_tag;
	resp->response_type.get_all_combos = result;
	return 0;
}
