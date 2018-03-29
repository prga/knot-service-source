/*
 * This file is part of the KNOT Project
 *
 * Copyright (c) 2018, CESAR. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef  _GNU_SOURCE
#define  _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include <ell/ell.h>

#include <json-c/json.h>

#include <knot_types.h>
#include <knot_protocol.h>
#include <hal/linux_log.h>

#include "settings.h"
#include "proto.h"
#include "msg.h"

#define MIN(a,b) ((a) < (b) ? (a) : (b))

struct trust {
	int refs;
	pid_t	pid;			/* Peer PID */
	uint64_t id;			/* Session identification */
	bool rollback;			/* Remove from cloud if true */
	char *uuid;			/* Device UUID */
	char *token;			/* Device token */
	struct l_queue *schema;		/* Schema accepted by cloud */
	struct l_queue *schema_tmp;	/* Schema to be submitted to cloud */
	struct l_queue *config;		/* knot_config accepted from cloud */
};

/* Maps sockets to sessions: online devices only.  */
static struct l_hashmap *trust_map;

static struct trust *trust_ref(struct trust *trust)
{
	if (unlikely(!trust))
		return NULL;

	__sync_fetch_and_add(&trust->refs, 1);

	return trust;
}

static void trust_unref(struct trust *trust)
{
	if (unlikely(!trust))
                return;

        if (__sync_sub_and_fetch(&trust->refs, 1))
		return;

	l_free(trust->uuid);
	l_free(trust->token);
	l_queue_destroy(trust->schema, l_free);
	l_queue_destroy(trust->schema_tmp, l_free);
	l_queue_destroy(trust->config, l_free);
	l_free(trust);
}

static struct trust *trust_new(const char *uuid, const char *token,
			       uint64_t device_id, pid_t pid, bool rollback,
			       struct l_queue *schema, struct l_queue *config)
{
	struct trust *trust = l_new(struct trust, 1);

	trust->uuid = l_strdup(uuid);
	trust->token = l_strdup(token);
	trust->id = device_id;
	trust->pid = pid;
	trust->rollback = rollback;
	trust->schema = (schema ? : l_queue_new());
	trust->schema_tmp = l_queue_new();
	trust->config = (config ? : l_queue_new());
	trust->refs = 0;

	return trust_ref(trust);
}

static bool schema_sensor_id_cmp(const void *entry_data, const void *user_data)
{
	const knot_msg_schema *schema = entry_data;
	unsigned int sensor_id = L_PTR_TO_UINT(user_data);

	return sensor_id == schema->sensor_id;
}

static knot_msg_schema *trust_get_sensor_schema(const struct trust *trust,
						unsigned int sensor_id)
{
	return l_queue_find(trust->schema,
			    schema_sensor_id_cmp,
			    L_UINT_TO_PTR(sensor_id));
}

static knot_msg_schema *trust_get_sensor_schema_tmp(const struct trust *trust,
						    unsigned int sensor_id)
{
	return l_queue_find(trust->schema_tmp,
			    schema_sensor_id_cmp,
			    L_UINT_TO_PTR(sensor_id));
}

static void trust_sensor_schema_tmp_add(struct trust *trust,
					const knot_msg_schema *schema)
{
	knot_msg_schema *schema_copy;

	schema_copy = l_memdup(schema, sizeof(*schema));
	l_queue_push_tail(trust->schema_tmp, schema_copy);
}

static void trust_sensor_schema_tmp_free(struct trust *trust)
{
	l_queue_destroy(trust->schema_tmp, l_free);
	trust->schema_tmp = NULL;
}

static void trust_sensor_schema_complete(struct trust *trust)
{
	l_queue_destroy(trust->schema, l_free);
	trust->schema = NULL;
	trust->schema = trust->schema_tmp;
	trust->schema_tmp = NULL;
}

static bool config_sensor_id_cmp(const void *entry_data, const void *user_data)
{
	const knot_msg_config *config = entry_data;
	unsigned int sensor_id = L_PTR_TO_UINT(user_data);

	return config->sensor_id == sensor_id;
}

