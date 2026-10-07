#include "SOP_VGSCapture.h"

#include "VGS_Parameters.h"

#include <CH/CH_Manager.h>
#include <GA/GA_Handle.h>
#include <GA/GA_Types.h>
#include <GU/GU_Detail.h>
#include <GU/GU_DetailHandle.h>
#include <GU/GU_PackedGeometry.h>
#include <GU/GU_PrimPacked.h>
#include <OP/OP_AutoLockInputs.h>
#include <OP/OP_Director.h>
#include <OP/OP_NodeInfoParms.h>
#include <OP/OP_Operator.h>
#include <PXL/PXL_OCIO.h>
#include <ROP/ROP_Node.h>
#include <ROP/ROP_RenderManager.h>
#include <SYS/SYS_Math.h>
#include <UT/UT_Matrix3.h>
#include <UT/UT_Matrix4.h>
#include <UT/UT_ParallelUtil.h>
#include <UT/UT_Playback.h>
#include <UT/UT_Quaternion.h>
#include <UT/UT_UI.h>
#include <UT/UT_VarEncode.h>
#include <UT/UT_Vector3.h>
#include <UT/UT_Vector4.h>
#include <UT/UT_WorkBuffer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace vgs {

namespace {

/** The constant that turns a spherical harmonic DC term into a colour, and back. */
constexpr float C0 = 0.28209479177387814f;

/**
 * How far ahead of the playhead chunks are decompressed, in seconds. A chunk covers about
 * a second and takes a fraction of one to decode, so this is enough to have the next one
 * ready before playback arrives.
 */
constexpr double LookaheadSeconds = 1.5;

/**
 * Waits that are not playback: scrubbing and rendering want the exact frame. A bound
 * rather than forever, so a fault in the decoder cannot hang Houdini.
 */
constexpr double ExactWaitMs = 60000.0;

// ---- playback --------------------------------------------------------------------------

/**
 * Every capture SOP alive, so that stopping the playbar can recook the ones that dropped
 * their harmonics while it played: the frame it stops on is the one that should get them
 * back, and nothing else would cook it again.
 */
std::mutex nodesMutex;
std::set<SOP_VGSCapture *> nodes;
bool playbackHooked = false;

void recookAll() {
  std::vector<SOP_VGSCapture *> all;
  {
    std::lock_guard<std::mutex> lock(nodesMutex);
    all.assign(nodes.begin(), nodes.end());
  }
  for (SOP_VGSCapture *node : all)
    node->forceRecook();
}

void onPlayback(void *, int mode, fpreal, fpreal) {
  if (mode == UT_Playback::STOPPED)
    recookAll();
}

/**
 * ROP renders under way, counted from their pre-render to their post-render events.
 *
 * Houdini does not cook a SOP again for a render when nothing about it changed, so a
 * render at the frame the viewport shows would take the viewport's thinned splats as they
 * stand. Every capture is recooked as a render starts, which gets it every splat and
 * every harmonic, and again once it is over, which gives the viewport its own settings
 * back - not after every frame of an animation, as the render moves the frame anyway.
 */
std::atomic<int> rendersRunning{0};

bool onRenderEvent(ROP_Node *, ROP_RenderEventType event, fpreal, void *) {
  if (event == ROP_EVENT_PRE_RENDER) {
    if (rendersRunning++ == 0)
      recookAll();
  } else if (event == ROP_EVENT_POST_RENDER) {
    if (rendersRunning > 0 && --rendersRunning == 0)
      recookAll();
  }
  return true;
}

/** Hooks every ROP as it is created or loaded, wherever it is. */
void onNodeEvent(OP_Node *, OP_EventType reason, void *data, void *) {
  if (reason != OP_CHILD_CREATED || !data)
    return;
  if (ROP_Node *rop = static_cast<OP_Node *>(data)->castToROPNode())
    rop->addRenderEventCallback(onRenderEvent, nullptr, false);
}

bool playing() {
  const UT_Playback *playback = UT_Playback::getPlayback();
  return playback && playback->isPlaying();
}

bool playingBackwards() {
  const UT_Playback *playback = UT_Playback::getPlayback();
  return playback && playback->getPlaybackMode() == UT_Playback::REVERSE;
}

/**
 * Whether this cook is for a render, which draws every splat and every harmonic whatever
 * the viewport is set to: a ROP render under way, an object cooked for a render, or a
 * session with no interface at all - hbatch, hython, a farm.
 */
bool rendering(const SOP_Node &sop) {
  if (rendersRunning > 0 || sop.isCookingRender())
    return true;
  if (!UTisUIAvailable())
    return true;
  ROP_RenderManager *manager = ROP_RenderManager::getManager();
  return manager && manager->isActive();
}

// ---- time --------------------------------------------------------------------------------

/** One timeline a capture is played on: where it starts, how fast, what happens at its ends. */
struct Timeline {
  fpreal phase = 0;
  fpreal speed = 1;
  LoopMode loop = LoopMode::Loop;
  /** How long after the scene's first frame this timeline starts moving, in seconds. */
  fpreal delay = 0;
};

/**
 * The capture's time at a scene time, after phase, delay, speed and looping. The scene's
 * first frame shows the capture at its phase; from `delay` on it runs at its speed,
 * backwards when that is negative.
 */
double captureSeconds(const vgsb_info &info, const Timeline &line, fpreal sceneTime) {
  const double duration = info.duration;
  const double elapsed =
      double(sceneTime - CHgetManager()->getGlobalStart() - line.delay) * line.speed;
  double seconds = line.phase * duration + elapsed;
  if (duration > 0) {
    if (line.loop == LoopMode::Loop) {
      // One frame interval past the last instant, so the loop does not show the last and
      // the first frame back to back as if they were one.
      const double period = duration + (info.frame_rate > 0 ? 1.0 / info.frame_rate : 0.0);
      seconds = std::fmod(seconds, period);
      if (seconds < 0)
        seconds += period;
    } else if (line.loop == LoopMode::PingPong) {
      // There and back in twice the duration: each end is shown once per turn.
      const double turn = 2.0 * duration;
      seconds = std::fmod(seconds, turn);
      if (seconds < 0)
        seconds += turn;
      if (seconds > duration)
        seconds = turn - seconds;
    }
  }
  return std::clamp(seconds, 0.0, duration);
}

/** The scene times that will be asked for next, soonest first. */
std::vector<fpreal> upcomingTimes(fpreal now, size_t count, bool isPlaying, bool isRendering) {
  const CH_Manager *channels = CHgetManager();
  const fpreal step = 1.0 / std::max(channels->getSamplesPerSec(), fpreal(1e-6));
  if (!isPlaying && !isRendering)
    return {now + step, now - step}; // scrubbing: whichever way the user steps next

  const fpreal first = channels->getGlobalStart();
  const fpreal last = channels->getGlobalEnd();
  const fpreal direction = (isPlaying && playingBackwards()) ? -1 : 1;
  const fpreal slack = step * 0.5;
  std::vector<fpreal> times;
  fpreal t = now;
  for (size_t i = 0; i < count; ++i) {
    t += direction * step;
    if (t > last + slack) {
      if (isRendering)
        break;
      t = first;
    } else if (t < first - slack) {
      t = last;
    }
    times.push_back(t);
  }
  return times;
}

bool sameTime(double a, double b) { return std::fabs(a - b) < 1e-6; }

// ---- variants ------------------------------------------------------------------------------

/** A well-mixed 64-bit hash, so neighbouring point numbers land far apart. */
uint64_t mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

/** A number in [0, 1) that depends only on `value` and `seed`. */
double random01(uint64_t value, uint64_t seed) {
  return double(mix(value ^ mix(seed + 0x51ED27Full)) >> 11) * (1.0 / 9007199254740992.0);
}

// ---- writing -------------------------------------------------------------------------------

struct Shape {
  /** Applied to positions and orientations, for captures whose up is not Y. */
  UT_QuaternionF turn{0, 0, 0, 1};
  bool turned = false;
  bool linearize = true;
  bool castShadows = false;
};

Shape shapeFor(UpAxis axis) {
  Shape shape;
  const float half = float(M_SQRT1_2);
  switch (axis) {
  case UpAxis::Z: // -90 degrees about X: the capture's +Z becomes +Y
    shape.turn = UT_QuaternionF(-half, 0, 0, half);
    shape.turned = true;
    break;
  case UpAxis::NegY: // 180 degrees about X
    shape.turn = UT_QuaternionF(1, 0, 0, 0);
    shape.turned = true;
    break;
  default:
    break;
  }
  return shape;
}

/** sRGB to the scene linear space of the session's OCIO config, as Bake GSplats does. */
PXL_OCIO::PHandle colourProcessor() {
  for (const char *source : {"sRGB", "sRGB Encoded Rec.709 (sRGB)", "srgb_tx"}) {
    PXL_OCIO::PHandle processor = PXL_OCIO::lookupProcessor(source, "scene_linear", "");
    if (PXL_OCIO::isValidTransform(processor))
      return processor;
  }
  return PXL_OCIO::PHandle();
}

GA_Attribute *pointFloats(GU_Detail &gdp, const char *name, int size, GA_TypeInfo type) {
  GA_Attribute *attribute = gdp.addFloatTuple(GA_ATTRIB_POINT, name, size);
  if (attribute)
    attribute->setTypeInfo(type);
  return attribute;
}

/** Where one copy goes: rotated, scaled uniformly, then moved to a point. */
struct Placement {
  UT_QuaternionF rotation{0, 0, 0, 1};
  UT_Vector3F position{0, 0, 0};
  float scale = 1;
  bool identity = true;
};

/**
 * The Houdini GSplat attributes Bake GSplats makes - Cd, orient, scale, GS_Alpha, and with
 * harmonics GS_SPH_R/G/B and restorient - created on an empty detail, their pages hardened
 * over every point so that copies can be written from many threads at once.
 */
struct Target {
  GA_RWHandleV3 position, colour, scale;
  GA_RWHandleQ orient;
  GA_RWHandleF alpha;
  GA_RWHandleV4 restOrient;
  GA_RWHandleM4 sphere[3];
  bool harmonics = false;
  bool convert = false;
  PXL_OCIO::PHandle processor;

