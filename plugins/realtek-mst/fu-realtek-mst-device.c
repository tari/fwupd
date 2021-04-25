/*
 * Copyright (C) 2021 Peter Marheine <pmarheine@chromium.org>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <linux/i2c-dev.h>

#include "fu-hwids.h"
#include "fu-realtek-mst-device.h"

/* some kind of operation attribute bits */
#define REG_CMD_ATTR 0x60
/* write set to begin executing, cleared when done */
#define CMD_ERASE_BUSY 0x01

#define REG_ERASE_OPCODE 0x61
#define CMD_OPCODE_ERASE_SECTOR 0x20
#define CMD_OPCODE_ERASE_BLOCK 0xD8

/* 24-bit address for commands */
#define REG_CMD_ADDR_HI 0x64
#define REG_CMD_ADDR_MID 0x65
#define REG_CMD_ADDR_LO 0x66

#define REG_READ_OPCODE 0x6A
#define CMD_OPCODE_READ 0x03

#define REG_WRITE_OPCODE 0x6D
#define CMD_OPCODE_WRITE 0x02

/* mode register address */
#define REG_MCU_MODE 0x6F
/* when bit is set in mode register, ISP mode is active */
#define MCU_MODE_ISP (1 << 7)
/* write set to begin write, reset by device when complete */
#define MCU_MODE_WRITE_BUSY (1 << 5)
/* when bit is set, write buffer contains data */
#define MCU_MODE_WRITE_BUF (1 << 4)

/* write data into write buffer */
#define REG_WRITE_FIFO 0x70

/* number of bytes to write minus 1 (0xff means 256 bytes) */
#define REG_WRITE_LEN 0x71


/* Indirect registers allow access to registers with 16-bit addresses. Write
 * 0x9F to the LO register, then the top byte of the address to HI, the
 * bottom byte of the address to LO, then read or write HI to read or write
 * the value of the target register. */
#define REG_INDIRECT_LO 0xF4
#define REG_INDIRECT_HI 0xF5

#define REG_GPIO88_CONFIG 0x104F
#define REG_GPIO88_VALUE 0xFE3F

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

#define FLASH_SIZE 0x100000
#define FLASH_USER1_ADDR 0x10000
#define FLASH_FLAG1_ADDR 0xfe304
#define FLASH_USER2_ADDR 0x80000
#define FLASH_FLAG2_ADDR 0xff304
#define FLASH_USER_SIZE 0x70000

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
	enum flash_bank active_bank;
};

// TODO implement in terms of FuI2cDevice?
G_DEFINE_TYPE (FuRealtekMstDevice, fu_realtek_mst_device, FU_TYPE_UDEV_DEVICE)

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

/** Write a value to a device register */
static gboolean
mst_write_register (FuRealtekMstDevice *self, guint8 address, guint8 value, GError **error)
{
	const guint8 command[] = {address, value};

	return fu_udev_device_pwrite_full (FU_UDEV_DEVICE (self), 0, command,
					   sizeof (command), error);
}

static gboolean
mst_write_register_multi (FuRealtekMstDevice *self, guint8 address,
			  const guint8 *data, gsize count, GError **error)
{
	g_autofree guint8 *command = g_malloc (count + 1);
	memcpy (command + 1, data, count);
	command[0] = address;
	return fu_udev_device_pwrite_full (FU_UDEV_DEVICE (self), 0,
					   command, count + 1, error);

}

/** Read a register from the device */
static gboolean
mst_read_register (FuRealtekMstDevice *self,
		   guint8 address,
		   guint8 *value,
		   GError **error)
{
	FuUdevDevice *udev_device = FU_UDEV_DEVICE (self);

	if (!fu_udev_device_pwrite (udev_device, 0, address, error))
		return FALSE;
	return fu_udev_device_pread (udev_device, 0, value, error);
}

static gboolean
mst_set_indirect_address (FuRealtekMstDevice *self, guint16 address, GError **error)
{
	if (!mst_write_register (self, REG_INDIRECT_LO, 0x9F, error))
		return FALSE;
	if (!mst_write_register (self, REG_INDIRECT_HI, address >> 8, error))
		return FALSE;
	return mst_write_register (self, REG_INDIRECT_LO, address, error);
}

static gboolean
mst_read_register_indirect (FuRealtekMstDevice *self, guint16 address, guint8 *value, GError **error)
{
	if (!mst_set_indirect_address (self, address, error))
		return FALSE;
	return mst_read_register (self, REG_INDIRECT_HI, value, error);
}

