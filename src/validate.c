#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "desktop_file.h"

struct KeyHashData {
  gboolean has_non_translated;
  gboolean has_translated;
};

struct KeyData {
  GHashTable *hash;
  char *filename;
  gboolean deprecated;
};

#define N_LANG 30

/* #define VERIFY_CANONICAL_ENCODING_NAME */

struct {
  const char *encoding;
  const char *langs[N_LANG];
} known_encodings[] = {
  {"ARMSCII-8", {"by"}},
  {"BIG5", {"zh_TW"}},
  {"CP1251", {"be", "bg"}},
  {"EUC-CN", {"zh_TW"}},
  {"EUC-JP", {"ja"}},
  {"EUC-KR", {"ko"}},
  {"GEORGIAN-ACADEMY", {}},
  {"GEORGIAN-PS", {"ka"}},
  {"ISO-8859-1", {"br", "ca", "da", "de", "en", "es", "eu", "fi", "fr", "gl", "it", "nl", "wa", "no", "pt", "pt", "sv"}},
  {"ISO-8859-2", {"cs", "hr", "hu", "pl", "ro", "sk", "sl", "sq", "sr"}},
  {"ISO-8859-3", {"eo"}},
  {"ISO-8859-5", {"mk", "sp"}},
  {"ISO-8859-7", {"el"}},
  {"ISO-8859-9", {"tr"}},
  {"ISO-8859-13", {"lv", "lt", "mi"}},
  {"ISO-8859-14", {"ga", "cy"}},
  {"ISO-8859-15", {"et"}},
  {"KOI8-R", {"ru"}},
  {"KOI8-U", {"uk"}},
  {"TCVN-5712", {"vi"}},
  {"TIS-620", {"th"}},
  {"VISCII", {}},
};

struct {
  const char *alias;
  const char *value;
} enc_aliases[] = {
  {"GB2312", "EUC-CN"},
  {"TCVN", "TCVN-5712" }
};

static gboolean fatal_error_occurred = FALSE;


void
print_fatal (const char *format, ...)
{
  va_list args;
  gchar *str;
  
  g_return_if_fail (format != NULL);
  
  va_start (args, format);
  str = g_strdup_vprintf (format, args);
  va_end (args);

  fputs (str, stdout);

  fflush (stdout);
  
  g_free (str);

  fatal_error_occurred = TRUE;
}

void
print_warning (const char *format, ...)
{
  va_list args;
  gchar *str;
  
  g_return_if_fail (format != NULL);
  
  va_start (args, format);
  str = g_strdup_vprintf (format, args);
  va_end (args);

  fputs (str, stdout);

  fflush (stdout);
  
  g_free (str);
}


gboolean
aliases_equal (const char *enc1, const char *enc2)
{
  while (*enc1 && *enc2)
    {
      while (*enc1 == '-' ||
	     *enc1 == '.' ||
	     *enc1 == '_')
	enc1++;
      
      while (*enc2 == '-' ||
	     *enc2 == '.' ||
	     *enc2 == '_')
	enc2++;

      if (g_ascii_tolower (*enc1) != g_ascii_tolower (*enc2))
	return FALSE;
      enc1++;
      enc2++;
    }

  while (*enc1 == '-' ||
	 *enc1 == '.' ||
	 *enc1 == '_')
    enc1++;
  
  while (*enc2 == '-' ||
	 *enc2 == '.' ||
	 *enc2 == '_')
    enc2++;

  if (*enc1 || *enc2)
    return FALSE;
  
  return TRUE;
}

const char *
get_canonical_encoding (const char *encoding)
{
  int i;
  for (i = 0; i < G_N_ELEMENTS (enc_aliases); i++)
    {
      if (aliases_equal (enc_aliases[i].alias, encoding))
	return enc_aliases[i].value;
    }

  for (i = 0; i < G_N_ELEMENTS (known_encodings); i++)
    {
      if (aliases_equal (known_encodings[i].encoding, encoding))
	return known_encodings[i].encoding;
    }
  
  return encoding;
}

gboolean
lang_tag_matches (const char *l, const char *spec)
{
  char *l2;
  
  if (strcmp (l, spec) == 0)
    return TRUE;

  l2 = strchr (l, '_');

  if (l2 && strchr (spec, '_') == NULL &&
      strncmp (l, spec, l2 - l) == 0)
    return TRUE;
  
  return FALSE;
}

