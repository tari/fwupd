/*
 * Copyright (C) 2021 Peter Marheine <pmarheine@chromium.org>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <libflashrom.h>
#include <linux/i2c-dev.h>

#include "fu-hwids.h"
#include "fu-realtek-mst-device.h"

enum dual_bank_mode {
  DUAL_BANK_USER_ONLY = 0,
  DUAL_BANK_DIFF = 1,
  DUAL_BANK_COPY = 2,
  DUAL_BANK_USER_ONLY_FLAG = 3,
  DUAL_BANK_MAX_VALUE = 3,
};

enum flash_bank {
  FLASH_BANK_BOOT = 0,
  FLASH_BANK_USER1 = 1,
  FLASH_BANK_USER2 = 2,
  FLASH_BANK_MAX_VALUE = 2,
  FLASH_BANK_INVALID = 255,
};

struct dual_bank_info {
  gboolean is_enabled;
  enum dual_bank_mode mode;
  enum flash_bank active_bank;
  guint8 user1_version[2];
  guint8 user2_version[2];
};

struct _FuRealtekMstDevice {
  FuUdevDevice parent_instance;
  gchar *dp_aux_dev_name;
  FuUdevDevice *bus_device;
  struct flashrom_programmer *programmer;
  enum flash_bank active_bank;
};

G_DEFINE_TYPE (FuRealtekMstDevice, fu_realtek_mst_device,
	       FU_TYPE_UDEV_DEVICE)

#define USER_IMAGE_SIZE 0x70000
#define USER1_BASE_ADDRESS 0x10000
#define USER2_BASE_ADDRESS 0x80000

struct fmap_area {
	guint32 offset;
	guint32 size;
	guint8 name[32];
	guint16 flags;
} __attribute__((packed));

const struct {
	guint8 signature[8];
	guint8 ver_major;
	guint8 ver_minor;
	guint64 base;
	guint32 size;
	guint8 name[32];
	guint16 nareas;
	struct fmap_area areas[];
} __attribute__((packed)) FMAP = {
	.signature = "__FMAP__",
	.ver_major = 1,
	.ver_minor = 1,
	.base = 0x000000,
	.size = 0x100000,
	.name = "RTD2142",
	.nareas = 4,
	.areas = {
		{USER1_BASE_ADDRESS, USER_IMAGE_SIZE, "USER1"},
		{USER2_BASE_ADDRESS, USER_IMAGE_SIZE, "USER2"},
		{0xfe304, 5, "FLAG1"},
		{0xff304, 5, "FLAG2"},
	}
};

typedef struct flashrom_layout FlashLayout;

static gboolean
flash_layout_create (struct flashrom_flashctx *context, FlashLayout **out,
		     GError **error)
{
	int rc = flashrom_layout_read_fmap_from_buffer (out,
							context,
							(guint8 *) &FMAP,
							sizeof (FMAP));
	if (rc != 0) {
		g_set_error (error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL,
			"failed to parse flashmap: code %d", rc);
		return FALSE;
	}
	return TRUE;
}

static void
flash_layout_dispose (FlashLayout *layout)
{
	flashrom_layout_release (layout);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlashLayout, flash_layout_dispose);

static gboolean fu_realtek_mst_device_set_quirk_kv (FuDevice *device,
						    const gchar *key,
						    const gchar *value,
						    GError **error)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (device);

	if (g_strcmp0 (key, "RealtekMstDpAuxName") == 0) {
		self->dp_aux_dev_name = g_strdup (value);
	} else {
		g_set_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED,
			     "unsupported quirk key: %s", key);
		return FALSE;
	}
	return TRUE;
}

static FuUdevDevice *fu_realtek_mst_device_locate_bus (FuRealtekMstDevice *self,
						       GError **error)
{
	g_autoptr(GUdevClient) udev_client = g_udev_client_new (NULL);
	g_autoptr(GUdevEnumerator)
		udev_enumerator = g_udev_enumerator_new (udev_client);
	g_autoptr(GList) matches = NULL;
	g_autoptr(FuUdevDevice) bus_device = NULL;

	g_udev_enumerator_add_match_subsystem (udev_enumerator,
					       "drm_dp_aux_dev");
	g_udev_enumerator_add_match_sysfs_attr (udev_enumerator,
						"name",
						self->dp_aux_dev_name);
	matches = g_udev_enumerator_execute (udev_enumerator);

	/* from a drm_dp_aux_dev with the given name, locate its sibling i2c
	 * device and in turn the i2c-dev under that representing the actual
	 * I2C bus that runs over DPDDC on the port represented by the
	 * drm_dp_aux_dev */
	for (GList *element = matches; element != NULL; element = element->next) {
		g_autoptr(FuUdevDevice)
			device = fu_udev_device_new (element->data);
		g_autoptr(GPtrArray) i2c_devices = NULL;

		if (bus_device != NULL) {
			g_debug ("Ignoring additional aux device %s",
				 fu_udev_device_get_sysfs_path (device));
			continue;
		}

		i2c_devices = fu_udev_device_get_siblings_with_subsystem (device, "i2c");
		for (guint i = 0; i < i2c_devices->len; i++) {
			FuUdevDevice *i2c_device = g_ptr_array_index (i2c_devices, i);
			g_autoptr(GPtrArray) i2c_buses =
				fu_udev_device_get_children_with_subsystem (i2c_device, "i2c-dev");

			if (i2c_buses->len == 0) {
				g_debug ("no i2c-dev found under %s",
					 fu_udev_device_get_sysfs_path (i2c_device));
				continue;
			}
			if (i2c_buses->len > 1) {
				g_debug ("ignoring %u additional i2c-dev under %s",
					 i2c_buses->len - 1,
					 fu_udev_device_get_sysfs_path (i2c_device));
			}

			bus_device = g_ptr_array_steal_index_fast (i2c_buses, 0);
			g_debug ("Found I2C bus at %s",
				 fu_udev_device_get_sysfs_path (bus_device));
			break;
		}
	}

	if (bus_device == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "did not find an i2c-dev associated with DP aux \"%s\"",
			     self->dp_aux_dev_name);
		return NULL;
	}
	return g_steal_pointer (&bus_device);
}