static gboolean
mst_write_register_indirect (FuRealtekMstDevice *self, guint16 address, guint8 value, GError **error)
{
	if (!mst_set_indirect_address (self, address, error))
		return FALSE;
	return mst_write_register (self, REG_INDIRECT_HI, value, error);
}

/**
 * Wait until a device register reads an expected value.
 *
 * Waiting up to @timeout_seconds, poll the given @address for the read value
 * bitwise-ANDed with @mask to be equal to @expected.
 *
 * Returns an error if the timeout expires or in case of an I/O error.
 */
static gboolean
mst_poll_register (FuRealtekMstDevice *self,
		   guint8 address,
		   guint8 mask,
		   guint8 expected,
		   guint timeout_seconds,
		   GError **error)
{
	guint8 value;
	struct timespec deadline;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	deadline = (struct timespec){
		.tv_sec = now.tv_sec + timeout_seconds,
		.tv_nsec = now.tv_nsec,
	};

	if (!mst_read_register (self, address, &value, error))
		return FALSE;
	while ((value & mask) != expected
		&& (now.tv_sec < deadline.tv_sec ||
			(now.tv_sec == deadline.tv_sec
				&& now.tv_nsec <= deadline.tv_nsec))) {
		g_usleep(G_TIME_SPAN_MILLISECOND);
		clock_gettime(CLOCK_MONOTONIC, &now);

		if (!mst_read_register (self, address, &value, error))
			return FALSE;
	}
	if ((value & mask) == expected)
		return TRUE;

	g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
		     "register %x still reads %x after %ds, wanted %x (mask %x)",
		     address, value, timeout_seconds, expected, mask);
	return FALSE;
}

static gboolean
mst_set_gpio88 (FuRealtekMstDevice *self, gboolean level, GError **error)
{
	guint8 value;

	/* ensure pin is configured as push-pull GPIO */
	if (!mst_read_register_indirect (self, REG_GPIO88_CONFIG, &value, error))
		return FALSE;
	if (!mst_write_register_indirect (self,
					  REG_GPIO88_CONFIG,
					  (value & 0xF0) | 1,
					  error))
		return FALSE;

	/* set output level */
	g_debug ("set pin 88 = %d", level);
	if (!mst_read_register_indirect (self, REG_GPIO88_VALUE, &value, error))
		return FALSE;
	return mst_write_register_indirect (self, REG_GPIO88_VALUE,
					    (value & 0xFE) | (level != FALSE),
					    error);
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
	guint8 response[11];

	/* switch to DDCCI mode */
	if (!mst_write_register (self, 0xca, 0x09, error))
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
	guint8 *active_version;
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

	g_debug ("firmware version reports user1 %d.%d, user2 %d.%d",
		 info.user1_version[0], info.user1_version[1],
		 info.user2_version[0], info.user2_version[1]);
	if (info.active_bank == FLASH_BANK_USER1)
		active_version = info.user1_version;
	else if (info.active_bank == FLASH_BANK_USER2)
		active_version = info.user2_version;
	else
		/* only user bank versions are reported, can't tell otherwise */
		return TRUE;

	version_str = g_strdup_printf ("%u.%u",
				       active_version[0], active_version[1]);
	fu_device_set_version (FU_DEVICE (self), version_str);

	return TRUE;
}

static gboolean
flash_iface_read (FuRealtekMstDevice *self,
		  guint32 address, guint8 *buf, const gsize buf_size,
		  GError **error)
{
	gsize bytes_read = 0;
	guint8 byte;
	g_return_val_if_fail(address < FLASH_SIZE, FALSE);
	g_return_val_if_fail(buf_size <= FLASH_SIZE, FALSE);
	g_debug ("read %#" G_GSIZE_MODIFIER "x bytes from %#08x", buf_size, address);

	/* read must start one byte prior to the desired address and ignore the
	 * first byte of data, since the first read value is unpredictable */
	address = (address - 1) & 0xFFFFFF;
	if (!mst_write_register (self, REG_CMD_ADDR_HI, address >> 16, error))
		return FALSE;
	if (!mst_write_register (self, REG_CMD_ADDR_MID, address >> 8, error))
		return FALSE;
	if (!mst_write_register (self, REG_CMD_ADDR_LO, address, error))
		return FALSE;
	if (!mst_write_register (self, REG_READ_OPCODE, CMD_OPCODE_READ, error))
		return FALSE;

	/* ignore first byte of data */
	if (!fu_udev_device_pwrite (FU_UDEV_DEVICE (self), 0, 0x70, error))
		return FALSE;
	if (!fu_udev_device_pread (FU_UDEV_DEVICE (self), 0, &byte, error))
		return FALSE;

	while (bytes_read < buf_size) {
		/* read up to 256 bytes in one transaction */
		gsize read_len = buf_size - bytes_read;
		if (read_len > 256)
			read_len = 256;

		if (!fu_udev_device_pread_full (FU_UDEV_DEVICE (self), 0,
						buf + bytes_read, read_len,
						error))
			return FALSE;

		bytes_read += read_len;
		fu_device_set_progress_full (FU_DEVICE (self), bytes_read, buf_size);
	}
	return TRUE;
}