static int8_t msg_unregister(int node_socket, int proto_socket)
{
	struct trust *trust;
	int8_t result;

	trust = l_hashmap_remove(trust_map, L_INT_TO_PTR(node_socket));
	if (!trust) {
		hal_log_info("Permission denied!");
		result = KNOT_CREDENTIAL_UNAUTHORIZED;
		goto done;
	}

	hal_log_info("rmnode: %.36s", trust->uuid);
	result = proto_rmnode(proto_socket, trust->uuid, trust->token);
	if (result != KNOT_SUCCESS)
		goto done;

	trust_unref(trust);
	result = KNOT_SUCCESS;

done:
	return result;
}

/*
 * Checks if the config message received from the cloud is valid.
 * Validates if the values are valid and if the event_flags are consistent
 * with desired events.
 * No need to check if sensor_id,event_flags and time_sec are positive for
 * they are unsigned from protocol.
 */
static int config_is_valid(struct l_queue *config_list)
{
	struct l_queue_entry *entry;
	knot_msg_config *config;
	int diff_int, diff_dec;

	entry = (struct l_queue_entry *) l_queue_get_entries(config_list);
	while (entry) {
		config = entry->data;

		/* Check if event_flags are valid */
		if ((config->values.event_flags | KNOT_EVT_FLAG_NONE) &&
			!(config->values.event_flags & (KNOT_EVT_FLAG_TIME |
						KNOT_EVT_FLAG_LOWER_THRESHOLD |
						KNOT_EVT_FLAG_UPPER_THRESHOLD |
						KNOT_EVT_FLAG_CHANGE |
						KNOT_EVT_FLAG_UNREGISTERED)))
			/*
			 * TODO: DEFINE KNOT_CONFIG ERRORS IN PROTOCOL
			 * KNOT_INVALID_CONFIG in new protocol
			 */
			return KNOT_ERROR_UNKNOWN;

		/* Check consistency of time_sec */
		if (config->values.event_flags & KNOT_EVT_FLAG_TIME) {
			if (config->values.time_sec == 0)
				/*
				 * TODO: DEFINE KNOT_CONFIG ERRORS IN PROTOCOL
				 * KNOT_INVALID_CONFIG in new protocol
				 */
				return KNOT_ERROR_UNKNOWN;
		} else {
			if (config->values.time_sec > 0)
				/*
				 * TODO: DEFINE KNOT_CONFIG ERRORS IN PROTOCOL
				 * KNOT_INVALID_CONFIG in new protocol
				 */
				return KNOT_ERROR_UNKNOWN;
		}

		/* Check consistency of limits */
		if (config->values.event_flags &
					(KNOT_EVT_FLAG_LOWER_THRESHOLD |
					KNOT_EVT_FLAG_UPPER_THRESHOLD)) {

			diff_int = config->values.upper_limit.val_f.value_int -
				config->values.lower_limit.val_f.value_int;

			diff_dec = config->values.upper_limit.val_f.value_dec -
				config->values.lower_limit.val_f.value_dec;

			if (diff_int < 0)
				/*
				 * TODO: DEFINE KNOT_CONFIG ERRORS IN PROTOCOL
				 * KNOT_INVALID_CONFIG in new protocol
				 */
				return KNOT_ERROR_UNKNOWN;
			else if (diff_int == 0 && diff_dec <= 0)
				/*
				 * TODO: DEFINE KNOT_CONFIG ERRORS IN PROTOCOL
				 * KNOT_INVALID_CONFIG in new protocol
				 */
				return KNOT_ERROR_UNKNOWN;
		}
		entry = entry->next;
	}
	return KNOT_SUCCESS;
}

#if 0
/*
 * Parses the json from the cloud with the set_data.
 * Whenever the GW sends a data to the thing, it will also insert another field
 * in the data in the cloud informing that this data have already been sent.
 * When/if the user updates the data, the field is erased and the data is sent
 * again, regardless if the value is the same or not.
 */
