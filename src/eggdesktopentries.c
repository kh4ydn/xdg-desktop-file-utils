/* eggdesktopentries.c - desktop entries parser
 *
 *  Copyright 2004  Ray Strode <halfline@hawaii.rr.com>
 *
 * GLib is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * GLib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GLib; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *   Boston, MA 02111-1307, USA.
 */

#include "eggdesktopentries.h"
#include "eggdirfuncs.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "glib.h"
#include "eggintl.h"

#define EGG_DESKTOP_ENTRIES_DEFAULT_GROUP "Desktop Entry"
#define EGG_DESKTOP_ENTRIES_LEGACY_GROUP "KDE Desktop Entry"

typedef struct _EggDesktopEntriesGroup EggDesktopEntriesGroup;

typedef enum
{
  EGG_DESKTOP_ENTRIES_ENCODING_GUESS = 0,
  EGG_DESKTOP_ENTRIES_ENCODING_UTF8,
  EGG_DESKTOP_ENTRIES_ENCODING_MIXED
} EggDesktopEntriesEncoding;

struct _EggDesktopEntries
{
  GList *groups;
  gchar **locales;
  char *default_group_name;

  EggDesktopEntriesGroup *current_group;

  GString *parse_buffer; /* Holds up to one line of not-yet-parsed data */

  EggDesktopEntriesEncoding encoding;
  EggDesktopEntriesFlags flags;

  /* Used for sizing the output buffer during serialization
   */
  gsize approximate_size; 
};

struct _EggDesktopEntriesGroup
{
  const char *name;  /* NULL for above first group (which will be comments) */
  GList *entries;

  /* Used only for increased lookup performance
   */
  GHashTable *lookup_map;
};

typedef struct 
{
  char *key;  /* NULL for comments */
  char *value;
} EggDesktopEntry;


static gint egg_find_file_in_user_data_dir       (const char  *file, 
					          GError     **error);
static gint egg_find_file_in_secondary_data_dirs (const char  *file,
						  GError     **error);
static gint egg_find_file_in_data_dir            (const gchar *file,
						  GError **error);

static EggDesktopEntriesGroup *egg_desktop_entries_lookup_group (EggDesktopEntries *entries, 
                                                             const gchar      *group_name);
static EggDesktopEntry        *egg_desktop_entries_lookup_entry (EggDesktopEntries       *entries,
		 		                             EggDesktopEntriesGroup  *group,
				                             const gchar           *key);

static void egg_desktop_entries_remove_group_node (EggDesktopEntries  *entries,
                                                 GList            *group_node);

static gchar    *egg_desktop_entries_get_locale_modifier   (const gchar *locale);
static gchar    *egg_desktop_entries_get_locale_country    (const gchar *locale);
static gchar    *egg_desktop_entries_get_locale_lang       (const gchar *locale);
static gchar    *egg_desktop_entries_get_locale_encoding   (const gchar *locale);
static gchar    *egg_desktop_entries_get_fallback_encoding (const gchar *locale);

static gboolean line_is_comment (const gchar *line);
static gboolean line_is_group   (const gchar *line);
static gboolean line_is_entry   (const gchar *line);

static gchar    *egg_desktop_entries_key_to_utf8 (EggDesktopEntries *entries,
					        const gchar     *key,
					        const gchar     *value);
static gchar    *egg_desktop_entries_parse_value_as_string (EggDesktopEntries  *entries,
					                  const gchar      *value,
					                   GError          **error);

static gint      egg_desktop_entries_parse_value_as_integer (EggDesktopEntries  *entries,
						         const gchar    *value,
						         GError        **error);
static gboolean  egg_desktop_entries_parse_value_as_boolean (EggDesktopEntries  *entries,
							 const gchar    *value,
							 GError        **error);

static void       egg_desktop_entries_parse_comment (EggDesktopEntries  *entries,
  	                                           const char       *line,
			                           gsize             length,
					           GError          **error);
static void      egg_desktop_entries_parse_group (EggDesktopEntries  *entries,
	                                        const char       *line,
			                        gsize             length,
					        GError          **error);
static void      egg_desktop_entries_parse_entry (EggDesktopEntries  *entries,
	                                       const gchar       *line,
			                       gsize             length,
			                       GError          **error);

static gchar    *key_get_locale (const gchar *key);

GQuark
egg_desktop_entries_error_quark (void)
{
  static GQuark error_quark = 0;

  if (error_quark == 0)
    error_quark = g_quark_from_static_string ("g-desktop-entries-error-quark");

  return error_quark;
}

EggDesktopEntries *
egg_desktop_entries_new (EggDesktopEntriesFlags flags)
{
  EggDesktopEntries *entries;

  entries = g_new (EggDesktopEntries, 1);

  entries->current_group = g_new0 (EggDesktopEntriesGroup, 1);
  entries->groups = g_list_prepend (NULL, entries->current_group);
  entries->default_group_name = NULL;
  entries->locales = NULL;
  entries->parse_buffer = g_string_sized_new (128);
  entries->encoding = EGG_DESKTOP_ENTRIES_ENCODING_GUESS;
  entries->flags = flags;
  entries->approximate_size = 0;

  if (entries->flags & EGG_DESKTOP_ENTRIES_DISCARD_TRANSLATIONS)
    {
      gchar *locales[] = { NULL };

      egg_desktop_entries_keep_locales (entries, locales);
    }

  return entries;
}

static gint
egg_find_file_in_user_data_dir (const char *file, GError **error)
{
  gchar *data_dir, *path;
  gint fd;

  path = NULL;
  fd = -1;

  data_dir = egg_get_user_data_dir ();

  if (data_dir) 
    {
      path = g_build_filename (data_dir, file, NULL);
      g_free (data_dir);
      data_dir = NULL;
    }

  fd = open (path, O_RDONLY);

  if (fd < 0)
    g_set_error (error, G_FILE_ERROR,
                 g_file_error_from_errno (errno),
		 _("File could not be opened: %s"),
                 g_strerror (errno));

  return fd;
}

static gint
egg_find_file_in_secondary_data_dirs (const char *file, GError **error)
{
  gchar *data_dir, *path, **secondary_data_dirs;
  int i, fd;
  GError *file_error;

  file_error = NULL;
  path = NULL;
  fd = -1;

  secondary_data_dirs = egg_get_secondary_data_dirs ();

  i = 0;
  while (secondary_data_dirs && (data_dir = secondary_data_dirs[i]) && fd < 0)
    {
        path = g_build_filename (data_dir, file, NULL);

        fd = open (path, O_RDONLY);

        if (fd < 0 && !file_error)
          g_set_error (error, G_FILE_ERROR,
                       g_file_error_from_errno (errno),
                       _("File could not be opened: %s"),
                       g_strerror (errno));
        i++;
    }

  g_strfreev (secondary_data_dirs);

  if (file_error)
    g_propagate_error (error, file_error);

  return fd;
}

static gint
egg_find_file_in_data_dir (const gchar *file, GError **error)
{
  gint fd;
  GError *file_error;
  GError *secondary_error;

  file_error = NULL;
  secondary_error = NULL;

  fd = egg_find_file_in_user_data_dir (file, &file_error);

  if (fd < 0)
    { 
      fd = egg_find_file_in_secondary_data_dirs (file, &secondary_error);

      if (fd >= 0)
       {
         g_error_free (file_error);
         file_error = NULL;
       }
    }

  if (file_error)
    g_propagate_error (error, file_error);
  else if (secondary_error)
    g_propagate_error (error, secondary_error);

  return fd;
}