static const guint32 SECTOR_SIZE = 4096;
static const guint32 BLOCK_SIZE = 65536;

static gboolean
flash_iface_erase_sector (FuRealtekMstDevice *self, guint32 address,
			  GError **error)
{
	/* address must be 4k-aligned */
	g_return_val_if_fail((address & 0xFFF) == 0, FALSE);
	g_debug ("sector erase %#08x-%#08x", address, address + SECTOR_SIZE);

	/* sector address */
	if (!mst_write_register (self, REG_CMD_ADDR_HI, address >> 16, error))
		return FALSE;
	if (!mst_write_register (self, REG_CMD_ADDR_MID, address >> 8, error))
		return FALSE;
	if (!mst_write_register (self, REG_CMD_ADDR_LO, address, error))
		return FALSE;
	/* command type + WREN */
	if (!mst_write_register (self, REG_CMD_ATTR, 0xB8, error))
		return FALSE;
	/* sector erase opcode */
	if (!mst_write_register (self, REG_ERASE_OPCODE, CMD_OPCODE_ERASE_SECTOR, error))
		return FALSE;
	/* begin operation and wait for completion */
	if (!mst_write_register (self, REG_CMD_ATTR, 0xB8 | CMD_ERASE_BUSY, error))
		return FALSE;
	return mst_poll_register (self, REG_CMD_ATTR, CMD_ERASE_BUSY, 0, 10, error);
}
static gboolean
flash_iface_erase_block (FuRealtekMstDevice *self, guint32 address, GError **error)
{
	/* address must be 64k-aligned */
	g_return_val_if_fail((address & 0xFFFF) == 0, FALSE);
	g_debug ("block erase %#08x-%#08x", address, address + BLOCK_SIZE);

	/* block address */
	if (!mst_write_register (self, REG_CMD_ADDR_HI, address >> 16, error))
		return FALSE;
	if (!mst_write_register (self, REG_CMD_ADDR_MID, 0, error))
		return FALSE;
	if (!mst_write_register (self, REG_CMD_ADDR_LO, 0, error))
		return FALSE;
	/* command type + WREN */
	if (!mst_write_register (self, REG_CMD_ATTR, 0xB8, error))
		return FALSE;
	/* block erase opcode */
	if (!mst_write_register (self, REG_ERASE_OPCODE, CMD_OPCODE_ERASE_BLOCK, error))
		return FALSE;
	/* begin operation and wait for completion */
	if (!mst_write_register (self, REG_CMD_ATTR, 0xB8 | CMD_ERASE_BUSY, error))
		return FALSE;
	return mst_poll_register (self, REG_CMD_ATTR, CMD_ERASE_BUSY, 0, 10, error);
}

static gboolean
flash_iface_write (FuRealtekMstDevice *self, guint32 address,
		   GBytes *data, GError **error)
{
	gsize total_size;
	gsize remaining_size;
	const guint8 *data_bytes = g_bytes_get_data (data, &total_size);
	remaining_size = total_size;

	g_debug ("write %#" G_GSIZE_MODIFIER "x bytes at %#08x", remaining_size, address);
	while (remaining_size > 0) {
		gsize chunk_size = remaining_size > 256 ? 256 : remaining_size;
		/* write opcode */
		if (!mst_write_register (self, REG_WRITE_OPCODE, 0x02, error))
			return FALSE;
		/* write length */
		if (!mst_write_register (self, REG_WRITE_LEN, chunk_size - 1, error))
			return FALSE;
		/* target address */
		if (!mst_write_register (self, REG_CMD_ADDR_HI, address >> 16, error))
			return FALSE;
		if (!mst_write_register (self, REG_CMD_ADDR_MID, address >> 8, error))
			return FALSE;
		if (!mst_write_register (self, REG_CMD_ADDR_LO, address, error))
			return FALSE;
		/* ensure write buffer is empty */
		if (!mst_poll_register (self, REG_MCU_MODE, MCU_MODE_WRITE_BUF, 0, 10, error))
			return FALSE;
		/* write data into FIFO */
		if (!mst_write_register_multi (self, REG_WRITE_FIFO, data_bytes, chunk_size, error))
			return FALSE;
		/* begin operation and wait for completion */
		if (!mst_write_register (self, REG_MCU_MODE, MCU_MODE_ISP | MCU_MODE_WRITE_BUSY, error))
			return FALSE;
		if (!mst_poll_register (self, REG_MCU_MODE, MCU_MODE_WRITE_BUSY, 0, 10, error)) {
			g_prefix_error (error,
					"timed out waiting for write at %#x to complete: ",
					address);
			return FALSE;
		}

		remaining_size -= chunk_size;
		data_bytes += chunk_size;
		address += chunk_size;
		fu_device_set_progress_full (FU_DEVICE (self), total_size - remaining_size, total_size);
	}

	return TRUE;
}