static struct l_queue *parse_device_setdata(const char *json_str)
{
	json_object *jobj, *jobjarray, *jobjentry, *jobjkey;
	struct l_queue *list;
	knot_msg_data *entry;
	int sensor_id, i;
	knot_data data;
	json_type jtype;

	jobj = json_tokener_parse(json_str);
	if (!jobj)
		return NULL;

	list = l_queue_new();

	/*
	 * Getting 'set_data' from the device properties:
	 * {"uuid":
	 *		"set_data" : [
	 *			{"sensor_id": v,
	 *			"value": w}]
	 * }
	 */

	/* 'set_data' is an array */
	if (!json_object_object_get_ex(jobj, "set_data", &jobjarray))
		goto done;

	if (json_object_get_type(jobjarray) != json_type_array)
		goto done;

	for (i = 0; i < json_object_array_length(jobjarray); i++) {

		jobjentry = json_object_array_get_idx(jobjarray, i);
		if (!jobjentry)
			goto done;

		/* Getting 'sensor_id' */
		if (!json_object_object_get_ex(jobjentry, "sensor_id",
								&jobjkey))
			goto done;

		if (json_object_get_type(jobjkey) != json_type_int)
			goto done;

		sensor_id = json_object_get_int(jobjkey);

		/* Getting 'value' */
		memset(&data, 0, sizeof(knot_data));
		if (json_object_object_get_ex(jobjentry, "value",
								&jobjkey)) {
			jtype = json_object_get_type(jobjkey);
			if (jtype != json_type_int &&
				jtype != json_type_double &&
				jtype != json_type_boolean)
				goto done;

			parse_json_value_types(jobjkey,
						&data.values);
		}

		entry = l_new(knot_msg_data, 1);
		entry->sensor_id = sensor_id;
		memcpy(&(entry->payload), &data, sizeof(knot_data));
		l_queue_push_tail(list, entry);
	}
	json_object_put(jobj);

	return list;

done:
	l_queue_destroy(list, l_free);
	json_object_put(jobj);

	return NULL;
}

/*
 * Parses the json from the cloud with the get_data.
 */
static struct l_queue *parse_device_getdata(const char *json_str)
{
	json_object *jobj, *jobjarray, *jobjentry, *jobjkey;
	struct l_queue *list;
	knot_msg_item *entry;
	int sensor_id, i;

	jobj = json_tokener_parse(json_str);
	if (!jobj)
		return NULL;

	list = l_queue_new();

	/*
	 * Getting 'get_data' from the device properties
	 * {"devices":[{"uuid":
	 *		"get_data" : [
	 *			{"sensor_id": v
	 *			}]
	 * }
	 */

	/* 'set_data' is an array */
	if (!json_object_object_get_ex(jobj, "get_data", &jobjarray))
		goto done;

	if (json_object_get_type(jobjarray) != json_type_array)
		goto done;

	for (i = 0; i < json_object_array_length(jobjarray); i++) {

		jobjentry = json_object_array_get_idx(jobjarray, i);
		if (!jobjentry)
			goto done;

		/* Getting 'sensor_id' */
		if (!json_object_object_get_ex(jobjentry,
					       "sensor_id", &jobjkey))
			goto done;

		if (json_object_get_type(jobjkey) != json_type_int)
			goto done;

		sensor_id = json_object_get_int(jobjkey);

		entry = l_new(knot_msg_item, 1);
		entry->sensor_id = sensor_id;

		l_queue_push_tail(list, entry);
	}
	json_object_put(jobj);

	return list;

done:
	l_queue_destroy(list, l_free);
	json_object_put(jobj);

	return NULL;
}

static void update_msg_item_header(void *entry_data, void *user_data)
{
	knot_msg_item *kmitem = entry_data;
	kmitem->hdr.type = KNOT_MSG_GET_DATA;
	kmitem->hdr.payload_len = sizeof(kmitem->sensor_id);
}

/*
 * Includes the proper header in the getdata messages and returns a list with
 * all the sensor from which the data is requested.
 */
