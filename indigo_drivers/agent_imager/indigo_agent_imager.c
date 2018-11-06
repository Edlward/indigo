// Copyright (c) 2018 CloudMakers, s. r. o.
// All rights reserved.
//
// You can use this software under the terms of 'INDIGO Astronomy
// open-source license' (see LICENSE.md).
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHORS 'AS IS' AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// version history
// 2.0 by Peter Polakovic <peter.polakovic@cloudmakers.eu>

/** INDIGO Imager agent
 \file indigo_agent_imager.c
 */

#define DRIVER_VERSION 0x0001
#define DRIVER_NAME	"indigo_agent_imager"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include <pthread.h>

#include "indigo_driver_xml.h"
#include "indigo_agent_imager.h"

#define MAX_DEVICES							32
#define MAX_CACHED_PROPERTIES		512

#define DEVICE_PRIVATE_DATA							((agent_private_data *)device->private_data)
#define CLIENT_PRIVATE_DATA							((agent_private_data *)client->client_context)

#define AGENT_CCD_LIST_PROPERTY					(DEVICE_PRIVATE_DATA->agent_ccd_list_property)

#define AGENT_CCD_BATCH_PROPERTY				(DEVICE_PRIVATE_DATA->agent_ccd_batch_property)
#define AGENT_CCD_BATCH_COUNT_ITEM      (AGENT_CCD_BATCH_PROPERTY->items+0)
#define AGENT_CCD_BATCH_DELAY_ITEM     	(AGENT_CCD_BATCH_PROPERTY->items+1)

#define AGENT_START_PROCESS_PROPERTY		(DEVICE_PRIVATE_DATA->agent_start_process_property)
#define AGENT_START_CCD_BATCH_ITEM      (AGENT_START_PROCESS_PROPERTY->items+0)

#define AGENT_ABORT_PROCESS_PROPERTY		(DEVICE_PRIVATE_DATA->agent_abort_process_property)
#define AGENT_ABORT_PROCESS_ITEM      	(AGENT_ABORT_PROCESS_PROPERTY->items+0)


typedef struct {
	indigo_device *device;
	indigo_client *client;
	indigo_property *agent_ccd_list_property;
	indigo_property *agent_ccd_batch_property;
	indigo_property *agent_start_process_property;
	indigo_property *agent_abort_process_property;
	indigo_property *property_cache[MAX_CACHED_PROPERTIES];
} agent_private_data;

// -------------------------------------------------------------------------------- INDIGO agent common code

static indigo_property *get_property(indigo_device *device, indigo_property *device_list, const char *name) {
	for (int i = 0; i < device_list->count; i++) {
		if (device_list->items[i].sw.value) {
			char *device_name = device_list->items[i].name;
			indigo_property **cache = DEVICE_PRIVATE_DATA->property_cache;
			for (int j = 0; j < MAX_CACHED_PROPERTIES; j++) {
				if (cache[j] && !strcpy(cache[j]->device, device_name) && !strcpy(cache[j]->name, name))
					return cache[j];
			}
		}
	}
	return NULL;
}

static bool set_simple_property(indigo_device *device, indigo_property *device_list, const char *property_name, const char *value, double timeout) {
	if (*value == 0)
		return true;
	indigo_property *remote_property = get_property(device, device_list, property_name);
	indigo_property *local_property = NULL;
	if (remote_property) {
		switch (remote_property->type) {
			case INDIGO_TEXT_VECTOR: {
				local_property = indigo_init_text_property(NULL, remote_property->device, remote_property->name, NULL, NULL, INDIGO_OK_STATE, INDIGO_RW_PERM, 1);
				indigo_init_text_item(local_property->items, local_property->items->name, "", value);
				break;
			}
			case INDIGO_NUMBER_VECTOR: {
				local_property = indigo_init_number_property(NULL, remote_property->device, remote_property->name, NULL, NULL, INDIGO_OK_STATE, INDIGO_RW_PERM, 1);
				indigo_init_number_item(local_property->items, local_property->items->name, "", remote_property->items->number.min, remote_property->items->number.max, remote_property->items->number.step, atof(value));
				break;
			}
			case INDIGO_SWITCH_VECTOR: {
				local_property = indigo_init_switch_property(NULL, remote_property->device, remote_property->name, NULL, NULL, INDIGO_OK_STATE, INDIGO_RW_PERM, remote_property->rule, 1);
				indigo_init_switch_item(local_property->items, value, "", true);
				break;
			}
			default: {
				break;
			}
		}
		if (local_property) {
			indigo_change_property(DEVICE_PRIVATE_DATA->client, local_property);
			indigo_release_property(local_property);
			while (remote_property->state == INDIGO_BUSY_STATE && timeout > 0) {
				usleep(10000);
				timeout -= 0.01;
			}
			return remote_property->state == INDIGO_OK_STATE;
		}
	}
	return false;
}

