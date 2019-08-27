/* WirePlumber
 *
 * Copyright © 2019 Collabora Ltd.
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * module-pw-audio-softdsp-endpoint provides a WpEndpoint implementation
 * that wraps an audio device node in pipewire and plugs a DSP node, as well
 * as optional merger+volume nodes that are used as entry points for the
 * various streams that this endpoint may have
 */

#include <wp/wp.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/param/props.h>

#include "module-pw-audio-softdsp-endpoint/stream.h"
#include "module-pw-audio-softdsp-endpoint/adapter.h"
#include "module-pw-audio-softdsp-endpoint/convert.h"

#define MIN_QUANTUM_SIZE  64
#define MAX_QUANTUM_SIZE  1024
#define CONTROL_SELECTED 0

struct _WpPwAudioSoftdspEndpoint
{
  WpEndpoint parent;

  /* Properties */
  guint global_id;
  GVariant *streams;

  guint stream_count;
  gboolean selected;

  /* The task to signal the endpoint is initialized */
  GTask *init_task;
  gboolean init_abort;

  /* Audio Streams */
  WpAudioStream *adapter;
  GPtrArray *converters;
};

enum {
  PROP_0,
  PROP_GLOBAL_ID,
  PROP_STREAMS,
};

static GAsyncInitableIface *wp_endpoint_parent_interface = NULL;
static void wp_endpoint_async_initable_init (gpointer iface,
    gpointer iface_data);

G_DECLARE_FINAL_TYPE (WpPwAudioSoftdspEndpoint, endpoint,
    WP_PW, AUDIO_SOFTDSP_ENDPOINT, WpEndpoint)

G_DEFINE_TYPE_WITH_CODE (WpPwAudioSoftdspEndpoint, endpoint, WP_TYPE_ENDPOINT,
    G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE,
                           wp_endpoint_async_initable_init))

typedef GObject* (*WpObjectNewFinishFunc)(GObject *initable, GAsyncResult *res,
    GError **error);

static GObject *
object_safe_new_finish(WpPwAudioSoftdspEndpoint * self, GObject *initable,
    GAsyncResult *res, WpObjectNewFinishFunc new_finish_func)
{
  GObject *object = NULL;
  GError *error = NULL;

  /* Return NULL if we are already aborting */
  if (self->init_abort)
    return NULL;

  /* Get the object */
  object = G_OBJECT (new_finish_func (initable, res, &error));
  g_return_val_if_fail (object, NULL);

  /* Check for error */
  if (error) {
    g_clear_object (&object);
    g_warning ("WpPwAudioSoftdspEndpoint:%p Aborting construction", self);
    self->init_abort = TRUE;
    g_task_return_error (self->init_task, error);
    g_clear_object (&self->init_task);
    return NULL;
  }

  return object;
}

static gboolean
endpoint_prepare_link (WpEndpoint * ep, guint32 stream_id,
    WpEndpointLink * link, GVariant ** properties, GError ** error)
{
  WpPwAudioSoftdspEndpoint *self = WP_PW_AUDIO_SOFTDSP_ENDPOINT (ep);
  WpAudioStream *stream = NULL;

  /* Make sure the stream Id is valid */
  g_return_val_if_fail(stream_id < self->converters->len, FALSE);

  /* Make sure the stream is valid */
  stream = g_ptr_array_index (self->converters, stream_id);
  g_return_val_if_fail(stream, FALSE);

  /* Prepare the link */
  return wp_audio_stream_prepare_link (stream, properties, error);
}

static void
finish_endpoint_creation(WpPwAudioSoftdspEndpoint *self)
{
  /* Don't do anything if the endpoint has already been initialized */
  if (!self->init_task)
    return;

  /* Finish the creation of the endpoint */
  g_task_return_boolean (self->init_task, TRUE);
  g_clear_object(&self->init_task);
}

static void
on_audio_convert_created(GObject *initable, GAsyncResult *res, gpointer data)
{
  WpPwAudioSoftdspEndpoint *self = data;
  WpAudioStream *convert = NULL;
  guint stream_id = 0;
  g_autofree gchar *name = NULL;

  /* Get the audio convert */
  convert = WP_AUDIO_STREAM (object_safe_new_finish (self, initable, res,
      (WpObjectNewFinishFunc)wp_audio_stream_new_finish));
  if (!convert)
    return;

  /* Get the stream id */
  g_object_get (convert, "id", &stream_id, "name", &name, NULL);
  g_return_if_fail (stream_id >= 0);

  /* Set the streams */
  g_ptr_array_insert (self->converters, stream_id, convert);

  g_debug ("%s:%p Created audio convert %u %s", G_OBJECT_TYPE_NAME (self), self,
      stream_id, name);

  /* Finish the endpoint creation when all the streams are created */
  if (--self->stream_count == 0)
    finish_endpoint_creation(self);
}