static struct l_queue *msg_getdata(int node_socket,
				   json_raw_t device_message, ssize_t *result)
{
	struct trust *trust;
	struct l_queue *messages;

	trust = l_hashmap_lookup(trust_map, L_INT_TO_PTR(node_socket));
	if (!trust) {
		hal_log_info("Permission denied!");
		*result = KNOT_CREDENTIAL_UNAUTHORIZED;
		return NULL;
	}
	*result = KNOT_SUCCESS;

	messages = parse_device_getdata(device_message.data);
	l_queue_foreach(messages, update_msg_item_header, NULL);

	return messages;
}

static void update_msg_data_header(void *entry_data, void *user_data)
{
	knot_msg_data *kmdata = entry_data;
	kmdata->hdr.type = KNOT_MSG_SET_DATA;
	kmdata->hdr.payload_len = sizeof(kmdata->sensor_id) +
					sizeof(kmdata->payload);
}

/*
 * Includes the proper header in the setdata messages and returns a list with
 * all the sensor data that will be sent to the thing.
 */
static struct l_queue *msg_setdata(int node_socket,
				   json_raw_t device_message, ssize_t *result)
{
	struct trust *trust;
	struct l_queue *messages;

	trust = l_hashmap_lookup(trust_map, L_INT_TO_PTR(node_socket));
	if (!trust) {
		hal_log_info("Permission denied!");
		*result = KNOT_CREDENTIAL_UNAUTHORIZED;
		return NULL;
	}

	*result = KNOT_SUCCESS;

	messages = parse_device_setdata(device_message.data);
	l_queue_foreach(messages, update_msg_data_header, NULL);

	return messages;
}

static void duplicate_and_append(knot_msg_config *config,
				 struct l_queue *msg_config_list)
{
	knot_msg_config *msg_config = l_memdup(config, sizeof(*config));
	l_queue_push_tail(msg_config_list, msg_config);
}

static struct l_queue *config_to_msg_config_list(struct l_queue *config_list)
{
	struct l_queue *msg_config_list;

	if (l_queue_isempty(config_list))
		return NULL;

	msg_config_list = l_queue_new();

	l_queue_foreach(config_list,
			(l_queue_foreach_func_t) duplicate_and_append,
			msg_config_list);

	return msg_config_list;
}

static bool exists_and_confirmed(struct config *received,
				 struct l_queue *current_list)
{
	struct config *current = l_queue_find(current_list,
					      (l_queue_match_func_t) config_cmp,
					      received);
	return current && current->confirmed;
}

static struct l_queue *get_changed_config(struct l_queue *current,
					  struct l_queue *received)
{
	struct l_queue *received_copy;
	struct l_queue *changed_configs;

	/*
	 * TODO:
	 * If a sensor_id is not in the list anymore, notify the thing.
	 */
	/*
	 * TODO:
	 * Define which approach is better, the current or when at least one
	 * config changes, the whole config message should be sent.
	 */
	received_copy = queue_clone(received);
	l_queue_foreach_remove(received_copy,
			       (l_queue_remove_func_t) exists_and_confirmed,
			       current);
	changed_configs = config_to_msg_config_list(received_copy);

	if (received_copy)
		l_queue_destroy(received_copy, NULL);

	return changed_configs;
}

/*
 * Parses the JSON from cloud to get all the configs. If the config is valid,
 * checks if any changed, and put them in the list that will be sent to the
 * thing. Returns the list with the messages to be sent or NULL if any error.
 */
