#include "SOP_VGSCapture.h"

#include "VGS_Parameters.h"

#include <CH/CH_Manager.h>
#include <GA/GA_Handle.h>
#include <GA/GA_Types.h>
#include <GU/GU_Detail.h>
#include <OP/OP_Director.h>
#include <OP/OP_NodeInfoParms.h>
#include <OP/OP_Operator.h>
#include <PXL/PXL_OCIO.h>
#include <ROP/ROP_Node.h>
#include <ROP/ROP_RenderManager.h>
#include <SYS/SYS_Math.h>
#include <UT/UT_Matrix4.h>
#include <UT/UT_ParallelUtil.h>
#include <UT/UT_Playback.h>
#include <UT/UT_Quaternion.h>
#include <UT/UT_UI.h>
#include <UT/UT_VarEncode.h>
#include <UT/UT_Vector3.h>
#include <UT/UT_Vector4.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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

struct Timeline {
  fpreal phase = 0;
  fpreal speed = 1;
  LoopMode loop = LoopMode::Loop;
};

/**
 * The capture's time at a scene time, after phase, speed and looping. The scene's first
 * frame shows the capture at its phase, and from there it runs at its speed, backwards
 * when that is negative.
 */
double captureSeconds(const vgsb_info &info, const Timeline &line, fpreal sceneTime) {
  const double duration = info.duration;
  const double elapsed = double(sceneTime - CHgetManager()->getGlobalStart()) * line.speed;
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

/**
 * Copies a decoded frame into `gdp` as Houdini GSplats, the attributes Bake GSplats
 * makes: Cd, orient, scale, GS_Alpha, and with harmonics GS_SPH_R/G/B and restorient.
 * One page of points per task, so that no two tasks share a page.
 */
void writeFrame(GU_Detail &gdp, const vgsb_frame &frame, const Shape &shape) {
  const exint count = exint(frame.count);
  const int coefficients = frame.sh ? frame.sh_coefficients : 0;

  gdp.clearAndDestroy();
  const GA_Offset base = count > 0 ? gdp.appendPointBlock(count) : GA_Offset(0);

  GA_RWHandleV3 position(gdp.getP());
  GA_RWHandleV3 colour(pointFloats(gdp, "Cd", 3, GA_TYPE_COLOR));
  GA_RWHandleQ orient(pointFloats(gdp, "orient", 4, GA_TYPE_QUATERNION));
  GA_RWHandleV3 scale(pointFloats(gdp, "scale", 3, GA_TYPE_VOID));
  GA_RWHandleF alpha(pointFloats(gdp, "GS_Alpha", 1, GA_TYPE_VOID));
  GA_RWHandleV4 restOrient;
  GA_RWHandleM4 sphere[3];
  if (coefficients > 0) {
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
  GA_RWHandleF shown(gdp.addFloatTuple(GA_ATTRIB_DETAIL, "vgs_seconds", 1));
  if (shown.isValid())
    shown.set(GA_DETAIL_OFFSET, float(frame.seconds));

  if (count == 0)
    return;

  const PXL_OCIO::PHandle processor = shape.linearize ? colourProcessor() : PXL_OCIO::PHandle();
  const bool convert = shape.linearize && PXL_OCIO::isValidTransform(processor) &&
                       !PXL_OCIO::isNoOpTransform(processor);

  const exint pages = (count + GA_PAGE_SIZE - 1) / GA_PAGE_SIZE;
  UTparallelFor(UT_BlockedRange<exint>(0, pages), [&](const UT_BlockedRange<exint> &range) {
    std::vector<float> colours(size_t(GA_PAGE_SIZE) * 3);
    for (exint page = range.begin(); page != range.end(); ++page) {
      const exint begin = page * GA_PAGE_SIZE;
      const exint end = std::min(count, begin + GA_PAGE_SIZE);

      // Colour: the DC term turned back into the capture's colour, then converted a page
      // at a time.
      for (exint i = begin; i < end; ++i)
        for (int c = 0; c < 3; ++c)
          colours[size_t(i - begin) * 3 + size_t(c)] = frame.radiance[i * 4 + c] * C0 + 0.5f;
      if (convert)
        PXL_OCIO::transform(processor, colours.data(), int(end - begin), 3);

      for (exint i = begin; i < end; ++i) {
        const GA_Offset offset = base + i;
        const float *p = frame.positions + i * 3;
        UT_Vector3F point(p[0], p[1], p[2]);
        // wxyz in, Houdini's xyzw out.
        const float *r = frame.rotations + i * 4;
        const UT_QuaternionF rest(r[1], r[2], r[3], r[0]);
        UT_QuaternionF turned = rest;
        if (shape.turned) {
          point = shape.turn.rotate(point);
          turned = shape.turn * rest;
        }
        position.set(offset, point);
        orient.set(offset, turned);
        const float *s = frame.scales + i * 3;
        scale.set(offset, UT_Vector3F(s[0], s[1], s[2]));
        alpha.set(offset, frame.radiance[i * 4 + 3]);
        const float *rgb = &colours[size_t(i - begin) * 3];
        colour.set(offset, UT_Vector3F(rgb[0], rgb[1], rgb[2]));

        if (coefficients > 0) {
          // Houdini evaluates the harmonics against the orientation the splat had when
          // they were fitted, so a turn applied here does not turn the colours with it.
          restOrient.set(offset, UT_Vector4F(rest.x(), rest.y(), rest.z(), rest.w()));
          for (int c = 0; c < 3; ++c) {
            UT_Matrix4F terms(0.0f);
            float *values = terms.data();
            values[0] = frame.radiance[i * 4 + c];
            for (int k = 0; k < coefficients && k < 15; ++k)
              values[1 + k] = frame.sh[(size_t(k) * size_t(count) + size_t(i)) * 3 + size_t(c)];
            sphere[c].set(offset, terms);
          }
        }
      }
    }
  });
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
  closePlayer();
}

void SOP_VGSCapture::opChanged(OP_EventType reason, void *data) {
  SOP_Node::opChanged(reason, data);
  syncStartFrame(*this, reason, data);
}

void SOP_VGSCapture::getDescriptiveParmName(UT_String &name) const { name = parm::File; }

void SOP_VGSCapture::closePlayer() {
  if (player)
    vgsb_close(player);
  player = nullptr;
  openPath.clear();
}

void SOP_VGSCapture::reload() {
  closePlayer();
  failedPath.clear();
  failure.clear();
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

bool SOP_VGSCapture::ensurePlayer(const UT_StringHolder &path, bool includeSh) {
  if (player && openPath == path)
    return true;
  closePlayer();
  // A capture that failed to open is not retried every frame, only once its path changes
  // or somebody presses Reload.
  if (failedPath == path)
    return false;
  player = vgsb_open(path.c_str(), includeSh ? 1 : 0, envSlots(), envThreads());
  if (!player) {
    failedPath = path;
    failure = vgsb_last_error();
    return false;
  }
  vgsb_get_info(player, &info);
  openPath = path;
  // The slider of Start Frame spans this capture, here and on the object it sits in.
  const int last = lastFrameOf(info.duration, info.frame_rate);
  fitStartFrameRange(*this, last);
  if (OP_Node *owner = getCreator())
    if (owner->getOperator()->getName() == TypeName && owner != this)
      fitStartFrameRange(*owner, last);
  failedPath.clear();
  failure.clear();
  return true;
}

OP_ERROR SOP_VGSCapture::cookMySop(OP_Context &context) {
  flags().setTimeDep(true);
  const fpreal now = context.getTime();

  UT_String pathText;
  evalString(pathText, parm::File, 0, now);
  const UT_StringHolder path(pathText);
  if (!path.isstring()) {
    closePlayer();
    gdp->clearAndDestroy();
    shownSeconds = -1;
    shownSplats = 0;
    return error();
  }

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

  if (!ensurePlayer(path, includeSh)) {
    gdp->clearAndDestroy();
    addError(SOP_MESSAGE, failure.empty() ? "cannot open the capture" : failure.c_str());
    return error();
  }
  vgsb_set_include_sh(player, includeSh ? 1 : 0);
  vgsb_set_density(player, density);

  Timeline line;
  line.phase = evalFloat(parm::Phase, 0, now);
  line.speed = evalFloat(parm::Speed, 0, now);
  // The menu's first entry defers to the capture; the others are its modes, one along.
  const int menu = std::clamp(int(evalInt(parm::LoopMode, 0, now)), 0, 3);
  line.loop = menu == LoopFromCapture
                  ? LoopMode(std::clamp(int(info.playback_mode), 0, 2))
                  : LoopMode(menu - 1);

  const double seconds = captureSeconds(info, line, now);
  const size_t lookahead =
      std::max(size_t(envSlots()),
               size_t(std::ceil(LookaheadSeconds * CHgetManager()->getSamplesPerSec())));
  std::vector<double> schedule{seconds};
  for (fpreal t : upcomingTimes(now, lookahead, isPlaying, isRendering))
    schedule.push_back(captureSeconds(info, line, t));
  vgsb_schedule(player, schedule.data(), int(schedule.size()));

  const double wait = isPlaying ? envPlaybackWaitMs() : ExactWaitMs;
  vgsb_frame frame{};
  const int status = vgsb_acquire(player, seconds, wait, &frame);
  if (status == VGSB_TIMEOUT) {
    // Not ready in time during playback: keep showing the frame before.
    return error();
  }
  if (status != VGSB_OK) {
    failure = vgsb_last_error();
    failedPath = path;
    closePlayer();
    gdp->clearAndDestroy();
    addError(SOP_MESSAGE, failure.c_str());
    return error();
  }

  Shape shape = shapeFor(UpAxis(std::clamp(int(evalInt(parm::UpAxis, 0, now)), 0, 2)));
  shape.linearize = evalInt(parm::Linearize, 0, now) != 0;
  shape.castShadows = evalInt(parm::CastShadows, 0, now) != 0;
  writeFrame(*gdp, frame, shape);
  shownSeconds = frame.seconds;
  shownSplats = exint(frame.count);
  vgsb_release(player);
  return error();
}

void SOP_VGSCapture::getNodeSpecificInfoText(OP_Context &context, OP_NodeInfoParms &parms) {
  SOP_Node::getNodeSpecificInfoText(context, parms);
  if (!player) {
    if (!failure.empty())
      parms.appendSprintf("VGS: %s\n", failure.c_str());
    return;
  }
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
  if (shownSeconds >= 0)
    parms.appendSprintf("Showing: %.3f s, %lld splats\n", shownSeconds, (long long)shownSplats);
}

} // namespace vgs
