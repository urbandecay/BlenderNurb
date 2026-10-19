/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * Define actions data-block for the animation system.
 * A collection of animation curves and drivers to be assigned to data-blocks
 * or sequenced in the non-linear-editor (NLA).
 */

#pragma once

#include <cmath>

#include "DNA_ID.h"
#include "DNA_listBase.h"
#include "DNA_userdef_types.h" /* ThemeWireColor */
#include "DNA_view2d_types.h"

#include "BLI_enum_flags.hh"

#include <type_traits>

namespace blender {

struct AnimData;
struct Collection;
struct FCurve;
struct GHash;
struct Object;
struct SpaceLink;
namespace gpu {
class VertBuf;
class Batch;
}  // namespace gpu

/* Forward declarations so the actual declarations can happen top-down. */
struct ActionLayer;
struct ActionSlot;
struct ActionStrip;
struct ActionChannelbag;

/* Declarations of the C++ wrappers. */
namespace animrig {
class Action;
class Slot;
class SlotRuntime;
class Channelbag;
class ChannelGroup;
class Layer;
class Strip;
class StripKeyframeData;
}  // namespace animrig

/* The last_slot_handle is set to a high value to disambiguate slot handles from
 * array indices.
 *
 * This particular value was obtained by taking the 31 most-significant bits of
 * the TentHash value (Nathan's hash function) for the string "Quercus&Laksa"
 * (Sybren's cats). */
#define DNA_DEFAULT_ACTION_LAST_SLOT_HANDLE 0x37627bf5

/** #bMotionPathVert::flag */
enum eMotionPathVert_Flag : int {
  /* vert is selected */
  MOTIONPATH_VERT_SEL = (1 << 0),
  MOTIONPATH_VERT_KEY = (1 << 1),
};
ENUM_OPERATORS(eMotionPathVert_Flag);

/* bMotionPath->flag */
enum eMotionPath_Flag : int {
  /* motion path is being edited */
  MOTIONPATH_FLAG_EDIT = (1 << 1),
  /* Custom colors */
  MOTIONPATH_FLAG_CUSTOM = (1 << 2),
  /* Draw lines or only points */
  MOTIONPATH_FLAG_LINES = (1 << 3),
  /* Bake to scene camera. */
  MOTIONPATH_FLAG_BAKE_CAMERA = (1 << 4),
};
ENUM_OPERATORS(eMotionPath_Flag);

/* bAnimVizSettings->recalc */
enum eAnimViz_RecalcFlags : short {
  /* Motion-paths need recalculating. */
  ANIMVIZ_RECALC_PATHS = (1 << 0),
};
ENUM_OPERATORS(eAnimViz_RecalcFlags);

/* bAnimVizSettings->path_type */
enum eMotionPaths_Types : short {
  /* show the paths along their entire ranges */
  MOTIONPATH_TYPE_RANGE = 0,
  /* only show the parts of the paths around the current frame */
  MOTIONPATH_TYPE_ACFRA = 1,
};

/* bAnimVizSettings->path_range */
enum eMotionPath_Ranges : short {
  /* Default is scene */
  MOTIONPATH_RANGE_SCENE = 0,
  MOTIONPATH_RANGE_KEYS_SELECTED = 1,
  MOTIONPATH_RANGE_KEYS_ALL = 2,
  MOTIONPATH_RANGE_MANUAL = 3,
};

/* bAnimVizSettings->path_viewflag */
enum eMotionPaths_ViewFlag : short {
  /* show frames on path */
  MOTIONPATH_VIEW_FNUMS = (1 << 0),
  /* show keyframes on path */
  MOTIONPATH_VIEW_KFRAS = (1 << 1),
  /* show keyframe/frame numbers */
  MOTIONPATH_VIEW_KFNOS = (1 << 2),
  /* draw lines on path */
  /* MOTIONPATH_VIEW_LINES = (1 << 4), */ /* UNUSED */
};
ENUM_OPERATORS(eMotionPaths_ViewFlag);

/* bAnimVizSettings->path_bakeflag */
enum eMotionPath_BakeFlag : short {
  /** motion paths directly associated with this block of settings needs updating */
  /* MOTIONPATH_BAKE_NEEDS_RECALC = (1 << 0), */ /* UNUSED */
  /** motion paths exist for AnimVizSettings instance - set when calc for first time,
   * and unset when clearing */
  MOTIONPATH_BAKE_HAS_PATHS = (1 << 2),
  /* Bake the path in camera space. */
  MOTIONPATH_BAKE_CAMERA_SPACE = (1 << 3),
};
ENUM_OPERATORS(eMotionPath_BakeFlag);

/* Object rotation modes. */
enum eRotationModes : short {
  /* quaternion rotations (default, and for older Blender versions) */
  ROT_MODE_QUAT = 0,
  /* euler rotations - keep in sync with enum in BLI_math_rotation.h */
  /** Blender 'default' (classic) - must be as 1 to sync with BLI_math_rotation.h defines */
  ROT_MODE_EUL = 1,
  ROT_MODE_XYZ = 1,
  ROT_MODE_XZY = 2,
  ROT_MODE_YXZ = 3,
  ROT_MODE_YZX = 4,
  ROT_MODE_ZXY = 5,
  ROT_MODE_ZYX = 6,
  /* NOTE: space is reserved here for 18 other possible
   * euler rotation orders not implemented
   */
  /* axis angle rotations */
  ROT_MODE_AXISANGLE = -1,

