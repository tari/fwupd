/*
 * Copyright (C) 2021 Peter Marheine <pmarheine@chromium.org>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <glib-object.h>

#define FU_TYPE_SPI_FLASH_ADAPTER fu_spi_flash_adapter_get_type ()
G_DECLARE_INTERFACE (FuSpiFlashAdapter, fu_spi_flash_adapter, FU,
		     SPI_FLASH_ADAPTER, GObject)

struct _FuSpiFlashAdapter
{
	GTypeInterface parent_iface;

	gboolean (*ll_command) (FuSpiFlashAdapter *self,
				GBytes *tx_bytes,
				guint8 *rx_bytes, gsize rx_bytes_len,
				GError **error);
	/* If read or write functions are implemented, they will be used.
	 * Otherwise they will be performed in terms of ll_command with
	 * direct chip commands. */
	gboolean (*ll_read) (FuSpiFlashAdapter *self,
			     guint32 address, GBytes *data, guint32 *bytes_read,
			     GError **error);
	gboolean (*ll_write) (FuSpiFlashAdapter *self,
			      guint32 address, GBytes *data,
			      guint32 *bytes_written,
			      GError **error);
};