  Target(GU_Detail &gdp, exint total, bool withHarmonics, const Shape &shape) {
    gdp.clearAndDestroy();
    if (total > 0)
      gdp.appendPointBlock(total);
    position = GA_RWHandleV3(gdp.getP());
    colour = GA_RWHandleV3(pointFloats(gdp, "Cd", 3, GA_TYPE_COLOR));
    orient = GA_RWHandleQ(pointFloats(gdp, "orient", 4, GA_TYPE_QUATERNION));
    scale = GA_RWHandleV3(pointFloats(gdp, "scale", 3, GA_TYPE_VOID));
    alpha = GA_RWHandleF(pointFloats(gdp, "GS_Alpha", 1, GA_TYPE_VOID));
    harmonics = withHarmonics;
    if (harmonics) {
      restOrient = GA_RWHandleV4(pointFloats(gdp, "restorient", 4, GA_TYPE_VOID));
      sphere[0] = GA_RWHandleM4(pointFloats(gdp, "GS_SPH_R", 16, GA_TYPE_VOID));
      sphere[1] = GA_RWHandleM4(pointFloats(gdp, "GS_SPH_G", 16, GA_TYPE_VOID));
      sphere[2] = GA_RWHandleM4(pointFloats(gdp, "GS_SPH_B", 16, GA_TYPE_VOID));
    }
    if (!shape.castShadows) {
      // What Bake GSplats sets to keep Karma from casting shadows from the splats.
      UT_StringHolder name = UT_VarEncode::encodeAttrib("karma:object:rendervisibility");
      GA_RWHandleS visibility(gdp.addStringTuple(GA_ATTRIB_DETAIL, name, 1));
      if (visibility.isValid())
        visibility.set(GA_DETAIL_OFFSET, "-shadow");
    }
    // Copies at their own times share one page of points between two of them, so the
    // pages are made real up front rather than by whichever thread writes them first.
    if (total > 0) {
      for (GA_Attribute *attribute :
           {position.getAttribute(), colour.getAttribute(), orient.getAttribute(),
            scale.getAttribute(), alpha.getAttribute()})
        attribute->hardenAllPages();
      if (harmonics)
        for (GA_Attribute *attribute :
             {restOrient.getAttribute(), sphere[0].getAttribute(), sphere[1].getAttribute(),
              sphere[2].getAttribute()})
          attribute->hardenAllPages();
    }
    processor = shape.linearize ? colourProcessor() : PXL_OCIO::PHandle();
    convert = shape.linearize && PXL_OCIO::isValidTransform(processor) &&
              !PXL_OCIO::isNoOpTransform(processor);
  }
};

/** Splats per task when copies are written in parallel. */
constexpr exint Block = 4096;

/**
 * Writes splats [begin, end) of a decoded frame as one copy, starting at point `base`.
 * The capture's own orientation - before the up-axis turn and the copy's rotation - goes
 * to restorient: Houdini evaluates the harmonics against it, so turning a copy turns its
 * view-dependent colour with it.
 */
void writeSplats(Target &target, const vgsb_frame &frame, const Shape &shape,
                 const Placement &place, GA_Offset base, exint begin, exint end) {
  const exint count = exint(frame.count);
  const int coefficients = (target.harmonics && frame.sh) ? frame.sh_coefficients : 0;
  const UT_QuaternionF turn = place.identity ? shape.turn : place.rotation * shape.turn;
  const bool turned = shape.turned || !place.identity;

  std::vector<float> colours(size_t(end - begin) * 3);
  for (exint i = begin; i < end; ++i)
    for (int c = 0; c < 3; ++c)
      colours[size_t(i - begin) * 3 + size_t(c)] = frame.radiance[i * 4 + c] * C0 + 0.5f;
  if (target.convert)
    PXL_OCIO::transform(target.processor, colours.data(), int(end - begin), 3);

  for (exint i = begin; i < end; ++i) {
    const GA_Offset offset = base + i;
    const float *p = frame.positions + i * 3;
    UT_Vector3F point(p[0], p[1], p[2]);
    // wxyz in, Houdini's xyzw out.
    const float *r = frame.rotations + i * 4;
    const UT_QuaternionF rest(r[1], r[2], r[3], r[0]);
    UT_QuaternionF oriented = rest;
    if (turned) {
      point = turn.rotate(point * place.scale);
      oriented = turn * rest;
    }
    target.position.set(offset, point + place.position);
    target.orient.set(offset, oriented);
    const float *s = frame.scales + i * 3;
    target.scale.set(offset, UT_Vector3F(s[0], s[1], s[2]) * place.scale);
    target.alpha.set(offset, frame.radiance[i * 4 + 3]);
    const float *rgb = &colours[size_t(i - begin) * 3];
    target.colour.set(offset, UT_Vector3F(rgb[0], rgb[1], rgb[2]));

    if (target.harmonics) {
      target.restOrient.set(offset, UT_Vector4F(rest.x(), rest.y(), rest.z(), rest.w()));
      for (int c = 0; c < 3; ++c) {
        UT_Matrix4F terms(0.0f);
        float *values = terms.data();
        values[0] = frame.radiance[i * 4 + c];
        for (int k = 0; k < coefficients && k < 15; ++k)
          values[1 + k] = frame.sh[(size_t(k) * size_t(count) + size_t(i)) * 3 + size_t(c)];
        target.sphere[c].set(offset, terms);
      }
    }
  }
}

// ---- the plan for one cook -----------------------------------------------------------------

/** Every copy on the same capture, timeline and time: decoded once, written once per copy. */
struct Group {
  std::string path;
  Timeline line;
  double seconds = 0;
  std::vector<Placement> copies;
  exint splats = 0; // per copy, known once the frame is decoded
};

/** A key for grouping copies whose timelines are the same. */
struct GroupKey {
  std::string path;
  double phase, speed, delay;
  int loop;
  bool operator<(const GroupKey &o) const {
    if (path != o.path)
      return path < o.path;
    if (phase != o.phase)
      return phase < o.phase;
    if (speed != o.speed)
      return speed < o.speed;
    if (delay != o.delay)
      return delay < o.delay;
    return loop < o.loop;
  }
};

LoopMode loopFor(int menuOrAttribute, const vgsb_info &info, bool fromMenu) {
  // The menu's first entry, or -1 in an attribute, defers to the capture; the others are
  // its modes.
  const int value = fromMenu ? menuOrAttribute - 1 : menuOrAttribute;
  if (value < 0)
    return LoopMode(std::clamp(int(info.playback_mode), 0, 2));
  return LoopMode(std::clamp(value, 0, 2));
}

/** A cache ceiling for captures played at several times at once, from VGS_CACHE_MB. */
uint64_t cacheBytes() {
  const char *value = std::getenv("VGS_CACHE_MB");
  long megabytes = 4096;
  if (value && *value) {
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end != value && parsed >= 0)
      megabytes = parsed;
  }
  return uint64_t(megabytes) * 1024 * 1024;
}

} // namespace

