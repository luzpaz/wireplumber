/* WirePlumber
 *
 * Copyright © 2019 Collabora Ltd.
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <wp/wp.h>
#include <pipewire/pipewire.h>

struct _WpClientPermissions
{
  WpPlugin parent;
  WpObjectManager *om;
};

G_DECLARE_FINAL_TYPE (WpClientPermissions, wp_client_permissions,
                      WP, CLIENT_PERMISSIONS, WpPlugin)
G_DEFINE_TYPE (WpClientPermissions, wp_client_permissions, WP_TYPE_PLUGIN)

static void
wp_client_permissions_init (WpClientPermissions * self)
{
}

static void
client_added (WpObjectManager * om, WpClient *client, WpClientPermissions * self)
{
  guint32 id = wp_proxy_get_bound_id (WP_PROXY (client));
  const char *access = wp_pipewire_object_get_property (
      WP_PIPEWIRE_OBJECT (client), PW_KEY_ACCESS);

  wp_debug_object (self, "Client added: %d, access: %s", id, access);

  if (!g_strcmp0 (access, "flatpak") || !g_strcmp0 (access, "restricted")) {
    wp_debug_object (self, "Granting full access to client %d", id);
    wp_client_update_permissions (client, 1, -1, PW_PERM_RWX);
  }
}

static void
wp_client_permissions_enable (WpPlugin * plugin, WpTransition * transition)
{
  WpClientPermissions * self = WP_CLIENT_PERMISSIONS (plugin);
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (plugin));

  g_return_if_fail (core);

  self->om = wp_object_manager_new ();
  wp_object_manager_add_interest (self->om, WP_TYPE_CLIENT, NULL);
  wp_object_manager_request_object_features (self->om, WP_TYPE_CLIENT,
      WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
  g_signal_connect (self->om, "object-added", (GCallback) client_added, self);
  wp_core_install_object_manager (core, self->om);

  wp_object_update_features (WP_OBJECT (self), WP_PLUGIN_FEATURE_ENABLED, 0);
}

static void
wp_client_permissions_disable (WpPlugin * plugin)
{
  WpClientPermissions * self = WP_CLIENT_PERMISSIONS (plugin);

  g_clear_object (&self->om);
}

static void
wp_client_permissions_class_init (WpClientPermissionsClass * klass)
{
  WpPluginClass *plugin_class = (WpPluginClass *) klass;

  plugin_class->enable = wp_client_permissions_enable;
  plugin_class->disable = wp_client_permissions_disable;
}

WP_PLUGIN_EXPORT gboolean
wireplumber__module_init (WpCore * core, GVariant * args, GError ** error)
{
  wp_plugin_register (g_object_new (wp_client_permissions_get_type (),
          "name", "client-permissions",
          "core", core,
          NULL));
  return TRUE;
}