const char *
get_encoding_from_lang (const char *lang)
{
  int i, j;
  
  for (i = 0; i < G_N_ELEMENTS (known_encodings); i++)
    {
      for (j = 0; j < N_LANG; j++)
	{
	  if (known_encodings[i].langs[j] && lang_tag_matches (lang, known_encodings[i].langs[j]))
	    return known_encodings[i].encoding;
	}
    }
  return NULL;
}

const char *
get_encoding (const char *locale)
{
  char *encoding;

  encoding = strchr(locale, '.');

  if (encoding)
    {
      encoding++;

      return get_canonical_encoding (encoding);
    }
  
  return get_encoding_from_lang (locale);
}

void
validate_string (const char *value, const char *key, const char *locale, char *filename, GnomeDesktopFile *df)
{
  const char *p;
  gboolean ok = TRUE;
  char *k;

  p = value;
  while (*p)
    {
      if (!(g_ascii_isprint (*p) || *p == '\n' || *p == '\t'))
	{
	  ok = FALSE;
	  break;
	}
      
      p++;
    }

  if (!ok)
    {
      if (locale)
	k = g_strdup_printf ("%s[%s]", key, locale);
      else
	k = g_strdup_printf ("%s", key);
      print_fatal ("Error in file %s, Invalid characters in value of key %s. Keys of type string may contain ASCII characters except control characters\n", filename, k);
      g_free (k);
    }
}

void
validate_strings (const char *value, const char *key, const char *locale, char *filename, GnomeDesktopFile *df)
{
  const char *p;
  gboolean ok = TRUE;
  char *k;

  p = value;
  while (*p)
    {
      if (!(g_ascii_isprint (*p) || *p == '\n' || *p == '\t'))
	{
	  ok = FALSE;
	  break;
	}
      
      p++;
    }

  if (!ok)
    {
      if (locale)
	k = g_strdup_printf ("%s[%s]", key, locale);
      else
	k = g_strdup_printf ("%s", key);
      print_fatal ("Error in file %s, Invalid characters in value of key %s. Keys of type strings may contain ASCII characters except control characters\n", filename, k);
      g_free (k);
    }
}

void
validate_localestring (const char *value, const char *key, const char *locale, char *filename, GnomeDesktopFile *df)
{
  char *k;
  const char *encoding;
  char *res;
  GError *error;

  if (locale)
    k = g_strdup_printf ("%s[%s]", key, locale);
  else
    k = g_strdup_printf ("%s", key);


  if (gnome_desktop_file_get_encoding (df) == GNOME_DESKTOP_FILE_ENCODING_UTF8)
    {
      if (!g_utf8_validate (value, -1, NULL))
	print_fatal ("Error, value for key %s in file %s contains invalid UTF-8 characters, even though the encoding is UTF-8.\n", k, filename);
    }
  else if (gnome_desktop_file_get_encoding (df) == GNOME_DESKTOP_FILE_ENCODING_LEGACY)
    {
      if (locale)
	{
	  encoding = get_encoding (locale);

	  if (encoding)
	    {
	      error = NULL;
	      res = g_convert (value, -1,            
			       "UTF-8",
			       encoding,
			       NULL,     
			       NULL,  
			       &error);
	      if (!res && error && error->code == G_CONVERT_ERROR_ILLEGAL_SEQUENCE)
		print_fatal ("Error, value for key %s in file %s contains characters that are invalid in the %s encoding.\n", k, filename, encoding);
	      else if (!res && error && error->code == G_CONVERT_ERROR_NO_CONVERSION)
		print_warning ("Warning, encoding (%s) for key %s in file %s is not supported by iconv.\n", encoding, k, filename);
		
	      g_free (res);
	    }
	  else
	    print_fatal ("Error in file %s, no encoding specified for locale %s\n", filename, locale);
 	}
      else
	{
	  guchar *p = (guchar *)value;
	  gboolean ok = TRUE;
	  /* non-translated strings in legacy-mixed has to be ascii. */
	  while (*p)
	    {
	      if (*p > 127)
		{
		  ok = FALSE;
		  break;
		}
	      
	      p++;
	    }
	  if (!ok)
	    print_fatal ("Error in file %s, untranslated localestring key %s has non-ascii characters in its value\n", filename, key);
	}
    }

  g_free (k);
}

