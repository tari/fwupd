/*
 * Copyright (C) 2021 Peter Marheine <pmarheine@chromium.org>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <glib-object.h>
#include "fu-device.h"

#define FU_TYPE_SPI_FLASH (fu_spi_flash_get_type ())
G_DECLARE_FINAL_TYPE (FuSpiFlash, fu_spi_flash, FU, SPI_FLASH,
		      FuDevice);

gboolean fu_spi_flash_read_status (FuSpiFlash *self, guint8 *value, GError **error);
gboolean fu_spi_flash_write_status (FuSpiFlash *self, guint8 value, GError **error);
gboolean fu_spi_flash_read (FuSpiFlash *self, guint32 address, GBytes *data,
			    GError **error);
gboolean fu_spi_flash_enable_write (FuSpiFlash *self, gboolean enable, GError **error);
gboolean fu_spi_flash_write (FuSpiFlash *self, guint32 address, GBytes *data,
			     GError **error);
gboolean fu_spi_flash_erase (FuSpiFlash *self, guint32 address, guint32 size,
			     GError **error);