EggDesktopEntries *
egg_desktop_entries_new_from_file (EggDesktopEntriesFlags   flags,
  	                         const gchar           *file,
                                 GError               **error)
{
  EggDesktopEntries *entries;
  GError *entries_error;
  gsize bytes_read;
  gint fd;
  struct stat stat_buf;
  gchar read_buf[1024];
  int saved_errno;
  
  entries_error = NULL;

  fd = open (file, O_RDONLY);

  if (fd < 0)
    {
      saved_errno = errno;

      if (!g_path_is_absolute (file))
        fd = egg_find_file_in_data_dir (file, &entries_error);

      if (fd < 0)
	{
          if (entries_error)
	    g_propagate_error (error, entries_error);
          else
            g_set_error (error, G_FILE_ERROR,
                         g_file_error_from_errno (saved_errno),
                         _("Failed to open file '%s': %s"),
                         file, g_strerror (saved_errno));

	  return NULL;
	}
    }

  fstat (fd, &stat_buf);

  if (stat_buf.st_size == 0)
    {
      g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		   EGG_DESKTOP_ENTRIES_ERROR_PARSE,
		   _("File is empty"));
      return NULL;
    }

  entries = egg_desktop_entries_new (flags);

  entries_error = NULL;
  bytes_read = 0;
  do
    {
      bytes_read = read (fd, read_buf,  1024);

      if (bytes_read == 0)  /* End of File */
        break;

      if (bytes_read < 0)
        {
          if (errno == EINTR)
            continue;

          g_set_error (error, G_FILE_ERROR,
                       g_file_error_from_errno (errno),
                       _("Failed to read from file '%s': %s"),
                       file, g_strerror (errno));
	  close (fd);
	  return NULL;
        }

      egg_desktop_entries_parse_data (entries, read_buf, bytes_read, 
				      &entries_error);
    }
  while (!entries_error);
  close (fd);

  if (entries_error) 
    {
      g_propagate_error (error, entries_error);
      egg_desktop_entries_free (entries);

      return NULL;
    }

  egg_desktop_entries_flush_parse_buffer (entries, &entries_error);

  if (entries_error) 
    {
      g_propagate_error (error, entries_error);
      egg_desktop_entries_free (entries);

      return NULL;
    }

  return entries;
}

/**
 * egg_desktop_entries_free: 
 * @entries: a #EggDesktopEntries
 * 
 * Frees a #EggDesktopEntries. 
 **/
void
egg_desktop_entries_free (EggDesktopEntries *entries)
{
  GList *tmp;

  g_return_if_fail (entries != NULL);

  if (entries->parse_buffer)
    g_string_free (entries->parse_buffer, TRUE);

  tmp = entries->groups;
  while (tmp != NULL)
    {
      GList *group_node;

      group_node = tmp;

      tmp = tmp->next;

      egg_desktop_entries_remove_group_node (entries, group_node);
    }

  g_assert (entries->groups == NULL);

  if (entries->locales != NULL)
    g_strfreev (entries->locales);
  
  g_free (entries);
}

void 
egg_desktop_entries_keep_locales (EggDesktopEntries *entries,
			          gchar **locales)
{
  g_return_if_fail (entries != NULL);

  if (entries->locales != NULL)
    g_strfreev (entries->locales);

  if (locales == NULL)
    entries->locales = NULL;
  else
    {
      int i, length;

      for (i = 0, length = 1; locales[i] != NULL; i++, length++);

      entries->locales = g_new (char *, length);

      for (i = 0; locales[i] != NULL; i++)
	entries->locales[i] = g_strdup (locales[i]);
      entries->locales[i] = NULL;
    }
}

static gboolean
g_deskop_entries_locale_is_interesting (EggDesktopEntries  *entries, 
               				const char       *locale)
{
  char *lang, *country, *modifier;
  const char *interesting_locale;
  gboolean is_interesting;
  int i;

  /* NULL means to keep all locales
   */
  if (entries->locales == NULL)
    return TRUE;

  lang = egg_desktop_entries_get_locale_lang (locale);
  country = egg_desktop_entries_get_locale_country (locale);
  modifier = egg_desktop_entries_get_locale_modifier (locale);

  is_interesting = FALSE;
  for (i = 0; (interesting_locale = entries->locales[i]); i++)
    {
      char *interesting_locale_lang,
	   *interesting_locale_country,
	   *interesting_locale_modifier;

      /* first see if there is an exact match
       */
      if (strcasecmp (interesting_locale, locale) == 0)
	{
	  is_interesting = TRUE;
	  break;
	}

      /* a locale is also interesting if it is a more general version of 
       * locale already designated interesting
       */

      if (lang && country && modifier)
        continue;

      interesting_locale_lang = egg_desktop_entries_get_locale_lang (locale);
      interesting_locale_country = egg_desktop_entries_get_locale_country (locale);
      interesting_locale_modifier = egg_desktop_entries_get_locale_modifier (locale);

      if (lang && country 
	  && interesting_locale_lang
	  && interesting_locale_country
	  && interesting_locale_modifier)
	{
	  is_interesting = (strcasecmp (lang, interesting_locale_lang) == 0) &&
			   (strcasecmp (country, interesting_locale_country) == 0);

	  g_free (interesting_locale_lang);
	  g_free (interesting_locale_country);
	  g_free (interesting_locale_modifier);

	  if (is_interesting)
	    break;
	}
      else if (lang && modifier
	       && interesting_locale_lang
	       && interesting_locale_country
	       && interesting_locale_modifier)
	{
	  is_interesting = (strcasecmp (lang, interesting_locale_lang) == 0) &&
			   (strcasecmp (modifier, interesting_locale_modifier) == 0);
	  g_free (interesting_locale_lang);
	  g_free (interesting_locale_country);
	  g_free (interesting_locale_modifier);

	  if (is_interesting)
	    break;
	}
      else if (lang)
	{
	  is_interesting = (strcasecmp (lang, interesting_locale_lang) == 0);

	  g_free (interesting_locale_lang);
	  g_free (interesting_locale_country);
	  g_free (interesting_locale_modifier);

	  if (is_interesting)
	    break;
	}
    }

  g_free (lang);
  g_free (country);
  g_free (modifier);

  return is_interesting;
}

static void
egg_desktop_entries_parse_line (EggDesktopEntries  *entries,
	                      const char       *line,
			      gsize             length,
			      GError          **error)
{
  GError *parse_error;
  const gchar *line_start;

  g_return_if_fail (entries != NULL);
  g_return_if_fail (line != NULL);

  parse_error = NULL;

  line_start = line;
  while (isspace (*line_start))
    line_start++;

  if (line_is_comment (line_start))
    egg_desktop_entries_parse_comment (entries, line, length, &parse_error);
  else if (line_is_group (line_start))
    egg_desktop_entries_parse_group (entries, line_start, length - (line_start - line), &parse_error);
  else if (line_is_entry (line_start))
    egg_desktop_entries_parse_entry (entries, line_start, length - (line_start - line), &parse_error);
  else
    {
      g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR, 
		   EGG_DESKTOP_ENTRIES_ERROR_PARSE,
		   _("desktop entries contain line '%s' which is not "
		     "an entry, group, or comment"), line);
      return;
    }

  if (parse_error)
    g_propagate_error (error, parse_error);
}

