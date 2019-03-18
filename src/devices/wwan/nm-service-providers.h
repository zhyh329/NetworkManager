/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2019 Red Hat, Inc.
 */

#ifndef __NETWORKMANAGER_SERVICE_PROVIDERS_H__
#define __NETWORKMANAGER_SERVICE_PROVIDERS_H__

typedef void (*NMServiceProvidersGsmApnCallback) (const char *apn,
                                                  char *username,
                                                  char *password,
                                                  const char *gateway,
                                                  char *auth_method,
                                                  const GSList *dns,
                                                  GError *error,
                                                  gpointer user_data);

void nm_service_providers_find_gsm_apn (const gchar *service_providers,
                                        const gchar *mccmnc,
                                        GCancellable *cancellable,
                                        NMServiceProvidersGsmApnCallback callback,
                                        gpointer user_data);

#endif /* __NETWORKMANAGER_SERVICE_PROVIDERS_H__ */