void
validate_regexps (const char *value, const char *key, const char *locale, char *filename, GnomeDesktopFile *df)
{
  const char *p;
  gboolean ok = TRUE;
  char *k;

  p = value;
  while (*p)
    {
      if (!(g_ascii_isprint (*p) || *p == '\n' || *p == '\t'))
	{
	  ok = FALSE;
	  break;
	}
      
      p++;
    }

  if (!ok)
    {
      if (locale)
	k = g_strdup_printf ("%s[%s]", key, locale);
      else
	k = g_strdup_printf ("%s", key);
      print_fatal ("Error in file %s, Invalid characters in value of key %s. Keys of type regexps may contain ASCII characters except control characters\n", filename, k);
      g_free (k);
    }
}

void
validate_boolean (const char *value, const char *key, const char *locale, char *filename, GnomeDesktopFile *df)
{
  if (strcmp (value, "true") != 0 &&
      strcmp (value, "false") != 0)
    print_fatal ("Error in file %s, Invalid characters in value of key %s. Boolean values must be \"false\" or \"true\", the value was \"%s\".\n", filename, key, value);
  
}

void
validate_boolean_or_01 (const char *value, const char *key, const char *locale, char *filename, GnomeDesktopFile *df)
{
  if (strcmp (value, "true") != 0 &&
      strcmp (value, "false") != 0 &&
      strcmp (value, "0") != 0 &&
      strcmp (value, "1") != 0)
    print_fatal ("Error in file %s, Invalid characters in value of key %s. Boolean values must be \"false\" or \"true\", the value was \"%s\".\n", filename, key, value);

  if (strcmp (value, "0") == 0 ||
      strcmp (value, "1") == 0)
    print_warning ("Warning in file %s, boolean key %s has value %s. Boolean values should be \"false\" or \"true\", although 0 and 1 is allowed in this field for backwards compatibility.\n", filename, key, value);
}

void
validate_numeric (const char *value, const char *key, const char *locale, char *filename, GnomeDesktopFile *df)
{
  float d;
  int res;
  
  res = sscanf( value, "%f", &d);
  if (res == 0)
    print_fatal ("Error in file %s, numeric key %s has value %s, which doesn't look like a number.\n", filename, key, value);
}

struct {
  char *keyname;
  void (*validate_type) (const char *value, const char *key, const char *locale, char *filename, GnomeDesktopFile *df);
  gboolean deprecated;
} key_table[] = {
  { "Encoding", validate_string },
  { "Version", validate_numeric },
  { "Name", validate_localestring },
  { "Type", validate_string },
  { "FilePattern", validate_regexps },
  { "TryExec", validate_string },
  { "NoDisplay", validate_boolean },
  { "Comment", validate_localestring },
  { "Exec", validate_string },
  { "Actions", validate_strings },
  { "Icon", validate_string },
  { "MiniIcon", validate_string, TRUE },
  { "Hidden", validate_boolean },
  { "Path", validate_string },
  { "Terminal", validate_boolean_or_01 },
  { "TerminalOptions", validate_string, /* FIXME: Should be deprecated? */ },
  { "SwallowTitle", validate_localestring },
  { "SwallowExec", validate_string }, 
  { "MimeType", validate_regexps },
  { "Patterns", validate_regexps },
  { "DefaultApp", validate_string },
  { "Dev", validate_string },
  { "FSType", validate_string },
  { "MountPoint", validate_string },
  { "ReadOnly", validate_boolean_or_01 },
  { "UnmountIcon", validate_string },
  { "SortOrder", validate_strings /* Fixme: Also comma-separated */},
  { "URL", validate_string },
};