// ---- the node ------------------------------------------------------------------------------

void SOP_VGSCapture::installHooks() {
  OPgetDirector()->addGlobalOpChangedCallback(onNodeEvent, nullptr);
}

OP_Node *SOP_VGSCapture::create(OP_Network *net, const char *name, OP_Operator *op) {
  return new SOP_VGSCapture(net, name, op);
}

SOP_VGSCapture::SOP_VGSCapture(OP_Network *net, const char *name, OP_Operator *op)
    : SOP_Node(net, name, op) {
  std::lock_guard<std::mutex> lock(nodesMutex);
  nodes.insert(this);
  if (!playbackHooked) {
    if (UT_Playback *playback = UT_Playback::getPlayback()) {
      playback->addPlayCallback(onPlayback, nullptr);
      playbackHooked = true;
    }
  }
}

SOP_VGSCapture::~SOP_VGSCapture() {
  {
    std::lock_guard<std::mutex> lock(nodesMutex);
    nodes.erase(this);
  }
  closeAll();
}

void SOP_VGSCapture::opChanged(OP_EventType reason, void *data) {
  SOP_Node::opChanged(reason, data);
  syncStartFrame(*this, reason, data);
}

void SOP_VGSCapture::getDescriptiveParmName(UT_String &name) const { name = parm::File; }

