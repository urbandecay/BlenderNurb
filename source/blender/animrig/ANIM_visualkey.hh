/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 *
 * \brief Functions to work with the visual keying system.
 */

#include "BLI_vector.hh"

namespace blender {

struct PointerRNA;
struct PropertyRNA;

namespace animrig {

/**
 * This helper function determines if visual-keyframing should be used when
 * inserting keyframes for an Object transform, when using "standard" keying
 * with the 'Visual Keying' option in Auto-Keying settings enabled.
 */
bool visualkey_can_use(PointerRNA *ptr, PropertyRNA *prop);

/**
 * This helper function extracts the value to use for visual-keyframing
 * In the event that it is not possible to perform visual keying, try to fall-back
 * to using the default method. Assumes that all data it has been passed is valid.
 */
Vector<float> visualkey_get_values(PointerRNA *ptr, PropertyRNA *prop);

}  // namespace animrig
}  // namespace blender