  ROT_MODE_MIN = ROT_MODE_AXISANGLE, /* sentinel for Py API */
  ROT_MODE_MAX = ROT_MODE_ZYX,
};

/* Action Group flags */
enum eActionGroup_Flag : uint32_t {
  /* group is selected */
  AGRP_SELECTED = (1 << 0),
  /* group is 'active' / last selected one */
  AGRP_ACTIVE = (1 << 1),
  /* keyframes/channels belonging to it cannot be edited */
  AGRP_PROTECTED = (1 << 2),
  /* for UI (DopeSheet), sub-channels are shown */
  AGRP_EXPANDED = (1 << 3),
  /* sub-channels are not evaluated */
  AGRP_MUTED = (1 << 4),
  /* sub-channels are not visible in Graph Editor */
  AGRP_NOTVISIBLE = (1 << 5),
  /* for UI (Graph Editor), sub-channels are shown */
  AGRP_EXPANDED_G = (1 << 6),

  /* sub channel modifiers off */
  AGRP_MODIFIERS_OFF = (1 << 7),

  /** F-Curves from this Action Group are always visible. */
  AGRP_CURVES_ALWAYS_VISIBLE = (1 << 17),

  AGRP_TEMP = (1 << 30),
  AGRP_MOVED = (1u << 31),
};
ENUM_OPERATORS(eActionGroup_Flag)

/** Flags for the action. */
enum eAction_Flags : int {
  /* flags for displaying in UI */
  ACT_COLLAPSED = (1 << 0),
  ACT_SELECTED = (1 << 1),

  /* flags for evaluation/editing */
  ACT_MUTED = (1 << 9),
  /* ACT_PROTECTED = (1 << 10), */ /* UNUSED */
  /* ACT_DISABLED = (1 << 11), */  /* UNUSED */
  /** The action has a manually set intended playback frame range. */
  ACT_FRAME_RANGE = (1 << 12),
  /** The action is intended to be a cycle (requires ACT_FRAME_RANGE). */
  ACT_CYCLIC = (1 << 13),
};
ENUM_OPERATORS(eAction_Flags);

/** DopeSheet filter-flag. */
enum eDopeSheet_FilterFlag : int {
  /* general filtering */
  /** only include channels relating to selected data */
  ADS_FILTER_ONLYSEL = (1 << 0),

