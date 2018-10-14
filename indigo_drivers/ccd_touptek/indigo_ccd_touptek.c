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

/** INDIGO ToupTek CCD driver
 \file indigo_ccd_touptek.c
 */

#define DRIVER_VERSION 0x0006
#define DRIVER_NAME "indigo_ccd_touptek"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>

#include <toupcam.h>

#include "indigo_usb_utils.h"
#include "indigo_driver_xml.h"

#include "indigo_ccd_touptek.h"

#define PRIVATE_DATA        ((touptek_private_data *)device->private_data)

typedef struct {
	ToupcamInstV2 cam;
	HToupCam handle;
	bool present;
	indigo_device *camera;
	indigo_device *guider;
	indigo_timer *exposure_timer, *temperature_timer, *guider_timer;
	unsigned char *buffer;
	int remaining;
	int bits;
	bool can_check_temperature;
} touptek_private_data;

// -------------------------------------------------------------------------------- INDIGO CCD device implementation

static void pull_callback(unsigned event, void* callbackCtx) {
	ToupcamFrameInfoV2 frameInfo;
	HRESULT result;
	indigo_device *device = (indigo_device *)callbackCtx;
	INDIGO_DRIVER_DEBUG(DRIVER_NAME, "event #%d", event);
	switch (event) {
		case TOUPCAM_EVENT_IMAGE: {
			if (PRIVATE_DATA->remaining) {
				PRIVATE_DATA->remaining--;
				result = Toupcam_PullImageV2(PRIVATE_DATA->handle, PRIVATE_DATA->buffer + FITS_HEADER_SIZE, PRIVATE_DATA->bits, &frameInfo);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_PullImageV2(%d, ->[%d x %d, %x, %d])->%d", PRIVATE_DATA->bits, frameInfo.width, frameInfo.height, frameInfo.flag, frameInfo.seq, result);
				result = Toupcam_Pause(PRIVATE_DATA->handle, 1);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_Pause(1)->%d", result);
				indigo_process_image(device, PRIVATE_DATA->buffer, frameInfo.width, frameInfo.height, PRIVATE_DATA->bits, true, NULL);
				CCD_EXPOSURE_PROPERTY->state = INDIGO_OK_STATE;
			}
			break;
		}
		case TOUPCAM_EVENT_TIMEOUT:
		case TOUPCAM_EVENT_DISCONNECTED:
		case TOUPCAM_EVENT_ERROR: {
			CCD_EXPOSURE_PROPERTY->state = INDIGO_ALERT_STATE;
			break;
		}
	}
	indigo_update_property(device, CCD_EXPOSURE_PROPERTY, NULL);
}

static void ccd_temperature_callback(indigo_device *device) {
	if (!CONNECTION_CONNECTED_ITEM->sw.value)
		return;
	if (PRIVATE_DATA->can_check_temperature) {
		short temperature;
		if (Toupcam_get_Temperature(PRIVATE_DATA->handle, &temperature) >= 0) {
			CCD_TEMPERATURE_ITEM->number.value = temperature / 1.0;
			if (CCD_TEMPERATURE_PROPERTY->perm == INDIGO_RW_PERM && fabs(CCD_TEMPERATURE_ITEM->number.value - CCD_TEMPERATURE_ITEM->number.target) > 1.0) {
				if (!CCD_COOLER_PROPERTY->hidden && CCD_COOLER_OFF_ITEM->sw.value)
					CCD_TEMPERATURE_PROPERTY->state = INDIGO_OK_STATE;
				else
					CCD_TEMPERATURE_PROPERTY->state = INDIGO_BUSY_STATE;
			} else {
				CCD_TEMPERATURE_PROPERTY->state = INDIGO_OK_STATE;
			}
			indigo_update_property(device, CCD_TEMPERATURE_PROPERTY, NULL);
		}
	}
	indigo_reschedule_timer(device, 5, &PRIVATE_DATA->temperature_timer);
}

