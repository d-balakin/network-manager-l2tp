/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/***************************************************************************
 * nm-l2tp.c : GNOME UI dialogs for configuring L2TP VPN connections
 *
 * Copyright (C) 2008 Dan Williams, <dcbw@redhat.com>
 * Copyright (C) 2008 - 2011 Red Hat, Inc.
 * Based on work by David Zeuthen, <davidz@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include <glib/gi18n-lib.h>
#include <string.h>
#include <gtk/gtk.h>

#ifdef NM_L2TP_OLD
#define NM_VPN_LIBNM_COMPAT
#include <nm-vpn-plugin-ui-interface.h>
#include <nm-setting-vpn.h>
#include <nm-setting-connection.h>
#include <nm-setting-ip4-config.h>
#include <nm-ui-utils.h>

#define L2TP_PLUGIN_UI_ERROR                     NM_SETTING_VPN_ERROR
#define L2TP_PLUGIN_UI_ERROR_INVALID_PROPERTY    NM_SETTING_VPN_ERROR_INVALID_PROPERTY
#define L2TP_PLUGIN_UI_ERROR_MISSING_PROPERTY    NM_SETTING_VPN_ERROR_MISSING_PROPERTY
#define L2TP_PLUGIN_UI_ERROR_FAILED              NM_SETTING_VPN_ERROR_UNKNOWN

#else /* !NM_L2TP_OLD */

#include <NetworkManager.h>
#include <nma-ui-utils.h>

#define L2TP_PLUGIN_UI_ERROR                     NM_CONNECTION_ERROR
#define L2TP_PLUGIN_UI_ERROR_INVALID_PROPERTY    NM_CONNECTION_ERROR_INVALID_PROPERTY
#define L2TP_PLUGIN_UI_ERROR_MISSING_PROPERTY    NM_CONNECTION_ERROR_MISSING_PROPERTY
#define L2TP_PLUGIN_UI_ERROR_FAILED              NM_CONNECTION_ERROR_FAILED
#endif

#include "nm-l2tp-service-defines.h"
#include "nm-l2tp.h"
#include "import-export.h"
#include "advanced-dialog.h"
#include "ipsec-dialog.h"

#define L2TP_PLUGIN_NAME    _("Layer 2 Tunneling Protocol (L2TP)")
#define L2TP_PLUGIN_DESC    _("Compatible with L2TP VPN servers.")

typedef void (*ChangedCallback) (GtkWidget *widget, gpointer user_data);

/************** plugin class **************/

static void l2tp_plugin_ui_interface_init (NMVpnEditorPluginInterface *iface_class);

G_DEFINE_TYPE_EXTENDED (L2tpPluginUi, l2tp_plugin_ui, G_TYPE_OBJECT, 0,
						G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_EDITOR_PLUGIN,
						l2tp_plugin_ui_interface_init))

/************** UI widget class **************/

static void l2tp_plugin_ui_widget_interface_init (NMVpnEditorInterface *iface_class);

G_DEFINE_TYPE_EXTENDED (L2tpPluginUiWidget, l2tp_plugin_ui_widget, G_TYPE_OBJECT, 0,
						G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_EDITOR,
						l2tp_plugin_ui_widget_interface_init))

#define L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), L2TP_TYPE_PLUGIN_UI_WIDGET, L2tpPluginUiWidgetPrivate))

typedef struct {
	GtkBuilder *builder;
	GtkWidget *widget;
	GtkSizeGroup *group;
	GtkWindowGroup *window_group;
	gboolean window_added;
	GHashTable *advanced;
	GHashTable *ipsec;
	gboolean new_connection;
} L2tpPluginUiWidgetPrivate;

enum {
	PROP_0,
	PROP_NAME,
	PROP_DESC,
	PROP_SERVICE,

	LAST_PROP
};

/**
 * Return copy of string #s with the leading and trailing spaces removed
 * result must be freed with g_free()
 **/
static char *
strstrip (const char *s)
{
	size_t size;
	char *end;
	char *scpy;

	/* leading */
	while (*s && isspace (*s))
		s++;

	scpy = g_strdup (s);
	size = strlen (scpy);

	if (!size)
		return scpy;

	end = scpy + size - 1;

	while (end >= scpy && isspace (*end))
		end--;
	*(end + 1) = '\0';

	return scpy;
}