static void
on_audio_adapter_created(GObject *initable, GAsyncResult *res,
    gpointer data)
{
  WpPwAudioSoftdspEndpoint *self = data;
  enum pw_direction direction = wp_endpoint_get_direction(WP_ENDPOINT(self));
  g_autoptr (WpCore) core = wp_endpoint_get_core(WP_ENDPOINT(self));
  g_autofree gchar *name = NULL;
  const struct pw_node_info *adapter_info = NULL;
  GVariantDict d;
  GVariantIter iter;
  const gchar *stream;
  int i;

  /* Get the proxy adapter */
  self->adapter = WP_AUDIO_STREAM (object_safe_new_finish (self, initable,
      res, (WpObjectNewFinishFunc)wp_audio_stream_new_finish));
  if (!self->adapter)
    return;

  /* Get the adapter info */
  adapter_info = wp_audio_stream_get_info (self->adapter);
  g_return_if_fail (adapter_info);

  /* Give a proper name to this endpoint based on adapter properties */
  if (0 == g_strcmp0(spa_dict_lookup (adapter_info->props, "device.api"), "alsa")) {
    name = g_strdup_printf ("%s on %s (%s / node %d)",
        spa_dict_lookup (adapter_info->props, "api.alsa.pcm.name"),
        spa_dict_lookup (adapter_info->props, "api.alsa.card.name"),
        spa_dict_lookup (adapter_info->props, "api.alsa.path"),
        adapter_info->id);
    g_object_set (self, "name", name, NULL);
  }

  /* Create the audio converters */
  g_variant_iter_init (&iter, self->streams);
  for (i = 0; g_variant_iter_next (&iter, "&s", &stream); i++) {
    wp_audio_convert_new (WP_ENDPOINT(self), i, stream, direction, adapter_info,
        on_audio_convert_created, self);

    /* Register the stream */
    g_variant_dict_init (&d, NULL);
    g_variant_dict_insert (&d, "id", "u", i);
    g_variant_dict_insert (&d, "name", "s", stream);
    wp_endpoint_register_stream (WP_ENDPOINT (self), g_variant_dict_end (&d));
  }
  self->stream_count = i;
}

static void
endpoint_finalize (GObject * object)
{
  WpPwAudioSoftdspEndpoint *self = WP_PW_AUDIO_SOFTDSP_ENDPOINT (object);

  g_clear_pointer(&self->streams, g_variant_unref);

  /* Destroy the proxy adapter */
  g_clear_object(&self->adapter);

  /* Destroy all the converters */
  g_clear_pointer (&self->converters, g_ptr_array_unref);

  /* Destroy the done task */
  g_clear_object(&self->init_task);

  G_OBJECT_CLASS (endpoint_parent_class)->finalize (object);
}