  /* temporary filters */
  /** for 'Drivers' editor - only include Driver data from AnimData */
  ADS_FILTER_ONLYDRIVERS = (1 << 1),
  /** for 'NLA' editor - only include NLA data from AnimData */
  ADS_FILTER_ONLYNLA = (1 << 2),
  /** for Graph Editor - used to indicate whether to include a filtering flag or not */
  ADS_FILTER_SELEDIT = (1 << 3),

  /* general filtering */
  /** for 'DopeSheet' Editors - include 'summary' line */
  ADS_FILTER_SUMMARY = (1 << 4),

  /**
   * Show all Action slots; if not set, only show the Slot of the
   * data-block that's being animated by the Action.
   */
  ADS_FILTER_ONLY_SLOTS_OF_ACTIVE = (1 << 5),

  /* datatype-based filtering */
  ADS_FILTER_NOSHAPEKEYS = (1 << 6),
  ADS_FILTER_NOMESH = (1 << 7),
  /** For animation-data on object level, if we only want to concentrate on materials/etc. */
  ADS_FILTER_NOOBJ = (1 << 8),
  ADS_FILTER_NOLAT = (1 << 9),
  ADS_FILTER_NOCAM = (1 << 10),
  ADS_FILTER_NOMAT = (1 << 11),
  ADS_FILTER_NOLAM = (1 << 12),
  ADS_FILTER_NOCUR = (1 << 13),
  ADS_FILTER_NOWOR = (1 << 14),
  ADS_FILTER_NOSCE = (1 << 15),
  ADS_FILTER_NOPART = (1 << 16),
  ADS_FILTER_NOMBA = (1 << 17),
  ADS_FILTER_NOARM = (1 << 18),
  ADS_FILTER_NONTREE = (1 << 19),
  ADS_FILTER_NOTEX = (1 << 20),
  ADS_FILTER_NOSPK = (1 << 21),
  ADS_FILTER_NOLINESTYLE = (1 << 22),
  ADS_FILTER_NOMODIFIERS = (1 << 23),
  /* NOTE: all new datablock filters will have to go in filterflag2 (see below) */

  /* NLA-specific filters */
  /** if the AnimData block has no NLA data, don't include to just show Action-line */
  ADS_FILTER_NLA_NOACT = (1 << 25),

  /* general filtering 3 */
  /** include 'hidden' channels too (i.e. those from hidden Objects/Bones) */
  ADS_FILTER_INCL_HIDDEN = (1 << 26),
  /** show only F-Curves which are disabled/have errors - for debugging drivers */
  ADS_FILTER_ONLY_ERRORS = (1 << 28),

#if 0
  /** combination filters (some only used at runtime) */
  ADS_FILTER_NOOBDATA = (ADS_FILTER_NOCAM | ADS_FILTER_NOMAT | ADS_FILTER_NOLAM |
                         ADS_FILTER_NOCUR | ADS_FILTER_NOPART | ADS_FILTER_NOARM |
                         ADS_FILTER_NOSPK | ADS_FILTER_NOMODIFIERS),
#endif
};
ENUM_OPERATORS(eDopeSheet_FilterFlag);

/* DopeSheet filter-flags - Overflow (filterflag2) */
enum eDopeSheet_FilterFlag2 : int {
  ADS_FILTER_NOCACHEFILES = (1 << 1),
  ADS_FILTER_NOMOVIECLIPS = (1 << 2),
  ADS_FILTER_NOHAIR = (1 << 3),
  ADS_FILTER_NOPOINTCLOUD = (1 << 4),
  ADS_FILTER_NOVOLUME = (1 << 5),

  /** Include working drivers with variables using their fallback values into Only Show Errors. */
  ADS_FILTER_DRIVER_FALLBACK_AS_ERROR = (1 << 6),

  ADS_FILTER_NOLIGHTPROBE = (1 << 7),
};
ENUM_OPERATORS(eDopeSheet_FilterFlag2);

/* DopeSheet general flags */
enum eDopeSheet_Flag : int {
  /** when summary is shown, it is collapsed, so all other channels get hidden */
  ADS_FLAG_SUMMARY_COLLAPSED = (1 << 0),
  /** show filters for datablocks */
  ADS_FLAG_SHOW_DBFILTERS = (1 << 1),