static indigo_result ccd_attach(indigo_device *device) {
	assert(device != NULL);
	assert(PRIVATE_DATA != NULL);
	if (indigo_ccd_attach(device, DRIVER_VERSION) == INDIGO_OK) {
		// --------------------------------------------------------------------------------
		unsigned long long flags = PRIVATE_DATA->cam.model->flag;
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "flags = %0LX", flags);
		char name[128], label[128];
		CCD_INFO_PIXEL_WIDTH_ITEM->number.value = PRIVATE_DATA->cam.model->xpixsz;
		CCD_INFO_PIXEL_HEIGHT_ITEM->number.value = PRIVATE_DATA->cam.model->ypixsz;
		CCD_INFO_PIXEL_SIZE_ITEM->number.value = (CCD_INFO_PIXEL_WIDTH_ITEM->number.value + CCD_INFO_PIXEL_HEIGHT_ITEM->number.value) / 2.0;
		CCD_MODE_PROPERTY->perm = INDIGO_RW_PERM;
		CCD_MODE_PROPERTY->count = 0;
		CCD_INFO_WIDTH_ITEM->number.value = 0;
		CCD_INFO_HEIGHT_ITEM->number.value = 0;
		for (int i = 0; i < PRIVATE_DATA->cam.model->preview; i++) {
			int frame_width = PRIVATE_DATA->cam.model->res[i].width;
			int frame_height = PRIVATE_DATA->cam.model->res[i].height;
			if (frame_width > CCD_INFO_WIDTH_ITEM->number.value)
				CCD_INFO_WIDTH_ITEM->number.value = frame_width;
			if (frame_height > CCD_INFO_HEIGHT_ITEM->number.value)
				CCD_INFO_HEIGHT_ITEM->number.value = frame_height;
			if (flags & TOUPCAM_FLAG_RAW8) {
				snprintf(name, sizeof(name), "RAW8_%d", i);
				snprintf(label, sizeof(label), "RAW %d x %dx8", frame_width, frame_height);
				indigo_init_switch_item(CCD_MODE_ITEM + CCD_MODE_PROPERTY->count, name, label, false);
				CCD_MODE_PROPERTY->count++;
			}
			if (flags & TOUPCAM_FLAG_RAW16 || flags & TOUPCAM_FLAG_RAW14 || flags & TOUPCAM_FLAG_RAW12 || flags & TOUPCAM_FLAG_RAW10) {
				snprintf(name, sizeof(name), "RAW16_%d", i);
				snprintf(label, sizeof(label), "RAW %d x %dx16", frame_width, frame_height);
				indigo_init_switch_item(CCD_MODE_ITEM + CCD_MODE_PROPERTY->count, name, label, false);
				CCD_MODE_PROPERTY->count++;
			}
			if ((flags & TOUPCAM_FLAG_MONO) == 0) {
				snprintf(name, sizeof(name), "RGB_%d", i);
				snprintf(label, sizeof(label), "RGB %d x %d", frame_width, frame_height);
				indigo_init_switch_item(CCD_MODE_ITEM + CCD_MODE_PROPERTY->count, name, label, false);
				CCD_MODE_PROPERTY->count++;
			}
		}
		CCD_MODE_ITEM->sw.value = true;
		CCD_FRAME_WIDTH_ITEM->number.value = CCD_FRAME_WIDTH_ITEM->number.max = CCD_FRAME_LEFT_ITEM->number.max = CCD_INFO_WIDTH_ITEM->number.value;
		CCD_FRAME_HEIGHT_ITEM->number.value = CCD_FRAME_HEIGHT_ITEM->number.max = CCD_FRAME_TOP_ITEM->number.max = CCD_INFO_HEIGHT_ITEM->number.value;
		CCD_FRAME_BITS_PER_PIXEL_ITEM->number.min = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.max = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.value = 0;
		if (flags & TOUPCAM_FLAG_RAW8) {
			CCD_INFO_BITS_PER_PIXEL_ITEM->number.value = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.min = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.max = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.value = 8;
		}
		if (flags & TOUPCAM_FLAG_RAW16 || flags & TOUPCAM_FLAG_RAW14 || flags & TOUPCAM_FLAG_RAW12 || flags & TOUPCAM_FLAG_RAW10) {
			if (CCD_FRAME_BITS_PER_PIXEL_ITEM->number.min == 0)
				CCD_FRAME_BITS_PER_PIXEL_ITEM->number.min = 16;
			CCD_INFO_BITS_PER_PIXEL_ITEM->number.value = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.max = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.value = 16;
		}
		if ((flags & TOUPCAM_FLAG_ROI_HARDWARE) == 0) {
			CCD_FRAME_PROPERTY->perm = INDIGO_RO_PERM;
		}
		if (PRIVATE_DATA->cam.model->flag & TOUPCAM_FLAG_GETTEMPERATURE) {
			CCD_TEMPERATURE_PROPERTY->hidden = false;
			if (PRIVATE_DATA->cam.model->flag & TOUPCAM_FLAG_PUTTEMPERATURE) {
				CCD_TEMPERATURE_PROPERTY->perm = INDIGO_RW_PERM;
				if (PRIVATE_DATA->cam.model->flag & TOUPCAM_FLAG_TEC_ONOFF) {
					CCD_COOLER_PROPERTY->hidden = false;
					indigo_set_switch(CCD_COOLER_PROPERTY, CCD_COOLER_OFF_ITEM, true);
				}
			} else {
				CCD_TEMPERATURE_PROPERTY->perm = INDIGO_RO_PERM;
			}
		}
		CCD_BIN_PROPERTY->hidden = true;
		PRIVATE_DATA->buffer = (unsigned char *)indigo_alloc_blob_buffer(3 * CCD_INFO_WIDTH_ITEM->number.value * CCD_INFO_HEIGHT_ITEM->number.value + FITS_HEADER_SIZE);
		// --------------------------------------------------------------------------------
		INDIGO_DEVICE_ATTACH_LOG(DRIVER_NAME, device->name);
		return indigo_ccd_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}