static void
egg_desktop_entries_parse_comment (EggDesktopEntries  *entries,
  	                         const char       *line,
			         gsize             length,
			         GError          **error)
{
  EggDesktopEntry *entry;

  if (entries->flags & EGG_DESKTOP_ENTRIES_DISCARD_COMMENTS)
    return;

  g_assert (entries->current_group != NULL);

  entry = g_new0 (EggDesktopEntry, 1);

  entry->key = NULL;

  entry->value = g_new (char, length + 1);
  strncpy (entry->value, line, length);
  entry->value[length] = '\0';
    
  entries->current_group->entries = g_list_prepend (entries->current_group->entries, entry);
}

static void
egg_desktop_entries_parse_group (EggDesktopEntries  *entries,
	                       const char       *line,
			       gsize             length,
			       GError          **error)
{
  gchar *group_name; 
  const gchar *group_name_start, *group_name_end;
  glong group_name_length;

  /* advance past opening '[' 
  */
  group_name_start = line + 1;
  group_name_end = line + length - 1;

  while (*group_name_end != ']')
    group_name_end--;

  group_name_length = group_name_end - group_name_start + 1;

  group_name = g_new (char, group_name_length);
  strncpy (group_name, group_name_start, group_name_length);
  group_name[group_name_length - 1] = '\0';

  if (!entries->default_group_name)
    {
      if (strcmp (EGG_DESKTOP_ENTRIES_DEFAULT_GROUP, group_name) != 0
          && strcmp (EGG_DESKTOP_ENTRIES_LEGACY_GROUP, group_name))
	{
          g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
  		       EGG_DESKTOP_ENTRIES_ERROR_BAD_DEFAULT_GROUP,
		       _("desktop entries file does not start with "
                         "'%s' or '%s'"),
		       EGG_DESKTOP_ENTRIES_DEFAULT_GROUP,
                       EGG_DESKTOP_ENTRIES_LEGACY_GROUP);
	  g_free (group_name);
	  return;
	}
      entries->default_group_name = g_strdup (group_name);
    }

  egg_desktop_entries_add_group (entries, group_name);
  g_free (group_name);
}

static void
egg_desktop_entries_parse_entry (EggDesktopEntries  *entries,
	                       const gchar       *line,
			       gsize             length,
			       GError          **error)
{
  char *key, *value, *key_end, *value_start, *locale;
  gulong key_len, value_len;

  key_end = value_start = index (line, '=');

  g_assert (key_end != NULL);

  key_end--;
  value_start++;

  /* Pull the key name from the line (chomping trailing whitespace)
   */
  while (isspace (*key_end))
    key_end--;

  key_len = key_end - line + 2;

  g_assert (key_len <= length);

  key = g_new0 (gchar, key_len);
  strncpy (key, line, key_len);
  key[key_len - 1] = '\0';

  /* Pull the value from the line (chugging leading whitespace)
   */
  while (isspace (*value_start))
    value_start++;

  value_len = line + length - value_start + 1; 

  value = g_new0 (gchar, value_len);
  strncpy (value, value_start, value_len);
  value[value_len - 1] = '\0';

  if (entries->encoding == EGG_DESKTOP_ENTRIES_ENCODING_UTF8 
      && !g_utf8_validate (value, -1, NULL))
    {
      g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		   EGG_DESKTOP_ENTRIES_ERROR_UNKNOWN_ENCODING,
		   _("desktop entries contain line '%s' "
		     "which is not UTF-8"), line);

      g_free (key);
      g_free (value);
      return;
    }

  if (entries->encoding == EGG_DESKTOP_ENTRIES_ENCODING_GUESS
        && entries->current_group 
        && entries->current_group->name 
        && strcmp (egg_desktop_entries_get_default_group (entries),
                   entries->current_group->name) == 0
        && strcmp (key, "Encoding") == 0)
    {
      if (strcasecmp (value, "Legacy-Mixed") == 0)
	  entries->encoding = EGG_DESKTOP_ENTRIES_ENCODING_MIXED;
      else if (strcasecmp (value, "UTF-8") == 0)
	  entries->encoding = EGG_DESKTOP_ENTRIES_ENCODING_UTF8;
      else
	{
          g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		       EGG_DESKTOP_ENTRIES_ERROR_UNKNOWN_ENCODING,
		      _("desktop entries contain unknown encoding '%s'"), value);

	  g_free (key);
	  g_free (value);
	  return;
	}
    }

  /* Is this key a translation? If so, is it one that we care about?
   */
  locale = key_get_locale (key);
  if (locale == NULL || g_deskop_entries_locale_is_interesting (entries, locale))
    egg_desktop_entries_add_entry (entries, entries->current_group->name, key, value);

  if (locale)
    g_free (locale);

  g_free (key);
  g_free (value);
}

static gchar *
key_get_locale (const gchar *key)
{
  gchar *locale;

  locale = g_strrstr (key, "[");

  if (locale)
    {
      locale = g_strdup (locale + 1);
      locale[strlen (locale)] = '\0';
    }

  return locale;
}

void 
egg_desktop_entries_parse_data (EggDesktopEntries  *entries,
	                      const gchar       *data,
			      gsize             length,
			      GError          **error)
{
  GError *parse_error;
  gsize i;

  g_return_if_fail (entries != NULL);

  parse_error = NULL;

  for (i = 0; i < length; i++)
    {
      if (data[i] == '\n')
        {
	  /* When a newline is encountered flush the parse buffer so that the
	   * line can be parsed.  Note that completely blank lines won't show
	   * up in the parse buffer, so they get parsed directly.
	   */
	  if (entries->parse_buffer->len > 0)
	    egg_desktop_entries_flush_parse_buffer (entries, &parse_error);
	  else
	    egg_desktop_entries_parse_comment (entries, "", 1, &parse_error);

	  if (parse_error) 
	    {
	      g_propagate_error (error, parse_error);
	      return;
	    }
        }
      else
	g_string_append_c (entries->parse_buffer, data[i]);
    }

  entries->approximate_size += length;
}

/**
 * egg_desktop_entries_flush_parse_buffer: 
 * @entries: a #EggDesktopEntries
 * 
 * Parses data that is in the parse buffer.  Note the parse buffer is
 * automatically flushed on new lines.
 **/
void
egg_desktop_entries_flush_parse_buffer (EggDesktopEntries  *entries,
				      GError          **error)
{
  GError *file_error = NULL;

  g_return_if_fail (entries != NULL);

  file_error = NULL;

  if (entries->parse_buffer->len > 0) 
    {
      egg_desktop_entries_parse_line (entries, entries->parse_buffer->str, 
				    entries->parse_buffer->len,
				    &file_error);
      g_string_erase (entries->parse_buffer, 0, -1);

      if (file_error) 
	{
	  g_propagate_error (error, file_error);
	  return;
	}
    }
}