void SOP_VGSCapture::closeAll() {
  for (auto &entry : players)
    if (entry.second.player)
      vgsb_close(entry.second.player);
  players.clear();
}

void SOP_VGSCapture::reload() {
  closeAll();
  failures.clear();
  forceRecook();
}

void SOP_VGSCapture::reloadFrom(OP_Node &node) {
  if (node.getOperator()->getName() == TypeName && node.getOpTypeID() == SOP_OPTYPE_ID) {
    static_cast<SOP_VGSCapture &>(node).reload();
    return;
  }
  if (!node.isNetwork())
    return;
  OP_Network &network = static_cast<OP_Network &>(node);
  for (int i = 0; i < network.getNchildren(); ++i) {
    OP_Node *child = network.getChild(i);
    if (child && child->getOpTypeID() == SOP_OPTYPE_ID && child->getOperator()->getName() == TypeName)
      static_cast<SOP_VGSCapture *>(child)->reload();
  }
}

SOP_VGSCapture::Opened *SOP_VGSCapture::open(const std::string &path, bool includeSh,
                                             int slots) {
  auto found = players.find(path);
  if (found != players.end()) {
    if (found->second.slots >= slots)
      return &found->second;
    // More copies at more times than it was opened for: reopen with room for them all.
    vgsb_close(found->second.player);
    players.erase(found);
  }
  if (failures.count(path))
    return nullptr;
  Opened opened;
  opened.slots = std::clamp(slots, 2, 64);
  opened.player = vgsb_open(path.c_str(), includeSh ? 1 : 0, opened.slots, envThreads());
  if (!opened.player) {
    failures[path] = vgsb_last_error();
    return nullptr;
  }
  vgsb_get_info(opened.player, &opened.info);
  return &players.emplace(path, opened).first->second;
}