static gboolean
fu_realtek_mst_device_probe (FuDevice *device, GError **error)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (device);
	FuContext *context = fu_device_get_context (device);
	const gchar *quirk_name = NULL;
	const gchar *hardware_family = NULL;
	g_autofree gchar *physical_id = NULL;
	g_autofree gchar *instance_id = NULL;
	g_autofree gchar *family_instance_id = NULL;

	if (!FU_DEVICE_CLASS (fu_realtek_mst_device_parent_class)->probe (device, error))
		return FALSE;

	physical_id = g_strdup_printf ("I2C_PATH=%s",
				       fu_udev_device_get_sysfs_path (
					       FU_UDEV_DEVICE (device)));
	fu_device_set_physical_id (device, physical_id);

	/* set custom instance ID and load matching quirks */
	instance_id = g_strdup_printf ("REALTEK-MST\\Name_%s",
				       fu_udev_device_get_sysfs_attr (
					       FU_UDEV_DEVICE (device),
					       "name",
					       NULL));
	fu_device_add_instance_id (device, instance_id);

	hardware_family = fu_context_get_hwid_value (context, FU_HWIDS_KEY_FAMILY);
	family_instance_id = g_strdup_printf ("%s&Family_%s", instance_id, hardware_family);
	fu_device_add_instance_id_full (device, family_instance_id,
					FU_DEVICE_INSTANCE_FLAG_ONLY_QUIRKS);

	/* having loaded quirks, check this device is supported */
	quirk_name = fu_device_get_name (device);
	if (g_strcmp0 (quirk_name, "RTD2142") != 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "only RTD2142 is supported");
		return FALSE;
	}

	if (self->dp_aux_dev_name == NULL) {
		g_set_error_literal (error, FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "RealtekMstDpAuxName must be specified");
		return FALSE;
	}

	if ((self->bus_device = fu_realtek_mst_device_locate_bus (self, error)) == NULL)
		return FALSE;

	return TRUE;
}