static struct l_queue *msg_config(int node_socket,
				  json_raw_t device_message, ssize_t *result)
{
	struct trust *trust;
	struct l_queue *config = NULL;
	struct l_queue *changed_config = NULL;

	trust = l_hashmap_lookup(trust_map, L_INT_TO_PTR(node_socket));
	if (!trust) {
		hal_log_info("Permission denied!");
		*result = KNOT_CREDENTIAL_UNAUTHORIZED;
		return NULL;
	}

	config = parse_device_config(device_message.data);
	/* config_is_valid() returns 0 if SUCCESS */
	if (config_is_valid(config)) {
		hal_log_error("Invalid config message");
		l_queue_destroy(config, l_free);
		/*
		 * TODO: DEFINE KNOT_CONFIG ERRORS IN PROTOCOL
		 * KNOT_INVALID_CONFIG in new protocol
		 */
		*result = KNOT_NO_DATA;
		return NULL;
	}

	changed_config = get_changed_config(trust->config, config);

	trust_config_update(trust, config);

	*result = KNOT_SUCCESS;

	return changed_config;
}
#endif

static int get_socket_credentials(int sock, struct ucred *cred)
{
	socklen_t sklen;

	memset(cred, 0, sizeof(struct ucred));
	sklen = sizeof(struct ucred);
	if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, cred, &sklen) == -1) {
		hal_log_error("getsockopt(%d): %s(%d)", sock,
			strerror(errno), errno);
		return KNOT_ERROR_UNKNOWN;
	}

	return KNOT_SUCCESS;
}

static bool msg_register_has_valid_length(const knot_msg_register *kreq,
					  size_t length)
{
	/* Min PDU len containing at least one char representing name */
	return length > (sizeof(kreq->hdr) + sizeof(kreq->id));
}

static bool msg_register_has_valid_device_name(const knot_msg_register *kreq)
{
	return kreq->devName[0] != '\0';
}

/* device_name must have length of KNOT_PROTOCOL_DEVICE_NAME_LEN */
static void msg_register_get_device_name(const knot_msg_register *kreq,
					 char *device_name)
{
	size_t length;
	/*
	 * Make sure the device name is at maximum 63 bytes leaving 1 byte left
	 * for the terminating null character
	 */
	memset(device_name, 0, KNOT_PROTOCOL_DEVICE_NAME_LEN);

	length = MIN(kreq->hdr.payload_len - sizeof(kreq->id),
			KNOT_PROTOCOL_DEVICE_NAME_LEN - 1);

	strncpy(device_name, kreq->devName, length);
}

static void msg_credential_create(knot_msg_credential *message,
				  const char *uuid, const char *token)
{
	strncpy(message->uuid, uuid, sizeof(message->uuid));
	strncpy(message->token, token, sizeof(message->token));

	/* Payload length includes the result, UUID and TOKEN */
	message->hdr.payload_len = sizeof(*message) - sizeof(knot_msg_header);
}

static int8_t msg_register(int node_socket, int proto_socket,
			   const knot_msg_register *kreq, size_t ilen,
			   knot_msg_credential *krsp)
{
	char device_name[KNOT_PROTOCOL_DEVICE_NAME_LEN];
	char uuid[KNOT_PROTOCOL_UUID_LEN + 1];
	char token[KNOT_PROTOCOL_TOKEN_LEN + 1];
	struct ucred cred;
	struct trust *trust;
	int8_t result;

	if (!msg_register_has_valid_length(kreq, ilen)
		|| !msg_register_has_valid_device_name(kreq)) {
		hal_log_error("Missing device name!");
		return KNOT_REGISTER_INVALID_DEVICENAME;
	}

	/*
	 * Credential (Process ID) verification will work for unix socket
	 * only. For other socket types additional authentication mechanism
	 * will be required.
	 */
	result = get_socket_credentials(node_socket, &cred);
	if (result != KNOT_SUCCESS)
		hal_log_info("sock:%d, pid:%ld",
			     node_socket, (long int) cred.pid);

	/*
	 * Due to radio packet loss, peer may re-transmits register request
	 * if response does not arrives in 20 seconds. If this device was
	 * previously added we just send the uuid/token again.
	 */
	hal_log_info("Registering (id 0x%" PRIx64 ") fd:%d", kreq->id, node_socket);
	trust = l_hashmap_lookup(trust_map, L_INT_TO_PTR(node_socket));
	if (trust && kreq->id == trust->id && trust->pid == cred.pid) {
		hal_log_info("Register: trusted device");
		msg_credential_create(krsp, trust->uuid, trust->token);
		return KNOT_SUCCESS;
	}

	msg_register_get_device_name(kreq, device_name);
	memset(uuid, 0, sizeof(uuid));
	memset(token, 0, sizeof(token));
	result = proto_mknode(proto_socket, device_name, kreq->id, uuid, token);
	if (result != KNOT_SUCCESS)
		return result;

	hal_log_info("UUID: %s, TOKEN: %s", uuid, token);

	result = proto_signin(proto_socket, uuid, token, NULL, NULL);
	if (result != KNOT_SUCCESS)
		return result;

	msg_credential_create(krsp, uuid, token);

	trust = trust_new(uuid, token, kreq->id,
			  (cred.pid ? : INT32_MAX), true, NULL, NULL);

	l_hashmap_insert(trust_map, L_INT_TO_PTR(node_socket), trust);

	return KNOT_SUCCESS;
}

