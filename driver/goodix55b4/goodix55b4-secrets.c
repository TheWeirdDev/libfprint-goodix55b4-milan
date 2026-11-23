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
#define PSK_LEN 32

typedef struct
{
  guint8   psk[PSK_LEN];
  guint8   hash[PSK_LEN];
  gboolean custom;
} Goodix55b4Psk;

/* Parse one 64-char hex line into exactly PSK_LEN bytes. */
static gboolean
parse_hex_line (const char *line, guint8 out[PSK_LEN])
{
  gsize n = 0;

  for (const char *p = line; *p && *p != '\n' && *p != '\r'; p++)
    {
      int hi, lo;
      if (g_ascii_isspace (*p))
        continue;
      hi = g_ascii_xdigit_value (p[0]);
      lo = p[1] ? g_ascii_xdigit_value (p[1]) : -1;
      if (hi < 0 || lo < 0 || n >= PSK_LEN)
        return FALSE;
      out[n++] = (guint8) ((hi << 4) | lo);
      p++;
    }
  return n == PSK_LEN;
}

/* Load the key file into *psk. Returns FALSE (and sets *why) on any problem;
 * the caller then falls back to the zero PSK. */
static gboolean
load_psk_file (const char *path, Goodix55b4Psk *psk, GError **why)
{
  g_autofree char *contents = NULL;
  g_auto(GStrv) lines = NULL;

  if (!g_file_get_contents (path, &contents, NULL, why))
    return FALSE;

  lines = g_strsplit (contents, "\n", 3);
  if (g_strv_length (lines) < 2 ||
      !parse_hex_line (lines[0], psk->psk) ||
      !parse_hex_line (lines[1], psk->hash))
    {
      g_set_error (why, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "expected two lines of 64 hex characters (PSK, then PMK "
                   "hash)");
      return FALSE;
    }
  psk->custom = TRUE;
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
      const char *path = (env && *env) ? env : PSK_FILE_DEFAULT;
      gboolean explicit_path = env && *env;

      memset (&psk, 0, sizeof (psk));
      memcpy (psk.hash, goodix55b4_zero_psk_hash, PSK_LEN);

      if (explicit_path || g_file_test (path, G_FILE_TEST_EXISTS))
        {
          g_autoptr(GError) error = NULL;
          Goodix55b4Psk loaded;

          memset (&loaded, 0, sizeof (loaded));
          if (load_psk_file (path, &loaded, &error))
            {
              psk = loaded;
              fp_info ("using the PSK from %s (the sensor keeps its Windows "
                       "pairing key)", path);
            }
          else
            {
              fp_warn ("ignoring PSK file %s: %s; falling back to the zero "
                       "PSK", path, error->message);
            }
        }

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
