/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

namespace blender {

struct FCurve;

namespace animrig {
class Channelbag;
struct FCurveDescriptor;
}  // namespace animrig

namespace io::usd {

FCurve *create_fcurve(animrig::Channelbag &channelbag,
                      const animrig::FCurveDescriptor &fcurve_descriptor,
                      int sample_count);

void set_fcurve_sample(FCurve *fcu, int64_t sample_index, float frame, float value);

}  // namespace io::usd
}  // namespace blender