static int8_t msg_auth(int node_socket, int proto_socket,
		       const knot_msg_authentication *kmauth)
{
	char uuid[KNOT_PROTOCOL_UUID_LEN + 1];
	char token[KNOT_PROTOCOL_TOKEN_LEN + 1];
	struct l_queue *schema;
	struct l_queue *config;
	struct trust *trust;
	int8_t result;

	trust = l_hashmap_lookup(trust_map, L_INT_TO_PTR(node_socket));
	if (trust) {
		hal_log_info("Authenticated already");
		return KNOT_SUCCESS;
	}

	/*
	 * PDU is not null-terminated. Copy UUID and token to
	 * a null-terminated string.
	 */
	memset(uuid, 0, sizeof(uuid));
	memset(token, 0, sizeof(token));

	strncpy(uuid, kmauth->uuid, sizeof(kmauth->uuid));
	strncpy(token, kmauth->token, sizeof(kmauth->token));
	result = proto_signin(proto_socket, uuid, token, &schema, &config);
	if (result != KNOT_SUCCESS)
		return result;

	if (schema == NULL) {
		result = KNOT_SCHEMA_EMPTY;
		goto fail;
	}

	if (config_is_valid(config)) {
		hal_log_error("Invalid config message");
		l_queue_destroy(config, l_free);
		config = NULL;
	}

	/* TODO: should we receive the ID? Should we get the socket PID? */
	trust = trust_new(uuid, token, 0, 0, false, schema, config);
	l_hashmap_insert(trust_map, L_INT_TO_PTR(node_socket), trust);

	return KNOT_SUCCESS;

fail:
	l_queue_destroy(config, l_free);
	return result;
}

static int8_t msg_schema(int node_socket, int proto_socket,
			 const knot_msg_schema *schema, bool eof)
{
	int8_t result;
	struct trust *trust;

	trust = l_hashmap_lookup(trust_map, L_INT_TO_PTR(node_socket));
	if (!trust) {
		hal_log_info("Permission denied!");
		result = KNOT_CREDENTIAL_UNAUTHORIZED;
		goto done;
	}

	/*
	 * For security reason, remove from rollback avoiding clonning attack.
	 * If schema is being sent means that credentals (UUID/token) has been
	 * properly received (registration complete).
	 */
	trust->rollback = false;

	/*
	 * {
	 *	"schema" : [
	 *		{"sensor_id": x, "value_type": w,
	 *			"unit": z "type_id": y, "name": "foo"}
	 * 	]
	 * }
	 */

	/*
	 * Checks whether the schema was received before and if not, adds
	 * to a temporary list until receiving complete schema.
	 */
	if (!trust_get_sensor_schema_tmp(trust, schema->sensor_id))
		trust_sensor_schema_tmp_add(trust, schema);

	 /* TODO: missing timer to wait for end of schema transfer */

	if (!eof) {
		result = KNOT_SUCCESS;
		goto done;
	}

	result = proto_schema(proto_socket, trust->uuid,
			      trust->token, trust->schema_tmp);
	if (result != KNOT_SUCCESS) {
		trust_sensor_schema_tmp_free(trust);
		goto done;
	}

	/* If succeed: free old schema and use the new one */
	trust_sensor_schema_complete(trust);

done:
	return result;
}