void
enum_keys (GnomeDesktopFile *df,
	   const char       *key, /* If NULL, value is comment line */
	   const char       *locale,
	   const char       *value, /* This is raw unescaped data */
	   gpointer          user_data)
{
  struct KeyData *data = user_data;
  struct KeyHashData *hash_data;
  const char *p;
  int i;
  
  if (key == NULL)
    {
      if (!g_utf8_validate (value, -1, NULL))
	print_warning ("Warning, file %s contains non UTF-8 comments\n", data->filename);

      return;
    }

  hash_data = g_hash_table_lookup (data->hash, key);
  if (hash_data == NULL)
    {
      hash_data = g_new0 (struct KeyHashData, 1);
      g_hash_table_insert (data->hash, (char *)key, hash_data);
    }

  if (locale == NULL) {
    if (hash_data->has_non_translated)
	print_fatal ("Error, file %s contains multiple assignments of key %s\n", data->filename, key);
      
    hash_data->has_non_translated = TRUE;
  } else {
    hash_data->has_translated = TRUE;
  }

#ifdef VERIFY_CANONICAL_ENCODING_NAME
  if (locale)
    {
      const char *encoding;
      const char *canonical;
      
      encoding = strchr(locale, '.');

      if (encoding)
	{
	  encoding++;

	  canonical = get_canonical_encoding (encoding);
	  if (strcmp (encoding, canonical) != 0)
	    print_warning ("Warning in file %s, non-canonical encoding %s specified. The canonical name of the encoding is %s\n", data->filename, encoding, canonical);
	}
    }
#endif

  for (i = 0; i < G_N_ELEMENTS (key_table); i++)
    {
      if (strcmp (key_table[i].keyname, key) == 0)
	break;
    }

  if (i < G_N_ELEMENTS (key_table))
    {
      if (key_table[i].validate_type)
	(*key_table[i].validate_type) (value, key, locale, data->filename, df);
      if (key_table[i].deprecated)
	print_warning ("Warning, file %s contains key %s. Usage of this key is not recommended, since it has been deprecated\n", data->filename, key);
      
    }
  else
    {
      if (strncmp (key, "X-", 2) != 0)
	print_fatal ("Error, file %s contains unknown key %s, extensions to the spec should use keys starting with \"X-\".\n", data->filename, key);
    }

  /* Validation of specific keys */

  if (strcmp (key, "Icon") == 0)
    {
      if (strchr (value, '.') == NULL)
	print_warning ("Warning, icon '%s' specified in file %s does not seem to contain a filename extension\n", value, data->filename);
    }
  
  if (strcmp (key, "Exec") == 0)
    {
      if (strstr (value, "NO_XALF") != NULL)
	print_fatal ("Error, The Exec string for file %s includes the nonstandard broken NO_XALF prefix\n", data->filename);

      p = value;
      while (*p)
	{
	  if (*p == '%')
	    {
	      p++;
	      if (*p != 'f' && *p != 'F' &&
		  *p != 'u' && *p != 'U' &&
		  *p != 'd' && *p != 'D' &&
		  *p != 'n' && *p != 'N' &&
		  *p != 'i' && *p != 'm' &&
		  *p != 'c' && *p != 'k' &&
		  *p != 'v' && *p != '%')
		print_fatal ("Error, The Exec string for file %s includes non-standard parameter %%%c\n", data->filename, *p);
	      if (*p == 0)
		break;
	    }
	  p++;
	}
    }

  
}


static void
enum_hash_keys (gpointer       key,
		gpointer       value,
		gpointer       user_data)
{
  struct KeyData *data = user_data;
  struct KeyHashData *hash_data = value;

  if (hash_data->has_translated &&
      !hash_data->has_non_translated)
    print_fatal ("Error in file %s, key %s is translated, but no untranslated version exists\n", data->filename, (char *)key);
  
}

void
generic_keys (GnomeDesktopFile *df, char *filename)
{
  struct KeyData data = {0 };
  
  data.hash = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
  data.filename = filename;
    
  gnome_desktop_file_foreach_key (df, NULL, TRUE,
				  enum_keys, &data);

  g_hash_table_foreach (data.hash,
			enum_hash_keys,
			&data);

  g_hash_table_destroy (data.hash);
}

struct SectionData {
  gboolean has_desktop_entry;
  gboolean has_kde_desktop_entry;
  GHashTable *hash;
  char *filename;
};

