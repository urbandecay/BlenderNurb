/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "dupli_parent_finder.hh"

#include "BLI_assert.h"

#include "DNA_object_types.h"

namespace blender::io {

void DupliParentFinder::insert(const DupliObject *dupli_ob)
{
  dupli_set_.add(dupli_ob->ob);

  PersistentID dupli_pid(dupli_ob);
  pid_to_dupli_.add(dupli_pid, dupli_ob);
}

bool DupliParentFinder::is_duplicated(const Object *object) const
{
  return dupli_set_.contains(object);
}

const DupliObject *DupliParentFinder::find_suitable_export_parent(
    const DupliObject *dupli_ob) const
{
  return find_instancer(dupli_ob);
}

const DupliObject *DupliParentFinder::find_instancer(const DupliObject *dupli_ob) const
{
  PersistentID dupli_pid(dupli_ob);
  PersistentID parent_pid = dupli_pid.instancer_pid();

  const DupliObject *const *found = pid_to_dupli_.lookup_ptr(parent_pid);
  if (!found) {
    return nullptr;
  }

  const DupliObject *instancer_dupli = *found;
  return instancer_dupli;
}

}  // namespace blender::io