static gboolean
check_validity (L2tpPluginUiWidget *self, GError **error)
{
	L2tpPluginUiWidgetPrivate *priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GtkWidget *widget;
	const char *str;
	char *s=NULL;

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "gateway_entry"));
	str = gtk_entry_get_text (GTK_ENTRY (widget));
	if (!str || !strlen (s = strstrip (str))) {
		g_free(s);
		g_set_error (error,
		             L2TP_PLUGIN_UI_ERROR,
		             L2TP_PLUGIN_UI_ERROR_INVALID_PROPERTY,
		             NM_L2TP_KEY_GATEWAY);
		return FALSE;
	}

	return TRUE;
}

static void
stuff_changed_cb (GtkWidget *widget, gpointer user_data)
{
	g_signal_emit_by_name (L2TP_PLUGIN_UI_WIDGET (user_data), "changed");
}

static void
advanced_dialog_close_cb (GtkWidget *dialog, gpointer user_data)
{
	gtk_widget_hide (dialog);
	/* gtk_widget_destroy() will remove the window from the window group */
	gtk_widget_destroy (dialog);
}

static void
ipsec_dialog_close_cb (GtkWidget *dialog, gpointer user_data)
{
	gtk_widget_hide (dialog);
	/* gtk_widget_destroy() will remove the window from the window group */
	gtk_widget_destroy (dialog);
}

static void
advanced_dialog_response_cb (GtkWidget *dialog, gint response, gpointer user_data)
{
	L2tpPluginUiWidget *self = L2TP_PLUGIN_UI_WIDGET (user_data);
	L2tpPluginUiWidgetPrivate *priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GError *error = NULL;

	if (response != GTK_RESPONSE_OK) {
		advanced_dialog_close_cb (dialog, self);
		return;
	}

	if (priv->advanced)
		g_hash_table_destroy (priv->advanced);
	priv->advanced = advanced_dialog_new_hash_from_dialog (dialog, &error);
	if (!priv->advanced) {
		g_message (_("%s: error reading advanced settings: %s"), __func__, error->message);
		g_error_free (error);
	}
	advanced_dialog_close_cb (dialog, self);

	stuff_changed_cb (NULL, self);
}

static void
ipsec_dialog_response_cb (GtkWidget *dialog, gint response, gpointer user_data)
{
	L2tpPluginUiWidget *self = L2TP_PLUGIN_UI_WIDGET (user_data);
	L2tpPluginUiWidgetPrivate *priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GError *error = NULL;

	if (response != GTK_RESPONSE_OK) {
		ipsec_dialog_close_cb (dialog, self);
		return;
	}

	if (priv->ipsec)
		g_hash_table_destroy (priv->ipsec);
	priv->ipsec = ipsec_dialog_new_hash_from_dialog (dialog, &error);
	if (!priv->ipsec) {
		g_message (_("%s: error reading ipsec settings: %s"), __func__, error->message);
		g_error_free (error);
	}
	ipsec_dialog_close_cb (dialog, self);

	stuff_changed_cb (NULL, self);
}

static void
advanced_button_clicked_cb (GtkWidget *button, gpointer user_data)
{
	L2tpPluginUiWidget *self = L2TP_PLUGIN_UI_WIDGET (user_data);
	L2tpPluginUiWidgetPrivate *priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GtkWidget *dialog, *toplevel;

	toplevel = gtk_widget_get_toplevel (priv->widget);
	g_return_if_fail (gtk_widget_is_toplevel (toplevel));

	dialog = advanced_dialog_new (priv->advanced);
	if (!dialog) {
		g_warning (_("%s: failed to create the Advanced dialog!"), __func__);
		return;
	}

	gtk_window_group_add_window (priv->window_group, GTK_WINDOW (dialog));
	if (!priv->window_added) {
		gtk_window_group_add_window (priv->window_group, GTK_WINDOW (toplevel));
		priv->window_added = TRUE;
	}

	gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (toplevel));
	g_signal_connect (G_OBJECT (dialog), "response", G_CALLBACK (advanced_dialog_response_cb), self);
	g_signal_connect (G_OBJECT (dialog), "close", G_CALLBACK (advanced_dialog_close_cb), self);

	gtk_widget_show_all (dialog);
}