static indigo_result select_device(indigo_device *device, indigo_client *client, indigo_property *property, indigo_property *list) {
	indigo_property *ccd_connection = indigo_init_switch_property(NULL, "", CONNECTION_PROPERTY_NAME, NULL, NULL, INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ONE_OF_MANY_RULE, 2);
	indigo_init_switch_item(ccd_connection->items + 0, CONNECTION_CONNECTED_ITEM_NAME, NULL, false);
	indigo_init_switch_item(ccd_connection->items + 1, CONNECTION_DISCONNECTED_ITEM_NAME, NULL, true);
	for (int i = 1; i < list->count; i++) {
		if (list->items[i].sw.value) {
			list->items[i].sw.value = false;
			strncpy(ccd_connection->device, list->items[i].name, INDIGO_NAME_SIZE);
			indigo_change_property(client, ccd_connection);
			break;
		}
	}
	indigo_property_copy_values(list, property, false);
	for (int i = 1; i < list->count; i++) {
		if (list->items[i].sw.value) {
			indigo_set_switch(ccd_connection, ccd_connection->items, true);
			list->state = INDIGO_BUSY_STATE;
			indigo_update_property(device, list, NULL);
			strncpy(ccd_connection->device, list->items[i].name, INDIGO_NAME_SIZE);
			indigo_change_property(client, ccd_connection);
			return INDIGO_OK;
		}
	}
	list->state = INDIGO_OK_STATE;
	indigo_update_property(device, list, NULL);
	return INDIGO_OK;
}

static indigo_result add_device(struct indigo_client *client, struct indigo_device *device, indigo_property *property, indigo_property *device_list) {
	int count = device_list->count;
	for (int i = 1; i < count; i++) {
		if (!strcmp(property->device, device_list->items[i].name)) {
			return INDIGO_OK;
		}
	}
	if (count < MAX_DEVICES) {
		indigo_delete_property(CLIENT_PRIVATE_DATA->device, device_list, NULL);
		indigo_init_switch_item(device_list->items + count, property->device, property->device, false);
		device_list->count++;
		indigo_define_property(CLIENT_PRIVATE_DATA->device, device_list, NULL);
	}
	return INDIGO_OK;
}

static indigo_result connect_device(struct indigo_client *client, struct indigo_device *device, indigo_property *property, indigo_property *device_list) {
	indigo_item *connected_device = indigo_get_item(property, CONNECTION_CONNECTED_ITEM_NAME);
	for (int i = 1; i < device_list->count; i++) {
		if (!strcmp(property->device, device_list->items[i].name) && device_list->items[i].sw.value) {
			if (device_list->state == INDIGO_BUSY_STATE) {
				if (property->state == INDIGO_ALERT_STATE) {
					device_list->state = INDIGO_ALERT_STATE;
				} else if (connected_device->sw.value && property->state == INDIGO_OK_STATE) {
					indigo_property all_properties;
					memset(&all_properties, 0, sizeof(all_properties));
					strcpy(all_properties.device, property->device);
					indigo_enumerate_properties(client, &all_properties);
					device_list->state = INDIGO_OK_STATE;
				}
				indigo_update_property(CLIENT_PRIVATE_DATA->device, device_list, NULL);
				return INDIGO_OK;
			} else if (device_list->state == INDIGO_OK_STATE && !connected_device->sw.value) {
				device_list->state = INDIGO_ALERT_STATE;
				indigo_update_property(CLIENT_PRIVATE_DATA->device, device_list, NULL);
				return INDIGO_OK;
			}
		}
	}
	return INDIGO_OK;
}

