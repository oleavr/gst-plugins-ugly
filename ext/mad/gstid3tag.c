/* GStreamer
 * Copyright (C) 2003-2004 Benjamin Otte <otte@gnome.org>
 *
 * gstid3tag.c: plugin for reading / modifying id3 tags
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstmad.h"
#include <stdlib.h>
#include <string.h>
#include <gst/gsttaginterface.h>

#define ID3_TYPE_FIND_SIZE 40960
GST_DEBUG_CATEGORY_STATIC (gst_id3_tag_debug);
#define GST_CAT_DEFAULT gst_id3_tag_debug

#define GST_TYPE_ID3_TAG (gst_id3_tag_get_type(GST_ID3_TAG_PARSE_BASE ))
#define GST_ID3_TAG(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_ID3_TAG, GstID3Tag))
#define GST_ID3_TAG_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_ID3_TAG, GstID3TagClass))
#define GST_IS_ID3_TAG(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_ID3_TAG))
#define GST_IS_ID3_TAG_CLASS(obj) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_ID3_TAG))
#define GST_ID3_TAG_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_ID3_TAG, GstID3TagClass))

typedef struct _GstID3Tag GstID3Tag;
typedef struct _GstID3TagClass GstID3TagClass;

typedef enum
{
  GST_ID3_TAG_STATE_READING_V2_TAG,
  GST_ID3_TAG_STATE_SEEKING_TO_V1_TAG,
  GST_ID3_TAG_STATE_READING_V1_TAG,
  GST_ID3_TAG_STATE_SEEKING_TO_NORMAL,
  GST_ID3_TAG_STATE_NORMAL_START,
  GST_ID3_TAG_STATE_NORMAL
}
GstID3TagState;

typedef enum
{
  GST_ID3_TAG_PARSE_BASE = 0,
  GST_ID3_TAG_PARSE_DEMUX = 1,
  GST_ID3_TAG_PARSE_MUX = 2,
  GST_ID3_TAG_PARSE_ANY = 3
}
GstID3ParseMode;

#define IS_DEMUXER(tag) ((tag)->parse_mode & GST_ID3_TAG_PARSE_DEMUX)
#define IS_MUXER(tag) ((tag)->parse_mode & GST_ID3_TAG_PARSE_MUX)
#define CAN_BE_DEMUXER(tag) (GST_ID3_TAG_GET_CLASS(tag)->type & GST_ID3_TAG_PARSE_DEMUX)
#define CAN_BE_MUXER(tag) (GST_ID3_TAG_GET_CLASS(tag)->type & GST_ID3_TAG_PARSE_MUX)

struct _GstID3Tag
{
  GstElement element;

  /* pads */
  GstPad *sinkpad;
  GstPad *srcpad;

  /* caps */
  GstID3ParseMode parse_mode;
  GstCaps *found_caps;

  /* tags */
  GstTagList *event_tags;
  GstTagList *parsed_tags;

  /* state */
  GstID3TagState state;

  GstBuffer *buffer;
  gboolean prefer_v1tag;
  glong v1tag_size;
  glong v1tag_size_new;
  guint64 v1tag_offset;
  gboolean v1tag_render;
  glong v2tag_size;
  glong v2tag_size_new;
  gboolean v2tag_render;
};

struct _GstID3TagClass
{
  GstElementClass parent_class;

  GstID3ParseMode type;
};

/* signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  ARG_0,
  ARG_V1_TAG,
  ARG_V2_TAG,
  ARG_PREFER_V1
      /* FILL ME */
};

GST_DEBUG_CATEGORY_EXTERN (mad_debug);

static GstStaticPadTemplate id3_tag_src_any_template_factory =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
  /* FIXME: for spider - should be GST_PAD_ALWAYS, */
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("ANY")
    );

static GstStaticPadTemplate id3_tag_src_id3_template_factory =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-id3")
    );

static GstStaticPadTemplate id3_tag_sink_any_template_factory =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    /* FIXME: find a way to extend this generically */
    GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)1; audio/x-flac")
    );

static GstStaticPadTemplate id3_tag_sink_id3_template_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-id3")
    );


static void gst_id3_tag_class_init (gpointer g_class, gpointer class_data);
static void gst_id3_tag_init (GTypeInstance * instance, gpointer g_class);
static void gst_id3_tag_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_id3_tag_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static gboolean gst_id3_tag_src_event (GstPad * pad, GstEvent * event);
static const GstQueryType *gst_id3_tag_get_query_types (GstPad * pad);

static gboolean gst_id3_tag_src_query (GstPad * pad, GstQuery * query);

static gboolean gst_id3_tag_sink_event (GstPad * pad, GstEvent * event);
static GstFlowReturn gst_id3_tag_chain (GstPad * pad, GstBuffer * buffer);
static GstPadLinkReturn gst_id3_tag_src_link (GstPad * pad, GstPad * peer);

static GstElementStateReturn gst_id3_tag_change_state (GstElement * element);

static GstElementClass *parent_class = NULL;

/* static guint gst_id3_tag_signals[LAST_SIGNAL] = { 0 }; */

GType
gst_id3_tag_get_type (guint type)
{
  static GType id3_tag_type[4] = { 0, 0, 0, 0 };
  static gchar *name[4] = { "GstID3TagBase", "GstID3Demux", "GstID3Mux",
    "GstID3Tag"
  };

  g_assert (type < 4);

  if (!id3_tag_type[type]) {
    GTypeInfo id3_tag_info = {
      sizeof (GstID3TagClass),
      NULL,
      NULL,
      gst_id3_tag_class_init,
      NULL,
      GUINT_TO_POINTER (type),
      sizeof (GstID3Tag),
      0,
      gst_id3_tag_init,
    };
    static const GInterfaceInfo tag_setter_info = {
      NULL,
      NULL,
      NULL
    };

    id3_tag_type[type] = g_type_register_static (
        (type == GST_ID3_TAG_PARSE_BASE) ? GST_TYPE_ELEMENT :
        GST_TYPE_ID3_TAG, name[type], &id3_tag_info, 0);

    if (type & GST_ID3_TAG_PARSE_MUX) {
      g_type_add_interface_static (id3_tag_type[type], GST_TYPE_TAG_SETTER,
          &tag_setter_info);
    }
  }
  return id3_tag_type[type];
}