  /** use fuzzy/partial string matches when ADS_FILTER_BY_FCU_NAME is enabled
   * (WARNING: expensive operation) */
  ADS_FLAG_FUZZY_NAMES = (1 << 2),
  /** do not sort datablocks (mostly objects) by name (NOTE: potentially expensive operation) */
  ADS_FLAG_NO_DB_SORT = (1 << 3),
  /** Invert the search filter */
  ADS_FLAG_INVERT_FILTER = (1 << 4),
};
ENUM_OPERATORS(eDopeSheet_Flag);

enum SpaceActionOverlays_Flag : int {
  ADS_OVERLAY_SHOW_OVERLAYS = (1 << 0),
  ADS_SHOW_SCENE_STRIP_FRAME_RANGE = (1 << 1)
};
ENUM_OPERATORS(SpaceActionOverlays_Flag);

/* SpaceAction flag */
enum eSAction_Flag : short {
  /* during transform (only set for TimeSlide) */
  SACTION_MOVING = (1 << 0),
  /* show sliders */
  SACTION_SLIDERS = (1 << 1),
  /* draw time in seconds instead of time in frames */
  SACTION_DRAWTIME = (1 << 2),
  /* don't filter action channels according to visibility */
  // SACTION_NOHIDE = (1 << 3), /* Deprecated, old animation systems. */
  /* don't kill overlapping keyframes after transform */
  SACTION_NOTRANSKEYCULL = (1 << 4),
  /* don't include keyframes that are out of view */
  // SACTION_HORIZOPTIMISEON = (1 << 5), /* Deprecated, old irrelevant trick. */
  /* show markers local to the active action in Action Editor mode. */
  SACTION_POSEMARKERS_SHOW = (1 << 6),
  /* don't draw action channels using group colors (where applicable) */
  /* SACTION_NODRAWGCOLORS = (1 << 7), DEPRECATED */
  /* SACTION_NODRAWCFRANUM = (1 << 8), DEPRECATED */
  /* don't perform realtime updates */
  SACTION_NOREALTIMEUPDATES = (1 << 10),
  /* move markers as well as keyframes */
  SACTION_MARKERS_MOVE = (1 << 11),
  /* show interpolation type */
  SACTION_SHOW_INTERPOLATION = (1 << 12),
  /* show extremes */
  SACTION_SHOW_EXTREMES = (1 << 13),
  /* show markers region */
  SACTION_SHOW_MARKERS = (1 << 14),
};
ENUM_OPERATORS(eSAction_Flag);

/** #SpaceAction_Runtime.flag */
enum eSAction_Runtime_Flag : char {
  /** Temporary flag to force channel selections to be synced with main */
  SACTION_RUNTIME_FLAG_NEED_CHAN_SYNC = (1 << 0),
};
ENUM_OPERATORS(eSAction_Runtime_Flag);

/** #SpaceAction.mode */
enum eAnimEdit_Context : char {
  /** Action on the active object. */
  SACTCONT_ACTION = 0,
  /** List of all shape-keys on the active object, linked with their F-Curves. */
  SACTCONT_SHAPEKEY = 1,
  /** Dope-sheet (default). */
  SACTCONT_DOPESHEET = 3,
  /** Mask. */
  SACTCONT_MASK = 4,
  /** Cache file */
  SACTCONT_CACHEFILE = 5,
  /** Timeline. */
  SACTCONT_TIMELINE = 6,
};

/* Old snapping enum that is only needed because of the versioning code. */
enum DNA_DEPRECATED eAnimEdit_AutoSnap : int {
  /* snap to 1.0 frame/second intervals */
  SACTSNAP_STEP = 1,
  /* snap to actual frames/seconds (nla-action time) */
  SACTSNAP_FRAME = 2,
  /* snap to nearest marker */
  SACTSNAP_MARKER = 3,
  /* snap to actual seconds (nla-action time) */
  SACTSNAP_SECOND = 4,
  /* snap to 1.0 second increments */
  SACTSNAP_TSTEP = 5,
};

/* SAction->cache_display */
enum eTimeline_Cache_Flag : char {
  TIME_CACHE_DISPLAY = (1 << 0),
  TIME_CACHE_SOFTBODY = (1 << 1),
  TIME_CACHE_PARTICLES = (1 << 2),
  TIME_CACHE_CLOTH = (1 << 3),
  TIME_CACHE_SMOKE = (1 << 4),
  TIME_CACHE_DYNAMICPAINT = (1 << 5),
  TIME_CACHE_RIGIDBODY = (1 << 6),
  TIME_CACHE_SIMULATION_NODES = static_cast<char>(1 << 7),
};
ENUM_OPERATORS(eTimeline_Cache_Flag)

/* ************************************************ */
/* Visualization */

/* Motion Paths ------------------------------------ */
/* (used for objects) */

/** Data point for motion path (`mpv`). */
struct bMotionPathVert {
  /** Coordinates of point in world space or NDC space. */
  float co[3] = {};
  /** Quick settings. */
  eMotionPathVert_Flag flag = {};
};

/* ........ */

/* Motion Path data cache (mpath)
 * - for elements providing transforms (i.e. Objects or PoseChannels)
 */
struct bMotionPath {
  /** Path samples. */
  bMotionPathVert *points = nullptr;
  /** The number of cached verts. */
  int length = 0;