OP_ERROR SOP_VGSCapture::cookMySop(OP_Context &context) {
  flags().setTimeDep(true);
  const fpreal now = context.getTime();

  // The points a scatter puts copies on: the first input, or failing that the Points
  // path, resolved from this SOP and then from the object it sits in, whose Points field
  // it follows.
  OP_AutoLockInputs inputs(this);
  if (inputs.lock(context) >= UT_ERROR_ABORT)
    return error();
  const GU_Detail *points = nullptr;
  GU_DetailHandleAutoReadLock *pointsLock = nullptr;
  GU_DetailHandle pointsHandle;
  if (getInput(0)) {
    points = inputGeo(0, context);
  } else {
    UT_String pointsPath;
    evalString(pointsPath, parm::Points, 0, now);
    if (pointsPath.isstring()) {
      SOP_Node *source = getSOPNode(pointsPath, 0);
      if (!source && getCreator())
        source = getCreator()->getSOPNode(pointsPath, 0);
      if (!source || source == this) {
        addError(SOP_MESSAGE, "VGS: the Points path is not a SOP");
        return error();
      }
      addExtraInput(source, OP_INTEREST_DATA);
      pointsHandle = source->getCookedGeoHandle(context);
      pointsLock = new GU_DetailHandleAutoReadLock(pointsHandle);
      points = pointsLock->getGdp();
    }
  }
  std::unique_ptr<GU_DetailHandleAutoReadLock> pointsLockOwner(pointsLock);

  UT_String pathText;
  evalString(pathText, parm::File, 0, now);
  mainPath = pathText.toStdString();

  const bool isRendering = rendering(*this);
  const bool isPlaying = !isRendering && playing();

  // Harmonics are most of what a frame costs to decode and to copy, and view-dependent
  // colour is hard to judge on a moving picture; paused, scrubbed or rendered, they come
  // back.
  const bool includeSh =
      evalInt(parm::UseSh, 0, now) != 0 && !(isPlaying && evalInt(parm::NoShPlaying, 0, now) != 0);
  // Renders draw every splat whatever the viewport is set to.
  const float density =
      isRendering ? 1.0f : splatFraction(float(evalFloat(parm::Density, 0, now)));

  const fpreal basePhase = evalFloat(parm::Phase, 0, now);
  const fpreal baseSpeed = evalFloat(parm::Speed, 0, now);
  const int loopMenu = std::clamp(int(evalInt(parm::LoopMode, 0, now)), 0, 3);
  const exint variants = std::max<exint>(0, evalInt(parm::Variants, 0, now));
  const fpreal spread = std::clamp(evalFloat(parm::PhaseSpread, 0, now), fpreal(0), fpreal(1));
  const fpreal variation = std::max(evalFloat(parm::SpeedVariation, 0, now), fpreal(0));
  const uint64_t seed = uint64_t(evalInt(parm::Seed, 0, now));
  const fpreal fps = std::max(CHgetManager()->getSamplesPerSec(), fpreal(1e-6));

  // ---- which copies, on which timelines ----
  //
  // Copies on the same capture with the same timeline form a group: one decode, written
  // once per copy. With Variants at N, the copies without timings of their own fall into
  // at most N groups per capture however many points there are.
  std::vector<Group> groups;
  std::map<GroupKey, size_t> groupOf;
  std::set<std::string> missing;
  auto place = [&](const std::string &path, const Timeline &line, const Placement &where,
                   int loopValue, bool loopFromMenu) {
    if (path.empty())
      return;
    Opened *opened = open(path, includeSh, envSlots());
    if (!opened) {
      missing.insert(path);
      return;
    }
    Timeline resolved = line;
    resolved.loop = loopFor(loopValue, opened->info, loopFromMenu);
    const GroupKey key{path, resolved.phase, resolved.speed, resolved.delay, int(resolved.loop)};
    auto found = groupOf.find(key);
    if (found == groupOf.end()) {
      found = groupOf.emplace(key, groups.size()).first;
      Group group;
      group.path = path;
      group.line = resolved;
      groups.push_back(std::move(group));
    }
    groups[found->second].copies.push_back(where);
  };

  if (!points) {
    // No scatter: the capture once, where it is, on the node's own timeline.
    Timeline line;
    line.phase = basePhase;
    line.speed = baseSpeed;
    place(mainPath, line, Placement(), loopMenu, true);
  } else {
    GA_ROHandleS pathAttribute(points, GA_ATTRIB_POINT, pointattrib::Path);
    GA_ROHandleI startAttribute(points, GA_ATTRIB_POINT, pointattrib::StartFrame);
    GA_ROHandleF offsetAttribute(points, GA_ATTRIB_POINT, pointattrib::Offset);
    GA_ROHandleF speedAttribute(points, GA_ATTRIB_POINT, pointattrib::Speed);
    GA_ROHandleI loopAttribute(points, GA_ATTRIB_POINT, pointattrib::Loop);
    GA_ROHandleI variantAttribute(points, GA_ATTRIB_POINT, pointattrib::Variant);
    GA_ROHandleQ orientAttribute(points, GA_ATTRIB_POINT, "orient");
    GA_ROHandleF pscaleAttribute(points, GA_ATTRIB_POINT, "pscale");

    GA_Offset offset;
    GA_FOR_ALL_PTOFF(points, offset) {
      const GA_Index index = points->pointIndex(offset);
      std::string path = mainPath;
      if (pathAttribute.isValid()) {
        const UT_StringHolder value = pathAttribute.get(offset);
        if (value.isstring())
          path = value.toStdString();
      }

      // Which of the node's variants this point plays, unless it names one itself.
      uint64_t variant = 0;
      if (variantAttribute.isValid())
        variant = uint64_t(std::max<exint>(0, variantAttribute.get(offset)));
      else if (variants > 0)
        variant = mix(uint64_t(index) ^ mix(seed)) % uint64_t(variants);
      else
        variant = uint64_t(index);

      Timeline line;
      const double where = variants > 0 ? double(variant) / double(variants)
                                         : random01(variant, seed);
      line.phase = basePhase + spread * where;
      line.phase -= std::floor(line.phase);
      line.speed = baseSpeed * (1.0 + variation * (2.0 * random01(variant, seed ^ 0xABCDEFull) - 1.0));

      // Attributes of the point's own take precedence over the variant.
      if (startAttribute.isValid() && !path.empty()) {
        if (Opened *opened = open(path, includeSh, envSlots())) {
          const int last = lastFrameOf(opened->info.duration, opened->info.frame_rate);
          if (last > 0)
            line.phase = std::clamp(double(startAttribute.get(offset)) / last, 0.0, 1.0);
        }
      }
      if (speedAttribute.isValid())
        line.speed = speedAttribute.get(offset);
      if (offsetAttribute.isValid())
        line.delay = offsetAttribute.get(offset) / fps;

      Placement at;
      at.position = points->getPos3(offset);
      if (orientAttribute.isValid())
        at.rotation = orientAttribute.get(offset);
      if (pscaleAttribute.isValid())
        at.scale = pscaleAttribute.get(offset);
      at.identity = false;

      if (loopAttribute.isValid())
        place(path, line, at, loopAttribute.get(offset), false);
      else
        place(path, line, at, loopMenu, true);
    }
  }

  for (const std::string &path : missing) {
    const auto failed = failures.find(path);
    UT_WorkBuffer message;
    message.sprintf("VGS: %s: %s", path.c_str(),
                    failed != failures.end() ? failed->second.c_str() : "cannot open");
    addWarning(SOP_MESSAGE, message.buffer());
  }
  if (groups.empty()) {
    gdp->clearAndDestroy();
    shownSeconds = -1;
    shownSplats = shownCopies = shownTimelines = 0;
    if (!missing.empty() && !points)
      addError(SOP_MESSAGE, "VGS: cannot open the capture");
    return error();
  }

  // The slider of Start Frame spans the node's own capture, here and on its object.
  auto main = players.find(mainPath);
  if (main != players.end()) {
    const int last = lastFrameOf(main->second.info.duration, main->second.info.frame_rate);
    fitStartFrameRange(*this, last);
    if (OP_Node *owner = getCreator())
      if (owner->getOperator()->getName() == TypeName && owner != this)
        fitStartFrameRange(*owner, last);
  }

  // ---- what each capture decodes, now and next ----
  const size_t lookahead =
      std::max(size_t(envSlots()), size_t(std::ceil(LookaheadSeconds * fps)));
  const std::vector<fpreal> upcoming = upcomingTimes(now, lookahead, isPlaying, isRendering);
  std::map<std::string, std::vector<size_t>> groupsOf;
  for (size_t g = 0; g < groups.size(); ++g)
    groupsOf[groups[g].path].push_back(g);

  for (auto &entry : groupsOf) {
    const std::string &path = entry.first;
    std::vector<double> nowTimes;
    for (size_t g : entry.second) {
      Opened &opened = players.find(path)->second;
      groups[g].seconds = captureSeconds(opened.info, groups[g].line, now);
      if (std::none_of(nowTimes.begin(), nowTimes.end(),
                       [&](double t) { return sameTime(t, groups[g].seconds); }))
        nowTimes.push_back(groups[g].seconds);
    }
    // Room for every instant shown now and the next frame of each, so that playback finds
    // them decoded.
    Opened *opened = open(path, includeSh, std::max(envSlots(), int(nowTimes.size()) * 2 + 2));
    if (!opened)
      continue;
    vgsb_set_include_sh(opened->player, includeSh ? 1 : 0);
    vgsb_set_density(opened->player, density);
    // Several instants at once reach into several chunks at once: keep them decoded, or
    // every frame would decode a chunk again. One instant needs only the chunk it is on.
    if (nowTimes.size() > 1)
      vgsb_set_cache(opened->player, 1 << 20, cacheBytes());
    else
      vgsb_set_cache(opened->player, 0, 0);

    std::vector<double> schedule = nowTimes;
    for (fpreal t : upcoming)
      for (size_t g : entry.second)
        schedule.push_back(captureSeconds(opened->info, groups[g].line, t));
    vgsb_schedule(opened->player, schedule.data(), int(schedule.size()));
  }

  // ---- everything decoded? ----
  //
  // Checked for every group before anything is written: during playback a frame that is
  // not ready leaves the whole previous result showing, rather than some copies missing.
  const double wait = isPlaying ? envPlaybackWaitMs() : ExactWaitMs;
  bool withHarmonics = false;
  exint total = 0;
  for (Group &group : groups) {
    auto found = players.find(group.path);
    if (found == players.end())
      continue;
    vgsb_frame frame{};
    const int status = vgsb_acquire(found->second.player, group.seconds, wait, &frame);
    if (status == VGSB_TIMEOUT)
      return error(); // not ready in time during playback: keep showing the frame before
    if (status != VGSB_OK) {
      failures[group.path] = vgsb_last_error();
      vgsb_close(found->second.player);
      players.erase(found);
      addError(SOP_MESSAGE, failures[group.path].c_str());
      return error();
    }
    group.splats = exint(frame.count);
    withHarmonics = withHarmonics || (frame.sh && frame.sh_coefficients > 0);
    total += group.splats * exint(group.copies.size());
    vgsb_release(found->second.player);
  }

  // ---- written ----
  Shape shape = shapeFor(UpAxis(std::clamp(int(evalInt(parm::UpAxis, 0, now)), 0, 2)));
  shape.linearize = evalInt(parm::Linearize, 0, now) != 0;
  shape.castShadows = evalInt(parm::CastShadows, 0, now) != 0;

  // Auto: splats for the viewport, which misplaces Gaussian splats inside packed
  // primitives; instances for renders, which draw them right for a fraction of the memory.
  const ScatterOutput output = ScatterOutput(std::clamp(int(evalInt(parm::Output, 0, now)), 0, 2));
  const bool packed = points && (output == ScatterOutput::Packed ||
                                 (output == ScatterOutput::Auto && isRendering));
  if (packed) {
    // Each timeline's splats written once, into a geometry of its own, and a packed
    // instance of it on every point of the group: the cost is the timelines', not the
    // copies'. SOP Import turns them into USD instances, which Karma XPU renders as such.
    gdp->clearAndDestroy();
    exint copies = 0;
    for (const Group &group : groups) {
      auto found = players.find(group.path);
      if (found == players.end() || group.splats == 0)
        continue;
      vgsb_frame frame{};
      if (vgsb_acquire(found->second.player, group.seconds, wait, &frame) != VGSB_OK)
        continue;
      const exint count = std::min(exint(frame.count), group.splats);
      auto *variant = new GU_Detail;
      {
        Target inner(*variant, count, withHarmonics, shape);
        const exint blocks = (count + Block - 1) / Block;
        UTparallelFor(UT_BlockedRange<exint>(0, blocks), [&](const UT_BlockedRange<exint> &range) {
          for (exint block = range.begin(); block != range.end(); ++block)
            writeSplats(inner, frame, shape, Placement(), GA_Offset(0), block * Block,
                        std::min(count, (block + 1) * Block));
        });
      }
      vgsb_release(found->second.player);
      GU_DetailHandle handle;
      handle.allocateAndSet(variant, true);
      const GU_ConstDetailHandle shared(handle);
      for (const Placement &at : group.copies) {
        GU_PrimPacked *instance = GU_PackedGeometry::packGeometry(*gdp, shared);
        if (!instance)
          continue;
        UT_Matrix3F turn;
        at.rotation.getRotationMatrix(turn);
        UT_Matrix3D local(1.0);
        local.scale(at.scale, at.scale, at.scale);
        local *= UT_Matrix3D(turn);
        instance->setLocalTransform(local);
        gdp->setPos3(instance->getPointOffset(0), at.position);
        ++copies;
      }
    }
    if (!shape.castShadows) {
      UT_StringHolder name = UT_VarEncode::encodeAttrib("karma:object:rendervisibility");
      GA_RWHandleS visibility(gdp->addStringTuple(GA_ATTRIB_DETAIL, name, 1));
      if (visibility.isValid())
        visibility.set(GA_DETAIL_OFFSET, "-shadow");
    }
    GA_RWHandleF shown(gdp->addFloatTuple(GA_ATTRIB_DETAIL, "vgs_seconds", 1));
    if (shown.isValid())
      shown.set(GA_DETAIL_OFFSET, float(groups.front().seconds));
    shownSeconds = groups.front().seconds;
    shownSplats = total;
    shownCopies = copies;
    shownTimelines = exint(groups.size());
    return error();
  }

  Target target(*gdp, total, withHarmonics, shape);

  GA_Offset base = GA_Offset(0);
  exint copies = 0;
  for (const Group &group : groups) {
    auto found = players.find(group.path);
    if (found == players.end() || group.splats == 0)
      continue;
    vgsb_frame frame{};
    if (vgsb_acquire(found->second.player, group.seconds, wait, &frame) != VGSB_OK)
      continue; // held a moment ago; nothing better to do than leave these points empty
    const exint count = std::min(exint(frame.count), group.splats);
    const exint blocks = (count + Block - 1) / Block;
    const exint tasks = blocks * exint(group.copies.size());
    const GA_Offset groupBase = base;
    UTparallelFor(UT_BlockedRange<exint>(0, tasks), [&](const UT_BlockedRange<exint> &range) {
      for (exint task = range.begin(); task != range.end(); ++task) {
        const exint copy = task / blocks;
        const exint begin = (task % blocks) * Block;
        const exint end = std::min(count, begin + Block);
        writeSplats(target, frame, shape, group.copies[size_t(copy)],
                    groupBase + copy * count, begin, end);
      }
    });
    vgsb_release(found->second.player);
    base += group.splats * exint(group.copies.size());
    copies += exint(group.copies.size());
  }

  GA_RWHandleF shown(gdp->addFloatTuple(GA_ATTRIB_DETAIL, "vgs_seconds", 1));
  if (shown.isValid())
    shown.set(GA_DETAIL_OFFSET, float(groups.front().seconds));

  shownSeconds = groups.front().seconds;
  shownSplats = total;
  shownCopies = copies;
  shownTimelines = exint(groups.size());

  return error();
}

