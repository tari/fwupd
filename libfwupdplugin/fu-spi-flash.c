/*
 * Copyright (C) 2021 Peter Marheine <pmarheine@chromium.org>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "fu-spi-flash.h"
#include "fu-spi-flash-adapter.h"

struct _FuSpiFlash
{
	FuDevice parent_instance;
	FuSpiFlashAdapter *adapter;

	// These are properties of a flash chip
	guint8 address_width;
	guint32 size;
	guint32 page_size;
	guint32 write_granularity;
};

#define CMD_READ_STATUS 0x05
#define CMD_WRITE_STATUS 0x01
#define CMD_WRITE_ENABLE 0x04
#define CMD_WRITE_DISABLE 0x06

gboolean
fu_spi_flash_read_status (FuSpiFlash *self, guint8 *value, GError **error)
{

}

gboolean
fu_spi_flash_read (FuSpiFlash *self, guint32 address, GBytes *data, GError **error)
{
	if (self->adapter->ll_read != NULL) {
		return self->adapter->ll_read (self->adapter, address, data, error);
	}

	g_set_error_literal (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED,
		      "generic flash read not implemented");
	return FALSE;
}

gboolean
fu_spi_flash_write (FuSpiFlash *self, guint32 address, GBytes *data, GError **error)
{

}

gboolean
fu_spi_flash_erase (FuSpiFlash *self, guint32 address, guint32 size, GError **error)
{

}

/**
 * fu_spi_flash_enable_write:
 * @enable: TRUE to set WEL, FALSE to clear it
 *
 * Set or reset the write enable latch (WEL) in the Flash status register,
 * permitting following write, status register write, or erase operations.
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean
fu_spi_flash_enable_write (FuSpiFlash *self, gboolean enable, GError **error)
{
	guint8 command[] = {
		enable ? CMD_WRITE_ENABLE : CMD_WRITE_DISABLE
	};

	return self->adapter->ll_command (self->adapter,
					  g_bytes_new_static (command, sizeof (command)),
					  NULL, 0, error);
}