gchar *
egg_desktop_entries_to_data (EggDesktopEntries   *entries,
	                   gsize             *length,
			   GError           **error)
{
  GString *data_string;
  gchar *data;
  GList *group_node, *entry_node;

  g_return_val_if_fail (entries != NULL, NULL);

  data_string = g_string_sized_new (2 * entries->approximate_size);

  for (group_node = g_list_last (entries->groups); 
       group_node != NULL; 
       group_node = group_node->prev)
    {
      EggDesktopEntriesGroup *group;

      group = (EggDesktopEntriesGroup *) group_node->data;

      if (group->name != NULL)
	g_string_append_printf (data_string, "[%s]\n", group->name);

      for (entry_node = g_list_last (group->entries);
	   entry_node != NULL;
	   entry_node = entry_node->prev)
	{
	  EggDesktopEntry *entry;

	  entry = (EggDesktopEntry *) entry_node->data;

	  if (entry->key != NULL)
	    g_string_append_printf (data_string, "%s=%s\n", entry->key, entry->value);
	  else
	    g_string_append_printf (data_string, "%s\n", entry->value);
	}
    }

  if (length)
    *length = data_string->len;

  data = data_string->str;

  g_string_free (data_string, FALSE);

  return data;
}

/**
 * egg_desktop_entries_get_keys: 
 * @entries: a #EggDesktopEntries
 * @group_name: a group name
 * @length: the number of keys returned
 * 
 * Returns all keys for the group name @group. The vector of 
 * returned keys will be %NULL-terminated, so @length may optionally be %NULL.
 * 
 * Return value: a newly-allocated %NULL-terminated array of strings. Use
 *               g_strfreev() to free it.
 **/
gchar **
egg_desktop_entries_get_keys (EggDesktopEntries  *entries,
			    const gchar      *group_name, 
			    gsize            *length,
			    GError          **error)
{
  EggDesktopEntriesGroup *group;
  GList *tmp;
  gchar **keys;
  gsize i, num_keys;

  g_return_val_if_fail (entries != NULL, NULL);
  g_return_val_if_fail (group_name != NULL, NULL);

  group = egg_desktop_entries_lookup_group (entries, group_name);

  if (!group) 
    {
      g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		   EGG_DESKTOP_ENTRIES_ERROR_GROUP_NOT_FOUND,
		   _("desktop entries do not have group '%s'"),
		   group_name);
      return NULL;
    }

  num_keys = g_list_length (group->entries);

  keys = (gchar **) g_new (gchar **, num_keys + 1);

  tmp = group->entries;
  for (i = 0; i < num_keys; i++)
    {
      EggDesktopEntry *entry;

      entry = (EggDesktopEntry *) tmp->data;
      keys[i] = g_strdup (entry->key);

      tmp = tmp->next;
    }
  keys[i] = NULL;

  if (length)
    *length = num_keys;

  return keys;
}

/**
 * egg_desktop_entries_get_default_group: 
 * @entries: a #EggDesktopEntries
 * 
 * Returns the name of the default group of the file.  This will
 * normally be "Desktop Entry", but may also be "KDE Desktop Entry"
 * in legacy files.
 * 
 * Return value: The default group of the desktop file, or %NULL if
 *               not available yet.
 **/
const gchar *
egg_desktop_entries_get_default_group (EggDesktopEntries  *entries)
{
  g_return_val_if_fail (entries != NULL, NULL);

  return entries->default_group_name;
}

/**
 * egg_desktop_entries_get_groups: 
 * @entries: a #EggDesktopEntries
 * @length: the number of groups returned
 * 
 * Returns all groups in the .desktop file loaded with @entries.  The
 * vector of returned groups will be %NULL-terminated, so @length may 
 * optionally be %NULL.
 * 
 * Return value: a newly-allocated %NULL-terminated array of strings. Use
 *               g_strfreev() to free it.
 **/
gchar **
egg_desktop_entries_get_groups (EggDesktopEntries *entries, 
			      gsize           *length)
{
  GList *tmp;
  gchar **groups;
  gsize i, num_groups;

  g_return_val_if_fail (entries != NULL, NULL);

  num_groups = g_list_length (entries->groups);
  groups = (gchar **) g_new (gchar **, num_groups + 1);

  tmp = entries->groups;
  for (i = 0; i < num_groups; i++)
    {
      EggDesktopEntriesGroup *group;

      group = (EggDesktopEntriesGroup *) tmp->data;
      groups[i] = g_strdup (group->name);

      tmp = tmp->next;
    }
  groups[i] = NULL;

  if (length)
    *length = num_groups;

  return groups;
}

char *
egg_desktop_entries_get_value (EggDesktopEntries  *entries,
			     const gchar      *group_name,
			     const gchar      *key, 
			     GError          **error)
{
  EggDesktopEntriesGroup *group;
  EggDesktopEntry *entry;
  gchar *value;

  g_return_val_if_fail (entries != NULL, NULL);
  g_return_val_if_fail (group_name != NULL, NULL);
  g_return_val_if_fail (key != NULL, NULL);

  entry = NULL;
  value = NULL;

  group = egg_desktop_entries_lookup_group (entries, group_name);

  if (!group)
    {
      g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		   EGG_DESKTOP_ENTRIES_ERROR_GROUP_NOT_FOUND,
		   _("desktop entries do not have group '%s'"),
		   group_name);
      return NULL;
    }

  entry = egg_desktop_entries_lookup_entry (entries, group, key);

  if (entry)
    value = g_strdup (entry->value);
  else
    g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
  	         EGG_DESKTOP_ENTRIES_ERROR_KEY_NOT_FOUND,
	         _("desktop entries do not have key '%s'"), key);

  return value;
}

/**
 * egg_desktop_entries_get_string: 
 * @entries: a #EggDesktopEntries
 * @group: a group name
 * @key: a key
 * @error: return location for a #GError
 *
 * Returns the value associated with @key under @group.  In the event
 * the key cannot be found, %NULL is returned and @error is set to
 * #EGG_DESKTOP_ENTRIES_ERROR_KEY_NOT_FOUND.  In the event that the @group
 * cannot be found, %NULL is returned and @error is set to 
 * #EGG_DESKTOP_ENTRIES_ERROR_GROUP_NOT_FOUND.
 * 
 * Return value: a string or %NULL if the specified key cannot be
 *               found.
 **/
char *
egg_desktop_entries_get_string (EggDesktopEntries  *entries,
			      const gchar      *group,
			      const gchar      *key, 
			      GError          **error)
{
  char *value, *string_value;
  GError *entries_error;

  g_return_val_if_fail (entries != NULL, NULL);
  g_return_val_if_fail (group != NULL, NULL);
  g_return_val_if_fail (key != NULL, NULL);

  entries_error = NULL;

  value = egg_desktop_entries_get_value (entries, group, key, &entries_error);

  if (entries_error)
    {
      g_propagate_error (error, entries_error);
      return NULL;
    }

  string_value = egg_desktop_entries_parse_value_as_string (entries, value, &entries_error);
  g_free (value);

  if (entries_error)
    {
      if (g_error_matches (entries_error,
			   EGG_DESKTOP_ENTRIES_ERROR,
			   EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE))
	{
	  g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		       EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE,
		       _("desktop entries contain key '%s' "
			 "which has value that cannot be interpreted."),
		       key);
	  g_error_free (entries_error);
	}
      else
        g_propagate_error (error, entries_error);
    }

  return string_value;
}

/**
 * egg_desktop_entries_get_string_list: 
 * @entries: a #EggDesktopEntries
 * @group: a group name
 * @key: a key
 * @length: the number of localized strings returned
 * @error: return location for a #GError
 *
 * Returns the values associated with @key under @group.  In the event
 * the key cannot be found, %NULL is returned and @error is set to
 * #EGG_DESKTOP_ENTRIES_ERROR_KEY_NOT_FOUND.  In the event that the @group
 * cannot be found, %NULL is returned and @error is set to 
 * #EGG_DESKTOP_ENTRIES_ERROR_GROUP_NOT_FOUND.
 * 
 * Return value: a string or %NULL if the specified key cannot be
 *               found.
 **/