static gboolean
fu_realtek_mst_device_detach (FuDevice *device, GError **error)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (device);

	/* Switch to programming mode (stops regular operation) */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_RESTART);
	if (!mst_write_register (self, REG_MCU_MODE, MCU_MODE_ISP, error))
		return FALSE;
	g_debug ("wait for ISP mode ready");
	if (!mst_poll_register (self, REG_MCU_MODE, MCU_MODE_ISP, MCU_MODE_ISP, 60, error))
		return FALSE;

	/* magic value makes the MCU clock run faster than normal; this both
	 * helps programming performance and fixes flakiness where register
	 * writes sometimes get nacked for no apparent reason */
	if (!mst_write_register_indirect (self, 0x06A0, 0x74, error))
		return FALSE;

	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_IS_BOOTLOADER);
	fu_device_set_status (device, FWUPD_STATUS_IDLE);

	/* Disable hardware write protect, assuming Flash ~WP is connected to
	 * device pin 88, a GPIO. */
	return mst_set_gpio88 (self, 1, error);
}

static gboolean
fu_realtek_mst_device_write_firmware (FuDevice *device,
				      FuFirmware *firmware,
				      FwupdInstallFlags flags,
				      GError **error)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (device);
	/* write an inactive bank: USER2 if USER1 is active, otherwise USER1
	 * (including if the boot bank is active) */
	guint32 base_addr = self->active_bank == FLASH_BANK_USER1 ? FLASH_USER2_ADDR : FLASH_USER1_ADDR;
	guint32 flag_addr = self->active_bank == FLASH_BANK_USER1 ? FLASH_FLAG2_ADDR : FLASH_FLAG1_ADDR;
	GBytes *firmware_bytes = fu_firmware_get_bytes (firmware, error);
	g_autofree guint8 *readback_buf = NULL;
	const guint8 flag_data[] = {0xaa, 0xaa, 0xaa, 0xff, 0xff};
	g_return_val_if_fail(g_bytes_get_size (firmware_bytes) == FLASH_USER_SIZE, FALSE);

	/* erase old image */
	g_debug ("erase old image from %#x", base_addr);
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_ERASE);
	for (guint32 offset = 0; offset < FLASH_USER_SIZE; offset += FLASH_USER_SIZE) {
		fu_device_set_progress_full (device, offset, FLASH_USER_SIZE);
		if (!flash_iface_erase_block (self, base_addr + offset, error))
			return FALSE;
	}

	/* write new image */
	g_debug ("write new image to %#x", base_addr);
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);
	if (!flash_iface_write (self, base_addr, firmware_bytes, error))
		return FALSE;

	fu_device_set_status (device, FWUPD_STATUS_DEVICE_VERIFY);
	readback_buf = g_malloc (FLASH_USER_SIZE);
	if (!flash_iface_read (self, base_addr, readback_buf, FLASH_USER_SIZE, error))
		return FALSE;
	if (memcmp (g_bytes_get_data (firmware_bytes, NULL), readback_buf, FLASH_USER_SIZE) != 0) {
		g_set_error (error, FWUPD_ERROR, FWUPD_ERROR_WRITE,
			     "flash contents after write do not match firmware image");
		return FALSE;
	}

	/* Erase old flag and write new one. The MST appears to modify the
	 * flag value once booted, so we always write the same value here and
	 * it picks up what we've updated. */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_ERASE);
	if (!flash_iface_erase_sector (self, flag_addr & ~(SECTOR_SIZE - 1), error))
		return FALSE;
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);
	return flash_iface_write (self, flag_addr,
				  g_bytes_new_static (flag_data, sizeof (flag_data)),
				  error);
}

