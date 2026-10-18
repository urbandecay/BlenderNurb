/* SPDX-FileCopyrightText: 2016 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spclip
 */

#include "DNA_space_types.h"

#include "BLI_listbase.h"

#include "BKE_context.hh"
#include "BKE_movieclip.hh"
#include "BKE_report.hh"
#include "BKE_tracking.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_clip.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "clip_intern.hh"
#include "tracking_ops_intern.hh"

namespace blender {

/********************** detect features operator *********************/

static wmOperatorStatus detect_features_exec(bContext *C, wmOperator *op)
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieClipFlag clip_flag = MovieClipFlag(clip->flag & MCLIP_PROXY_FLAGS);
  ImBuf *ibuf = BKE_movieclip_get_ibuf_flag(
      clip, &sc->user, clip_flag, MovieClipCacheFlag::SkipCache);
  MovieTracking *tracking = &clip->tracking;
  MovieTrackingObject *tracking_object = BKE_tracking_object_get_active(tracking);
  const int margin = RNA_int_get(op->ptr, "margin");
  const int min_distance = RNA_int_get(op->ptr, "min_distance");
  const float threshold = RNA_float_get(op->ptr, "threshold");
  const int framenr = ED_space_clip_get_clip_frame_number(sc);
  if (!ibuf) {
    BKE_report(op->reports, RPT_ERROR, "Feature detection requires valid clip frame");
    return OPERATOR_CANCELLED;
  }

  /* Deselect existing tracks. */
  ed_tracking_deselect_all_tracks(&tracking_object->tracks);

  /* Run detector. */
  BKE_tracking_detect_harris(tracking,
                             &tracking_object->tracks,
                             ibuf,
                             framenr,
                             margin,
                             threshold / 100000.0f,
                             min_distance);

  IMB_freeImBuf(ibuf);

  BKE_tracking_dopesheet_tag_update(tracking);
  WM_event_add_notifier(C, NC_MOVIECLIP | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

void CLIP_OT_detect_features(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Detect Features";
  ot->description = "Automatically detect features and place markers to track";
  ot->idname = "CLIP_OT_detect_features";

  /* API callbacks. */
  ot->exec = detect_features_exec;
  ot->poll = ED_space_clip_tracking_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_int(ot->srna,
              "margin",
              16,
              0,
              INT_MAX,
              "Margin",
              "Only features further than margin pixels from the image "
              "edges are considered",
              0,
              300);
  RNA_def_float(ot->srna,
                "threshold",
                0.5f,
                0.0001f,
                FLT_MAX,
                "Threshold",
                "Threshold level to consider feature good enough for tracking",
                0.0001f,
                FLT_MAX);
  RNA_def_int(ot->srna,
              "min_distance",
              120,
              0,
              INT_MAX,
              "Distance",
              "Minimal distance accepted between two features",
              0,
              300);
}

}  // namespace blender