static indigo_result ccd_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(DEVICE_CONTEXT != NULL);
	assert(property != NULL);
	if (indigo_property_match(CONNECTION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CONNECTION -> CCD_INFO, CCD_COOLER, CCD_TEMPERATURE
		indigo_property_copy_values(CONNECTION_PROPERTY, property, false);
		if (CONNECTION_CONNECTED_ITEM->sw.value) {
			if (PRIVATE_DATA->handle == NULL) {
				if (indigo_try_global_lock(device) != INDIGO_OK) {
					INDIGO_DRIVER_ERROR(DRIVER_NAME, "indigo_try_global_lock(): failed to get lock.");
				} else {
					PRIVATE_DATA->handle = Toupcam_Open(PRIVATE_DATA->cam.id);
				}
			}
			device->gp_bits = 1;
			if (PRIVATE_DATA->handle) {
				if (PRIVATE_DATA->cam.model->flag & TOUPCAM_FLAG_GETTEMPERATURE)
					PRIVATE_DATA->temperature_timer = indigo_set_timer(device, 5.0, ccd_temperature_callback);
				else
					PRIVATE_DATA->temperature_timer = NULL;
				HRESULT result;
				int rawMode;
				int bitDepth;
				unsigned resolutionIndex;
				char name[16];
				if (PRIVATE_DATA->cam.model->flag & TOUPCAM_FLAG_MONO) {
					rawMode = 1;
				} else {
					result = Toupcam_get_Option(PRIVATE_DATA->handle, TOUPCAM_OPTION_RAW, &rawMode);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_get_Option(TOUPCAM_OPTION_RAW, ->%d)->%d", rawMode, result);
				}
				if (rawMode) {
					result = Toupcam_get_Option(PRIVATE_DATA->handle, TOUPCAM_OPTION_BITDEPTH, &bitDepth);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_get_Option(TOUPCAM_OPTION_BITDEPTH, ->%d)->%d", bitDepth, result);
					result = Toupcam_get_eSize(PRIVATE_DATA->handle, &resolutionIndex);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_get_eSize(->%d)->%d", resolutionIndex, result);
					sprintf(name, "RAW%d_%d", bitDepth ? 16 : 8, resolutionIndex);
				} else {
					result = Toupcam_get_eSize(PRIVATE_DATA->handle, &resolutionIndex);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_get_eSize(->%d)->%d", resolutionIndex, result);
					sprintf(name, "RGB_%d", resolutionIndex);
				}
				for (int i = 0; i < CCD_MODE_PROPERTY->count; i++) {
					if (strcmp(name, CCD_MODE_PROPERTY->items[i].name) == 0) {
						indigo_set_switch(CCD_MODE_PROPERTY, CCD_MODE_PROPERTY->items + i, true);
					}
				}
			} else {
				CONNECTION_PROPERTY->state = INDIGO_ALERT_STATE;
				indigo_set_switch(CONNECTION_PROPERTY, CONNECTION_DISCONNECTED_ITEM, true);
				device->gp_bits = 0;
			}
		} else {
			indigo_cancel_timer(device, &PRIVATE_DATA->temperature_timer);
			if (PRIVATE_DATA->buffer != NULL) {
				free(PRIVATE_DATA->buffer);
				PRIVATE_DATA->buffer = NULL;
			}
			if (PRIVATE_DATA->guider && PRIVATE_DATA->guider->gp_bits == 0) {
				if (((touptek_private_data *)PRIVATE_DATA->guider->private_data)->handle == NULL) {
					Toupcam_Close(PRIVATE_DATA->handle);
				}
				PRIVATE_DATA->handle = NULL;
				indigo_global_unlock(device);
			}
			device->gp_bits = 0;
			CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
		}
	} else if (indigo_property_match(CCD_EXPOSURE_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CCD_EXPOSURE
		if (CCD_EXPOSURE_PROPERTY->state == INDIGO_BUSY_STATE)
			return INDIGO_OK;
		HRESULT result;
		indigo_property_copy_values(CCD_EXPOSURE_PROPERTY, property, false);
		result = Toupcam_Stop(PRIVATE_DATA->handle);
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_Stop()->%d", result);
		PRIVATE_DATA->bits = 0;
		for (int i = 0; i < CCD_MODE_PROPERTY->count; i++) {
			indigo_item *item = CCD_MODE_PROPERTY->items + i;
			if (item->sw.value) {
				if (strncmp(item->name, "RAW8", 4) == 0) {
					result = Toupcam_put_Option(PRIVATE_DATA->handle, TOUPCAM_OPTION_RAW, 1);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_put_Option(TOUPCAM_OPTION_RAW, 1)->%d", result);
					result = Toupcam_put_Option(PRIVATE_DATA->handle, TOUPCAM_OPTION_BITDEPTH, 0);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_put_Option(TOUPCAM_OPTION_BITDEPTH, 0)->%d", result);
					unsigned resolutionIndex = atoi(item->name + 5);
					result = Toupcam_put_eSize(PRIVATE_DATA->handle, resolutionIndex);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_put_eSize(%d)->%d", resolutionIndex, result);
					PRIVATE_DATA->bits = 8;
				} else if (strncmp(item->name, "RAW16", 4) == 0) {
					result = Toupcam_put_Option(PRIVATE_DATA->handle, TOUPCAM_OPTION_RAW, 1);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_put_Option(TOUPCAM_OPTION_RAW, 1)->%d", result);
					result = Toupcam_put_Option(PRIVATE_DATA->handle, TOUPCAM_OPTION_BITDEPTH, 1);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_put_Option(TOUPCAM_OPTION_BITDEPTH, 1)->%d", result);
					unsigned resolutionIndex = atoi(item->name + 6);
					result = Toupcam_put_eSize(PRIVATE_DATA->handle, resolutionIndex);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_put_eSize(%d)->%d", resolutionIndex, result);
					PRIVATE_DATA->bits = 16;
				} else if (strncmp(item->name, "RGB", 3) == 0) {
					result = Toupcam_put_Option(PRIVATE_DATA->handle, TOUPCAM_OPTION_RAW, 0);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_put_Option(TOUPCAM_OPTION_RAW, 0)->%d", result);
					unsigned resolutionIndex = atoi(item->name + 4);
					result = Toupcam_put_eSize(PRIVATE_DATA->handle, resolutionIndex);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_put_eSize(%d)->%d", resolutionIndex, result);
					PRIVATE_DATA->bits = 24;
				}
			}
		}
		result = Toupcam_put_ExpoTime(PRIVATE_DATA->handle, CCD_EXPOSURE_ITEM->number.target * 1000000);
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_put_ExpoTime(%ld)->%d", (unsigned)(CCD_EXPOSURE_ITEM->number.target * 1000000), result);
		PRIVATE_DATA->remaining = 1;
		result = Toupcam_StartPullModeWithCallback(PRIVATE_DATA->handle, pull_callback, device);
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_StartPullModeWithCallback()->%d", result);
		CCD_EXPOSURE_PROPERTY->state = INDIGO_BUSY_STATE;
	} else if (indigo_property_match(CCD_ABORT_EXPOSURE_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CCD_ABORT_EXPOSURE
		HRESULT result;
		indigo_property_copy_values(CCD_ABORT_EXPOSURE_PROPERTY, property, false);
		if (CCD_ABORT_EXPOSURE_ITEM->sw.value) {
			CCD_ABORT_EXPOSURE_ITEM->sw.value = false;
			result = Toupcam_Stop(PRIVATE_DATA->handle);
			INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Toupcam_Stop()->%d", result);
			PRIVATE_DATA->remaining = 0;
			if (result >=0)
				CCD_ABORT_EXPOSURE_PROPERTY->state = INDIGO_OK_STATE;
			else
				CCD_ABORT_EXPOSURE_PROPERTY->state = INDIGO_ALERT_STATE;
		}
	} else if (indigo_property_match(CCD_COOLER_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CCD_COOLER
		indigo_property_copy_values(CCD_COOLER_PROPERTY, property, false);
		if (Toupcam_put_Option(PRIVATE_DATA->handle, TOUPCAM_OPTION_TEC, CCD_COOLER_ON_ITEM->sw.value ? 1 : 0) >= 0)
			CCD_COOLER_PROPERTY->state = INDIGO_OK_STATE;
		else
			CCD_COOLER_PROPERTY->state = INDIGO_ALERT_STATE;
		indigo_update_property(device, CCD_COOLER_PROPERTY, NULL);
		return INDIGO_OK;
	} else if (indigo_property_match(CCD_TEMPERATURE_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CCD_TEMPERATURE
		indigo_property_copy_values(CCD_TEMPERATURE_PROPERTY, property, false);
		if (Toupcam_put_Temperature(PRIVATE_DATA->handle, (short)(CCD_TEMPERATURE_ITEM->number.target * 10))) {
			CCD_TEMPERATURE_PROPERTY->state = INDIGO_OK_STATE;
			if (!CCD_COOLER_PROPERTY->hidden && CCD_COOLER_OFF_ITEM->sw.value) {
				if (Toupcam_put_Option(PRIVATE_DATA->handle, TOUPCAM_OPTION_TEC, 1) >= 0) {
					indigo_set_switch(CCD_COOLER_PROPERTY, CCD_COOLER_ON_ITEM, true);
					CCD_COOLER_PROPERTY->state = INDIGO_OK_STATE;
				} else {
					CCD_COOLER_PROPERTY->state = INDIGO_ALERT_STATE;
				}
				indigo_update_property(device, CCD_COOLER_PROPERTY, NULL);
			}
		} else {
			CCD_TEMPERATURE_PROPERTY->state = INDIGO_ALERT_STATE;
		}
		indigo_update_property(device, CCD_TEMPERATURE_PROPERTY, NULL);
		return INDIGO_OK;
		// --------------------------------------------------------------------------------
	}
	return indigo_ccd_change_property(device, client, property);
}

static indigo_result ccd_detach(indigo_device *device) {
	assert(device != NULL);
	if (CONNECTION_CONNECTED_ITEM->sw.value)
		indigo_device_disconnect(NULL, device->name);
	if (device == device->master_device)
		indigo_global_unlock(device);
	INDIGO_DEVICE_DETACH_LOG(DRIVER_NAME, device->name);
	return indigo_ccd_detach(device);
}

// -------------------------------------------------------------------------------- INDIGO guider device implementation


static indigo_result guider_attach(indigo_device *device) {
	assert(device != NULL);
	assert(PRIVATE_DATA != NULL);
	if (indigo_guider_attach(device, DRIVER_VERSION) == INDIGO_OK) {
		INDIGO_DEVICE_ATTACH_LOG(DRIVER_NAME, device->name);
		return indigo_guider_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}

static indigo_result guider_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(DEVICE_CONTEXT != NULL);
	assert(property != NULL);
	if (indigo_property_match(CONNECTION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CONNECTION
		indigo_property_copy_values(CONNECTION_PROPERTY, property, false);
		if (CONNECTION_CONNECTED_ITEM->sw.value) {
			if (PRIVATE_DATA->handle == NULL) {
				if (indigo_try_global_lock(device) != INDIGO_OK) {
					INDIGO_DRIVER_ERROR(DRIVER_NAME, "indigo_try_global_lock(): failed to get lock.");
				} else {
					PRIVATE_DATA->handle = Toupcam_Open(PRIVATE_DATA->cam.id);
				}
			}
			device->gp_bits = 1;
			if (PRIVATE_DATA->handle) {
			} else {
				CONNECTION_PROPERTY->state = INDIGO_ALERT_STATE;
				indigo_set_switch(CONNECTION_PROPERTY, CONNECTION_DISCONNECTED_ITEM, true);
				device->gp_bits = 0;
			}
		} else {
			if (PRIVATE_DATA->buffer != NULL) {
				free(PRIVATE_DATA->buffer);
				PRIVATE_DATA->buffer = NULL;
			}
			if (PRIVATE_DATA->camera && PRIVATE_DATA->camera->gp_bits == 0) {
				if (((touptek_private_data *)PRIVATE_DATA->camera->private_data)->handle == NULL) {
					Toupcam_Close(PRIVATE_DATA->handle);
					indigo_global_unlock(device);
				}
				PRIVATE_DATA->handle = NULL;
			}
			device->gp_bits = 0;
			CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
		}
	} else if (indigo_property_match(GUIDER_GUIDE_DEC_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- GUIDER_GUIDE_DEC
		HRESULT result = 0;
		indigo_property_copy_values(GUIDER_GUIDE_DEC_PROPERTY, property, false);
		if (GUIDER_GUIDE_NORTH_ITEM->number.value > 0)
			result = Toupcam_ST4PlusGuide(PRIVATE_DATA->handle, 0, GUIDER_GUIDE_NORTH_ITEM->number.value);
		else if (GUIDER_GUIDE_SOUTH_ITEM->number.value > 0)
			result = Toupcam_ST4PlusGuide(PRIVATE_DATA->handle, 1, GUIDER_GUIDE_SOUTH_ITEM->number.value);
		GUIDER_GUIDE_DEC_PROPERTY->state = SUCCEEDED(result) ? INDIGO_OK_STATE : INDIGO_ALERT_STATE;
		indigo_update_property(device, GUIDER_GUIDE_DEC_PROPERTY, NULL);
		return INDIGO_OK;
	} else if (indigo_property_match(GUIDER_GUIDE_RA_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- GUIDER_GUIDE_RA
		HRESULT result = 0;
		indigo_property_copy_values(GUIDER_GUIDE_RA_PROPERTY, property, false);
		if (GUIDER_GUIDE_EAST_ITEM->number.value > 0)
			result = Toupcam_ST4PlusGuide(PRIVATE_DATA->handle, 2, GUIDER_GUIDE_EAST_ITEM->number.value);
		else if (GUIDER_GUIDE_WEST_ITEM->number.value > 0)
			result = Toupcam_ST4PlusGuide(PRIVATE_DATA->handle, 3, GUIDER_GUIDE_WEST_ITEM->number.value);
		GUIDER_GUIDE_RA_PROPERTY->state = SUCCEEDED(result) ? INDIGO_OK_STATE : INDIGO_ALERT_STATE;
		indigo_update_property(device, GUIDER_GUIDE_RA_PROPERTY, NULL);
		return INDIGO_OK;
		// --------------------------------------------------------------------------------
	}
	return indigo_guider_change_property(device, client, property);
}

static indigo_result guider_detach(indigo_device *device) {
	assert(device != NULL);
	if (CONNECTION_CONNECTED_ITEM->sw.value)
		indigo_device_disconnect(NULL, device->name);
	if (device == device->master_device)
		indigo_global_unlock(device);
	INDIGO_DEVICE_DETACH_LOG(DRIVER_NAME, device->name);
	return indigo_guider_detach(device);
}

// -------------------------------------------------------------------------------- hot-plug support

static bool hotplug_callback_initialized = false;
static indigo_device *devices[TOUPCAM_MAX];

static void hotplug_callback(void* pCallbackCtx) {
	for (int i = 0; i < TOUPCAM_MAX; i++) {
		indigo_device *device = devices[i];
		if (device)
			PRIVATE_DATA->present = false;
	}
	ToupcamInstV2 cams[TOUPCAM_MAX];
	int cnt = Toupcam_EnumV2(cams);
	for (int j = 0; j < cnt; j++) {
		ToupcamInstV2 cam = cams[j];
		bool found = false;
		for (int i = 0; i < TOUPCAM_MAX; i++) {
			indigo_device *device = devices[i];
			if (device && memcmp(PRIVATE_DATA->cam.id, cam.id, sizeof(64)) == 0) {
				found = true;
				PRIVATE_DATA->present = true;
				break;
			}
		}
		if (!found) {
			static indigo_device ccd_template = INDIGO_DEVICE_INITIALIZER(
				"",
				ccd_attach,
				indigo_ccd_enumerate_properties,
				ccd_change_property,
				NULL,
				ccd_detach
				);
			touptek_private_data *private_data = malloc(sizeof(touptek_private_data));
			assert(private_data != NULL);
			memset(private_data, 0, sizeof(touptek_private_data));
			private_data->cam = cam;
			private_data->present = true;
			indigo_device *camera = malloc(sizeof(indigo_device));
			assert(camera != NULL);
			memcpy(camera, &ccd_template, sizeof(indigo_device));
			snprintf(camera->name, INDIGO_NAME_SIZE, "ToupTek %s #%s", cam.displayname, cam.id);
			camera->private_data = private_data;
			private_data->camera = camera;
			for (int i = 0; i < TOUPCAM_MAX; i++) {
				if (devices[j] == NULL) {
					indigo_async((void *)(void *)indigo_attach_device, devices[j] = camera);
					break;
				}
			}
			if (cam.model->flag & TOUPCAM_FLAG_ST4) {
				static indigo_device guider_template = INDIGO_DEVICE_INITIALIZER(
					"",
					guider_attach,
					indigo_guider_enumerate_properties,
					guider_change_property,
					NULL,
					guider_detach
					);
				indigo_device *guider = malloc(sizeof(indigo_device));
				assert(guider != NULL);
				memcpy(guider, &guider_template, sizeof(indigo_device));
				snprintf(guider->name, INDIGO_NAME_SIZE, "ToupTek %s (guider) #%s", cam.displayname, cam.id);
				guider->private_data = private_data;
				private_data->guider = guider;
				indigo_async((void *)(void *)indigo_attach_device, guider);
			}
		}
	}
	for (int i = 0; i < TOUPCAM_MAX; i++) {
		indigo_device *device = devices[i];
		if (device && !PRIVATE_DATA->present) {
			indigo_device *guider = PRIVATE_DATA->guider;
			if (guider) {
				indigo_detach_device(guider);
				free(guider);
			}
			indigo_detach_device(device);
			if (device->private_data)
				free(device->private_data);
			free(device);
			devices[i] = NULL;
		}
	}
}


indigo_result indigo_ccd_touptek(indigo_driver_action action, indigo_driver_info *info) {
	static indigo_driver_action last_action = INDIGO_DRIVER_SHUTDOWN;

	SET_DRIVER_INFO(info, "ToupTek Camera", __FUNCTION__, DRIVER_VERSION, last_action);

	if (action == last_action)
		return INDIGO_OK;

	switch(action) {
		case INDIGO_DRIVER_INIT: {
			last_action = action;
			for (int i = 0; i < TOUPCAM_MAX; i++)
				devices[i] = NULL;
			if (!hotplug_callback_initialized) {
				Toupcam_HotPlug(hotplug_callback, NULL);
				hotplug_callback_initialized = true;
			}
			INDIGO_DRIVER_LOG(DRIVER_NAME, "ToupTek SDK version %s", Toupcam_Version());
			hotplug_callback(NULL);
			break;
		}
		case INDIGO_DRIVER_SHUTDOWN:
			break;

		case INDIGO_DRIVER_INFO:
			break;
	}

	return INDIGO_OK;
}