static void
ipsec_button_clicked_cb (GtkWidget *button, gpointer user_data)
{
	L2tpPluginUiWidget *self = L2TP_PLUGIN_UI_WIDGET (user_data);
	L2tpPluginUiWidgetPrivate *priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GtkWidget *dialog, *toplevel;

	toplevel = gtk_widget_get_toplevel (priv->widget);
	g_return_if_fail (gtk_widget_is_toplevel (toplevel));

	dialog = ipsec_dialog_new (priv->ipsec);
	if (!dialog) {
		g_warning (_("%s: failed to create the IPsec dialog!"), __func__);
		return;
	}

	gtk_window_group_add_window (priv->window_group, GTK_WINDOW (dialog));
	if (!priv->window_added) {
		gtk_window_group_add_window (priv->window_group, GTK_WINDOW (toplevel));
		priv->window_added = TRUE;
	}

	gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (toplevel));
	g_signal_connect (G_OBJECT (dialog), "response", G_CALLBACK (ipsec_dialog_response_cb), self);
	g_signal_connect (G_OBJECT (dialog), "close", G_CALLBACK (ipsec_dialog_close_cb), self);

	gtk_widget_show_all (dialog);
}

static void
setup_password_widget (L2tpPluginUiWidget *self,
                       const char *entry_name,
                       NMSettingVpn *s_vpn,
                       const char *secret_name,
                       gboolean new_connection)
{
	L2tpPluginUiWidgetPrivate *priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GtkWidget *widget;
	const char *value;

	widget = (GtkWidget *) gtk_builder_get_object (priv->builder, entry_name);
	g_assert (widget);
	gtk_size_group_add_widget (priv->group, widget);

	if (s_vpn) {
		value = nm_setting_vpn_get_secret (s_vpn, secret_name);
		gtk_entry_set_text (GTK_ENTRY (widget), value ? value : "");
	}

	g_signal_connect (widget, "changed", G_CALLBACK (stuff_changed_cb), self);
}

static void
show_toggled_cb (GtkCheckButton *button, L2tpPluginUiWidget *self)
{
	L2tpPluginUiWidgetPrivate *priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GtkWidget *widget;
	gboolean visible;

	visible = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button));

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "user_password_entry"));
	g_assert (widget);
	gtk_entry_set_visibility (GTK_ENTRY (widget), visible);
}

static void
password_storage_changed_cb (GObject *entry,
                             GParamSpec *pspec,
                             gpointer user_data)
{
	L2tpPluginUiWidget *self = L2TP_PLUGIN_UI_WIDGET (user_data);

	stuff_changed_cb (NULL, self);
}

static void
init_password_icon (L2tpPluginUiWidget *self,
                    NMSettingVpn *s_vpn,
                    const char *secret_key,
                    const char *entry_name)
{
	L2tpPluginUiWidgetPrivate *priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GtkWidget *entry;
	const char *value = NULL;
	NMSettingSecretFlags pw_flags = NM_SETTING_SECRET_FLAG_NONE;

	/* If there's already a password and the password type can't be found in
	 * the VPN settings, default to saving it.  Otherwise, always ask for it.
	 */
	entry = GTK_WIDGET (gtk_builder_get_object (priv->builder, entry_name));
	g_assert (entry);

	nma_utils_setup_password_storage (entry, 0, (NMSetting *) s_vpn, secret_key,
	                                  TRUE, FALSE);

	/* If there's no password and no flags in the setting,
	 * initialize flags as "always-ask".
	 */
	if (s_vpn)
		nm_setting_get_secret_flags (NM_SETTING (s_vpn), secret_key, &pw_flags, NULL);
	value = gtk_entry_get_text (GTK_ENTRY (entry));
	if ((!value || !strlen (value)) && (pw_flags == NM_SETTING_SECRET_FLAG_NONE))
		nma_utils_update_password_storage (entry, NM_SETTING_SECRET_FLAG_NOT_SAVED,
		                                   (NMSetting *) s_vpn, secret_key);

	g_signal_connect (entry, "notify::secondary-icon-name",
	                  G_CALLBACK (password_storage_changed_cb), self);
}