  /** For drawing paths, the start frame number. Inclusive. */
  int start_frame = 0;
  /** For drawing paths, the end frame number. Exclusive. */
  int end_frame = 0;

  /** Optional custom color. */
  float color[3] = {};
  float color_post[3] = {};
  /** Line thickness. */
  int line_thickness = 0;
  /** Baking settings. */
  eMotionPath_Flag flag = {};

  char _pad2[4] = {};
  /* Used for drawing. */
  gpu::VertBuf *points_vbo = nullptr;
  gpu::Batch *batch_line = nullptr;
  gpu::Batch *batch_points = nullptr;
  void *_pad = nullptr;
};

/* Visualization General --------------------------- */
/* for Objects */

/* Animation Visualization Settings (avs) */
struct bAnimVizSettings {
  /* General Settings ------------------------ */
  eAnimViz_RecalcFlags recalc = {};

  /* Motion Path Settings ------------------- */
  eMotionPaths_Types path_type = {};
  /** Number of frames between points indicated on the paths. */
  short path_step = 0;
  eMotionPath_Ranges path_range = {};

  eMotionPaths_ViewFlag path_viewflag = {};
  eMotionPath_BakeFlag path_bakeflag = {};
  char _pad[4] = {};

  /** Start and end frames of path-calculation range. Both are inclusive. */
  int path_sf = 0, path_ef = 0;
  /** Number of frames before/after current frame to show. Both are inclusive. */
  int path_bc = 0, path_ac = 0;
};

/* ************************************************ */
/* Action */

/* Groups -------------------------------------- */

/**
 * Action-Channel Group (agrp)
 *
 * These are stored as a list per-Action, and are only used to
 * group that Action's channels in an Animation Editor.
 *
 * Even though all FCurves live in a big list per Action, each group they are in also
 * holds references to the achans within that list which belong to it. Care must be taken to
 * ensure that action-groups never end up being the sole 'owner' of a channel.
 *
 * Groups organize channels in animation editors and can have custom colors.
 */
struct bActionGroup {
  struct bActionGroup *next = nullptr, *prev = nullptr;

  /**
   * List of channels in this group for legacy actions.
   *
   * NOTE: this must not be touched by standard listbase functions
   * which would clear links to other channels.
   */
  ListBaseT<FCurve> channels = {nullptr, nullptr};