void
enum_sections (GnomeDesktopFile *df,
	       const char       *name,
	       gpointer          data)
{
  struct SectionData *section = data;

  if (name && strcmp (name, "Desktop Entry") == 0)
    section->has_desktop_entry = TRUE;
  else if (name && strcmp (name, "KDE Desktop Entry") == 0)
    section->has_kde_desktop_entry = TRUE;
  else if (name && strncmp (name, "X-", 2) != 0)
    print_fatal ("Error, file %s contains section %s, extensions to the spec should use section names starting with \"X-\".\n", section->filename, name);

  if (name)
    {
      if (g_hash_table_lookup (section->hash, name))
	print_fatal ("Error, file %s contains multiple sections named %s\n", section->filename, name);
      else
	g_hash_table_insert (section->hash, (char *)name, (char *)name);
    }
}

gboolean
required_section (GnomeDesktopFile *df, char *filename)
{
  struct SectionData section = {FALSE, FALSE};
  
  section.hash = g_hash_table_new (g_str_hash, g_str_equal);
  section.filename = filename;
    
  gnome_desktop_file_foreach_section (df, enum_sections, &section);

  if (!section.has_desktop_entry && !section.has_kde_desktop_entry)
    {
      print_fatal ("Error, file %s doesn't contain a desktop entry section\n", filename);
      return FALSE;
    }
  else if (section.has_kde_desktop_entry)
    {
      print_warning ("Warning, file %s contains a \"KDE Desktop Entry\" section. This has been deprecated in favor of \"Desktop Entry\"\n", filename);
    }

  g_hash_table_destroy (section.hash);
  
  return TRUE;
}

gboolean
required_keys (GnomeDesktopFile *df, char *filename)
{
  char *val;
  
  if (gnome_desktop_file_get_raw (df, NULL,
				  "Encoding",
				  NULL, &val))
    {
      if (strcmp (val, "UTF-8") != 0 &&
	  strcmp (val, "Legacy-Mixed") != 0)
	print_fatal ("Error, file %s specifies unknown encoding type '%s'.\n", filename, val);
      return FALSE;
    }
  else
    {
      print_fatal ("Error, file %s does not contain the \"Encoding\" key. This is a required field for all desktop files.\n", filename);
    }

  if (!gnome_desktop_file_get_raw (df, NULL,
				   "Name",
				   NULL, &val))
    {
      print_fatal ("Error, file %s does not contain the \"Name\" key. This is a required field for all desktop files.\n", filename);
    }

  if (gnome_desktop_file_get_raw (df, NULL,
				  "Type",
				  NULL, &val))
    {
      if (strcmp (val, "Application") != 0 &&
	  strcmp (val, "Link") != 0 &&
	  strcmp (val, "FSDevice") != 0 &&
	  strcmp (val, "MimeType") != 0 &&
	  strcmp (val, "Directory") != 0 &&
	  strcmp (val, "Service") != 0 &&
	  strcmp (val, "ServiceType") != 0)
	{
	  print_fatal ("Error, file %s specifies an invalid type '%s'.\n", filename, val);
	  return FALSE;
	}
    }
  else
    {
      print_fatal ("Error, file %s does not contain the \"Type\" key. This is a required field for all desktop files.\n", filename);
    }
  return TRUE;
}

static void
validate_desktop_file (GnomeDesktopFile *df, char *filename)
{
  char *name;
  char *comment;
  
  if (!required_section (df, filename))
    return;
  if (!required_keys (df, filename))
    return;

  generic_keys (df, filename);

  if (gnome_desktop_file_get_raw (df, NULL, "Name", NULL, &name) &&
      gnome_desktop_file_get_raw (df, NULL, "Comment", NULL, &comment))
    {
      if (strcmp (name, comment) == 0)
	print_warning ("Warning in file %s, the fields Name and Comment have the same value\n", filename);
    }      
}



int 
main (int argc, char *argv[])
{
  char *contents;
  GnomeDesktopFile *df;
  GError *error;
  char *filename;

  if (argc == 2)
    filename = argv[1];
  else
    {
      g_printerr ("Usage: %s <desktop-file>\n", argv[0]);
      exit (1);
    }
  
  if (!g_file_get_contents (filename, &contents,
			    NULL, NULL)) {
    g_printerr ("error reading desktop file '%s'\n", filename);
    return 1;
  }

  error = NULL;
  df = gnome_desktop_file_new_from_string (contents, &error);
  
  if (!df)
    {
      g_printerr ("Error parsing %s: %s\n", filename, error->message);
      return 1;
    }

  if (df)
    validate_desktop_file (df, filename);

  if (fatal_error_occurred)
    return 1;
  else
    return 0;
}