static gboolean
init_plugin_ui (L2tpPluginUiWidget *self, NMConnection *connection, GError **error)
{
	L2tpPluginUiWidgetPrivate *priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	NMSettingVpn *s_vpn;
	GtkWidget *widget;
	const char *value;

	s_vpn = nm_connection_get_setting_vpn (connection);

	priv->group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "gateway_entry"));
	if (!widget)
		return FALSE;
	gtk_size_group_add_widget (priv->group, widget);
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_L2TP_KEY_GATEWAY);
		if (value && strlen (value))
			gtk_entry_set_text (GTK_ENTRY (widget), value);
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "user_entry"));
	if (!widget)
		return FALSE;
	gtk_size_group_add_widget (priv->group, widget);
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_L2TP_KEY_USER);
		if (value && strlen (value))
			gtk_entry_set_text (GTK_ENTRY (widget), value);
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "domain_entry"));
	if (!widget)
		return FALSE;
	gtk_size_group_add_widget (priv->group, widget);
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_L2TP_KEY_DOMAIN);
		if (value && strlen (value))
			gtk_entry_set_text (GTK_ENTRY (widget), value);
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "advanced_button"));
	g_signal_connect (G_OBJECT (widget), "clicked", G_CALLBACK (advanced_button_clicked_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "ipsec_button"));
	g_signal_connect (G_OBJECT (widget), "clicked", G_CALLBACK (ipsec_button_clicked_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder,  "show_passwords_checkbutton"));
	g_return_val_if_fail (widget != NULL, FALSE);
	g_signal_connect (G_OBJECT (widget), "toggled",
	                  (GCallback) show_toggled_cb,
	                  self);

	/* Fill the VPN passwords *before* initializing the PW type combo, since
	 * knowing if there is a password when initializing the type combo is helpful.
	 */
	setup_password_widget (self,
	                       "user_password_entry",
	                       s_vpn,
	                       NM_L2TP_KEY_PASSWORD,
	                       priv->new_connection);

	init_password_icon (self,
	                   s_vpn,
	                   NM_L2TP_KEY_PASSWORD,
	                   "user_password_entry");

	return TRUE;
}

static GObject *
get_widget (NMVpnEditor *iface)
{
	L2tpPluginUiWidget *self = L2TP_PLUGIN_UI_WIDGET (iface);
	L2tpPluginUiWidgetPrivate *priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);

	return G_OBJECT (priv->widget);
}

static void
hash_copy_pair (gpointer key, gpointer data, gpointer user_data)
{
	NMSettingVpn *s_vpn = NM_SETTING_VPN (user_data);

	nm_setting_vpn_add_data_item (s_vpn, (const char *) key, (const char *) data);
}

static void
save_password_and_flags (NMSettingVpn *s_vpn,
                         GtkBuilder *builder,
                         const char *entry_name,
                         const char *secret_key)
{
	NMSettingSecretFlags flags;
	const char *password;
	GtkWidget *entry;

	/* Get secret flags */
	entry = GTK_WIDGET (gtk_builder_get_object (builder, entry_name));
	flags = nma_utils_menu_to_secret_flags (entry);

	/* Save password and convert flags to legacy data items */
	switch (flags) {
	case NM_SETTING_SECRET_FLAG_NONE:
	case NM_SETTING_SECRET_FLAG_AGENT_OWNED:
		password = gtk_entry_get_text (GTK_ENTRY (entry));
		if (password && strlen (password))
			nm_setting_vpn_add_secret (s_vpn, secret_key, password);
		break;
	default:
		break;
	}

	/* Set new secret flags */
	nm_setting_set_secret_flags (NM_SETTING (s_vpn), secret_key, flags, NULL);
}

static gboolean
update_connection (NMVpnEditor *iface,
                   NMConnection *connection,
                   GError **error)
{
	L2tpPluginUiWidget *self = L2TP_PLUGIN_UI_WIDGET (iface);
	L2tpPluginUiWidgetPrivate *priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	NMSettingVpn *s_vpn;
	GtkWidget *widget;
	const char *str;
	char *s=NULL;
	gboolean valid = FALSE;

	if (!check_validity (self, error))
		return FALSE;

	s_vpn = NM_SETTING_VPN (nm_setting_vpn_new ());
	g_object_set (s_vpn, NM_SETTING_VPN_SERVICE_TYPE, NM_DBUS_SERVICE_L2TP, NULL);

	/* Gateway */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "gateway_entry"));
	str = gtk_entry_get_text (GTK_ENTRY (widget));
	if (str)
		s = strstrip(str);
	if (s && strlen (s))
		nm_setting_vpn_add_data_item (s_vpn, NM_L2TP_KEY_GATEWAY, s);
	g_free(s);

	/* Username */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder,  "user_entry"));
	str = gtk_entry_get_text (GTK_ENTRY (widget));
	if (str && strlen (str))
		nm_setting_vpn_add_data_item (s_vpn, NM_L2TP_KEY_USER, str);

	/* User password and flags */
	save_password_and_flags (s_vpn,
	                         priv->builder,
	                         "user_password_entry",
	                         NM_L2TP_KEY_PASSWORD);

	/* Domain */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "domain_entry"));
	str = gtk_entry_get_text (GTK_ENTRY (widget));
	if (str && strlen (str))
		nm_setting_vpn_add_data_item (s_vpn, NM_L2TP_KEY_DOMAIN, str);

	if (priv->advanced)
		g_hash_table_foreach (priv->advanced, hash_copy_pair, s_vpn);
	if (priv->ipsec)
		g_hash_table_foreach (priv->ipsec, hash_copy_pair, s_vpn);

	nm_connection_add_setting (connection, NM_SETTING (s_vpn));
	valid = TRUE;

	return valid;
}

