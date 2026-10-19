/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup balembic
 */

#include "abc_reader_transform.h"
#include "abc_util.h"

#include "DNA_object_types.h"

#include "BLT_translation.hh"

#include "BKE_object.hh"

namespace blender {

using Alembic::Abc::ISampleSelector;

namespace io::alembic {

AbcTransformReader::AbcTransformReader(const AbcReaderConstructorArgs &args) : AbcObjectReader(args)
{
  /* Transform-only Alembic nodes have no geometry data. Preserve their names and transforms
   * so the imported hierarchy remains intact. */
  m_object_name = m_iobject.getName();

  Alembic::AbcGeom::IXform xform(m_iobject, Alembic::AbcGeom::kWrapExisting);
  m_schema = xform.getSchema();
}

bool AbcTransformReader::valid() const
{
  return m_schema.valid();
}

bool AbcTransformReader::accepts_object_type(
    const Alembic::AbcCoreAbstract::ObjectHeader &alembic_header,
    const Object *const ob,
    const char **r_err_str) const
{
  if (!Alembic::AbcGeom::IXform::matches(alembic_header)) {
    *r_err_str = RPT_(
        "Object type mismatch, Alembic object path pointed to XForm when importing, but not any "
        "more");
    return false;
  }

  if (ob->type != OB_NO_DATA) {
    *r_err_str = RPT_("Object type mismatch, Alembic object path points to XForm");
    return false;
  }

  return true;
}

void AbcTransformReader::readObjectData(Main *bmain, const ISampleSelector & /*sample_sel*/)
{
  m_object = BKE_object_add_only_object(bmain, OB_NO_DATA, m_object_name.c_str());
  m_object->data = nullptr;
}

}  // namespace io::alembic
}  // namespace blender