/* elementfactory information */
GstElementDetails gst_id3_tag_details[3] = {
  GST_ELEMENT_DETAILS ("id3 tag extractor",
      "Codec/Demuxer/Audio",
      "Extract ID3 tagging information",
      "Benjamin Otte <otte@gnome.org>"),
  GST_ELEMENT_DETAILS ("id3 muxer",
      "Codec/Muxer/Audio",
      "Add ID3 tagging information",
      "Benjamin Otte <otte@gnome.org>"),
  GST_ELEMENT_DETAILS ("id3 tag extractor",
      "Tag",
      "Extract tagging information from mp3s",
      "Benjamin Otte <otte@gnome.org>")
};

static void
gst_id3_tag_class_init (gpointer g_class, gpointer class_data)
{
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (g_class);
  GObjectClass *gobject_class = G_OBJECT_CLASS (g_class);
  GstID3TagClass *tag_class = GST_ID3_TAG_CLASS (g_class);

  tag_class->type = GPOINTER_TO_UINT (class_data);

  if (tag_class->type == GST_ID3_TAG_PARSE_BASE) {
    parent_class = g_type_class_peek_parent (g_class);
    gstelement_class->change_state = gst_id3_tag_change_state;
  } else {
    gst_element_class_set_details (gstelement_class,
        &gst_id3_tag_details[tag_class->type - 1]);
  }

  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_id3_tag_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_id3_tag_get_property);

  if (tag_class->type & GST_ID3_TAG_PARSE_DEMUX) {
    g_object_class_install_property (gobject_class, ARG_PREFER_V1,
        g_param_spec_boolean ("prefer-v1", "prefer version 1 tag",
            "Prefer tags from tag at end of file", FALSE,
            G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
    gst_element_class_add_pad_template (gstelement_class,
        gst_static_pad_template_get (&id3_tag_src_any_template_factory));
  } else {
    gst_element_class_add_pad_template (gstelement_class,
        gst_static_pad_template_get (&id3_tag_src_id3_template_factory));
  }

  if (tag_class->type & GST_ID3_TAG_PARSE_MUX) {
    g_object_class_install_property (gobject_class, ARG_V2_TAG,
        g_param_spec_boolean ("v2-tag", "add version 2 tag",
            "Add version 2 tag at start of file", TRUE,
            G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
    g_object_class_install_property (gobject_class, ARG_V1_TAG,
        g_param_spec_boolean ("v1-tag", "add version 1 tag",
            "Add version 1 tag at end of file", FALSE,
            G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
  }
  if (tag_class->type == GST_ID3_TAG_PARSE_MUX) {
    gst_element_class_add_pad_template (gstelement_class,
        gst_static_pad_template_get (&id3_tag_sink_any_template_factory));
  } else {
    gst_element_class_add_pad_template (gstelement_class,
        gst_static_pad_template_get (&id3_tag_sink_id3_template_factory));
  }
}

static GstCaps *
gst_id3_tag_get_caps (GstPad * pad)
{
  GstID3Tag *tag = GST_ID3_TAG (gst_pad_get_parent (pad));

  if (tag->found_caps) {
    GstCaps *caps = gst_caps_copy (tag->found_caps);

    if (CAN_BE_MUXER (tag)) {
      gst_caps_append (caps,
          gst_caps_from_string ("application/x-gst-tags; application/x-id3"));
    }
    return caps;
  } else {
    return gst_caps_copy (gst_pad_get_pad_template_caps (pad));
  }
}

static void
gst_id3_tag_add_src_pad (GstID3Tag * tag)
{
  g_assert (tag->srcpad == NULL);
  tag->srcpad =
      gst_pad_new_from_template (gst_element_class_get_pad_template
      (GST_ELEMENT_GET_CLASS (tag), "src"), "src");
  gst_pad_set_event_function (tag->srcpad,
      GST_DEBUG_FUNCPTR (gst_id3_tag_src_event));
  gst_pad_set_query_function (tag->srcpad,
      GST_DEBUG_FUNCPTR (gst_id3_tag_src_query));
  gst_pad_set_query_type_function (tag->srcpad,
      GST_DEBUG_FUNCPTR (gst_id3_tag_get_query_types));
  gst_pad_set_getcaps_function (tag->srcpad,
      GST_DEBUG_FUNCPTR (gst_id3_tag_get_caps));
  gst_pad_set_link_function (tag->srcpad,
      GST_DEBUG_FUNCPTR (gst_id3_tag_src_link));
  gst_element_add_pad (GST_ELEMENT (tag), tag->srcpad);
}

static void
gst_id3_tag_init (GTypeInstance * instance, gpointer g_class)
{
  GstID3Tag *tag = GST_ID3_TAG (instance);

  if (GST_ID3_TAG_GET_CLASS (tag)->type != GST_ID3_TAG_PARSE_BASE) {
    /* create the sink and src pads */
    tag->sinkpad =
        gst_pad_new_from_template (gst_element_class_get_pad_template
        (GST_ELEMENT_CLASS (g_class), "sink"), "sink");
    gst_element_add_pad (GST_ELEMENT (tag), tag->sinkpad);
    gst_pad_set_event_function (tag->sinkpad,
        GST_DEBUG_FUNCPTR (gst_id3_tag_sink_event));
    gst_pad_set_chain_function (tag->sinkpad,
        GST_DEBUG_FUNCPTR (gst_id3_tag_chain));
  }
  if (GST_ID3_TAG_GET_CLASS (tag)->type == GST_ID3_TAG_PARSE_MUX) {
    /* only the muxer class here, all other use sometimes pads */
    gst_id3_tag_add_src_pad (tag);
  }
  /* FIXME: for the alli^H^H^H^Hspider - gst_id3_tag_add_src_pad (tag); */
  tag->parse_mode = GST_ID3_TAG_PARSE_BASE;
  tag->buffer = NULL;
}

static void
gst_id3_tag_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  GstID3Tag *tag;

  tag = GST_ID3_TAG (object);

  switch (prop_id) {
    case ARG_V1_TAG:
      tag->v1tag_render = g_value_get_boolean (value);
      break;
    case ARG_V2_TAG:
      tag->v2tag_render = g_value_get_boolean (value);
      break;
    case ARG_PREFER_V1:
      tag->prefer_v1tag = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  /* make sure we render at least one tag */
  if (GST_ID3_TAG_GET_CLASS (tag)->type == GST_ID3_TAG_PARSE_MUX &&
      !tag->v1tag_render && !tag->v2tag_render) {
    g_object_set (object, prop_id == ARG_V1_TAG ? "v2-tag" : "v1-tag", TRUE,
        NULL);
  }
}
static void
gst_id3_tag_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstID3Tag *tag;

  tag = GST_ID3_TAG (object);

  switch (prop_id) {
    case ARG_V1_TAG:
      g_value_set_boolean (value, tag->v1tag_render);
      break;
    case ARG_V2_TAG:
      g_value_set_boolean (value, tag->v2tag_render);
      break;
    case ARG_PREFER_V1:
      g_value_set_boolean (value, tag->prefer_v1tag);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

#define gst_id3_tag_set_state(tag,new_state) G_STMT_START {				\
  GST_LOG_OBJECT (tag, "setting state to %s", #new_state );				\
  tag->state = new_state;								\
}G_STMT_END

static const GstQueryType *
gst_id3_tag_get_query_types (GstPad * pad)
{
  static const GstQueryType gst_id3_tag_src_query_types[] = {
    GST_QUERY_TOTAL,
    GST_QUERY_POSITION,
    0
  };

  return gst_id3_tag_src_query_types;
}

static gboolean
gst_id3_tag_src_query (GstPad * pad, GstQuery * query)
{
  gboolean res = FALSE;
  GstID3Tag *tag;

  tag = GST_ID3_TAG (gst_pad_get_parent (pad));

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:{
      GstFormat format;
      gint64 current, total;

      gst_query_parse_position (query, &format, NULL, NULL);
      switch (format) {
        case GST_FORMAT_BYTES:{
          GstPad *peer;

          if ((peer = gst_pad_get_peer (tag->sinkpad)) == NULL)
            break;

          if (tag->state == GST_ID3_TAG_STATE_NORMAL &&
              gst_pad_query_position (peer, &format, &current, &total)) {
            total -= tag->v2tag_size + tag->v1tag_size;
            total += tag->v2tag_size_new + tag->v1tag_size_new;
            if (tag->state == GST_ID3_TAG_STATE_NORMAL) {
              current -= tag->v2tag_size + tag->v2tag_size_new;
            } else {
              current = 0;
            }
            gst_query_set_position (query, format, current, total);

            res = TRUE;
          }
          gst_object_unref (peer);
          break;
        }
        default:
          break;
      }
      break;
    }
    default:
      break;
  }
  return res;
}

static gboolean
gst_id3_tag_src_event (GstPad * pad, GstEvent * event)
{
  GstID3Tag *tag;

  tag = GST_ID3_TAG (gst_pad_get_parent (pad));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
      if (GST_EVENT_SEEK_FORMAT (event) == GST_FORMAT_BYTES &&
          tag->state == GST_ID3_TAG_STATE_NORMAL &&
          GST_PAD_PEER (tag->sinkpad)) {
        GstEvent *new;
        gint diff = 0;

        switch (GST_EVENT_SEEK_METHOD (event)) {
          case GST_SEEK_METHOD_SET:
            diff = tag->v2tag_size - tag->v2tag_size_new;
            break;
          case GST_SEEK_METHOD_CUR:
            break;
          case GST_SEEK_METHOD_END:
            diff =
                GST_EVENT_SEEK_OFFSET (event) ? tag->v1tag_size_new -
                tag->v1tag_size : 0;
            break;
          default:
            g_assert_not_reached ();
            break;
        }
        new = gst_event_new_seek (GST_EVENT_SEEK_TYPE (event),
            GST_EVENT_SEEK_OFFSET (event) + diff);
        gst_event_unref (event);
        return gst_pad_push_event (tag->sinkpad, new);
      }
      break;
    default:
      break;
  }

  gst_event_unref (event);
  return FALSE;
}

GstTagList *
gst_mad_id3_to_tag_list (const struct id3_tag * tag)
{
  const struct id3_frame *frame;
  const id3_ucs4_t *ucs4;
  id3_utf8_t *utf8;
  GstTagList *tag_list;
  guint i = 0;

  tag_list = gst_tag_list_new ();

  while ((frame = id3_tag_findframe (tag, NULL, i++)) != NULL) {
    const union id3_field *field;
    unsigned int nstrings, j;
    const gchar *tag_name;

    /* find me the function to query the frame id */
    gchar *id = g_strndup (frame->id, 5);

    tag_name = gst_tag_from_id3_tag (id);
    if (tag_name == NULL) {
      g_free (id);
      continue;
    }

    if (strcmp (id, "COMM") == 0) {
      ucs4 = id3_field_getfullstring (&frame->fields[3]);
      g_assert (ucs4);

      utf8 = id3_ucs4_utf8duplicate (ucs4);
      if (utf8 == 0)
        continue;

      if (!g_utf8_validate ((char *) utf8, -1, NULL)) {
        g_warning ("converted string is not valid utf-8");
        g_free (utf8);
        continue;
      }

      gst_tag_list_add (tag_list, GST_TAG_MERGE_APPEND,
          GST_TAG_COMMENT, utf8, NULL);

      g_free (utf8);
      continue;
    }

    field = &frame->fields[1];
    nstrings = id3_field_getnstrings (field);

    for (j = 0; j < nstrings; ++j) {
      ucs4 = id3_field_getstrings (field, j);
      g_assert (ucs4);

      if (strcmp (id, ID3_FRAME_GENRE) == 0)
        ucs4 = id3_genre_name (ucs4);

      utf8 = id3_ucs4_utf8duplicate (ucs4);
      if (utf8 == 0)
        continue;

      if (!g_utf8_validate ((char *) utf8, -1, NULL)) {
        g_warning ("converted string is not valid utf-8");
        free (utf8);
        continue;
      }

      /* be sure to add non-string tags here */
      switch (gst_tag_get_type (tag_name)) {
        case G_TYPE_UINT:
        {
          guint tmp;
          gchar *check;

          tmp = strtoul ((char *) utf8, &check, 10);

          if (strcmp (tag_name, GST_TAG_DATE) == 0) {
            GDate *d;

            if (*check != '\0')
              break;
            if (tmp == 0)
              break;
            d = g_date_new_dmy (1, 1, tmp);
            tmp = g_date_get_julian (d);
            g_date_free (d);
          } else if (strcmp (tag_name, GST_TAG_TRACK_NUMBER) == 0) {
            if (*check == '/') {
              guint total;

              check++;
              total = strtoul (check, &check, 10);
              if (*check != '\0')
                break;

              gst_tag_list_add (tag_list, GST_TAG_MERGE_APPEND,
                  GST_TAG_TRACK_COUNT, total, NULL);
            }
          } else if (strcmp (tag_name, GST_TAG_ALBUM_VOLUME_NUMBER) == 0) {
            if (*check == '/') {
              guint total;

              check++;
              total = strtoul (check, &check, 10);
              if (*check != '\0')
                break;

              gst_tag_list_add (tag_list, GST_TAG_MERGE_APPEND,
                  GST_TAG_ALBUM_VOLUME_COUNT, total, NULL);
            }
          }

          if (*check != '\0')
            break;
          gst_tag_list_add (tag_list, GST_TAG_MERGE_APPEND, tag_name, tmp,
              NULL);
          break;
        }
        case G_TYPE_UINT64:
        {
          guint64 tmp;

          g_assert (strcmp (tag_name, GST_TAG_DURATION) == 0);
          tmp = strtoul ((char *) utf8, NULL, 10);
          if (tmp == 0) {
            break;
          }
          gst_tag_list_add (tag_list, GST_TAG_MERGE_APPEND,
              GST_TAG_DURATION, tmp * 1000 * 1000, NULL);
          break;
        }
        default:
          g_assert (gst_tag_get_type (tag_name) == G_TYPE_STRING);
          gst_tag_list_add (tag_list, GST_TAG_MERGE_APPEND, tag_name, utf8,
              NULL);
          break;
      }
      free (utf8);
    }
    g_free (id);
  }

  return tag_list;
}
static void
tag_list_to_id3_tag_foreach (const GstTagList * list, const gchar * tag_name,
    gpointer user_data)
{
  struct id3_frame *frame;
  union id3_field *field;
  guint values = gst_tag_list_get_tag_size (list, tag_name);
  const gchar *id = gst_tag_to_id3_tag (tag_name);
  struct id3_tag *tag = (struct id3_tag *) user_data;

  if (id == NULL)
    return;

  if (values == 0)
    return;

  frame = id3_frame_new (id);
  if (id3_tag_attachframe (tag, frame) != 0) {
    GST_WARNING ("could not attach frame (%s) to id3 tag", id);
    return;
  }
  /* encode in UTF-8 - libid3tag uses Latin1 by default... */
  field = id3_frame_field (frame, 0);
  id3_field_settextencoding (field, ID3_FIELD_TEXTENCODING_UTF_8);
  field = id3_frame_field (frame, 1);
  g_assert (field);
  while (values-- > 0) {
    gunichar *put;

    if (strcmp (tag_name, GST_TAG_DATE) == 0) {
      gchar *str;
      guint u;
      GDate *d;

      if (!gst_tag_list_get_uint_index (list, tag_name, values, &u))
        g_assert_not_reached ();
      d = g_date_new_julian (u);
      str = g_strdup_printf ("%u", (guint) (g_date_get_year (d)));
      put = g_utf8_to_ucs4_fast (str, -1, NULL);
      g_date_free (d);
      g_free (str);
    } else if (strcmp (tag_name, GST_TAG_TRACK_NUMBER) == 0) {
      gchar *str;
      guint u;

      if (!gst_tag_list_get_uint_index (list, tag_name, values, &u))
        g_assert_not_reached ();
      str = g_strdup_printf ("%u", u);
      put = g_utf8_to_ucs4_fast (str, -1, NULL);
      g_free (str);
    } else if (strcmp (tag_name, GST_TAG_COMMENT) == 0) {
      gchar *str;
      id3_ucs4_t ucs4_empty[] = { 0 };

      if (!gst_tag_list_get_string_index (list, tag_name, values, &str))
        g_assert_not_reached ();
      put = g_utf8_to_ucs4_fast (str, -1, NULL);
      g_free (str);

      if (id3_field_setlanguage (&frame->fields[1], "XXX") == -1 ||
          id3_field_setstring (&frame->fields[2], ucs4_empty) == -1 ||
          id3_field_setfullstring (&frame->fields[3], (id3_ucs4_t *) put) == -1)
        GST_WARNING ("could not add a string to the id3 COMM field");

      g_free (put);
      return;
    } else {
      gchar *str;

      if (gst_tag_get_type (tag_name) != G_TYPE_STRING) {
        GST_WARNING ("unhandled GStreamer tag %s", tag_name);
        return;
      }
      if (!gst_tag_list_get_string_index (list, tag_name, values, &str))
        g_assert_not_reached ();
      put = g_utf8_to_ucs4_fast (str, -1, NULL);
      g_free (str);
    }
    if (id3_field_addstring (field, (id3_ucs4_t *) put) != 0) {
      GST_WARNING ("could not add a string to id3 tag field");
      g_free (put);
      return;
    }
  }
  id3_field_settextencoding (field, ID3_FIELD_TEXTENCODING_UTF_8);
}
struct id3_tag *
gst_mad_tag_list_to_id3_tag (GstTagList * list)
{
  struct id3_tag *tag;

  tag = id3_tag_new ();

  gst_tag_list_foreach (list, tag_list_to_id3_tag_foreach, tag);
  return tag;
}
static GstTagList *
gst_id3_tag_get_tag_to_render (GstID3Tag * tag)
{
  GstTagList *ret = NULL;

  if (tag->event_tags)
    ret = gst_tag_list_copy (tag->event_tags);
  if (ret) {
    if (tag->parsed_tags)
      gst_tag_list_insert (ret, tag->parsed_tags, GST_TAG_MERGE_KEEP);
  } else if (tag->parsed_tags) {
    ret = gst_tag_list_copy (tag->parsed_tags);
  }
  if (ret && gst_tag_setter_get_list (GST_TAG_SETTER (tag))) {
    gst_tag_list_insert (ret, gst_tag_setter_get_list (GST_TAG_SETTER (tag)),
        gst_tag_setter_get_merge_mode (GST_TAG_SETTER (tag)));
  } else if (gst_tag_setter_get_list (GST_TAG_SETTER (tag))) {
    ret = gst_tag_list_copy (gst_tag_setter_get_list (GST_TAG_SETTER (tag)));
  }
  return ret;
}

static gboolean
gst_id3_tag_sink_event (GstPad * pad, GstEvent * event)
{
  GstID3Tag *tag = GST_ID3_TAG (gst_pad_get_parent (pad));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_DISCONTINUOUS:
      switch (tag->state) {
        case GST_ID3_TAG_STATE_READING_V2_TAG:{
          gint64 value, end_value;

          if (gst_event_discont_get_value (event, GST_FORMAT_BYTES, &value,
                  &end_value)
              || gst_event_discont_get_value (event, GST_FORMAT_DEFAULT, &value,
                  &end_value)) {
            if (value !=
                (tag->buffer ? GST_BUFFER_OFFSET (tag->buffer) +
                    GST_BUFFER_SIZE (tag->buffer)
                    : 0))
              GST_ELEMENT_ERROR (tag, CORE, EVENT, (NULL),
                  ("Got seek to %" G_GINT64_FORMAT " during ID3v2 tag reading"
                      " (allowed was %" G_GINT64_FORMAT ")", value,
                      (guint64) (tag->buffer ? GST_BUFFER_OFFSET (tag->buffer)
                          + GST_BUFFER_SIZE (tag->buffer) : 0)));
          }
          gst_event_unref (event);
          break;
        }
        case GST_ID3_TAG_STATE_SEEKING_TO_V1_TAG:
          /* just assume it's the right seek for now */
          gst_id3_tag_set_state (tag, GST_ID3_TAG_STATE_READING_V1_TAG);
          gst_event_unref (event);
          break;
        case GST_ID3_TAG_STATE_READING_V1_TAG:
          GST_ELEMENT_ERROR (tag, CORE, EVENT, (NULL),
              ("Seek during ID3v1 tag reading"));
          gst_event_unref (event);
          break;
        case GST_ID3_TAG_STATE_SEEKING_TO_NORMAL:
          /* just assume it's the right seek for now */
          gst_id3_tag_set_state (tag, GST_ID3_TAG_STATE_NORMAL_START);
          gst_event_unref (event);
          break;
        case GST_ID3_TAG_STATE_NORMAL_START:
          if (!CAN_BE_DEMUXER (tag)) {
            /* initial discont, ignore */
            gst_event_unref (event);
            break;
          } else {
            GST_ERROR_OBJECT (tag, "tag event not sent, FIXME");
            gst_id3_tag_set_state (tag, GST_ID3_TAG_STATE_NORMAL);
            /* fall through */
          }
        case GST_ID3_TAG_STATE_NORMAL:{
          gint64 value, end_value;
          GstEvent *new;

          if (gst_event_discont_get_value (event, GST_FORMAT_BYTES, &value,
                  &end_value)) {
            if (value > tag->v2tag_size) {
              value -= tag->v2tag_size;
            } else {
              /* FIXME: throw an error here? */
              value = 0;
            }
            new =
                gst_event_new_discontinuous (FALSE, GST_FORMAT_BYTES, value, 0);
            gst_event_unref (event);
            gst_pad_push_event (tag->srcpad, new);
          } else {
            gst_pad_event_default (pad, event);
          }
          break;
        }
        default:
          g_assert_not_reached ();
      }
      break;
    case GST_EVENT_TAG:
      if (tag->event_tags) {
        gst_tag_list_insert (tag->event_tags, gst_event_tag_get_list (event),
            GST_TAG_MERGE_PREPEND);
      } else {
        tag->event_tags = gst_tag_list_copy (gst_event_tag_get_list (event));
      }
      gst_event_unref (event);
      break;
    case GST_EVENT_EOS:
      if (tag->v1tag_render && IS_MUXER (tag)) {
        GstTagList *merged;
        struct id3_tag *id3;

        GST_LOG_OBJECT (tag, "rendering v1 tag after eos event");
        merged = gst_id3_tag_get_tag_to_render (tag);
        if (merged) {
          id3 = gst_mad_tag_list_to_id3_tag (merged);
          if (id3) {
            GstBuffer *tag_buffer;

            id3_tag_options (id3, ID3_TAG_OPTION_ID3V1, ID3_TAG_OPTION_ID3V1);
            tag_buffer = gst_buffer_new_and_alloc (128);
            if (128 != id3_tag_render (id3, tag_buffer->data))
              g_assert_not_reached ();
            gst_pad_push (tag->srcpad, tag_buffer);
            id3_tag_delete (id3);
          }
          gst_tag_list_free (merged);
        }
      }
      /* fall through */
    default:
      gst_pad_event_default (pad, event);
      break;
  }
  return TRUE;
}
typedef struct
{
  guint best_probability;
  GstCaps *caps;
  GstBuffer *buffer;
}
SimpleTypeFind;
guint8 *
simple_find_peek (gpointer data, gint64 offset, guint size)
{
  SimpleTypeFind *find = (SimpleTypeFind *) data;

  if (offset < 0)
    return NULL;

  if (GST_BUFFER_SIZE (find->buffer) >= offset + size) {
    return GST_BUFFER_DATA (find->buffer) + offset;
  }
  return NULL;
}
static void
simple_find_suggest (gpointer data, guint probability, const GstCaps * caps)
{
  SimpleTypeFind *find = (SimpleTypeFind *) data;

  if (probability > find->best_probability) {
    gst_caps_replace (&find->caps, gst_caps_copy (caps));
    find->best_probability = probability;
  }
}
static GstCaps *
gst_id3_tag_do_typefind (GstID3Tag * tag, GstBuffer * buffer)
{
  GList *walk, *type_list;
  SimpleTypeFind find;
  GstTypeFind gst_find;

  /* this will help us detecting the media stream type after
   * this id3 thingy... Please note that this is a cruel hack
   * for as long as spider doesn't support multi-type-finding.
   */
  walk = type_list = gst_type_find_factory_get_list ();

  find.buffer = buffer;
  find.best_probability = 0;
  find.caps = NULL;
  gst_find.data = &find;
  gst_find.peek = simple_find_peek;
  gst_find.get_length = NULL;
  gst_find.suggest = simple_find_suggest;
  while (walk) {
    GstTypeFindFactory *factory = GST_TYPE_FIND_FACTORY (walk->data);

    gst_type_find_factory_call_function (factory, &gst_find);
    if (find.best_probability >= GST_TYPE_FIND_MAXIMUM)
      break;
    walk = g_list_next (walk);
  }
  g_list_free (type_list);
  if (find.best_probability > 0) {
    return find.caps;
  } else {
    GST_ELEMENT_ERROR (tag, CORE, CAPS, (NULL), ("no caps found"));
    return NULL;
  }
}
static gboolean
gst_id3_tag_do_caps_nego (GstID3Tag * tag, GstBuffer * buffer)
{
  if (buffer != NULL && CAN_BE_DEMUXER (tag)) {
    tag->found_caps = gst_id3_tag_do_typefind (tag, buffer);
    if (!tag->found_caps) {
      return FALSE;
    }
  }
  if (!tag->srcpad)
    gst_id3_tag_add_src_pad (tag);
  if (!gst_pad_is_linked (tag->srcpad)) {
    GST_DEBUG_OBJECT (tag, "srcpad not linked, not proceeding");
    tag->parse_mode = GST_ID3_TAG_GET_CLASS (tag)->type;
    return TRUE;
  } else {
    GST_DEBUG_OBJECT (tag, "renegotiating");
    //return gst_pad_renegotiate (tag->srcpad) != GST_PAD_LINK_REFUSED;
    return TRUE;
  }
}

static GstPadLinkReturn
gst_id3_tag_src_link (GstPad * pad, GstPad * peer)
{
  GstID3Tag *tag;

  //const gchar *mimetype;

  tag = GST_ID3_TAG (gst_pad_get_parent (pad));

#if 0
  if (!tag->found_caps && CAN_BE_DEMUXER (tag))
    return GST_PAD_LINK_DELAYED;
  if (!CAN_BE_MUXER (tag) || !CAN_BE_DEMUXER (tag)) {
    tag->parse_mode = GST_ID3_TAG_GET_CLASS (tag)->type;
    return GST_PAD_LINK_OK;
  }

  mimetype = gst_structure_get_name (gst_caps_get_structure (caps, 0));

  if (strcmp (mimetype, "application/x-id3") == 0) {
    tag->parse_mode = GST_ID3_TAG_PARSE_MUX;
    GST_LOG_OBJECT (tag, "normal operation, using application/x-id3 output");
  } else if (strcmp (mimetype, "application/x-gst-tags") == 0) {
    tag->parse_mode = GST_ID3_TAG_PARSE_ANY;
    GST_LOG_OBJECT (tag, "fast operation, just outputting tags");
  } else {
    tag->parse_mode = GST_ID3_TAG_PARSE_DEMUX;
    GST_LOG_OBJECT (tag, "parsing operation, extracting tags");
  }
#endif
  if (GST_PAD_LINKFUNC (peer))
    return GST_PAD_LINKFUNC (peer) (peer, pad);
  else
    return GST_PAD_LINK_OK;
}
static void
gst_id3_tag_send_tag_event (GstID3Tag * tag)
{
  /* FIXME: what's the correct merge mode? Docs need to tell... */
  GstTagList *merged = gst_tag_list_merge (tag->event_tags, tag->parsed_tags,
      GST_TAG_MERGE_KEEP);

  if (tag->parsed_tags)
    gst_element_post_message (GST_ELEMENT (tag),
        gst_message_new_tag (GST_OBJECT (tag),
            gst_tag_list_copy (tag->parsed_tags)));

  if (merged) {
    GstEvent *event = gst_event_new_tag (merged);

    GST_EVENT_TIMESTAMP (event) = 0;
    gst_pad_push_event (tag->srcpad, event);
  }
}
static GstFlowReturn
gst_id3_tag_chain (GstPad * pad, GstBuffer * buffer)
{
  GstID3Tag *tag;

  tag = GST_ID3_TAG (gst_pad_get_parent (pad));
  GST_DEBUG_OBJECT (tag, "Chain, state = %d", tag->state);

  switch (tag->state) {
    case GST_ID3_TAG_STATE_SEEKING_TO_V1_TAG:
    case GST_ID3_TAG_STATE_SEEKING_TO_NORMAL:
      /* we're waiting for the seek to finish, just discard all the stuff */
      gst_buffer_unref (buffer);
      return GST_FLOW_OK;
    case GST_ID3_TAG_STATE_READING_V1_TAG:
      if (tag->buffer) {
        GstBuffer *temp;

        temp = gst_buffer_merge (tag->buffer, buffer);
        gst_buffer_unref (tag->buffer);
        tag->buffer = temp;
        gst_buffer_unref (buffer);
      } else {
        tag->buffer = buffer;
        tag->v1tag_offset = buffer->offset;
      }
      if (GST_BUFFER_SIZE (tag->buffer) < 128)
        return GST_FLOW_OK;
      g_assert (tag->v1tag_size == 0);
      tag->v1tag_size = id3_tag_query (GST_BUFFER_DATA (tag->buffer),
          GST_BUFFER_SIZE (tag->buffer));
      if (tag->v1tag_size == 128) {
        GstTagList *newtag;

        newtag = gst_tag_list_new_from_id3v1 (GST_BUFFER_DATA (tag->buffer));
        GST_LOG_OBJECT (tag, "have read ID3v1 tag");
        if (newtag) {
          if (tag->parsed_tags) {
            /* FIXME: use append/prepend here ? */
            gst_tag_list_insert (tag->parsed_tags, newtag,
                tag->prefer_v1tag ? GST_TAG_MERGE_REPLACE : GST_TAG_MERGE_KEEP);
            gst_tag_list_free (newtag);
          } else {
            tag->parsed_tags = newtag;
          }
        } else {
          GST_WARNING_OBJECT (tag, "detected ID3v1 tag, but couldn't parse it");
        }
      } else {
        if (tag->v1tag_size != 0) {
          GST_WARNING_OBJECT (tag, "bad non-ID3v1 tag at end of file");
        } else {
          GST_LOG_OBJECT (tag, "no ID3v1 tag (%" G_GUINT64_FORMAT ")",
              GST_BUFFER_OFFSET (tag->buffer));
          tag->v1tag_offset = G_MAXUINT64;
        }
      }
      gst_buffer_unref (tag->buffer);
      tag->buffer = NULL;
      if (tag->parse_mode != GST_ID3_TAG_PARSE_ANY) {
        /* seek to beginning */
        GST_LOG_OBJECT (tag, "seeking back to beginning");
        gst_id3_tag_set_state (tag, GST_ID3_TAG_STATE_SEEKING_TO_NORMAL);
        if (!gst_pad_push_event (tag->sinkpad,
                gst_event_new_seek (GST_FORMAT_BYTES | GST_SEEK_METHOD_SET |
                    GST_SEEK_FLAG_FLUSH, tag->v2tag_size))) {
          GST_ELEMENT_ERROR (tag, CORE, SEEK, (NULL),
              ("can't seek back to beginning from reading ID3v1 tag"));
        }
      } else {
        gst_id3_tag_send_tag_event (tag);
        /* set eos, we're done parsing tags */
        GST_LOG_OBJECT (tag, "setting EOS after reading ID3v1 tag");
        gst_id3_tag_set_state (tag, GST_ID3_TAG_STATE_NORMAL);
        //gst_element_set_eos (GST_ELEMENT (tag));
        gst_pad_push_event (tag->srcpad, gst_event_new (GST_EVENT_EOS));
      }
      return GST_FLOW_OK;
    case GST_ID3_TAG_STATE_READING_V2_TAG:
      if (tag->buffer) {
        GstBuffer *temp;

        temp = gst_buffer_merge (tag->buffer, buffer);
        gst_buffer_unref (tag->buffer);
        tag->buffer = temp;
        gst_buffer_unref (buffer);
      } else {
        tag->buffer = buffer;
      }
      if (GST_BUFFER_SIZE (tag->buffer) < 10)
        return GST_FLOW_OK;
      if (tag->v2tag_size == 0) {
        tag->v2tag_size = id3_tag_query (GST_BUFFER_DATA (tag->buffer),
            GST_BUFFER_SIZE (tag->buffer));
        /* no footers supported */
        if (tag->v2tag_size < 0)
          tag->v2tag_size = 0;
      }
      if (GST_BUFFER_SIZE (tag->buffer) < tag->v2tag_size + ID3_TYPE_FIND_SIZE)
        return GST_FLOW_OK;
      if (tag->v2tag_size != 0) {
        struct id3_tag *v2tag;

        v2tag = id3_tag_parse (GST_BUFFER_DATA (tag->buffer),
            GST_BUFFER_SIZE (tag->buffer));
        if (v2tag) {
          GstTagList *list;

          list = gst_mad_id3_to_tag_list (v2tag);
          id3_tag_delete (v2tag);
          GST_LOG_OBJECT (tag, "parsed ID3v2 tag");
          /* no other tag parsed yet */
          g_assert (tag->parsed_tags == NULL);
          tag->parsed_tags = list;
        } else {
          GST_WARNING_OBJECT (tag, "detected ID3v2 tag, but couldn't parse it");
        }
      }
      /* caps nego and typefinding */
      GST_LOG_OBJECT (tag,
          "removing first %ld bytes, because they're the ID3v2 tag",
          tag->v2tag_size);
      buffer =
          gst_buffer_create_sub (tag->buffer, tag->v2tag_size,
          GST_BUFFER_SIZE (tag->buffer) - tag->v2tag_size);
      /* the offsets will be corrected further down, we just copy them */
      if (GST_BUFFER_OFFSET_IS_VALID (tag->buffer))
        GST_BUFFER_OFFSET (buffer) =
            GST_BUFFER_OFFSET (tag->buffer) + tag->v2tag_size;
      if (GST_BUFFER_OFFSET_END_IS_VALID (tag->buffer))
        GST_BUFFER_OFFSET_END (buffer) =
            GST_BUFFER_OFFSET_END (tag->buffer) + tag->v2tag_size;
      gst_buffer_unref (tag->buffer);
      tag->buffer = NULL;
      if (tag->found_caps == NULL)
        if (!gst_id3_tag_do_caps_nego (tag, buffer))
          return GST_FLOW_OK;
      /* seek to ID3v1 tag */
      gst_id3_tag_set_state (tag, GST_ID3_TAG_STATE_SEEKING_TO_V1_TAG);
      if (gst_pad_push_event (tag->sinkpad,
              gst_event_new_seek (GST_FORMAT_BYTES | GST_SEEK_METHOD_END |
                  GST_SEEK_FLAG_FLUSH, -128))) {
        gst_buffer_unref (buffer);
        return GST_FLOW_OK;
      }
      gst_id3_tag_set_state (tag, GST_ID3_TAG_STATE_NORMAL_START);
      /* fall through */
    case GST_ID3_TAG_STATE_NORMAL_START:
      g_assert (tag->buffer == NULL);
      gst_id3_tag_send_tag_event (tag);

      if (IS_MUXER (tag) && tag->v2tag_render) {
        struct id3_tag *id3;
        GstTagList *merged;
        GstBuffer *tag_buffer;

        /* render tag */
        tag->v2tag_size_new = 0;
        merged = gst_id3_tag_get_tag_to_render (tag);
        if (merged) {
          id3 = gst_mad_tag_list_to_id3_tag (merged);
          if (id3) {
            glong estimated;

            estimated = id3_tag_render (id3, NULL);
            tag_buffer = gst_buffer_new_and_alloc (estimated);
            tag->v2tag_size_new =
                id3_tag_render (id3, GST_BUFFER_DATA (tag_buffer));
            g_assert (estimated >= tag->v2tag_size_new);
            GST_BUFFER_SIZE (tag_buffer) = tag->v2tag_size_new;
            gst_pad_push (tag->srcpad, tag_buffer);
            id3_tag_delete (id3);
          }
          gst_tag_list_free (merged);
        }
      }
      gst_id3_tag_set_state (tag, GST_ID3_TAG_STATE_NORMAL);
      tag->v1tag_size_new = (tag->v1tag_render &&
          IS_MUXER (tag) &&
          (tag->parsed_tags != NULL ||
              gst_tag_setter_get_list (GST_TAG_SETTER (tag)) !=
              NULL)) ? 128 : 0;
      /* fall through */
    case GST_ID3_TAG_STATE_NORMAL:
      if (tag->parse_mode == GST_ID3_TAG_PARSE_ANY) {
        gst_buffer_unref (buffer);
        //gst_element_set_eos (GST_ELEMENT (tag));
        gst_pad_push_event (tag->srcpad, gst_event_new (GST_EVENT_EOS));
      } else {
        if (GST_BUFFER_OFFSET_IS_VALID (buffer)) {
          if (buffer->offset >= tag->v1tag_offset) {
            gst_buffer_unref (buffer);
            return GST_FLOW_OK;
          } else if (buffer->offset + buffer->size > tag->v1tag_offset) {
            GstBuffer *sub = gst_buffer_create_sub (buffer, 0,
                buffer->size - 128);

            gst_buffer_unref (buffer);
            buffer = sub;
          }
        }
        if (tag->v2tag_size) {
          GstBuffer *sub =
              gst_buffer_create_sub (buffer, 0, GST_BUFFER_SIZE (buffer));
          if (GST_BUFFER_OFFSET_IS_VALID (buffer))
            GST_BUFFER_OFFSET (sub) =
                GST_BUFFER_OFFSET (buffer) - tag->v2tag_size +
                tag->v2tag_size_new;
          if (GST_BUFFER_OFFSET_END_IS_VALID (buffer))
            GST_BUFFER_OFFSET_END (sub) =
                GST_BUFFER_OFFSET_END (buffer) - tag->v2tag_size +
                tag->v2tag_size_new;
          gst_buffer_unref (buffer);
          buffer = sub;
        }
        gst_pad_push (tag->srcpad, buffer);
      }
      return GST_FLOW_OK;
  }
  return GST_FLOW_OK;
}

static GstElementStateReturn
gst_id3_tag_change_state (GstElement * element)
{
  GstID3Tag *tag;

  tag = GST_ID3_TAG (element);

  switch (GST_STATE_TRANSITION (element)) {
    case GST_STATE_NULL_TO_READY:
      break;
    case GST_STATE_READY_TO_PAUSED:
      g_assert (tag->parsed_tags == NULL);
      g_assert (tag->buffer == NULL);
      tag->v1tag_size = 0;
      tag->v1tag_offset = G_MAXUINT64;
      tag->v2tag_size = 0;
      if (CAN_BE_DEMUXER (tag)) {
        gst_id3_tag_set_state (tag, GST_ID3_TAG_STATE_READING_V2_TAG);
      } else {
        gst_id3_tag_set_state (tag, GST_ID3_TAG_STATE_NORMAL_START);
      }
      break;
    case GST_STATE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_PAUSED_TO_READY:
      if (tag->parsed_tags) {
        gst_tag_list_free (tag->parsed_tags);
        tag->parsed_tags = NULL;
      }
      if (tag->event_tags) {
        gst_tag_list_free (tag->event_tags);
        tag->event_tags = NULL;
      }
      if (tag->buffer) {
        gst_buffer_unref (tag->buffer);
        tag->buffer = NULL;
      }
      if (tag->found_caps) {
        gst_caps_unref (tag->found_caps);
        tag->found_caps = NULL;
      }
      tag->parse_mode = GST_ID3_TAG_PARSE_BASE;
      break;
    case GST_STATE_READY_TO_NULL:
      break;
  }

  return parent_class->change_state (element);
}

/*** PLUGIN INITIALIZATION ****************************************************/

static gboolean
plugin_init (GstPlugin * plugin)
{

  if (!gst_element_register (plugin, "mad", GST_RANK_PRIMARY,
          gst_mad_get_type ())
      || !gst_element_register (plugin, "id3demux", GST_RANK_PRIMARY,
          gst_id3_tag_get_type (GST_ID3_TAG_PARSE_DEMUX))
      || !gst_element_register (plugin, "id3mux", GST_RANK_NONE,        /* removed for spider */
          gst_id3_tag_get_type (GST_ID3_TAG_PARSE_MUX))
      /* FIXME 0.9: remove this element */
      || !gst_element_register (plugin, "id3tag", GST_RANK_NONE,
          gst_id3_tag_get_type (GST_ID3_TAG_PARSE_ANY))) {
    return FALSE;
  }

  GST_DEBUG_CATEGORY_INIT (gst_id3_tag_debug, "id3tag", 0,
      "id3 tag reader / setter");

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "mad",
    "id3 tag manipulation and mp3 decoding based on the mad library",
    plugin_init, VERSION, "GPL", GST_PACKAGE, GST_ORIGIN)