static gboolean
fu_realtek_mst_device_open (FuDevice *device, GError **error)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (device);
	const gchar *bus_path = fu_udev_device_get_device_file (self->bus_device);
	gint bus_fd;

	/* open the bus and not self */
	if ((bus_fd = g_open (bus_path, O_RDWR)) == -1) {
		g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
			"failed to open %s", bus_path);
		return FALSE;
	}
	fu_udev_device_set_fd (FU_UDEV_DEVICE (self), bus_fd);
	fu_udev_device_set_flags (FU_UDEV_DEVICE (device),
				  FU_UDEV_DEVICE_FLAG_NONE);
	g_debug ("bus opened");

	/* set target address to device address */
	if (!fu_udev_device_ioctl (FU_UDEV_DEVICE (self), I2C_SLAVE,
				   (guint8 *) 0x35, NULL, error))
		return FALSE;

	return FU_DEVICE_CLASS (fu_realtek_mst_device_parent_class)->open (device, error);
}

static gboolean
fu_realtek_mst_device_get_dual_bank_info (FuRealtekMstDevice *self,
					  struct dual_bank_info *info,
					  GError **error)
{
	FuUdevDevice *device = FU_UDEV_DEVICE (self);
	const guint8 enter_ddcci_mode[] = {0xca, 0x09};
	guint8 response[11];

	/* switch to DDCCI mode */
	if (!fu_udev_device_pwrite_full (device,
					 0, enter_ddcci_mode,
					 sizeof (enter_ddcci_mode),
					 error))
		return FALSE;

	/* wait for mode switch to complete */
	g_usleep (200 * G_TIME_SPAN_MILLISECOND);

	/* request dual bank state and read back */
	if (!fu_udev_device_pwrite (device, 0, 0x01, error))
		return FALSE;
	if (!fu_udev_device_pread_full (device, 0, response, sizeof (response), error))
		return FALSE;

	if (response[0] != 0xca || response[1] != 9) {
		/* unexpected response code or length usually means the current
		 * firmware doesn't support dual-bank mode at all */
		g_debug ("unexpected response code %#x, length %d",
			 response[0], response[1]);
		info->is_enabled = FALSE;
		return TRUE;
	}

	/* enable flag, assume anything other than 1 is unsupported */
	if (response[2] != 1) {
		info->is_enabled = FALSE;
		return TRUE;
	}
	info->is_enabled = TRUE;

	info->mode = response[3];
	if (info->mode > DUAL_BANK_MAX_VALUE) {
		g_debug ("unexpected dual bank mode value %#x", info->mode);
		info->is_enabled = FALSE;
		return TRUE;
	}

	info->active_bank = response[4];
	if (info->active_bank > FLASH_BANK_MAX_VALUE) {
		g_debug ("unexpected active flash bank value %#x",
			 info->active_bank);
		info->is_enabled = FALSE;
		return TRUE;
	}

	info->user1_version[0] = response[5];
	info->user1_version[1] = response[6];
	info->user2_version[0] = response[7];
	info->user2_version[1] = response[8];
	/* last two bytes of response are reserved */
	return TRUE;
}