static void
is_new_func (const char *key, const char *value, gpointer user_data)
{
	gboolean *is_new = user_data;

	/* If there are any VPN data items the connection isn't new */
	*is_new = FALSE;
}

static NMVpnEditor *
nm_vpn_plugin_ui_widget_interface_new (NMConnection *connection, GError **error)
{
	NMVpnEditor *object;
	L2tpPluginUiWidgetPrivate *priv;
	char *ui_file;
	gboolean new = TRUE;
	NMSettingVpn *s_vpn;

	if (error)
		g_return_val_if_fail (*error == NULL, NULL);

	object = NM_VPN_EDITOR (g_object_new (L2TP_TYPE_PLUGIN_UI_WIDGET, NULL));
	if (!object) {
		g_set_error (error, L2TP_PLUGIN_UI_ERROR, 0, _("could not create l2tp object"));
		return NULL;
	}

	priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (object);

	ui_file = g_strdup_printf ("%s/%s", UIDIR, "nm-l2tp-dialog.ui");
	priv->builder = gtk_builder_new ();

	gtk_builder_set_translation_domain (priv->builder, GETTEXT_PACKAGE);

	if (!gtk_builder_add_from_file(priv->builder, ui_file, error)) {
		g_warning (_("Couldn't load builder file: %s"),
				error && *error ? (*error)->message : "(unknown)");
		g_clear_error(error);
		g_set_error(error, L2TP_PLUGIN_UI_ERROR, 0,
					_("could not load required resources at %s"),
					ui_file);
		g_free(ui_file);
		g_object_unref(object);
		return NULL;
	}
	g_free (ui_file);

	priv->widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "l2tp-vbox"));
	if (!priv->widget) {
		g_set_error (error, L2TP_PLUGIN_UI_ERROR, 0, _("could not load UI widget"));
		g_object_unref (object);
		return NULL;
	}
	g_object_ref_sink (priv->widget);

	priv->window_group = gtk_window_group_new ();

	s_vpn = nm_connection_get_setting_vpn (connection);
	if (s_vpn)
		nm_setting_vpn_foreach_data_item (s_vpn, is_new_func, &new);
	priv->new_connection = new;

	if (!init_plugin_ui (L2TP_PLUGIN_UI_WIDGET (object), connection, error)) {
		g_object_unref (object);
		return NULL;
	}

	priv->advanced = advanced_dialog_new_hash_from_connection (connection, error);
	if (!priv->advanced) {
		g_object_unref (object);
		return NULL;
	}
	priv->ipsec = ipsec_dialog_new_hash_from_connection (connection, error);
	if (!priv->ipsec) {
		g_object_unref (object);
		return NULL;
	}

	return object;
}

static void
dispose (GObject *object)
{
	L2tpPluginUiWidget *plugin = L2TP_PLUGIN_UI_WIDGET (object);
	L2tpPluginUiWidgetPrivate *priv = L2TP_PLUGIN_UI_WIDGET_GET_PRIVATE (plugin);
	GtkWidget *widget;

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "user_password_entry"));
	g_signal_handlers_disconnect_by_func (G_OBJECT (widget),
	                                      (GCallback) password_storage_changed_cb,
	                                      plugin);

	if (priv->group)
		g_object_unref (priv->group);

	if (priv->window_group)
		g_object_unref (priv->window_group);

	if (priv->widget)
		g_object_unref (priv->widget);

	if (priv->builder)
		g_object_unref (priv->builder);

	if (priv->advanced)
		g_hash_table_destroy (priv->advanced);

	if (priv->ipsec)
		g_hash_table_destroy (priv->ipsec);

	G_OBJECT_CLASS (l2tp_plugin_ui_widget_parent_class)->dispose (object);
}

static void
l2tp_plugin_ui_widget_class_init (L2tpPluginUiWidgetClass *req_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (req_class);

	g_type_class_add_private (req_class, sizeof (L2tpPluginUiWidgetPrivate));

	object_class->dispose = dispose;
}

