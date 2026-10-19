/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_animation_utils.hh"

#include "ANIM_action.hh"
#include "ANIM_fcurve.hh"

#include "BKE_fcurve.hh"

#include "BLI_assert.h"

#include "DNA_action_types.h"
#include "DNA_anim_types.h"

namespace blender::io::usd {

FCurve *create_fcurve(animrig::Channelbag &channelbag,
                      const animrig::FCurveDescriptor &fcurve_descriptor,
                      const int sample_count)
{
  FCurve *fcurve = channelbag.fcurve_create_unique(nullptr, fcurve_descriptor);
  BLI_assert_msg(fcurve, "The same F-Curve is being created twice, this is unexpected.");
  if (fcurve) {
    BKE_fcurve_bezt_resize(*fcurve, sample_count);
  }
  return fcurve;
}

void set_fcurve_sample(FCurve *fcu, const int64_t sample_index, const float frame, const float value)
{
  BLI_assert(sample_index >= 0 && sample_index < fcu->totvert);
  BezTriple &bez = fcu->bezt[sample_index];
  bez.vec[1][0] = frame;
  bez.vec[1][1] = value;
  bez.ipo = BEZT_IPO_LIN;
  bez.f1 = bez.f2 = bez.f3 = BEZT_FLAG_SELECT;
  bez.h1 = bez.h2 = HD_AUTO;
}

}  // namespace blender::io::usd