static gboolean
fu_realtek_mst_device_probe_version (FuDevice *device, GError **error)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (device);
	struct dual_bank_info info;
	guint8 (*active_version)[2];
	g_autofree gchar *version_str = NULL;

	/* ensure probed state is cleared in case of error */
	fu_device_remove_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	self->active_bank = FLASH_BANK_INVALID;
	fu_device_set_version (device, NULL);

	if (!fu_realtek_mst_device_get_dual_bank_info (FU_REALTEK_MST_DEVICE (self),
						       &info, error))
		return FALSE;

	if (!info.is_enabled) {
		g_debug ("dual-bank mode is not enabled");
		return TRUE;
	}
	if (info.mode != DUAL_BANK_DIFF) {
		g_debug ("can only update from dual-bank-diff mode");
		return TRUE;
	}
	/* dual-bank mode seems to be fully supported, so we can update
	 * regardless of the active bank- if it's FLASH_BANK_BOOT, updating is
	 * possible even if the current version is unknown */
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);

	g_debug ("device is currently running from bank %u", info.active_bank);
	g_return_val_if_fail (info.active_bank <= FLASH_BANK_MAX_VALUE, FALSE);
	self->active_bank = info.active_bank;

	if (info.active_bank == FLASH_BANK_USER1)
		active_version = &info.user1_version;
	else if (info.active_bank == FLASH_BANK_USER2)
		active_version = &info.user2_version;
	else
		/* only user bank versions are reported, can't tell otherwise */
		return TRUE;

	version_str = g_strdup_printf ("%u.%u",
				       *active_version[0], *active_version[1]);
	fu_device_set_version (FU_DEVICE (self), version_str);

	return TRUE;
}

static gboolean
extract_i2c_bus_number (const gchar *bus_path,
			guint8 *bus_number,
			GError **error)
{
	g_autoptr(GRegex) regex = NULL;
	g_autoptr(GMatchInfo) match = NULL;
	gint start_index;
	guint64 parsed_number;

	if ((regex = g_regex_new ("i2c-(\\d+)$", 0, 0, error)) == NULL)
		return FALSE;
	if (!g_regex_match (regex, bus_path, 0, &match)) {
		g_set_error (error, FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to find bus number of i2c bus %s",
			     bus_path);
		return FALSE;
	}
	if (!g_match_info_fetch_pos (match, 1, &start_index, NULL))
		return FALSE;
	if (!g_ascii_string_to_unsigned (&bus_path[start_index],
					 10, 0, 255, &parsed_number, error))
		return FALSE;

	*bus_number = (guint8) parsed_number;
	return TRUE;
}