static FuFirmware*
fu_realtek_mst_device_read_firmware (FuDevice *device, GError **error)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (device);
	g_autofree void *image_bytes = NULL;
	guint32 bank_address;

	if (self->active_bank == FLASH_BANK_USER1)
		bank_address = FLASH_USER1_ADDR;
	else if (self->active_bank == FLASH_BANK_USER2)
		bank_address = FLASH_USER2_ADDR;
	else {
		g_set_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED,
			     "cannot read firmware from bank %u",
			     self->active_bank);
		return NULL;
	}

	image_bytes = g_malloc (FLASH_USER_SIZE);
	if (!flash_iface_read (self, bank_address, image_bytes, FLASH_USER_SIZE, error))
		return NULL;
	return fu_firmware_new_from_bytes(g_bytes_new_take (g_steal_pointer (&image_bytes), FLASH_USER_SIZE));
}

static GBytes*
fu_realtek_mst_device_dump_firmware (FuDevice *device, GError **error)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (device);
	g_autofree void *flash_contents = g_malloc(FLASH_SIZE);

	fu_device_set_status (device, FWUPD_STATUS_DEVICE_READ);
	if (!flash_iface_read (self, 0, flash_contents, FLASH_SIZE, error))
		return NULL;
	fu_device_set_status (device, FWUPD_STATUS_IDLE);

	return g_bytes_new_take (g_steal_pointer (&flash_contents), FLASH_SIZE);
}

static gboolean
fu_realtek_mst_device_attach (FuDevice *device, GError **error)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (device);
	guint8 value;

	/* re-enable hardware write protect via GPIO */
	if (!mst_set_gpio88 (self, 0, error))
		return FALSE;

	if (!mst_read_register (self, REG_MCU_MODE, &value, error))
		return FALSE;
	if ((value & MCU_MODE_ISP) != 0) {
		g_debug ("resetting device to exit ISP mode");
		fu_device_set_status (device, FWUPD_STATUS_DEVICE_RESTART);

		/* Set register EE bit 2 to request reset. This write can fail
		 * spuriously, so we ignore the write result and verify the device is
		 * no longer in programming mode after giving it time to reset. */
		if (!mst_read_register (self, 0xEE, &value, error))
			return FALSE;
		mst_write_register (self, 0xEE, value | 2, NULL);

		/* allow device some time to reset */
		g_usleep(G_TIME_SPAN_SECOND);

		/* verify device has exited programming mode and actually reset */
		if (!mst_read_register (self, REG_MCU_MODE, &value, error))
			return FALSE;
		if ((value & MCU_MODE_ISP) == MCU_MODE_ISP) {
			g_set_error_literal (error, FWUPD_ERROR,
					     FWUPD_ERROR_NEEDS_USER_ACTION,
					     "device failed to reset when requested");
			fu_device_add_flag (device, FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN);
			return FALSE;
		}
	} else {
		g_debug ("device is already in normal mode");
	}

	fu_device_remove_flag (device, FWUPD_DEVICE_FLAG_IS_BOOTLOADER);
	fu_device_set_status (device, FWUPD_STATUS_IDLE);
	return TRUE;
}

static void
fu_realtek_mst_device_init (FuRealtekMstDevice *self)
{
	FuDevice *device = FU_DEVICE (self);
	self->dp_aux_dev_name = NULL;
	self->bus_device = NULL;
	self->active_bank = FLASH_BANK_INVALID;

	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_DUAL_IMAGE);
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_PAIR);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE);

	fu_device_add_protocol (device, "com.realtek.rtd2142");
	fu_device_set_vendor (device, "Realtek");
	fu_device_set_summary (device, "DisplayPort MST hub");
	fu_device_add_icon (device, "video-display");
	fu_device_set_firmware_size (device, FLASH_USER_SIZE);
}

static void
fu_realtek_mst_device_finalize (GObject *object)
{
	FuRealtekMstDevice *self = FU_REALTEK_MST_DEVICE (object);
	g_free (self->dp_aux_dev_name);
	if (self->bus_device != NULL)
		g_object_unref (self->bus_device);
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
	/* read active image */
	klass_device->read_firmware = fu_realtek_mst_device_read_firmware;
	/* dump whole flash */
	klass_device->dump_firmware = fu_realtek_mst_device_dump_firmware;
}
