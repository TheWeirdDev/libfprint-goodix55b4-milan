/*
 * Goodix 55b4 - pairing-key (PSK) selection. See goodix55b4-secrets.h.
 */
#define FP_COMPONENT "goodix55b4"

#include "drivers_api.h"
#include "goodix55b4.h"
#include "goodix55b4-secrets.h"

#include <string.h>

#define PSK_FILE_ENV "GOODIX55B4_PSK_FILE"
#define PSK_FILE_DEFAULT "/etc/goodix55b4/psk.hex"

typedef struct
{
  guint8   psk[GOODIX55B4_PSK_LEN];
  guint8   hash[GOODIX55B4_PSK_LEN];
  gboolean custom;
} Goodix55b4Psk;

/* Parse one 64-char hex line into exactly GOODIX55B4_PSK_LEN bytes. */
static gboolean
parse_hex_line (const char *line, guint8 out[GOODIX55B4_PSK_LEN])
{
  gsize n = 0;

  for (const char *p = line; *p && *p != '\n' && *p != '\r'; p++)
    {
      int hi, lo;
      if (g_ascii_isspace (*p))
        continue;
      hi = g_ascii_xdigit_value (p[0]);
      lo = p[1] ? g_ascii_xdigit_value (p[1]) : -1;
      if (hi < 0 || lo < 0 || n >= GOODIX55B4_PSK_LEN)
        return FALSE;
      out[n++] = (guint8) ((hi << 4) | lo);
      p++;
    }
  return n == GOODIX55B4_PSK_LEN;
}

gboolean
goodix55b4_psk_load_file (const char *path,
                          guint8      psk[GOODIX55B4_PSK_LEN],
                          guint8      hash[GOODIX55B4_PSK_LEN],
                          GError    **error)
{
  g_autofree char *contents = NULL;
  g_auto(GStrv) lines = NULL;
  guint8 tmp_psk[GOODIX55B4_PSK_LEN];
  guint8 tmp_hash[GOODIX55B4_PSK_LEN];

  g_return_val_if_fail (path != NULL, FALSE);
  g_return_val_if_fail (psk != NULL && hash != NULL, FALSE);

  if (!g_file_get_contents (path, &contents, NULL, error))
    return FALSE;

  lines = g_strsplit (contents, "\n", 3);
  if (g_strv_length (lines) < 2 ||
      !parse_hex_line (lines[0], tmp_psk) ||
      !parse_hex_line (lines[1], tmp_hash))
    {
      explicit_bzero (tmp_psk, sizeof (tmp_psk));
      explicit_bzero (contents, strlen (contents));
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "expected two lines of 64 hex characters (PSK, then PMK "
                   "hash)");
      return FALSE;
    }
  memcpy (psk, tmp_psk, sizeof (tmp_psk));
  memcpy (hash, tmp_hash, sizeof (tmp_hash));
  explicit_bzero (tmp_psk, sizeof (tmp_psk));
  explicit_bzero (contents, strlen (contents));
  return TRUE;
}

gboolean
goodix55b4_psk_select (const char *path,
                       gboolean    explicit_path,
                       guint8      psk[GOODIX55B4_PSK_LEN],
                       guint8      hash[GOODIX55B4_PSK_LEN])
{
  g_autoptr(GError) error = NULL;

  g_return_val_if_fail (path != NULL, FALSE);
  g_return_val_if_fail (psk != NULL && hash != NULL, FALSE);

  memset (psk, 0, GOODIX55B4_PSK_LEN);
  memcpy (hash, goodix55b4_zero_psk_hash, GOODIX55B4_PSK_LEN);

  /* The default path is optional; an explicitly named one must load. */
  if (!explicit_path && !g_file_test (path, G_FILE_TEST_EXISTS))
    {
      fp_dbg ("no PSK file at %s; using the zero PSK", path);
      return FALSE;
    }
  if (!goodix55b4_psk_load_file (path, psk, hash, &error))
    {
      memset (psk, 0, GOODIX55B4_PSK_LEN);
      memcpy (hash, goodix55b4_zero_psk_hash, GOODIX55B4_PSK_LEN);
      fp_warn ("ignoring PSK file %s: %s; falling back to the zero PSK",
               path, error->message);
      return FALSE;
    }
  fp_info ("using the PSK from %s (the sensor keeps its Windows pairing "
           "key)", path);
  return TRUE;
}

static const Goodix55b4Psk *
get_psk (void)
{
  static Goodix55b4Psk psk;
  static gsize once = 0;

  if (g_once_init_enter (&once))
    {
      const char *env = g_getenv (PSK_FILE_ENV);
      gboolean explicit_path = env && *env;

      psk.custom = goodix55b4_psk_select (explicit_path ? env : PSK_FILE_DEFAULT,
                                          explicit_path, psk.psk, psk.hash);
      g_once_init_leave (&once, 1);
    }

  return &psk;
}

const guint8 *
goodix55b4_psk (void)
{
  return get_psk ()->psk;
}

const guint8 *
goodix55b4_psk_hash (void)
{
  return get_psk ()->hash;
}

gboolean
goodix55b4_psk_is_custom (void)
{
  return get_psk ()->custom;
}