static void
l2tp_plugin_ui_widget_init (L2tpPluginUiWidget *plugin)
{
}

static void
l2tp_plugin_ui_widget_interface_init (NMVpnEditorInterface *iface_class)
{
	/* interface implementation */
	iface_class->get_widget = get_widget;
	iface_class->update_connection = update_connection;
}

static NMConnection *
import (NMVpnEditorPlugin *iface, const char *path, GError **error)
{
	NMConnection *connection = NULL;
	char *ext;

	ext = strrchr (path, '.');
	if (!ext) {
		g_set_error (error,
		             L2TP_PLUGIN_UI_ERROR,
		             L2TP_PLUGIN_UI_ERROR_FAILED,
		             _("unknown L2TP file extension"));
		return NULL;
	}

	if (strcmp (ext, ".conf") && strcmp (ext, ".cnf")) {
		g_set_error (error,
		             L2TP_PLUGIN_UI_ERROR,
		             L2TP_PLUGIN_UI_ERROR_FAILED,
		             _("unknown L2TP file extension. Allowed .conf or .cnf"));
		return NULL;
	}

	if (!strstr (path, "l2tp")) {
		g_set_error (error,
		             L2TP_PLUGIN_UI_ERROR,
		             L2TP_PLUGIN_UI_ERROR_FAILED,
		             _("Filename doesn't contains 'l2tp' substring."));
		return NULL;
	}

	connection = do_import (path, error);

	if ((connection == NULL) && (*error != NULL))
		g_warning(_("Can't import file as L2TP config: %s"), (*error)->message);

	return connection;
}

static gboolean
export (NMVpnEditorPlugin *iface,
        const char *path,
        NMConnection *connection,
        GError **error)
{
	return do_export (path, connection, error);
}

static char *
get_suggested_filename (NMVpnEditorPlugin *iface, NMConnection *connection)
{
	NMSettingConnection *s_con;
	const char *id;

	g_return_val_if_fail (connection != NULL, NULL);

	s_con = nm_connection_get_setting_connection (connection);
	g_return_val_if_fail (s_con != NULL, NULL);

	id = nm_setting_connection_get_id (s_con);
	g_return_val_if_fail (id != NULL, NULL);

	return g_strdup_printf ("%s (l2tp).conf", id);
}

static NMVpnEditorPluginCapability
get_capabilities (NMVpnEditorPlugin *iface)
{
	return (NM_VPN_EDITOR_PLUGIN_CAPABILITY_IMPORT | NM_VPN_EDITOR_PLUGIN_CAPABILITY_EXPORT);
}

static NMVpnEditor *
get_editor (NMVpnEditorPlugin *iface, NMConnection *connection, GError **error)
{
	return nm_vpn_plugin_ui_widget_interface_new (connection, error);
}

static void
get_property (GObject *object, guint prop_id,
			  GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	case PROP_NAME:
		g_value_set_string (value, L2TP_PLUGIN_NAME);
		break;
	case PROP_DESC:
		g_value_set_string (value, L2TP_PLUGIN_DESC);
		break;
	case PROP_SERVICE:
		g_value_set_string (value, NM_DBUS_SERVICE_L2TP);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
l2tp_plugin_ui_class_init (L2tpPluginUiClass *req_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (req_class);

	object_class->get_property = get_property;

	g_object_class_override_property (object_class,
									  PROP_NAME,
									  NM_VPN_EDITOR_PLUGIN_NAME);

	g_object_class_override_property (object_class,
									  PROP_DESC,
									  NM_VPN_EDITOR_PLUGIN_DESCRIPTION);

	g_object_class_override_property (object_class,
									  PROP_SERVICE,
									  NM_VPN_EDITOR_PLUGIN_SERVICE);
}

static void
l2tp_plugin_ui_init (L2tpPluginUi *plugin)
{
}

static void
l2tp_plugin_ui_interface_init (NMVpnEditorPluginInterface *iface_class)
{
	/* interface implementation */
	iface_class->get_editor = get_editor;
	iface_class->get_capabilities = get_capabilities;
	iface_class->import_from_file = import;
	iface_class->export_to_file = export;
	iface_class->get_suggested_filename = get_suggested_filename;
}

G_MODULE_EXPORT NMVpnEditorPlugin *
nm_vpn_editor_plugin_factory (GError **error)
{
	if (error)
		g_return_val_if_fail (*error == NULL, NULL);

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

	return NM_VPN_EDITOR_PLUGIN (g_object_new (L2TP_TYPE_PLUGIN_UI, NULL));
}

