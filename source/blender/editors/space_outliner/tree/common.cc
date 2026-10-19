/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Functions and helpers shared between tree-display types or other tree related code.
 */

#include "BLI_listbase.h"

#include "BKE_idtype.hh"

#include "DNA_anim_types.h"
#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "../outliner_intern.hh"
#include "common.hh"

namespace blender::ed::outliner {

/* -------------------------------------------------------------------- */
/** \name ID Helpers.
 * \{ */

const char *outliner_idcode_to_plural(short idcode)
{
  const char *propname = BKE_idtype_idcode_to_name_plural(idcode);
  PropertyRNA *prop = RNA_struct_type_find_property(RNA_BlendData, propname);
  return (prop) ? RNA_property_ui_name(prop) : "UNKNOWN";
}

/** \} */

bool outliner_animdata_test(const AnimData *adt)
{
  if (adt) {
    return (adt->action || adt->drivers.first || adt->nla_tracks.first);
  }
  return false;
}

}  // namespace blender::ed::outliner