  /**
   * Span of channels in this group for layered actions.
   *
   * This specifies that span as a range of items in a Channelbag's fcurve
   * array.
   *
   * Note that empty groups (`fcurve_range_length == 0`) are allowed, and they
   * still have a position in the fcurves array, as specified by
   * `fcurve_range_start`. You can imagine these cases as a zero-width range
   * that sits at the border between the element at `fcurve_range_start` and the
   * element just before it.
   */
  int fcurve_range_start = 0;
  int fcurve_range_length = 0;

  /**
   * For layered actions: the Channelbag this group belongs to.
   *
   * This is needed in the keyframe drawing code, etc., to give direct access to
   * the fcurves in this group.
   */
  struct ActionChannelbag *channelbag = nullptr;

  /** Settings for this action-group. */
  eActionGroup_Flag flag = {};
  /**
   * Index of custom color set to use for the action group
   * (0=default, -1=custom set).
   */
  int customCol = 0;
  /** Name of the group. */
  char name[64] = "";

  /** Color set to use when customCol == -1. */
  ThemeWireColor cs = {};

#ifdef __cplusplus
  animrig::ChannelGroup &wrap();
  const animrig::ChannelGroup &wrap() const;
#endif
};

/* Actions -------------------------------------- */

/**
 * Container of animation data.
 *
 * \see animrig::Action for more detailed documentation.
 */
struct bAction {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_AC;
#endif

  /** ID-serialization for relinking. */
  ID id;

  /** Array of `layer_array_num` layers. */
  struct ActionLayer **layer_array = nullptr;
  int layer_array_num = 0;
  int layer_active_index = 0; /* Index into layer_array, -1 means 'no active'. */

  /** Array of `slot_array_num` slots. */
  struct ActionSlot **slot_array = nullptr;
  int slot_array_num = 0;
  int32_t last_slot_handle = DNA_DEFAULT_ACTION_LAST_SLOT_HANDLE;

  /* Storage for the underlying data of strips. Each strip type has its own
   * array, and strips reference this data with an enum indicating the strip
   * type and an int containing the index in the array to use.
   *
   * NOTE: when adding new strip data arrays, also update `duplicate_slot()`. */
  struct ActionStripKeyframeData **strip_keyframe_data_array = nullptr;
  int strip_keyframe_data_array_num = 0;

  char _pad0[4] = {};

  /* Note about legacy animation data:
   *
   * Blender 2.5 introduced a new animation system 'Animato'. This replaced the
   * IPO ('interpolation') curves with F-Curves. Both are considered 'legacy' at
   * different levels:
   *
   * - Actions with F-Curves in `curves`, as introduced in Blender 2.5, are
   *   considered 'legacy' but still functional in current Blender.
   * - Pre-2.5 data are deprecated and old files are automatically converted to
   *   the post-2.5 data model.
   */

  /** Legacy F-Curves (FCurve), introduced in Blender 2.5. */
  ListBaseT<FCurve> curves = {nullptr, nullptr};
  /** Legacy Groups of function-curves (bActionGroup), introduced in Blender 2.5. */
  ListBaseT<bActionGroup> groups = {nullptr, nullptr};

  /** Markers local to the Action. */
  ListBaseT<struct TimeMarker> markers = {nullptr, nullptr};

  /** Settings for this action. */
  eAction_Flags flag = {};
  /** Index of the active marker. */
  int active_marker = 0;

  /**
   * Type of ID-blocks that action can be assigned to
   * (if 0, will be set to whatever ID first evaluates it).
   */
  int idroot = 0;
  char _pad1[4] = {};

  /**
   * Start and end of the manually set intended playback frame range. Used by UI and
   * some editing tools, but doesn't directly affect animation evaluation in any way.
   */
  float frame_start = 0, frame_end = 0;

  PreviewImage *preview = nullptr;

#ifdef __cplusplus
  animrig::Action &wrap();
  const animrig::Action &wrap() const;
#endif
};

/* ************************************************ */
/* Action/Dope-sheet Editor */

/** Storage for Dope-sheet Editor data. */
struct bDopeSheet {
  /** Currently ID_SCE (for Dope-sheet). */
  ID *source = nullptr;
  /** Cache for channels (only initialized when pinned). */ /* XXX not used! */
  ListBase chanbase = {nullptr, nullptr};