static gboolean
fu_realtek_mst_device_detach (FuDevice *device, GError **error)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (device);
	g_autofree gchar *programmer_args = NULL;
	guint8 bus_number;
	gint rc;

	if (!extract_i2c_bus_number (fu_udev_device_get_device_file (self->bus_device),
				     &bus_number, error))
		return FALSE;

	/* switch device to programming mode and reset on programmer
	 * shutdown, which will exit programming mode and reload firmware
	 * from flash */
	programmer_args = g_strdup_printf ("bus=%u,enter-isp=1,reset-mcu=1",
					   bus_number);
	if ((rc = flashrom_programmer_init (&self->programmer,
					    "realtek_mst_i2c_spi",
					    programmer_args))) {
		g_set_error (error, FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to initialize programmer: %d", rc);
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_realtek_mst_device_write_firmware (FuDevice *device,
				      FuFirmware *firmware,
				      FwupdInstallFlags flags,
				      GError **error)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (device);
	int rc;
	struct flashrom_flashctx *flashctx;
	g_autoptr(FlashLayout) layout = NULL;
	size_t flash_size;
	enum flash_bank target_bank;
	g_autoptr(GBytes) firmware_bytes = NULL;
	const guint8 (*firmware_data)[USER_IMAGE_SIZE] = NULL;
	g_autofree void *program_buffer = NULL;

	if ((firmware_bytes = fu_firmware_get_bytes (firmware, error)) == NULL)
		return FALSE;
	g_return_val_if_fail (g_bytes_get_size (firmware_bytes) != USER_IMAGE_SIZE,
			      FALSE);
	firmware_data = g_bytes_get_data (firmware_bytes, NULL);

	rc = flashrom_flash_probe (&flashctx, self->programmer, NULL);
	if (rc != 0) {
		g_set_error (error, FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to find exactly one flash chip: %d", rc);
		return FALSE;
	}
	flash_size = flashrom_flash_getsize (flashctx);
	program_buffer = g_malloc0 (flash_size);

	if (!fu_memcpy_safe (program_buffer,
			     flash_size,
			     USER1_BASE_ADDRESS,
			     *firmware_data,
			     USER_IMAGE_SIZE,
			     0,
			     USER_IMAGE_SIZE,
			     error))
		return FALSE;

	/* first write only the bank that's not active (or USER1 if boot is
	 * currently active) */
	if (!flash_layout_create (flashctx, &layout, error))
		return FALSE;
	flashrom_layout_include_region (layout,
					self->active_bank == FLASH_BANK_USER1
					? "USER2" : "USER1");
	// TODO handle errors
	flashrom_image_write (flashctx, program_buffer, flash_size, NULL);

	// Get start bank (what's that mean?)
	// Get flag address
	// Not sure if we care about those, since we have fixed layout. Could
	// try to validate those against the static layout?
	// Write new bank (???)
	// Write flag for new bank

	flashrom_layout_set (flashctx, flashlayout);

}

static gboolean
fu_realtek_mst_device_attach (FuDevice *device, GError **error)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (device);
	gint rc;

	/* reset device by closing flashrom programmer */
	if ((rc = flashrom_programmer_shutdown (self->programmer))) {
		g_set_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to shut down programmer: %d", rc);
		return FALSE;
	}
	self->programmer = NULL;

	// TODO should wait a second for reset on re-attach, then poll
	// for readiness ("New Differ Bank After ISP")
	return TRUE;
}

static void
fu_realtek_mst_device_init (FuRealtekMstDevice *self)
{
	FuDevice *device = FU_DEVICE (self);
	self->dp_aux_dev_name = NULL;
	self->bus_device = NULL;
	self->programmer = NULL;
	self->active_bank = FLASH_BANK_INVALID;

	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_DUAL_IMAGE);
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_PAIR);

	fu_device_add_protocol (device, "com.realtek.rtd2142");
	fu_device_set_vendor (device, "Realtek");
	fu_device_set_summary (device, "DisplayPort MST hub");
	fu_device_add_icon (device, "video-display");
	fu_device_set_firmware_size (device, USER_IMAGE_SIZE);
}

static void
fu_realtek_mst_device_finalize (GObject *object)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (object);
	g_free (self->dp_aux_dev_name);
	if (self->bus_device != NULL)
		g_object_unref (self->bus_device);
	if (self->programmer != NULL) {
		/* should be cleared on device attach, but if not */
		g_warning ("MST programmer was not shutdown before finalization");
		flashrom_programmer_shutdown (self->programmer);
		self->programmer = NULL;
	}

	G_OBJECT_CLASS (fu_realtek_mst_device_parent_class)->finalize (object);
}

static void
fu_realtek_mst_device_class_init (FuRealtekMstDeviceClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	GObjectClass *klass_object = G_OBJECT_CLASS(klass);

	klass_object->finalize = fu_realtek_mst_device_finalize;
	klass_device->probe = fu_realtek_mst_device_probe;
	klass_device->set_quirk_kv = fu_realtek_mst_device_set_quirk_kv;
	klass_device->open = fu_realtek_mst_device_open;
	klass_device->setup = fu_realtek_mst_device_probe_version;
	klass_device->detach = fu_realtek_mst_device_detach;
	klass_device->attach = fu_realtek_mst_device_attach;
	klass_device->write_firmware = fu_realtek_mst_device_write_firmware;
	klass_device->reload = fu_realtek_mst_device_probe_version;
	/*
	klass_device->write_firmware = fu_flashrom_lspcon_i2c_spi_device_write_firmware;
	 */
}
