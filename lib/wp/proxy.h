/* WirePlumber
 *
 * Copyright © 2019 Collabora Ltd.
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef __WP_PROXY_H__
#define __WP_PROXY_H__

#include "object.h"
#include "core-interfaces.h"

G_BEGIN_DECLS

struct pw_proxy;

#define WP_TYPE_PROXY (wp_proxy_get_type ())
G_DECLARE_FINAL_TYPE (WpProxy, wp_proxy, WP, PROXY, WpObject)

guint32 wp_proxy_get_id (WpProxy * self);
guint32 wp_proxy_get_parent_id (WpProxy * self);
guint32 wp_proxy_get_spa_type (WpProxy * self);
const gchar * wp_proxy_get_spa_type_string (WpProxy * self);

WpObject * wp_proxy_get_core (WpProxy *self);

gboolean wp_proxy_is_destroyed (WpProxy * self);
struct pw_proxy * wp_proxy_get_pw_proxy (WpProxy * self);

G_END_DECLS

#endif