static int8_t msg_data(int node_socket, int proto_socket,
		       const knot_msg_data *kmdata)
{
	const knot_msg_schema *schema;
	const struct trust *trust;
	int err;
	int8_t result;
	uint8_t sensor_id;
	/*
	 * Pointer to KNOT data containing header, sensor id
	 * and a primitive KNOT type
	 */
	const knot_data *kdata = &(kmdata->payload);

	trust = l_hashmap_lookup(trust_map, L_INT_TO_PTR(node_socket));
	if (!trust) {
		hal_log_info("Permission denied!");
		result = KNOT_CREDENTIAL_UNAUTHORIZED;
		goto done;
	}

	sensor_id = kmdata->sensor_id;
	schema = trust_get_sensor_schema(trust, sensor_id);
	if (!schema) {
		hal_log_info("sensor_id(0x%02x): data type mismatch!",
			     sensor_id);
		result = KNOT_INVALID_DATA;
		goto done;
	}

	err = knot_schema_is_valid(schema->values.type_id,
				   schema->values.value_type,
				   schema->values.unit);
	if (err) {
		hal_log_info("sensor_id(0x%d), type_id(0x%04x): unit mismatch!",
			     sensor_id, schema->values.type_id);
		result = KNOT_INVALID_DATA;
		goto done;
	}

	hal_log_info("sensor:%d, unit:%d, value_type:%d", sensor_id,
		     schema->values.unit, schema->values.value_type);

	result = proto_data(proto_socket, trust->uuid, trust->token, sensor_id,
			    schema->values.value_type, kdata);

	proto_getdata(proto_socket, trust->uuid, trust->token, sensor_id);

done:
	return result;
}

static int8_t msg_config_resp(int node_socket, const knot_msg_item *response)
{
	struct trust *trust;
	uint8_t sensor_id;

	trust = l_hashmap_lookup(trust_map, L_INT_TO_PTR(node_socket));
	if (!trust) {
		hal_log_info("Permission denied!");
		return KNOT_CREDENTIAL_UNAUTHORIZED;
	}

	sensor_id = response->sensor_id;

	/* TODO: Always forward instead of avoid sending repeated configs */
	l_queue_remove_if(trust->config,
			  config_sensor_id_cmp,
			  L_UINT_TO_PTR(sensor_id));

	hal_log_info("THING %s received config for sensor %d", trust->uuid,
								sensor_id);

	return KNOT_SUCCESS;
}

/*
 * Works like msg_data() (copy & paste), but removes the received info from
 * the 'devices' database.
 */
static int8_t msg_setdata_resp(int node_socket, int proto_socket,
			       const knot_msg_data *kmdata)
{
	const knot_msg_schema *schema;
	const struct trust *trust;
	int8_t result;
	int err;
	uint8_t sensor_id;
	/*
	 * Pointer to KNOT data containing header, sensor id
	 * and a primitive KNOT type
	 */
	const knot_data *kdata = &(kmdata->payload);

	trust = l_hashmap_lookup(trust_map, L_INT_TO_PTR(node_socket));
	if (!trust) {
		hal_log_info("Permission denied!");
		result = KNOT_CREDENTIAL_UNAUTHORIZED;
		goto done;
	}

	sensor_id = kmdata->sensor_id;
	schema = trust_get_sensor_schema(trust, sensor_id);
	if (!schema) {
		hal_log_info("sensor_id(0x%02x): data type mismatch!",
								sensor_id);
		result = KNOT_INVALID_DATA;
		goto done;
	}

	err = knot_schema_is_valid(schema->values.type_id,
				   schema->values.value_type,
				   schema->values.unit);
	if (err) {
		hal_log_info("sensor_id(0x%d), type_id(0x%04x): unit mismatch!",
					sensor_id, schema->values.type_id);
		result = KNOT_INVALID_DATA;
		goto done;
	}

	hal_log_info("sensor:%d, unit:%d, value_type:%d", sensor_id,
				schema->values.unit, schema->values.value_type);

	/* Fetches the 'devices' db */
	proto_setdata(proto_socket, trust->uuid, trust->token,
								sensor_id);

	result = proto_data(proto_socket, trust->uuid, trust->token, sensor_id,
			    schema->values.value_type, kdata);
	if (result != KNOT_SUCCESS)
		goto done;

	hal_log_info("THING %s updated data for sensor %d", trust->uuid,
								sensor_id);
	result = KNOT_SUCCESS;

done:
	return result;
}