void SOP_VGSCapture::getNodeSpecificInfoText(OP_Context &context, OP_NodeInfoParms &parms) {
  SOP_Node::getNodeSpecificInfoText(context, parms);
  for (const auto &failed : failures)
    parms.appendSprintf("VGS: %s: %s\n", failed.first.c_str(), failed.second.c_str());
  auto main = players.find(mainPath);
  if (main == players.end())
    return;
  const vgsb_info &info = main->second.info;
  parms.appendSeparator();
  const struct {
    const char *label;
    const char *value;
  } texts[] = {
      {"Title", info.title},     {"Author", info.author},       {"Project", info.project},
      {"Take", info.take},       {"Studio", info.studio},       {"Copyright", info.copyright},
  };
  for (const auto &text : texts)
    if (text.value && *text.value)
      parms.appendSprintf("%s: %s\n", text.label, text.value);
  parms.appendSprintf("Duration: %.2f s, %d frames at %.3g fps\n", info.duration,
                      lastFrameOf(info.duration, info.frame_rate) + 1, info.frame_rate);
  parms.appendSprintf("Splats: up to %llu\n", (unsigned long long)info.max_splats);
  parms.appendSprintf("Spherical harmonics: degree %u\n", info.sh_degree);
  const char *modes[] = {"once", "loop", "ping-pong"};
  if (info.playback_mode >= 0 && info.playback_mode <= 2)
    parms.appendSprintf("Plays: %s\n", modes[info.playback_mode]);
  if (shownCopies > 1 || players.size() > 1)
    parms.appendSprintf("Showing: %lld copies on %lld timelines, %lld splats\n",
                        (long long)shownCopies, (long long)shownTimelines, (long long)shownSplats);
  else if (shownSeconds >= 0)
    parms.appendSprintf("Showing: %.3f s, %lld splats\n", shownSeconds, (long long)shownSplats);
}

} // namespace vgs