gchar **
egg_desktop_entries_get_string_list (EggDesktopEntries  *entries,
				   const gchar    *group,
				   const gchar    *key,
				   gsize          *length,
				   GError        **error)
{
  GError *entries_error;
  gchar **value_vector, *value;
  gint last_char_index;

  entries_error = NULL;

  value = egg_desktop_entries_get_string (entries, group, key, &entries_error);

  if (entries_error)
    g_propagate_error (error, entries_error);

  if (!value)
    return NULL;

  last_char_index = strlen (value) - 1;

  if (value[last_char_index] == ';')
    value[last_char_index] = '\0';

  value_vector = g_strsplit (value, ";", 0);
  g_free (value);

  if (length)
    for (*length = 0; value_vector[*length]; (*length)++);

  return value_vector;
}

/**
 * egg_desktop_entries_get_locale_string: 
 * @entries: a #EggDesktopEntries
 * @group: a group name
 * @key: a key
 * @locale: a locale or %NULL
 * @error: return location for a #GError
 * 
 * Returns the value associated with @key under @group translated in the
 * given @locale if available.  If @locale is %NULL then the current
 * locale is assumed. If @key cannot be found then %NULL is returned
 * and @error is set to #EGG_DESKTOP_ENTRIES_ERROR_KEY_NOT_FOUND. If the 
 * value associated with @key cannot be interpreted or no suitable 
 * translation can be found then the untranslated value is returned and 
 * @error is set to #EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE and 
 * #EGG_DESKTOP_ENTRIES_ERROR_KEY_NOT_FOUND, respectively. In the event that 
 * the @group cannot be found, %NULL is returned and @error is set to 
 * #EGG_DESKTOP_ENTRIES_ERROR_GROUP_NOT_FOUND.
 * 
 * Return value: a string or %NULL if the specified key cannot be
 *               found.
 **/
gchar *
egg_desktop_entries_get_locale_string (EggDesktopEntries  *entries,
				     const gchar    *group,
				     const gchar    *key,
				     const gchar    *locale,
				     GError        **error)
{
  gchar *lang, *country, *modifier, *candidate_key, *utf8_value,
        *translated_value;
  GError *entries_error;

  candidate_key = NULL;
  translated_value = NULL;
  entries_error = NULL;

  if (!locale)
    {
#ifdef HAVE_LC_MESSAGES
      locale = (const gchar *) setlocale (LC_MESSAGES, NULL);
#else
      locale = (const gchar *) setlocale (LC_CTYPE, NULL);
#endif

      if (!locale)
	locale = (const gchar *) "C";
    }

  if (!egg_desktop_entries_has_group (entries, group))
    {
      g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		   EGG_DESKTOP_ENTRIES_ERROR_GROUP_NOT_FOUND,
		   _("desktop entries do not have group '%s'"),
		   group);
      return NULL;
    }

  lang = egg_desktop_entries_get_locale_lang (locale);
  country = egg_desktop_entries_get_locale_country (locale);
  modifier = egg_desktop_entries_get_locale_modifier (locale);

  if (lang && country && modifier)
    {
      candidate_key = g_strdup_printf ("%s[%s_%s@%s]",
				       key, lang, country, modifier);

      translated_value = egg_desktop_entries_get_string (entries,
						       group,
						       candidate_key, NULL);
      g_free (candidate_key);
    }
  else if (lang && country)
    {
      candidate_key = g_strdup_printf ("%s[%s_%s]", key, lang, country);

      translated_value = egg_desktop_entries_get_string (entries, group,
						       candidate_key, NULL);
      g_free (candidate_key);
    }
  else if (lang && modifier)
    {
      candidate_key = g_strdup_printf ("%s[%s@%s]", key, lang, modifier);

      translated_value = egg_desktop_entries_get_string (entries, group,
						       candidate_key, NULL);
      g_free (candidate_key);
    }
  else if (lang)
    {
      candidate_key = g_strdup_printf ("%s[%s]", key, lang);

      translated_value = egg_desktop_entries_get_string (entries, group,
						       candidate_key, NULL);
      g_free (candidate_key);
    }
  else
    g_assert_not_reached ();

  if (translated_value)
    {
      utf8_value = egg_desktop_entries_key_to_utf8 (entries, candidate_key,
						  translated_value);

      if (!utf8_value)
	{
	  g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		       EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE,
		       _("desktop entries contain key '%s' "
			 "which has value that cannot be interpreted."),
		       candidate_key);

	  translated_value = NULL;
	}

      if (translated_value)
	g_free (translated_value);

      translated_value = utf8_value;
    }

  /* Fallback to untranslated key
   */
  if (!translated_value)
    {
      translated_value = egg_desktop_entries_get_string (entries, group, key,
   						       &entries_error);

      if (!translated_value)
	g_propagate_error (error, entries_error);
      else
	g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		     EGG_DESKTOP_ENTRIES_ERROR_KEY_NOT_FOUND,
		     _("desktop entries contain no translated value "
		       "for key '%s' with locale '%s'."),
		     key, locale);
    }

  return translated_value;
}

/**
 * egg_desktop_entries_get_locale_string_list: 
 * @entries: a #EggDesktopEntries
 * @group: a group name
 * @key: a key
 * @locale: a locale 
 * @length: the number of localized strings returned
 * @error: return location for a #GError
 * 
 * Returns the values associated with @key under @group translated in the
 * given @locale if available.  If @locale is %NULL then the current
 * locale is assumed. If @key cannot be found then %NULL is returned
 * and @error is set to #EGG_DESKTOP_ENTRIES_ERROR_KEY_NOT_FOUND. If the 
 * values associated with @key cannot be interpreted or no suitable 
 * translations can be found then the untranslated values are returned and 
 * @error is set to #EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE and 
 * #EGG_DESKTOP_ENTRIES_ERROR_KEY_NOT_FOUND, respectively. In the event that 
 * the @group cannot be found, %NULL is returned and @error is set to 
 * #EGG_DESKTOP_ENTRIES_ERROR_GROUP_NOT_FOUND.  The vector of returned strings
 * will be %NULL-terminated, so @length may optionally be %NULL.
 * 
 * Return value: a string or %NULL if the specified key cannot be
 *               found.
 **/
gchar **
egg_desktop_entries_get_locale_string_list (EggDesktopEntries  *entries,
					  const gchar    *group,
					  const gchar    *key,
					  const gchar    *locale,
					  gsize          *length,
					  GError        **error)
{
  GError *entries_error;
  gchar **value_vector, *value;

  entries_error = NULL;

  value = egg_desktop_entries_get_locale_string (entries, group, key, locale,
					       &entries_error);

  if (entries_error)
    g_propagate_error (error, entries_error);

  if (!value)
    return NULL;

  if (value[strlen (value) - 1] == ';')
    {
      value[strlen (value) - 1] = '\0';
    }

  value_vector = g_strsplit (value, ";", 0);

  g_free (value);

  if (length)
    for (*length = 0; value_vector[*length]; *(length)++);

  return value_vector;
}