static void
endpoint_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  WpPwAudioSoftdspEndpoint *self = WP_PW_AUDIO_SOFTDSP_ENDPOINT (object);

  switch (property_id) {
  case PROP_GLOBAL_ID:
    self->global_id = g_value_get_uint(value);
    break;
  case PROP_STREAMS:
    self->streams = g_value_dup_variant(value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
endpoint_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  WpPwAudioSoftdspEndpoint *self = WP_PW_AUDIO_SOFTDSP_ENDPOINT (object);

  switch (property_id) {
  case PROP_GLOBAL_ID:
    g_value_set_uint (value, self->global_id);
    break;
  case PROP_STREAMS:
    g_value_set_variant (value, self->streams);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static GVariant *
endpoint_get_control_value (WpEndpoint * ep, guint32 id)
{
  WpPwAudioSoftdspEndpoint *self = WP_PW_AUDIO_SOFTDSP_ENDPOINT (ep);
  guint stream_id, control_id;
  WpAudioStream *stream = NULL;

  if (id == CONTROL_SELECTED)
    return g_variant_new_boolean (self->selected);

  wp_audio_stream_id_decode (id, &stream_id, &control_id);

  /* Check if it is the adapter (master) */
  if (stream_id == WP_STREAM_ID_NONE)
    return wp_audio_stream_get_control_value (self->adapter, control_id);

  /* Otherwise get the stream_id and control_id */
  g_return_val_if_fail (stream_id < self->converters->len, NULL);
  stream = g_ptr_array_index (self->converters, stream_id);
  g_return_val_if_fail (stream, NULL);
  return wp_audio_stream_get_control_value (stream, control_id);
}

static gboolean
endpoint_set_control_value (WpEndpoint * ep, guint32 id, GVariant * value)
{
  WpPwAudioSoftdspEndpoint *self = WP_PW_AUDIO_SOFTDSP_ENDPOINT (ep);
  guint stream_id, control_id;
  WpAudioStream *stream = NULL;

  if (id == CONTROL_SELECTED) {
    self->selected = g_variant_get_boolean (value);
    wp_endpoint_notify_control_value (ep, CONTROL_SELECTED);
    return TRUE;
  }

  wp_audio_stream_id_decode (id, &stream_id, &control_id);

  /* Check if it is the adapter (master) */
  if (stream_id == WP_STREAM_ID_NONE)
    return wp_audio_stream_set_control_value (self->adapter, control_id, value);

  /* Otherwise get the stream_id and control_id */
  g_return_val_if_fail (stream_id < self->converters->len, FALSE);
  stream = g_ptr_array_index (self->converters, stream_id);
  g_return_val_if_fail (stream, FALSE);
  return wp_audio_stream_set_control_value (stream, control_id, value);
}

static void
wp_endpoint_init_async (GAsyncInitable *initable, int io_priority,
    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  WpPwAudioSoftdspEndpoint *self = WP_PW_AUDIO_SOFTDSP_ENDPOINT (initable);
  enum pw_direction direction = wp_endpoint_get_direction(WP_ENDPOINT(self));
  g_autoptr (WpCore) core = wp_endpoint_get_core(WP_ENDPOINT(self));
  GVariantDict d;

  /* Create the async task */
  self->init_task = g_task_new (initable, cancellable, callback, data);

  /* Create the adapter proxy */
  wp_audio_adapter_new (WP_ENDPOINT(self), WP_STREAM_ID_NONE, "master",
      direction, self->global_id, FALSE, on_audio_adapter_created, self);

  /* Register the selected control */
  self->selected = FALSE;
  g_variant_dict_init (&d, NULL);
  g_variant_dict_insert (&d, "id", "u", CONTROL_SELECTED);
  g_variant_dict_insert (&d, "name", "s", "selected");
  g_variant_dict_insert (&d, "type", "s", "b");
  g_variant_dict_insert (&d, "default-value", "b", self->selected);
  wp_endpoint_register_control (WP_ENDPOINT (self), g_variant_dict_end (&d));

  /* Call the parent interface */
  wp_endpoint_parent_interface->init_async (initable, io_priority, cancellable,
      callback, data);
}

static void
wp_endpoint_async_initable_init (gpointer iface, gpointer iface_data)
{
  GAsyncInitableIface *ai_iface = iface;

  /* Set the parent interface */
  wp_endpoint_parent_interface = g_type_interface_peek_parent (iface);

  /* Only set the init_async */
  ai_iface->init_async = wp_endpoint_init_async;
}

static void
endpoint_init (WpPwAudioSoftdspEndpoint * self)
{
  self->init_abort = FALSE;
  self->converters = g_ptr_array_new_with_free_func (g_object_unref);
}

static void
endpoint_class_init (WpPwAudioSoftdspEndpointClass * klass)
{
  GObjectClass *object_class = (GObjectClass *) klass;
  WpEndpointClass *endpoint_class = (WpEndpointClass *) klass;

  object_class->finalize = endpoint_finalize;
  object_class->set_property = endpoint_set_property;
  object_class->get_property = endpoint_get_property;

  endpoint_class->prepare_link = endpoint_prepare_link;
  endpoint_class->get_control_value = endpoint_get_control_value;
  endpoint_class->set_control_value = endpoint_set_control_value;

  /* Instal the properties */
  g_object_class_install_property (object_class, PROP_GLOBAL_ID,
      g_param_spec_uint ("global-id", "global-id",
          "The global Id this endpoint refers to", 0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_STREAMS,
      g_param_spec_variant ("streams", "streams",
          "The stream names for the streams to create",
          G_VARIANT_TYPE ("as"), NULL,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

void
endpoint_factory (WpFactory * factory, GType type, GVariant * properties,
  GAsyncReadyCallback ready, gpointer user_data)
{
  g_autoptr (WpCore) core = NULL;
  const gchar *name, *media_class;
  guint direction;
  guint global_id;
  g_autoptr (GVariant) streams = NULL;

  /* Make sure the type is correct */
  g_return_if_fail(type == WP_TYPE_ENDPOINT);

  /* Get the Core */
  core = wp_factory_get_core(factory);
  g_return_if_fail (core);

  /* Get the properties */
  if (!g_variant_lookup (properties, "name", "&s", &name))
      return;
  if (!g_variant_lookup (properties, "media-class", "&s", &media_class))
      return;
  if (!g_variant_lookup (properties, "direction", "u", &direction))
      return;
  if (!g_variant_lookup (properties, "global-id", "u", &global_id))
      return;
  if (!(streams = g_variant_lookup_value (properties, "streams",
          G_VARIANT_TYPE ("as"))))
      return;

  /* Create and return the softdsp endpoint object */
  g_async_initable_new_async (
      endpoint_get_type (), G_PRIORITY_DEFAULT, NULL, ready, user_data,
      "core", core,
      "name", name,
      "media-class", media_class,
      "direction", direction,
      "global-id", global_id,
      "streams", streams,
      NULL);
}

void
wireplumber__module_init (WpModule * module, WpCore * core, GVariant * args)
{
  /* Register the softdsp endpoint */
  wp_factory_new (core, "pw-audio-softdsp-endpoint", endpoint_factory);
}