// -------------------------------------------------------------------------------- INDIGO agent device implementation

static indigo_result agent_enumerate_properties(indigo_device *device, indigo_client *client, indigo_property *property);

static indigo_result agent_device_attach(indigo_device *device) {
	assert(device != NULL);
	assert(DEVICE_PRIVATE_DATA != NULL);
	if (indigo_agent_attach(device, DRIVER_VERSION) == INDIGO_OK) {
		// -------------------------------------------------------------------------------- Device properties
		AGENT_CCD_LIST_PROPERTY = indigo_init_switch_property(NULL, device->name, AGENT_CCD_LIST_PROPERTY_NAME, "Main", "Camera list", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ONE_OF_MANY_RULE, MAX_DEVICES);
		if (AGENT_CCD_LIST_PROPERTY == NULL)
			return INDIGO_FAILED;
		AGENT_CCD_LIST_PROPERTY->count = 1;
		indigo_init_switch_item(AGENT_CCD_LIST_PROPERTY->items, AGENT_DEVICE_LIST_NONE_ITEM_NAME, "None", true);
		// -------------------------------------------------------------------------------- Batch properties
		AGENT_CCD_BATCH_PROPERTY = indigo_init_number_property(NULL, device->name, AGENT_CCD_BATCH_PROPERTY_NAME, "Batch", "Batch settings", INDIGO_OK_STATE, INDIGO_RW_PERM, 2);
		if (AGENT_CCD_BATCH_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_number_item(AGENT_CCD_BATCH_COUNT_ITEM, AGENT_CCD_BATCH_COUNT_ITEM_NAME, "Frame count", 0, 1000, 1, 1);
		indigo_init_number_item(AGENT_CCD_BATCH_DELAY_ITEM, AGENT_CCD_BATCH_DELAY_ITEM_NAME, "Delay after each exposure", 0, 3600, 1, 0);
		AGENT_START_PROCESS_PROPERTY = indigo_init_switch_property(NULL, device->name, AGENT_START_PROCESS_PROPERTY_NAME, "Batch", "Start batch", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ANY_OF_MANY_RULE, 1);
		if (AGENT_START_PROCESS_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_switch_item(AGENT_START_CCD_BATCH_ITEM, AGENT_START_CCD_BATCH_ITEM_NAME, "Start batch", false);
		AGENT_ABORT_PROCESS_PROPERTY = indigo_init_switch_property(NULL, device->name, AGENT_ABORT_PROCESS_PROPERTY_NAME, "Batch", "Abort batch", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ANY_OF_MANY_RULE, 1);
		if (AGENT_ABORT_PROCESS_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_switch_item(AGENT_ABORT_PROCESS_ITEM, AGENT_ABORT_PROCESS_ITEM_NAME, "Abort batch", false);
		// --------------------------------------------------------------------------------
		INDIGO_DEVICE_ATTACH_LOG(DRIVER_NAME, device->name);
		return agent_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}

static indigo_result agent_enumerate_properties(indigo_device *device, indigo_client *client, indigo_property *property) {
	if (client!= NULL && client == DEVICE_PRIVATE_DATA->client)
		return INDIGO_OK;
	if (indigo_property_match(AGENT_CCD_LIST_PROPERTY, property))
		indigo_define_property(device, AGENT_CCD_LIST_PROPERTY, NULL);
	if (indigo_property_match(AGENT_CCD_BATCH_PROPERTY, property))
		indigo_define_property(device, AGENT_CCD_BATCH_PROPERTY, NULL);
	if (indigo_property_match(AGENT_START_PROCESS_PROPERTY, property))
		indigo_define_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
	if (indigo_property_match(AGENT_ABORT_PROCESS_PROPERTY, property))
		indigo_define_property(device, AGENT_ABORT_PROCESS_PROPERTY, NULL);
	return indigo_device_enumerate_properties(device, client, property);
}

static indigo_result agent_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(DEVICE_CONTEXT != NULL);
	assert(property != NULL);
	if (client == DEVICE_PRIVATE_DATA->client)
		return INDIGO_OK;
	if (indigo_property_match(AGENT_CCD_LIST_PROPERTY, property)) {
		return select_device(device, client, property, AGENT_CCD_LIST_PROPERTY);
	} else 	if (indigo_property_match(AGENT_CCD_BATCH_PROPERTY, property)) {
		indigo_property_copy_values(AGENT_CCD_BATCH_PROPERTY, property, false);
		AGENT_CCD_BATCH_PROPERTY->state = INDIGO_OK_STATE;
		indigo_update_property(device, AGENT_CCD_BATCH_PROPERTY, NULL);
	} else 	if (indigo_property_match(AGENT_START_PROCESS_PROPERTY, property)) {
		indigo_property_copy_values(AGENT_START_PROCESS_PROPERTY, property, false);
		AGENT_START_CCD_BATCH_ITEM->sw.value = false;
		AGENT_START_PROCESS_PROPERTY->state = INDIGO_OK_STATE;
		indigo_update_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
	} else 	if (indigo_property_match(AGENT_ABORT_PROCESS_PROPERTY, property)) {
		indigo_property_copy_values(AGENT_ABORT_PROCESS_PROPERTY, property, false);
		AGENT_ABORT_PROCESS_ITEM->sw.value = false;
		AGENT_ABORT_PROCESS_PROPERTY->state = INDIGO_OK_STATE;
		indigo_update_property(device, AGENT_ABORT_PROCESS_PROPERTY, NULL);
	}
	return indigo_agent_change_property(device, client, property);
}

static indigo_result agent_device_detach(indigo_device *device) {
	assert(device != NULL);
	indigo_release_property(AGENT_CCD_LIST_PROPERTY);
	indigo_release_property(AGENT_CCD_BATCH_PROPERTY);
	indigo_release_property(AGENT_START_PROCESS_PROPERTY);
	indigo_release_property(AGENT_ABORT_PROCESS_PROPERTY);
	return indigo_agent_detach(device);
}

// -------------------------------------------------------------------------------- INDIGO agent client implementation

static indigo_result agent_client_attach(indigo_client *client) {
	indigo_property all_properties;
	indigo_property **cache = CLIENT_PRIVATE_DATA->property_cache;
	for (int i = 0; i < MAX_CACHED_PROPERTIES; i++) {
		cache[i] = NULL;
	}
	memset(&all_properties, 0, sizeof(all_properties));
	indigo_enumerate_properties(client, &all_properties);
	return INDIGO_OK;
}

static indigo_result agent_define_property(struct indigo_client *client, struct indigo_device *device, indigo_property *property, const char *message) {
	if (device == CLIENT_PRIVATE_DATA->device)
		return INDIGO_OK;
	bool found = false;
	indigo_property **cache = CLIENT_PRIVATE_DATA->property_cache;
	for (int i = 0; i < MAX_CACHED_PROPERTIES; i++) {
		if (cache[i] == property) {
			found = true;
			break;
		}
	}
	if (!found) {
		for (int i = 0; i < MAX_CACHED_PROPERTIES; i++) {
			if (cache[i] == NULL) {
				cache[i] = property;
				break;
			}
		}
	}
	if (!strcmp(property->name, INFO_PROPERTY_NAME)) {
		indigo_item *interface = indigo_get_item(property, INFO_DEVICE_INTERFACE_ITEM_NAME);
		if (interface) {
			int mask = atoi(interface->text.value);
			if (mask & INDIGO_INTERFACE_CCD) {
				add_device(client, device, property, CLIENT_PRIVATE_DATA->agent_ccd_list_property);
			}
		}
	}
	return INDIGO_OK;
}

static indigo_result agent_update_property(struct indigo_client *client, struct indigo_device *device, indigo_property *property, const char *message) {
	if (device == CLIENT_PRIVATE_DATA->device)
		return INDIGO_OK;
	if (!strcmp(property->name, CONNECTION_PROPERTY_NAME)) {
		if (property->state != INDIGO_BUSY_STATE) {
			connect_device(client, device, property, CLIENT_PRIVATE_DATA->agent_ccd_list_property);
		}
	}
	return INDIGO_OK;
}

static indigo_result delete_device(struct indigo_client *client, struct indigo_device *device, indigo_property *property, indigo_property *device_list) {
	for (int i = 1; i < device_list->count; i++) {
		if (!strcmp(property->device, device_list->items[i].name)) {
			int size = (device_list->count - i - 1) * sizeof(indigo_item);
			if (size > 0) {
				memcpy(device_list->items + i, device_list->items + i + 1, size);
			}
			indigo_delete_property(CLIENT_PRIVATE_DATA->device, device_list, NULL);
			device_list->count--;
			indigo_define_property(CLIENT_PRIVATE_DATA->device, device_list, NULL);
			return INDIGO_OK;
		}
	}
	return INDIGO_OK;
}

static indigo_result agent_delete_property(indigo_client *client, struct indigo_device *device, indigo_property *property, const char *message) {
	if (device == CLIENT_PRIVATE_DATA->device)
		return INDIGO_OK;
	if (*property->name == 0 || !strcmp(property->name, INFO_PROPERTY_NAME)) {
		delete_device(client, device, property, CLIENT_PRIVATE_DATA->agent_ccd_list_property);
	}
	indigo_property **cache = CLIENT_PRIVATE_DATA->property_cache;
	for (int i = 0; i < MAX_CACHED_PROPERTIES; i++) {
		if (cache[i] == property) {
			cache[i] = NULL;
			break;
		}
	}
	return INDIGO_OK;
}

static indigo_result agent_client_detach(indigo_client *client) {
	// TBD
	return INDIGO_OK;
}

// -------------------------------------------------------------------------------- Initialization

static agent_private_data *private_data = NULL;

static indigo_device *agent_device = NULL;
static indigo_client *agent_client = NULL;

indigo_result indigo_agent_imager(indigo_driver_action action, indigo_driver_info *info) {
	static indigo_device agent_device_template = INDIGO_DEVICE_INITIALIZER(
		IMAGER_AGENT_NAME,
		agent_device_attach,
		agent_enumerate_properties,
		agent_change_property,
		NULL,
		agent_device_detach
	);

	static indigo_client agent_client_template = {
		IMAGER_AGENT_NAME, false, NULL, INDIGO_OK, INDIGO_VERSION_CURRENT, NULL,
		agent_client_attach,
		agent_define_property,
		agent_update_property,
		agent_delete_property,
		NULL,
		agent_client_detach
	};

	static indigo_driver_action last_action = INDIGO_DRIVER_SHUTDOWN;

	SET_DRIVER_INFO(info, "Imager agent", __FUNCTION__, DRIVER_VERSION, false, last_action);

	if (action == last_action)
		return INDIGO_OK;

	switch(action) {
		case INDIGO_DRIVER_INIT:
			last_action = action;
			private_data = malloc(sizeof(agent_private_data));
			assert(private_data != NULL);
			memset(private_data, 0, sizeof(agent_private_data));
			agent_device = malloc(sizeof(indigo_device));
			assert(agent_device != NULL);
			private_data->device = agent_device;
			memcpy(agent_device, &agent_device_template, sizeof(indigo_device));
			agent_device->private_data = private_data;
			indigo_attach_device(agent_device);

			agent_client = malloc(sizeof(indigo_client));
			assert(agent_client != NULL);
			private_data->client = agent_client;
			memcpy(agent_client, &agent_client_template, sizeof(indigo_client));
			agent_client->client_context = private_data;
			indigo_attach_client(agent_client);
			break;

		case INDIGO_DRIVER_SHUTDOWN:
			last_action = action;
			if (agent_device != NULL) {
				indigo_detach_device(agent_device);
				free(agent_device);
				agent_device = NULL;
			}
			if (agent_client != NULL) {
				indigo_detach_client(agent_client);
				free(agent_client);
				agent_client = NULL;
			}
			if (private_data != NULL) {
				free(private_data);
				private_data = NULL;
			}
			break;

		case INDIGO_DRIVER_INFO:
			break;
	}
	return INDIGO_OK;
}