/**
 * egg_desktop_entries_get_boolean: 
 * @entries: a #EggDesktopEntries
 * @group: a group name
 * @key: a key
 * @error: return location for a #GError
 * 
 * Returns the value associated with @key under @group as a boolean. 
 * If @key cannot be found then the return value is undefined
 * and @error is set to #EGG_DESKTOP_ENTRIES_ERROR_KEY_NOT_FOUND. Likewise,
 * if the value associated with @key cannot be interpreted as a 
 * boolean then the return value is also undefined and @error is set
 * to #EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE.
 * 
 * Return value: the value associated with the key as a boolean
 **/
gboolean
egg_desktop_entries_get_boolean (EggDesktopEntries  *entries,
			       const gchar      *group,
			       const gchar      *key,
			       GError          **error)
{
  GError *entries_error;
  gchar *value;
  gboolean bool_value;

  entries_error = NULL;

  value = egg_desktop_entries_get_value (entries, group, key, &entries_error);

  if (!value)
    {
      g_propagate_error (error, entries_error);
      return FALSE;
    }

  bool_value = egg_desktop_entries_parse_value_as_boolean (entries, value,
						       &entries_error);
  g_free (value);

  if (entries_error)
    {
      if (g_error_matches (entries_error,
			   EGG_DESKTOP_ENTRIES_ERROR,
			   EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE))
	{
	  g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		       EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE,
		       _("desktop entries contain key '%s' "
			 "which has value that cannot be interpreted."),
		       key);
	  g_error_free (entries_error);
	}
      else
        g_propagate_error (error, entries_error);
    }

  return bool_value;
}

/**
 * egg_desktop_entries_get_boolean_list: 
 * @entries: a #EggDesktopEntries
 * @group: a group name
 * @key: a key
 * @length: the number of booleans returned
 * @error: return location for a #GError
 * 
 * Returns the values associated with @key under @group as booleans. 
 * If @key cannot be found then the return value is undefined
 * and @error is set to #EGG_DESKTOP_ENTRIES_ERROR_KEY_NOT_FOUND. Likewise,
 * if the values associated with @key cannot be interpreted as 
 * booleans then the return value is also undefined and @error is set
 * to #EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE.
 * 
 * Return value: the values associated with the key as a boolean
 **/
gboolean *
egg_desktop_entries_get_boolean_list (EggDesktopEntries  *entries,
				    const gchar    *group,
				    const gchar    *key,
				    gsize          *length,
				    GError        **error)
{
  GError *entries_error;
  gchar **value_vector;
  gboolean *bool_values;
  gsize i, num_bools;

  entries_error = NULL;

  value_vector = egg_desktop_entries_get_string_list (entries, group, key,
				 		    &num_bools, &entries_error);

  if (entries_error)
    g_propagate_error (error, entries_error);

  if (!value_vector)
    return NULL;

  bool_values = g_new (gboolean, num_bools);

  for (i = 0; i < num_bools; i++)
    {
      bool_values[i] = egg_desktop_entries_parse_value_as_boolean (entries,
							         value_vector[i],
							         &entries_error);

      if (entries_error)
	{
	  g_propagate_error (error, entries_error);
	  g_strfreev (value_vector);
	  g_free (bool_values);

	  return NULL;
	}
    }
  g_strfreev (value_vector);

  if (length)
    *length = num_bools;

  return bool_values;
}

/**
 * egg_desktop_entries_get_integer: 
 * @entries: a #EggDesktopEntries
 * @group: a group name
 * @key: a key
 * @error: return location for a #GError
 * 
 * Returns the value associated with @key under @group as an integer. 
 * If @key cannot be found then the return value is undefined
 * and @error is set to #EGG_DESKTOP_ENTRIES_ERROR_KEY_NOT_FOUND. Likewise,
 * if the value associated with @key cannot be interpreted as an 
 * integer then the return value is also undefined and @error is set
 * to #EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE.
 * 
 * Return value: the value associated with the key as an integer.
 **/
gint
egg_desktop_entries_get_integer (EggDesktopEntries  *entries,
			       const gchar    *group,
			       const gchar    *key, 
			       GError        **error)
{
  GError *entries_error;
  gchar *value;
  gint int_value;

  entries_error = NULL;

  value = egg_desktop_entries_get_value (entries, group, key, &entries_error);

  if (entries_error)
    {
      g_propagate_error (error, entries_error);
      return 0;
    }

  int_value = egg_desktop_entries_parse_value_as_integer (entries, value,
	  					        &entries_error);
  g_free (value);

  if (entries_error)
    {
      if (g_error_matches (entries_error,
			   EGG_DESKTOP_ENTRIES_ERROR,
			   EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE))
	{
	  g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		       EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE,
		       _("desktop entries contain key '%s' "
			 "which has value that cannot be interpreted."), key);
	  g_error_free (entries_error);
	}
      else
        g_propagate_error (error, entries_error);
    }

  return int_value;
}

/**
 * egg_desktop_entries_get_integer_list: 
 * @entries: a #EggDesktopEntries
 * @group: a group name
 * @key: a key
 * @length: the number of integers returned
 * @error: return location for a #GError
 * 
 * Returns the values associated with @key under @group as integers. 
 * If @key cannot be found then the return value is undefined
 * and @error is set to #EGG_DESKTOP_ENTRIES_ERROR_KEY_NOT_FOUND. Likewise,
 * if the values associated with @key cannot be interpreted as 
 * integers then the return value is also undefined and @error is set
 * to #EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE.
 * 
 * Return value: the values associated with the key as a integer
 **/
gint *
egg_desktop_entries_get_integer_list (EggDesktopEntries  *entries,
				    const gchar    *group,
				    const gchar    *key,
				    gsize          *length,
				    GError        **error)
{
  GError *entries_error;
  gchar **value_vector;
  gint *int_values;
  gsize i, num_ints;

  entries_error = NULL;

  value_vector = egg_desktop_entries_get_string_list (entries, group, key,
						  &num_ints, &entries_error);

  if (entries_error)
    g_propagate_error (error, entries_error);

  if (!value_vector)
    return NULL;

  int_values = g_new (int, num_ints);

  for (i = 0; i < num_ints; i++)
    {
      int_values[i] = egg_desktop_entries_parse_value_as_integer (entries,
							      value_vector[i],
							      &entries_error);

      if (entries_error)
	{
	  g_propagate_error (error, entries_error);
	  g_strfreev (value_vector);
	  g_free (int_values);

	  return NULL;
	}
    }
  g_strfreev (value_vector);

  if (length)
    *length = num_ints;

  return int_values;
}

/**
 * egg_desktop_entries_has_group: 
 * @entries: a #EggDesktopEntries
 * @group: a group name
 * 
 * Looks whether the .desktop file has the group @group.
 * 
 * Return value: %TRUE if @group is a part of @entries, %FALSE otherwise.
 **/
gboolean
egg_desktop_entries_has_group (EggDesktopEntries  *entries,
			     const char       *group_name)
{
  GList *tmp;

  g_return_val_if_fail (entries != NULL, FALSE);
  g_return_val_if_fail (group_name != NULL, FALSE);

  for (tmp = entries->groups; tmp != NULL; tmp = tmp->next)
    {
      EggDesktopEntriesGroup *group;

      group = (EggDesktopEntriesGroup *) tmp->data;

      if (group && group->name && strcmp (group->name, group_name) == 0)
	return TRUE;
    }

  return FALSE;
}