  /** Object group for option to only include objects that belong to this Collection. */
  struct Collection *filter_grp = nullptr;
  /** String to search for in displayed names of F-Curves or NLA tracks. */
  char searchstr[64] = "";

  /** Flags to use for filtering data. */
  eDopeSheet_FilterFlag filterflag = {};
  eDopeSheet_FilterFlag2 filterflag2 = {};
  /** Standard flags. */
  eDopeSheet_Flag flag = {};

  /** `index + 1` of channel to rename - only gets set by renaming operator. */
  int renameIndex = 0;
};

struct SpaceAction_Runtime {
  eSAction_Runtime_Flag flag = {};
  char _pad0[7] = {};
};

struct SpaceActionOverlays {
  SpaceActionOverlays_Flag flag = {};
  char _pad0[4] = {};
};

/* Action Editor Space. This is defined here instead of in DNA_space_types.h */
struct SpaceAction {
  DNA_DEFINE_CXX_METHODS(SpaceAction)

  struct SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBaseT<struct ARegion> regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  /** Copied to region. */
  DNA_DEPRECATED View2D v2d;

  /** The currently active action (deprecated). */
  DNA_DEPRECATED bAction *action = nullptr;

  /** The currently active context (when not showing action). */
  bDopeSheet ads;

  /** For Time-Slide transform mode drawing - current frame? */
  float timeslide = 0;

  eSAction_Flag flag = {};
  /* Editing context */
  eAnimEdit_Context mode = {};
  /* Storage for sub-space types. */
  char mode_prev = 0;
  /* Snapping now lives on the Scene. */
  DNA_DEPRECATED char autosnap = 0;
  eTimeline_Cache_Flag cache_display = {};
  char _pad1[6] = {};

  SpaceActionOverlays overlays;

  SpaceAction_Runtime runtime;
};

/* ************************************************ */
/* Layered Animation data-types. */

/**
 * \see #animrig::Layer
 */
struct ActionLayer {
  /** User-Visible identifier, unique within the Animation. */
  char name[/*MAX_NAME*/ 64] = "";

  float influence = 1.0f; /* [0-1] */

  /** \see #animrig::Layer::flags() */
  uint8_t layer_flags = 0;

  /** \see #animrig::Layer::mixmode() */
  int8_t layer_mix_mode = 0;

  uint8_t _pad0[2] = {};

  /**
   * The layer's array of strips. See the documentation of
   * #animrig::Layer for the invariants of this array.
   */
  struct ActionStrip **strip_array = nullptr; /* Array of 'strip_array_num' strips. */
  int strip_array_num = 0;

  uint8_t _pad1[4] = {};

#ifdef __cplusplus
  animrig::Layer &wrap();
  const animrig::Layer &wrap() const;
#endif
};

/**
 * \see #animrig::Slot
 */
struct ActionSlot {
  /**
   * The string identifier of this Slot within the Action.
   *
   * The first two characters are the two-letter code corresponding to `idtype`
   * below (e.g. 'OB', 'ME', 'LA'), and the remaining characters store slot's
   * display name. Since the combination of the `idtype` and display name are
   * always unique within an action, this string identifier is as well.
   *
   * Typically this matches the ID name this slot was created for, including the
   * two letters indicating the ID type.
   *
   * \see #AnimData::slot_name
   */
  char identifier[/*MAX_ID_NAME*/ 258] = "";

  /**
   * Type of ID-block that this slot is intended for.
   *
   * If 0, will be set to whatever ID is first assigned.
   */
  int16_t idtype = 0;

