/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edutil
 */

#include <algorithm>
#include <limits>

#include "MEM_guardedalloc.h"

#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_cursor_utf8.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_report.hh"
#include "BKE_unit.hh"

#include "DNA_scene_types.h"

#include "WM_api.hh"
#include "WM_types.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern_run.hh"
#endif

#include "ED_numinput.hh"

#include "UI_interface.hh"

namespace blender {

/* Numeric input which isn't allowing full numeric editing. */
#define USE_FAKE_EDIT

/**
 * #NumInput.flag
 * (1 << 8) and below are reserved for public flags!
 */
enum {
  /** Enable full editing, with units and math operators support. */
  NUM_EDIT_FULL = (1 << 9),
#ifdef USE_FAKE_EDIT
  /** Fake edited state (temp, avoids issue with backspace). */
  NUM_FAKE_EDITED = (1 << 10),
#endif
};

/* NumInput.val_flag[] */
enum {
  /* (1 << 8) and below are reserved for public flags! */
  /** User has edited this value somehow. */
  NUM_EDITED = (1 << 9), /** Current expression for this value is invalid. */
  NUM_INVALID = (1 << 10),
#ifdef USE_FAKE_EDIT
  /** Current expression's result has to be negated. */
  NUM_NEGATE = (1 << 11), /** Current expression's result has to be inverted. */
  NUM_INVERSE = (1 << 12),
#endif
};

/* ************************** Functions *************************** */

bool ED_numinput_double_is_int(double value)
{
  value = std::abs(value);
  /* Use `>=` because `uint64_t::max` rounds up to 2^64 when converted to double. */
  if (!isfinite(value) || value >= double(std::numeric_limits<uint64_t>::max())) {
    return false;
  }
  return value == double(uint64_t(value));
}

/* ************************** NUMINPUT **************************** */

void initNumInput(NumInput *n)
{
  n->idx_max = 0;
  n->unit_sys = USER_UNIT_NONE;
  std::fill_n(n->unit_type, NUM_MAX_ELEMENTS, B_UNIT_NONE);
  n->unit_use_radians = false;

  n->flag = 0;
  std::fill_n(n->val_flag, NUM_MAX_ELEMENTS, 0);
  zero_v3(n->val);
  std::fill_n(n->val_no_units, NUM_MAX_ELEMENTS, 0.0);
  std::fill_n(n->val_org, NUM_MAX_ELEMENTS, 0.0f);
  std::fill_n(n->val_double, NUM_MAX_ELEMENTS, 0.0);
  std::fill_n(n->val_org_double, NUM_MAX_ELEMENTS, 0.0);
  for (int i = 0; i < NUM_MAX_ELEMENTS; i++) {
    n->axis_str[i][0] = '\0';
    n->axis_str_cur[i] = 0;
  }
  std::fill_n(n->val_inc, NUM_MAX_ELEMENTS, 1.0f);

  n->idx = 0;
  n->str[0] = '\0';
  n->str_cur = 0;
}

static double input_value(const NumInput &n, const int index)
{
  return n.flag & NUM_USE_DOUBLE ? n.val_double[index] : n.val[index];
}

static void input_value_set(NumInput &n, const int index, const double value)
{
  n.val_double[index] = value;
  n.val[index] = float(value);
}

void outputNumInput(NumInput *n, char *str, const UnitSettings &unit_settings)
{
  short j;
  const int ln = NUM_STR_REP_LEN;
  int prec = 2; /* draw-only, and avoids too much issues with radian->degrees conversion. */

  for (j = 0; j <= n->idx_max; j++) {
    /* if AFFECTALL and no number typed and cursor not on number, use first number */
    const short i = (n->flag & NUM_AFFECT_ALL && n->idx != j && !(n->val_flag[j] & NUM_EDITED)) ?
                        0 :
                        j;

    /* Use scale_length if needed! */
    const float fac = float(BKE_unit_value_scale(unit_settings, n->unit_type[j], 1.0));
    const double value = input_value(*n, i);

    if (n->val_flag[i] & NUM_EDITED) {
      /* Get the best precision, allows us to draw '10.0001' as '10' instead! */
      prec = ui::calc_float_precision(prec, value);
      if (i == n->idx) {
        const char *heading_exp = "", *trailing_exp = "";
        char before_cursor[NUM_STR_REP_LEN];
        char val[16];

#ifdef USE_FAKE_EDIT
        if (n->val_flag[i] & NUM_NEGATE) {
          heading_exp = (n->val_flag[i] & NUM_INVERSE) ? "-1/(" : "-(";
          trailing_exp = ")";
        }
        else if (n->val_flag[i] & NUM_INVERSE) {
          heading_exp = "1/(";
          trailing_exp = ")";
        }
#endif

        if (n->val_flag[i] & NUM_INVALID) {
          STRNCPY_UTF8(val, RPT_("Invalid"));
        }
        else {
          BKE_unit_value_as_string_adaptive(
              val,
              sizeof(val),
              (n->flag & NUM_USE_DOUBLE) ?
                  BKE_unit_value_scale(unit_settings, n->unit_type[j], value) :
                  double(n->val[i] * fac),
              prec,
              n->unit_sys,
              n->unit_type[i],
              true,
              false,
              true);
        }

        /* +1 because of trailing '\0' */
        BLI_strncpy_utf8(before_cursor, n->str, n->str_cur + 1);
        BLI_snprintf_utf8(&str[j * ln],
                          ln,
                          "[%s%s|%s%s] = %s",
                          heading_exp,
                          before_cursor,
                          &n->str[n->str_cur],
                          trailing_exp,
                          val);
      }
      else {
        const char *cur = (i == n->idx) ? "|" : "";
        if (n->unit_use_radians && n->unit_type[i] == B_UNIT_ROTATION) {
          /* Radian exception... */
          BLI_snprintf_utf8(&str[j * ln], ln, "%s%.6gr%s", cur, value, cur);
        }
        else {
          char tstr[NUM_STR_REP_LEN];
          BKE_unit_value_as_string_adaptive(
              tstr, ln, value, prec, n->unit_sys, n->unit_type[i], true, false, true);
          BLI_snprintf_utf8(&str[j * ln], ln, "%s%s%s", cur, tstr, cur);
        }
      }
    }
    else {
      const char *cur = (i == n->idx) ? "|" : "";
      BLI_snprintf_utf8(&str[j * ln], ln, "%sNONE%s", cur, cur);
    }
    /* We might have cut some multi-bytes UTF8 chars
     * (e.g. trailing degrees symbol values can become only 'A'). */
    BLI_str_utf8_invalid_strip(&str[j * ln], strlen(&str[j * ln]));
  }
}

bool hasNumInput(const NumInput *n)
{
  short i;

#ifdef USE_FAKE_EDIT
  if (n->flag & NUM_FAKE_EDITED) {
    return true;
  }
#endif

  for (i = 0; i <= n->idx_max; i++) {
    if (n->val_flag[i] & NUM_EDITED) {
      return true;
    }
  }

  return false;
}

template<typename Float> static bool apply_num_input(NumInput *n, Float *vec)
{
  short i, j;
  Float val;
  const bool use_double = n->flag & NUM_USE_DOUBLE;

  if (hasNumInput(n)) {
    for (j = 0; j <= n->idx_max; j++) {
#ifdef USE_FAKE_EDIT
      if (n->flag & NUM_FAKE_EDITED) {
        val = Float(use_double ? n->val_double[j] : n->val[j]);
      }
      else
#endif
      {
        /* if AFFECTALL and no number typed and cursor not on number, use first number */
        i = (n->flag & NUM_AFFECT_ALL && n->idx != j && !(n->val_flag[j] & NUM_EDITED)) ? 0 : j;
        val = (!(n->val_flag[i] & NUM_EDITED) && n->val_flag[i] & NUM_NULL_ONE) ?
                  Float(1) :
                  Float(use_double ? n->val_double[i] : n->val[i]);

        if (n->val_flag[i] & NUM_NO_NEGATIVE && val < 0.0f) {
          val = 0.0f;
        }
        if (n->val_flag[i] & NUM_NO_FRACTION && val != std::floor(val)) {
          val = std::floor(val + Float(0.5));
          if (n->val_flag[i] & NUM_NO_ZERO && val == 0.0f) {
            val = 1.0f;
          }
        }
        else if (n->val_flag[i] & NUM_NO_ZERO && val == 0.0f) {
          val = Float(0.0001);
        }
      }
      vec[j] = val;
    }
#ifdef USE_FAKE_EDIT
    n->flag &= ~NUM_FAKE_EDITED;
#endif
    return true;
  }

  /* Else, we set the 'org' values for numinput! */
  for (j = 0; j <= n->idx_max; j++) {
    n->val[j] = n->val_org[j] = vec[j];
    n->val_double[j] = n->val_org_double[j] = vec[j];
  }
  return false;
}

bool applyNumInput(NumInput *n, float *vec)
{
  return apply_num_input(n, vec);
}

bool applyNumInput(NumInput *n, double *vec)
{
  BLI_assert(n->flag & NUM_USE_DOUBLE);
  return apply_num_input(n, vec);
}

static void value_to_editstr(NumInput *n, int idx)
{
  const int prec = 6; /* editing, higher precision needed. */
  n->str_cur = BKE_unit_value_as_string_adaptive(n->str,
                                                 NUM_STR_REP_LEN,
                                                 double(n->val[idx]),
                                                 prec,
                                                 n->unit_sys,
                                                 n->unit_type[idx],
                                                 true,
                                                 false,
                                                 true);
}

static bool editstr_insert_at_cursor(NumInput *n, const char *buf, const int buf_len)
{
  int cur = n->str_cur;
  int len = strlen(&n->str[cur]) + 1; /* +1 for the trailing '\0'. */
  int n_cur = cur + buf_len;

  if (n_cur + len >= NUM_STR_REP_LEN) {
    return false;
  }

  memmove(&n->str[n_cur], &n->str[cur], len);
  memcpy(&n->str[cur], buf, sizeof(char) * buf_len);

  n->str_cur = n_cur;
  return true;
}

bool user_string_to_number(bContext *C,
                           const char *str,
                           const UnitSettings &unit,
                           int type,
                           double *r_value,
                           double *r_value_no_units,
                           const bool use_single_line_error,
                           char **r_error,
                           const bool use_double_units)
{
#ifdef WITH_PYTHON
  BPy_RunErrInfo err_info{};
  err_info.use_single_line_error = use_single_line_error;
  err_info.r_string = r_error;

  const double unit_scale = BKE_unit_value_scale(unit, type, 1.0);
  const UnitConversionPrecision precision = use_double_units ? UnitConversionPrecision::Double :
                                                               UnitConversionPrecision::Legacy;
  if (BKE_unit_string_contains_unit(str, type)) {
    /* Expanded double conversion factors can be longer than the original unit tokens. */
    char str_unit_convert[NUM_STR_REP_LEN * 16];
    STRNCPY_UTF8(str_unit_convert, str);
    BKE_unit_replace_string(
        str_unit_convert, sizeof(str_unit_convert), str, unit_scale, unit.system, type, precision);

    bool success = BPY_run_string_as_number(C, nullptr, str_unit_convert, &err_info, r_value);
    if (r_value_no_units) {
      /* When explicit units are in the string, the pre-scale value isn't directly recoverable.
       * Store the post-conversion value; callers relying on integer detection will naturally
       * reject non-integer results (e.g. "90deg" evaluates to ~1.5708 radians). */
      *r_value_no_units = *r_value;
    }
    return success;
  }

  bool success = BPY_run_string_as_number(C, nullptr, str, &err_info, r_value);
  if (r_value_no_units) {
    *r_value_no_units = *r_value;
  }
  *r_value = BKE_unit_apply_preferred_unit(unit, type, *r_value, precision);
  *r_value /= unit_scale;
  return success;

#else
  UNUSED_VARS(C, unit, type, use_single_line_error, r_error, use_double_units);
  *r_value = atof(str);
  if (r_value_no_units) {
    *r_value_no_units = *r_value;
  }
  return true;
#endif
}

static bool editstr_is_simple_numinput(const char ascii)
{
  if (ascii >= '0' && ascii <= '9') {
    return true;
  }
  if (ascii == '.') {
    return true;
  }
  return false;
}

bool handleNumInput(bContext *C, NumInput *n, const wmEvent *event)
{
  const char *utf8_buf = nullptr;
  const char event_ascii = WM_event_utf8_to_ascii(event);
  char ascii[2] = {'\0', '\0'};
  bool updated = false;
  short idx = n->idx, idx_max = n->idx_max;
  short dir = STRCUR_DIR_NEXT, mode = STRCUR_JUMP_NONE;
  int cur;

#ifdef USE_FAKE_EDIT
  if (U.flag & USER_FLAG_NUMINPUT_ADVANCED)
#endif
  {
    if (((event->modifier & (KM_CTRL | KM_ALT)) == 0) && (event_ascii != '\0') &&
        strchr("01234567890@%^&*-+/{}()[]<>.|", event_ascii))
    {
      if (!(n->flag & NUM_EDIT_FULL)) {
        n->flag |= NUM_EDITED;
        n->flag |= NUM_EDIT_FULL;
        n->val_flag[idx] |= NUM_EDITED;
      }
    }
  }

#ifdef USE_FAKE_EDIT
  /* XXX Hack around keyboards without direct access to '=' nor '*'... */
  if (ELEM(event_ascii, '=', '*')) {
    if (!(n->flag & NUM_EDIT_FULL)) {
      n->flag |= NUM_EDIT_FULL;
      n->val_flag[idx] |= NUM_EDITED;
      return true;
    }
    if (event->modifier & KM_CTRL) {
      n->flag &= ~NUM_EDIT_FULL;
      return true;
    }
  }
#endif

  switch (event->type) {
    case EVT_MODAL_MAP:
      if (ELEM(event->val, NUM_MODAL_INCREMENT_UP, NUM_MODAL_INCREMENT_DOWN)) {
        if (n->flag & NUM_USE_DOUBLE) {
          const double increment = event->val == NUM_MODAL_INCREMENT_UP ? n->val_inc[idx] :
                                                                          -n->val_inc[idx];
          const double value = n->val_double[idx] + increment;
          if (!std::isfinite(value)) {
            n->val_flag[idx] |= NUM_INVALID;
            return true;
          }
          input_value_set(*n, idx, value);
          const UnitSettings &unit = CTX_data_scene(C)->unit;
          const double scale = BKE_unit_value_scale(unit, n->unit_type[idx], 1.0);
          const double preferred = BKE_unit_apply_preferred_unit(
              unit, n->unit_type[idx], 1.0, UnitConversionPrecision::Double);
          n->val_no_units[idx] = value * scale / preferred;
          n->str_cur = BLI_snprintf_rlen(n->str, sizeof(n->str), "%.17g", n->val_no_units[idx]);
          n->val_flag[idx] = (n->val_flag[idx] &
                              ~(NUM_INVALID | NUM_NEGATE | NUM_INVERSE | NUM_INT_INPUT_VALUE)) |
                             NUM_EDITED;
          if (ED_numinput_double_is_int(n->val_no_units[idx])) {
            n->val_flag[idx] |= NUM_INT_INPUT_VALUE;
          }
          return true;
        }
        n->val[idx] += (event->val == NUM_MODAL_INCREMENT_UP) ? n->val_inc[idx] : -n->val_inc[idx];
        value_to_editstr(n, idx);
        n->val_flag[idx] |= NUM_EDITED;
        updated = true;
      }
      else {
        /* might be a char too... */
        utf8_buf = event->utf8_buf;
        ascii[0] = event_ascii;
      }
      break;
    case EVT_BACKSPACEKEY:
      /* Part specific to backspace... */
      if (!(n->val_flag[idx] & NUM_EDITED)) {
        copy_v3_v3(n->val, n->val_org);
        std::copy_n(n->val_org_double, NUM_MAX_ELEMENTS, n->val_double);
        if (n->flag & NUM_USE_DOUBLE) {
          for (int i = 0; i < NUM_MAX_ELEMENTS; i++) {
            n->axis_str[i][0] = '\0';
            n->axis_str_cur[i] = 0;
          }
        }
        n->val_flag[0] &= ~NUM_EDITED;
        n->val_flag[1] &= ~NUM_EDITED;
        n->val_flag[2] &= ~NUM_EDITED;
#ifdef USE_FAKE_EDIT
        n->flag |= NUM_FAKE_EDITED;
#else
        n->flag |= NUM_EDIT_FULL;
#endif
        updated = true;
        break;
      }
      else if ((event->modifier & KM_SHIFT) || !n->str[0]) {
        n->val[idx] = n->val_org[idx];
        n->val_double[idx] = n->val_org_double[idx];
        n->val_flag[idx] &= ~NUM_EDITED;
        n->str[0] = '\0';
        n->str_cur = 0;
        updated = true;
        break;
      }
      /* Else, common behavior with DELKEY,
       * only difference is remove char(s) before/after the cursor. */
      dir = STRCUR_DIR_PREV;
      ATTR_FALLTHROUGH;
    case EVT_DELKEY:
      if ((n->val_flag[idx] & NUM_EDITED) && n->str[0]) {
        int t_cur = cur = n->str_cur;
        if (event->modifier & KM_CTRL) {
          mode = STRCUR_JUMP_DELIM;
        }
        BLI_str_cursor_step_utf8(n->str,
                                 strlen(n->str),
                                 &t_cur,
                                 eStrCursorJumpDirection(dir),
                                 eStrCursorJumpType(mode),
                                 true);
        if (t_cur != cur) {
          if (t_cur < cur) {
            std::swap(t_cur, cur);
            n->str_cur = cur;
          }
          /* +1 for trailing '\0'. */
          memmove(&n->str[cur], &n->str[t_cur], strlen(&n->str[t_cur]) + 1);
          updated = true;
        }
        if (!n->str[0]) {
          n->val[idx] = n->val_org[idx];
          n->val_double[idx] = n->val_org_double[idx];
        }
      }
      else {
        return false;
      }
      break;
    case EVT_LEFTARROWKEY:
      dir = STRCUR_DIR_PREV;
      ATTR_FALLTHROUGH;
    case EVT_RIGHTARROWKEY:
      cur = n->str_cur;
      if (event->modifier & KM_CTRL) {
        mode = STRCUR_JUMP_DELIM;
      }
      BLI_str_cursor_step_utf8(n->str,
                               strlen(n->str),
                               &cur,
                               eStrCursorJumpDirection(dir),
                               eStrCursorJumpType(mode),
                               true);
      if (cur != n->str_cur) {
        n->str_cur = cur;
        return true;
      }
      return false;
    case EVT_HOMEKEY:
      if (n->str[0]) {
        n->str_cur = 0;
        return true;
      }
      return false;
    case EVT_ENDKEY:
      if (n->str[0]) {
        n->str_cur = strlen(n->str);
        return true;
      }
      return false;
    case EVT_TABKEY:
      if (n->flag & NUM_USE_DOUBLE) {
        STRNCPY(n->axis_str[idx], n->str);
        n->axis_str_cur[idx] = n->str_cur;
        idx = (idx + idx_max + ((event->modifier & KM_CTRL) ? 0 : 2)) % (idx_max + 1);
        n->idx = idx;
        STRNCPY(n->str, n->axis_str[idx]);
        n->str_cur = n->axis_str_cur[idx];
        return true;
      }
#ifdef USE_FAKE_EDIT
      n->val_flag[idx] &= ~(NUM_NEGATE | NUM_INVERSE);
#endif

      idx = (idx + idx_max + ((event->modifier & KM_CTRL) ? 0 : 2)) % (idx_max + 1);
      n->idx = idx;
      if (n->val_flag[idx] & NUM_EDITED) {
        value_to_editstr(n, idx);
      }
      else {
        n->str[0] = '\0';
        n->str_cur = 0;
      }
      return true;
    case EVT_PADPERIOD:
    case EVT_PERIODKEY:
      /* Force number-pad "." since some OS's/countries generate a comma char, see: #37992 */
      ascii[0] = '.';
      utf8_buf = ascii;
      break;
#if 0
    /* Those keys are not directly accessible in all layouts,
     * preventing to generate matching events.
     * So we use a hack (ASCII value) instead, see below. */
    case EQUALKEY:
    case PADASTERKEY:
      if (!(n->flag & NUM_EDIT_FULL)) {
        n->flag |= NUM_EDIT_FULL;
        n->val_flag[idx] |= NUM_EDITED;
        return true;
      }
      else if (event->modifier & KM_CTRL) {
        n->flag &= ~NUM_EDIT_FULL;
        return true;
      }
      break;
#endif

#ifdef USE_FAKE_EDIT
    case EVT_PADMINUS:
    case EVT_MINUSKEY:
      if ((event->modifier & KM_CTRL) || !(n->flag & NUM_EDIT_FULL)) {
        n->val_flag[idx] ^= NUM_NEGATE;
        updated = true;
      }
      break;
    case EVT_PADSLASHKEY:
    case EVT_SLASHKEY:
      if ((event->modifier & KM_CTRL) || !(n->flag & NUM_EDIT_FULL)) {
        n->val_flag[idx] ^= NUM_INVERSE;
        updated = true;
      }
      break;
#endif
    case EVT_CKEY:
      if (event->modifier & KM_CTRL) {
        /* Copy current `str` to the copy/paste buffer. */
        WM_clipboard_text_set(n->str, false);
        updated = true;
      }
      break;
    case EVT_VKEY:
      if (event->modifier & KM_CTRL) {
        /* extract the first line from the clipboard */
        int pbuf_len;
        char *pbuf = WM_clipboard_text_get_firstline(false, true, &pbuf_len);

        if (pbuf) {
          const bool success = editstr_insert_at_cursor(n, pbuf, pbuf_len);

          MEM_delete(pbuf);
          if (!success) {
            return false;
          }

          n->val_flag[idx] |= NUM_EDITED;
        }
        updated = true;
      }
      break;
    default:
      break;
  }

  if (!updated && !utf8_buf && event->utf8_buf[0]) {
    utf8_buf = event->utf8_buf;
    ascii[0] = event_ascii;
  }

  /* Up to this point, if we have a ctrl modifier, skip.
   * This allows to still access most of modals' shortcuts even in numinput mode.
   */
  if (!updated && (event->modifier & KM_CTRL)) {
    return false;
  }

  if ((!utf8_buf || !utf8_buf[0]) && ascii[0]) {
    /* Fall back to ascii. */
    utf8_buf = ascii;
  }

  if (utf8_buf && utf8_buf[0]) {
    /* Numeric input is a single line. Return must reach the operator's confirm handler,
     * including when event simulation or a platform supplies its control character. */
    const unsigned char first = static_cast<unsigned char>(utf8_buf[0]);
    if (first < 0x20 || first == 0x7f) {
      return false;
    }
    if (!(n->flag & NUM_EDIT_FULL)) {
      /* In simple edit mode, we only keep a few chars as valid! */
      /* no need to decode unicode, ASCII is first char only. */
      if (!editstr_is_simple_numinput(utf8_buf[0])) {
        return false;
      }
    }

    const int utf8_buf_len = BLI_str_utf8_size_or_error(utf8_buf);
    BLI_assert(utf8_buf_len != -1);
    if (!editstr_insert_at_cursor(n, utf8_buf, utf8_buf_len)) {
      return false;
    }

    n->val_flag[idx] |= NUM_EDITED;
  }
  else if (!updated) {
    return false;
  }

  /* At this point, our value has changed, try to interpret it with python
   * (if str is not empty!). */
  if (n->str[0]) {
    const double val_prev = input_value(*n, idx);
    Scene *sce = CTX_data_scene(C);
    char *error = nullptr;

    double val = 0.0;
    double val_no_units = 0.0;
    int success = user_string_to_number(C,
                                        n->str,
                                        sce->unit,
                                        n->unit_type[idx],
                                        &val,
                                        &val_no_units,
                                        false,
                                        &error,
                                        (n->flag & NUM_USE_DOUBLE) != 0);

    if (error) {
      ReportList *reports = CTX_wm_reports(C);
      printf("%s\n", error);
      BKE_report(reports, RPT_ERROR, error);
      BKE_report(reports, RPT_ERROR, "Numeric input evaluation");
      MEM_delete(error);
    }

    if (success) {
      input_value_set(*n, idx, val);
      n->val_flag[idx] &= ~NUM_INVALID;

      /* Store the pre-unit-scale value and check if it's an integer. */
      n->val_no_units[idx] = val_no_units;
      if (ED_numinput_double_is_int(val_no_units)) {
        n->val_flag[idx] |= NUM_INT_INPUT_VALUE;
      }
      else {
        n->val_flag[idx] &= ~NUM_INT_INPUT_VALUE;
      }
    }
    else {
      n->val_flag[idx] |= NUM_INVALID;
      n->val_flag[idx] &= ~NUM_INT_INPUT_VALUE;
    }

#ifdef USE_FAKE_EDIT
    if (n->val_flag[idx] & NUM_NEGATE) {
      input_value_set(*n, idx, -input_value(*n, idx));
      n->val_no_units[idx] = -n->val_no_units[idx];
    }
    if (n->val_flag[idx] & NUM_INVERSE) {
      val = input_value(*n, idx);
      /* If we invert on radians when user is in degrees,
       * you get unexpected results... See #53463. */
      if (!n->unit_use_radians && n->unit_type[idx] == B_UNIT_ROTATION) {
        val = RAD2DEG(val);
      }
      val = 1.0 / val;
      if (!n->unit_use_radians && n->unit_type[idx] == B_UNIT_ROTATION) {
        val = DEG2RAD(val);
      }
      input_value_set(*n, idx, val);
      n->val_no_units[idx] = 1.0 / n->val_no_units[idx];
      /* Inverse transforms the value to 1/x, invalidating the integer property. */
      n->val_flag[idx] &= ~NUM_INT_INPUT_VALUE;
    }
#endif

    if (UNLIKELY(!isfinite(input_value(*n, idx)))) {
      input_value_set(*n, idx, val_prev);
      n->val_flag[idx] |= NUM_INVALID;
    }
  }

  /* REDRAW SINCE NUMBERS HAVE CHANGED */
  return true;
}

}  // namespace blender