void
egg_desktop_entries_add_group (EggDesktopEntries *entries,
			     const gchar     *group_name)
{
  EggDesktopEntriesGroup *group;

  g_return_if_fail (entries != NULL);
  g_return_if_fail (group_name != NULL);
  g_return_if_fail (egg_desktop_entries_lookup_group (entries, group_name) == NULL);

  group = g_new0 (EggDesktopEntriesGroup, 1);
  group->name = g_strdup (group_name);
  
  if (entries->flags & EGG_DESKTOP_ENTRIES_GENERATE_LOOKUP_MAP)
    group->lookup_map = g_hash_table_new (g_str_hash, g_str_equal);

  entries->groups = g_list_prepend (entries->groups, group);

  entries->current_group = group;
}

static void
egg_desktop_entries_remove_group_node (EggDesktopEntries      *entries,
				       GList                  *group_node)
{
  EggDesktopEntriesGroup *group;

  group = (EggDesktopEntriesGroup *) group_node->data;

  /* If the current group gets deleted make the current group the first
   * group.
   */
  if (entries->current_group == group)
    {
      GList *first_group;

      first_group = entries->groups;

      if (first_group)
        entries->current_group = (EggDesktopEntriesGroup *) first_group->data;
      else
        entries->current_group = NULL;
    }

  entries->groups = g_list_remove_link (entries->groups, group_node);

  g_free ((gchar *) group->name);

  g_list_foreach (group->entries, (GFunc) g_free, NULL);
  g_list_free (group->entries);
  group->entries = NULL;

  if (group->lookup_map)
    {
      g_hash_table_destroy (group->lookup_map);
      group->lookup_map = NULL;
    }

  g_free (group);
  g_list_free_1 (group_node);
}

void
egg_desktop_entries_remove_group (EggDesktopEntries *entries,
				const gchar     *group_name)
{
  EggDesktopEntriesGroup *group;
  GList *group_node;

  g_return_if_fail (entries != NULL);
  g_return_if_fail (group_name != NULL);

  group = egg_desktop_entries_lookup_group (entries, group_name);

  if (!group)
    return;

  group_node = g_list_find (entries->groups, group);

  egg_desktop_entries_remove_group_node (entries, group_node);
}

void
egg_desktop_entries_add_entry (EggDesktopEntries *entries,
  			       const gchar       *group_name,
			       const gchar       *key,
			       const gchar       *value)
{
  EggDesktopEntriesGroup *group;
  EggDesktopEntry *entry;

  if (group_name == NULL)
    group = entries->current_group;

  group = egg_desktop_entries_lookup_group (entries, group_name);

  if (!group) 
    {
      egg_desktop_entries_add_group (entries, group_name); 
      group = (EggDesktopEntriesGroup *) entries->groups->data;
    }

  entry = g_new0 (EggDesktopEntry, 1);

  entry->key = g_strdup (key);
  entry->value =  g_strdup (value);
    
  if (entries->flags & EGG_DESKTOP_ENTRIES_GENERATE_LOOKUP_MAP)
    g_hash_table_replace (group->lookup_map, entry->key, entry); 

  group->entries = g_list_prepend (group->entries, entry);
}

void
egg_desktop_entries_remove_entry (EggDesktopEntries *entries,
				  const gchar       *group_name,
				  const gchar       *key)
{
  EggDesktopEntriesGroup *group;
  EggDesktopEntry *entry;

  entry = NULL;

  if (group_name == NULL)
    group = entries->current_group;

  group = egg_desktop_entries_lookup_group (entries, group_name);

  if (group == NULL)
    return;

  group->entries = g_list_remove (group->entries, entry);

  entry = egg_desktop_entries_lookup_entry (entries, group, key); 

  if (entry == NULL)
    return;

  if (group->lookup_map)
    g_hash_table_remove (group->lookup_map, entry->key);

  g_free (entry->key);
  g_free (entry->value);
  g_free (entry);
}

static EggDesktopEntriesGroup *
egg_desktop_entries_lookup_group (EggDesktopEntries *entries, 
				const gchar     *group_name)
{
  EggDesktopEntriesGroup *group;
  GList *tmp;

  group = NULL;
  for (tmp = entries->groups; tmp != NULL; tmp = tmp->next)
    {
      group = (EggDesktopEntriesGroup *) tmp->data;

      if (group && group->name && strcmp (group->name, group_name) == 0)
        break;

      group = NULL;
    }

  return group;
}

static EggDesktopEntry *
egg_desktop_entries_lookup_entry (EggDesktopEntries       *entries,
				EggDesktopEntriesGroup  *group,
				const gchar           *key)
{
  GList *tmp;
  EggDesktopEntry *entry;

  if (entries->flags & EGG_DESKTOP_ENTRIES_GENERATE_LOOKUP_MAP)
    return (EggDesktopEntry *) g_hash_table_lookup (group->lookup_map, key);

  entry = NULL;
  for (tmp = group->entries; tmp != NULL; tmp = tmp->next)
    {
      entry = (EggDesktopEntry *) tmp->data;

      if (entry->key && (strcmp (key, entry->key) == 0))
	break;

      entry = NULL;
    }

  return entry;
}

static gchar *
egg_desktop_entries_get_locale_modifier (const gchar *locale)
{
  gchar *p;

  p = g_strrstr (locale, "@");

  if (!p)
    return NULL;

  return g_strdup (p + 1);
}

static gchar *
egg_desktop_entries_get_locale_country (const gchar *locale)
{
  int country_len;
  gchar *country, *p, *q;

  p = strstr (locale, "_");

  if (!p)
    return NULL;

  p++;

  q = strstr (p, ".");

  if (!q)
    q = strstr (p, "@");

  if (!q)
    country_len = q - p;
  else
    country_len = strlen (p);

  if (country_len <= 0)
    return NULL;

  country = g_new (gchar, country_len + 1);

  strncpy (country, p, country_len);
  country[country_len] = '\0';

  return country;
}

static gchar *
egg_desktop_entries_get_locale_lang (const gchar *locale)
{
  int lang_len;
  gchar *lang, *p;

  p = strstr (locale, "_");

  if (!p)
    p = strstr (locale, ".");

  if (!p)
    p = strstr (locale, "@");

  if (p)
    {
      lang_len = p - locale;
      lang = g_new (gchar, lang_len + 1);
      strncpy (lang, locale, lang_len);
      lang[lang_len] = '\0';
    }
  else
    {
      lang = g_strdup (locale);
    }

  return lang;
}

static gchar *
egg_desktop_entries_get_locale_encoding (const gchar *locale)
{
  int encoding_len;
  gchar *encoding, *p, *q;

  p = strstr (locale, ".");

  if (!p)
    return NULL;

  p++;

  q = strstr (p, "@");

  if (!q)
    encoding_len = q - p;
  else
    encoding_len = strlen (p);

  if (encoding_len <= 0)
    {
      return egg_desktop_entries_get_fallback_encoding (locale);
    }
  else
    {
      encoding = g_new (gchar, encoding_len + 1);

      strncpy (encoding, p, encoding_len);
      encoding[encoding_len] = '\0';
    }

  return encoding;
}