  /**
   * Numeric identifier of this Slot within the Action.
   *
   * This number allows reorganization of the #bAction::slot_array without
   * invalidating references. Also these remain valid when copy-on-evaluate
   * copies are made.
   *
   * Unlike `identifier` above, this cannot be set by the user and never changes
   * after initial assignment, and thus serves as a "forever" identifier of the
   * slot.
   *
   * Only valid within the Action that owns this Slot.
   *
   * NOTE: keep this type in sync with `slot_handle_t` in BKE_action.hh.
   *
   * \see #animrig::Action::slot_for_handle()
   */
  int32_t handle = 0;

  /** \see #animrig::Slot::flags() */
  int8_t slot_flags = 0;
  uint8_t _pad1[7] = {};

  /** Runtime data. Set to nullptr when writing to disk. */
  animrig::SlotRuntime *runtime = nullptr;

#ifdef __cplusplus
  animrig::Slot &wrap();
  const animrig::Slot &wrap() const;
#endif
};

/**
 * \see #animrig::Strip
 */
struct ActionStrip {
  /**
   * \see #animrig::Strip::type()
   */
  int8_t strip_type = 0;
  uint8_t _pad0[3] = {};

  /**
   * The index of the "strip data" item that this strip uses, in the array of
   * strip data that corresponds to `strip_type`.
   *
   * Note that -1 indicates "no data".  This is an invalid state outside of
   * specific internal APIs, but it's the default value and therefore helps us
   * catch when strips aren't fully initialized before making their way outside
   * of those APIs.
   */
  int data_index = -1;

  float frame_start = -INFINITY; /** Start frame of the strip, in Animation time. */
  float frame_end = INFINITY;    /** End frame of the strip, in Animation time. */

  /**
   * Offset applied to the contents of the strip, in frames.
   *
   * This offset determines the difference between "Animation time" (which would
   * typically be the same as the scene time, until the animation system
   * supports strips referencing other Actions).
   */
  float frame_offset = 0;

  uint8_t _pad1[4] = {};

#ifdef __cplusplus
  animrig::Strip &wrap();
  const animrig::Strip &wrap() const;
#endif
};

/**
 * #ActionStrip::type = #Strip::Type::Keyframe.
 *
 * \see #animrig::StripKeyframeData
 */
struct ActionStripKeyframeData {
  struct ActionChannelbag **channelbag_array = nullptr;
  int channelbag_array_num = 0;

  uint8_t _pad[4] = {};

#ifdef __cplusplus
  animrig::StripKeyframeData &wrap();
  const animrig::StripKeyframeData &wrap() const;
#endif
};

/**
 * \see #animrig::Channelbag
 */
struct ActionChannelbag {
  int32_t slot_handle = 0;

  /* Channel groups. These index into the `fcurve_array` below to specify group
   * membership of the fcurves.
   *
   * Note that although the fcurves also have pointers back to the groups they
   * belong to, those pointers are not the source of truth. The source of truth
   * for membership is the information in the channel groups here.
   *
   * Invariants:
   * 1. The groups are sorted by their `fcurve_range_start` field. In other
   *    words, they are in the same order as their starting positions in the
   *    fcurve array.
   * 2. The grouped fcurves are tightly packed, starting at the first fcurve and
   *    having no gaps of ungrouped fcurves between them. Ungrouped fcurves come
   *    at the end, after all of the grouped fcurves. */
  int group_array_num = 0;
  struct bActionGroup **group_array = nullptr;

  uint8_t _pad[4] = {};

  int fcurve_array_num = 0;
  struct FCurve **fcurve_array = nullptr; /* Array of 'fcurve_array_num' FCurves. */

  /* TODO: Design & implement a way to integrate other channel types as well,
   * and still have them map to a certain slot */
#ifdef __cplusplus
  animrig::Channelbag &wrap();
  const animrig::Channelbag &wrap() const;
#endif
};

#ifdef __cplusplus
/* Some static assertions that things that should have the same type actually do. */
static_assert(std::is_same_v<decltype(ActionSlot::handle), decltype(bAction::last_slot_handle)>);
static_assert(
    std::is_same_v<decltype(ActionSlot::handle), decltype(ActionChannelbag::slot_handle)>);
#endif

}  // namespace blender