ssize_t msg_process(int sock, int proto_sock,
				const void *ipdu, size_t ilen,
				void *opdu, size_t omtu)
{
	const knot_msg *kreq = ipdu;
	knot_msg *krsp = opdu;
	uint8_t rtype = 0;
	int8_t result = KNOT_INVALID_DATA;
	bool eof;

	/* Verify if output PDU has a min length */
	if (omtu < sizeof(knot_msg)) {
		hal_log_error("Output PDU: invalid PDU length");
		return -EINVAL;
	}

	/* Set a default payload length for error */
	krsp->hdr.payload_len = sizeof(krsp->action.result);

	/* At least header should be received */
	if (ilen < sizeof(knot_msg_header)) {
		hal_log_error("KNOT PDU: invalid minimum length");
		return -EINVAL;
	}

	/* Checking PDU length consistency */
	if (ilen != (sizeof(kreq->hdr) + kreq->hdr.payload_len)) {
		hal_log_error("KNOT PDU: length mismatch");
		return -EINVAL;
	}

	hal_log_info("KNOT OP: 0x%02X LEN: %02x",
				kreq->hdr.type, kreq->hdr.payload_len);

	switch (kreq->hdr.type) {
	case KNOT_MSG_REGISTER_REQ:
		/* Payload length is set by the caller */
		result = msg_register(sock, proto_sock,
				      &kreq->reg, ilen, &krsp->cred);
		rtype = KNOT_MSG_REGISTER_RESP;
		break;
	case KNOT_MSG_UNREGISTER_REQ:
		result = msg_unregister(sock, proto_sock);
		rtype = KNOT_MSG_UNREGISTER_RESP;
		break;
	case KNOT_MSG_DATA:
		result = msg_data(sock, proto_sock, &kreq->data);
		rtype = KNOT_MSG_DATA_RESP;
		break;
	case KNOT_MSG_AUTH_REQ:
		result = msg_auth(sock, proto_sock, &kreq->auth);
		rtype = KNOT_MSG_AUTH_RESP;
		break;
	case KNOT_MSG_SCHEMA:
	case KNOT_MSG_SCHEMA_END:
		eof = kreq->hdr.type == KNOT_MSG_SCHEMA_END ? true : false;
		result = msg_schema(sock, proto_sock, &kreq->schema, eof);
		rtype = KNOT_MSG_SCHEMA_RESP;
		if (eof)
			rtype = KNOT_MSG_SCHEMA_END_RESP;
		break;
	case KNOT_MSG_CONFIG_RESP:
		result = msg_config_resp(sock, &kreq->item);
		/* No octets to be transmitted */
		return 0;
	case KNOT_MSG_DATA_RESP:
		result = msg_setdata_resp(sock, proto_sock, &kreq->data);
		return 0;
	default:
		/* TODO: reply unknown command */
		break;
	}

	krsp->hdr.type = rtype;

	krsp->action.result = result;

	/* Return the actual amount of octets to be transmitted */
	return (sizeof(knot_msg_header) + krsp->hdr.payload_len);
}

int msg_start(void)
{
	trust_map = l_hashmap_new();

	return 0;
}

void msg_stop(void)
{
	l_hashmap_destroy(trust_map, (l_hashmap_destroy_func_t) trust_unref);
}