static gchar *
egg_desktop_entries_get_fallback_encoding (const gchar *locale)
{
  gchar *locale_lang, *locale_country, **tag_vector,
    *tag, *tag_lang, *tag_country;
  const gchar *tags;

  static const gchar *tag_encoding_table[] = {
    "hy", "ARMSCII-8",
    "zh_TW", "BIG5",
    "be bg", "CP1251",
    "zh_CN", "EUC-CN",
    "ja", "EUC-JP",
    "ko", "EUC-KR",
    "ka", "GEORGIAN-PS",
    "br ca da de en es eu fi fr gl it nl no pt sv wa", "ISO-8859-1",
    "cs hr hu pl ro sk sl sq sr", "ISO-8859-2",
    "eo", "ISO-8859-3",
    "mk sp", "ISO-8859-5",
    "el", "ISO-8859-7",
    "tr", "ISO-8859-9",
    "lt lv mi", "ISO-8859-13",
    "cy ga", "ISO-8859-14",
    "et", "ISO-8859-15",
    "ru", "KOI8-R",
    "uk", "KOI8-U",
    "vi", "TCVN-5712",
    "th", "TIS-620",
    NULL
  };

  int i, j;

  locale_lang = egg_desktop_entries_get_locale_lang (locale);
  locale_country = egg_desktop_entries_get_locale_country (locale);

  i = 0;
  for (tags = tag_encoding_table[i]; (tags = tag_encoding_table[i]); i++)
    {
      tag_vector = g_strsplit (tags, " ", 0);

      j = 0;
      for (tag = tag_vector[j]; (tag = tag_vector[j]); j++)
	{
	  tag_lang = egg_desktop_entries_get_locale_lang (tag);
	  tag_country = egg_desktop_entries_get_locale_country (tag);

	  if (strcmp (locale_lang, tag_lang) == 0
	      && ((!locale_country && !tag_country)
		  || (locale_country && tag_country
		      && strcmp (locale_country, tag_country) == 0)))
	    {
	      g_free (tag_lang);
	      g_free (tag_country);
	      g_strfreev (tag_vector);

	      return g_strdup (tag_encoding_table[i + 1]);
	    }

	  g_free (tag_lang);
	  g_free (tag_country);
	}

      g_strfreev (tag_vector);
    }

  return NULL;
}

/* Lines starting with # or consisting entirely of whitespace are merely
 * recorded, not parsed. (This function assumes all leading whitespaces 
 * have been stripped)
 */
static gboolean
line_is_comment (const gchar *line)
{
  return (*line == '#' || *line == '\0' || *line == '\n');
}

/* A group in a desktop entries is made up of a starting '[' followed by one
 * or more letters making up the group name followed by ']'.
 */
static gboolean
line_is_group (const gchar *line)
{
  gchar *p;

  p = (gchar *) line;
  if (*p != '[')
    return FALSE;

  p = g_utf8_next_char (p);

  if (!*p)
    return FALSE;

  p = g_utf8_next_char (p);

  /* Group name must be non-empty
   */
  if (*p == ']')
    return FALSE;

  while (*p && *p != ']')
    p = g_utf8_next_char (p);

  if (!*p)
    return FALSE;

  return TRUE;
}

/* An entry in a desktop entries is made up of a key/value pair separated by
 * an equal sign (=)
 */
static gboolean
line_is_entry (const gchar *line)
{
  gchar *p;

  p = (gchar *) g_utf8_strchr (line, -1, '=');

  if (!p)
    return FALSE;

  /* Key must be non-empty
   */
  if (*p == line[0])
    return FALSE;

  return TRUE;
}

static gchar *
egg_desktop_entries_key_to_utf8 (EggDesktopEntries *entries,
			       const gchar     *key,
			       const gchar     *value)
{
  if (entries->encoding == EGG_DESKTOP_ENTRIES_ENCODING_UTF8
      || entries->encoding == EGG_DESKTOP_ENTRIES_ENCODING_GUESS)
    {
      if (g_utf8_validate (value, -1, NULL))
	return g_strdup (value);

      if (entries->encoding == EGG_DESKTOP_ENTRIES_ENCODING_UTF8)
	return NULL;
    }

  if (entries->encoding == EGG_DESKTOP_ENTRIES_ENCODING_MIXED
      || entries->encoding == EGG_DESKTOP_ENTRIES_ENCODING_GUESS)
    {
      char *encoding, *locale;

      locale = key_get_locale (key);

      if (locale == NULL)
	locale = g_strdup ("C");

      encoding = egg_desktop_entries_get_locale_encoding (locale);
      g_free (locale);

      return g_convert (value, strlen (value), "utf8", encoding, NULL, NULL,
		    NULL);

    }

  g_assert_not_reached ();

  return NULL;
}

static gchar *
egg_desktop_entries_parse_value_as_string (EggDesktopEntries  *entries,
					 const gchar      *value,
					 GError          **error)
{
  GError *parse_error;
  gchar *string_value, *p, *q;
  gsize length;

  parse_error = NULL;

  length = strlen (value) + 1;

  string_value = g_new (gchar, length);

  p = (gchar *) value;
  q = string_value;
  while (p < (value + length - 1))
    {
      if (*p == '\\')
	{
	  p++;
	  
	  switch (*p)
	    {
	      case 's':
                  *q = ' ';
		  length--;
              break;

              case 'n':
                  *q = '\n';
		  length--;
              break;

              case 't':
                  *q = '\t';
		  length--;
              break;

              case 'r':
                  *q = '\r';
		  length--;
              break;

              case '\\':
                  *q = '\\';
		  length--;
              break;

              default:
	         *q++ = '\\';
		 *q = *p;

		 if (parse_error == NULL)
		   {
		     char sequence[3];

		     sequence[0] = '\\';
		     sequence[1] = *p;
		     sequence[2] = '\0';

		     g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		                  EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE,
		                  _("desktop entries contain invalid escape "
				    "sequence '%s'"), sequence);
		   }
	      break;
	    }
	}
      else
	*q = *p;

      q++;
      p++;
    }

  if (p[-1] == '\\' && error == NULL)
    {
      g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
                   EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE,
		   _("desktop entries contain escape character at end of "
		     "line"));
    }

  *q = '\0';

  return string_value;
}

static gint
egg_desktop_entries_parse_value_as_integer (EggDesktopEntries  *entries,
					  const gchar      *value,
					  GError          **error)
{
  gchar *end_of_valid_int;
  gint int_value;

  int_value = 0;

  int_value = strtol (value, &end_of_valid_int, 0);

  if (*end_of_valid_int != '\0')
    {
      g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
		   EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE,
		   _("Value '%s' cannot be interpreted as a number."), value);
    }

  return int_value;
}

static gboolean
egg_desktop_entries_parse_value_as_boolean (EggDesktopEntries  *entries,
					  const gchar      *value,
					  GError          **error)
{
  if (value)
    {
      if ((strcmp (value, "1") == 0) || (strcasecmp (value, "true") == 0)
	  || (strcasecmp (value, "yes") == 0)
	  || (strcasecmp (value, "t") == 0) || (strcasecmp (value, "y") == 0))
	return TRUE;
      else if ((strcmp (value, "0") == 0)
	       || (strcasecmp (value, "false") == 0)
	       || (strcasecmp (value, "no") == 0)
	       || (strcasecmp (value, "f") == 0)
	       || (strcasecmp (value, "n") == 0))
	return FALSE;
    }

  g_set_error (error, EGG_DESKTOP_ENTRIES_ERROR,
	       EGG_DESKTOP_ENTRIES_ERROR_INVALID_VALUE,
	       _("Value '%s' cannot be interpreted as a boolean."), value);
  return FALSE;
}