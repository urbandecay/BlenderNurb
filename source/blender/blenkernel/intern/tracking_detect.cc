/* SPDX-FileCopyrightText: 2011 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * This file contains blender-side implementation of feature detection.
 */

#include "DNA_movieclip_types.h"
#include "DNA_object_types.h" /* SELECT */

#include "BKE_tracking.hh"

#include "IMB_imbuf_types.hh"

#include "libmv-capi.h"

namespace blender {

/* Get features detected by libmv and create tracks on the clip for them. */
static void detect_retrieve_libmv_features(MovieTracking *tracking,
                                           ListBaseT<MovieTrackingTrack> *tracksbase,
                                           libmv_Features *features,
                                           int framenr,
                                           int width,
                                           int height)
{
  int a;

  a = libmv_countFeatures(features);
  while (a--) {
    double x, y, size, score;
    float xu, yu;

    libmv_getFeature(features, a, &x, &y, &score, &size);

    /* In Libmv integer coordinate points to pixel center, in blender
     * it's not. Need to add 0.5px offset to center.
     */
    xu = (x + 0.5) / width;
    yu = (y + 0.5) / height;

    MovieTrackingTrack *track = BKE_tracking_track_add(
        tracking, tracksbase, xu, yu, framenr, width, height);
    track->flag |= TRACK_SELECT;
    track->pat_flag |= TRACK_SELECT;
    track->search_flag |= TRACK_SELECT;
  }
}

static void run_configured_detector(MovieTracking *tracking,
                                    ListBaseT<MovieTrackingTrack> *tracksbase,
                                    ImBuf *ibuf,
                                    int framenr,
                                    libmv_DetectOptions *options)
{
  libmv_Features *features = nullptr;

  if (ibuf->float_data()) {
    features = libmv_detectFeaturesFloat(ibuf->float_data(), ibuf->x, ibuf->y, 4, options);
  }
  else if (ibuf->byte_data()) {
    features = libmv_detectFeaturesByte(ibuf->byte_data(), ibuf->x, ibuf->y, 4, options);
  }

  if (features != nullptr) {
    detect_retrieve_libmv_features(tracking, tracksbase, features, framenr, ibuf->x, ibuf->y);

    libmv_featuresDestroy(features);
  }
}

void BKE_tracking_detect_fast(MovieTracking *tracking,
                              ListBaseT<MovieTrackingTrack> *tracksbase,
                              ImBuf *ibuf,
                              int framenr,
                              int margin,
                              int min_trackness,
                              int min_distance)
{
  libmv_DetectOptions options = {0};

  options.detector = LIBMV_DETECTOR_FAST;
  options.margin = margin;
  options.min_distance = min_distance;
  options.fast_min_trackness = min_trackness;

  run_configured_detector(tracking, tracksbase, ibuf, framenr, &options);
}

void BKE_tracking_detect_harris(MovieTracking *tracking,
                                ListBaseT<MovieTrackingTrack> *tracksbase,
                                ImBuf *ibuf,
                                int framenr,
                                int margin,
                                float threshold,
                                int min_distance)
{
  libmv_DetectOptions options = {0};

  options.detector = LIBMV_DETECTOR_HARRIS;
  options.margin = margin;
  options.min_distance = min_distance;
  options.harris_threshold = threshold;

  run_configured_detector(tracking, tracksbase, ibuf, framenr, &options);
}

}  // namespace blender
