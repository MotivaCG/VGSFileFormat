#include "VGS_Parameters.h"

#include "SOP_VGSCapture.h"

#include <CH/CH_Manager.h>
#include <OP/OP_Director.h>
#include <OP/OP_Node.h>
#include <OP/OP_Error.h>
#include <PRM/PRM_Conditional.h>
#include <PRM/PRM_Include.h>
#include <PRM/PRM_Parm.h>
#include <PRM/PRM_ParmList.h>
#include <PRM/PRM_Range.h>
#include <PRM/PRM_SpareData.h>
#include <SYS/SYS_Math.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace vgs {

// ---- capture timing --------------------------------------------------------------------

namespace {

std::mutex timingMutex;
std::map<std::string, Timing> timings;

} // namespace

int lastFrameOf(double duration, double frameRate) {
  return frameRate > 0 ? int(std::lround(duration * frameRate)) : 0;
}

bool captureTiming(const UT_StringHolder &path, Timing &out) {
  if (!path.isstring())
    return false;
  const std::string key = path.toStdString();
  {
    std::lock_guard<std::mutex> lock(timingMutex);
    const auto found = timings.find(key);
    if (found != timings.end()) {
      out = found->second;
      return true;
    }
  }
  // Opening authenticates the file and starts two idle lanes; closing stops them. Both
  // are quick next to decoding anything, and this happens once per path.
  vgsb_player *player = vgsb_open(path.c_str(), 0, 2, 1);
  if (!player)
    return false;
  vgsb_info info{};
  vgsb_get_info(player, &info);
  vgsb_close(player);

  Timing timing;
  timing.duration = info.duration;
  timing.frameRate = info.frame_rate;
  timing.lastFrame = lastFrameOf(info.duration, info.frame_rate);
  {
    std::lock_guard<std::mutex> lock(timingMutex);
    timings[key] = timing;
  }
  out = timing;
  return true;
}

void forgetTiming(const UT_StringHolder &path) {
  std::lock_guard<std::mutex> lock(timingMutex);
  timings.erase(path.toStdString());
}

float splatFraction(float field) {
  constexpr float low = 0.01f;
  static const float cubeLow = std::cbrt(low);
  const float t = std::clamp((field - low) / (1.0f - low), 0.0f, 1.0f);
  const float mapped = cubeLow + t * (1.0f - cubeLow);
  return mapped * mapped * mapped;
}

// ---- preferences -------------------------------------------------------------------------

namespace {

long envInteger(const char *name, long fallback, long low, long high) {
  const char *value = std::getenv(name);
  if (!value || !*value)
    return fallback;
  char *end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value)
    return fallback;
  return std::clamp(parsed, low, high);
}

} // namespace

int envSlots() { return int(envInteger("VGS_SLOTS", 4, 2, 64)); }
int envThreads() { return int(envInteger("VGS_THREADS", 0, 0, 256)); }
double envPlaybackWaitMs() { return double(envInteger("VGS_PLAYBACK_WAIT_MS", 250, 0, 60000)); }

// ---- callbacks -----------------------------------------------------------------------------

namespace {

UT_StringHolder capturePath(OP_Node &node, fpreal time) {
  UT_String path;
  node.evalString(path, parm::File, 0, time);
  return UT_StringHolder(path);
}

void showStartFrame(OP_Node &node, fpreal time) {
  Timing timing;
  if (!captureTiming(capturePath(node, time), timing))
    return;
  const fpreal phase = node.evalFloat(parm::Phase, 0, time);
  node.setInt(parm::StartFrame, 0, time, exint(std::lround(phase * timing.lastFrame)));
}

int onReload(void *data, int, fpreal64 time, const PRM_Template *) {
  OP_Node &node = *static_cast<OP_Node *>(data);
  forgetTiming(capturePath(node, time));
  SOP_VGSCapture::reloadFrom(node);
  showStartFrame(node, time);
  return 1;
}

int onFitScene(void *data, int, fpreal64 time, const PRM_Template *) {
  OP_Node &node = *static_cast<OP_Node *>(data);
  UT_String message;
  if (!fitScene(node, time, message))
    node.opMessage(OP_ERR_ANYTHING, message.c_str());
  return 1;
}

} // namespace

void fitStartFrameRange(OP_Node &node, int lastFrame) {
  PRM_ParmList *list = node.getParmList();
  PRM_Parm *start = list ? list->getParmPtr(parm::StartFrame) : nullptr;
  if (!start)
    return;
  const fpreal top = fpreal(std::max(lastFrame, 1));
  const PRM_Range *current = start->getRangePtr();
  if (current && current->getUIMax() == top)
    return;
  const PRM_Range range(PRM_RANGE_RESTRICTED, 0, PRM_RANGE_UI, top);
  start->setRange(&range);
  // The parameter pane redraws the slider on this event.
  node.opChanged(OP_PARM_UICHANGED, reinterpret_cast<void *>(intptr_t(list->getParmIndex(parm::StartFrame))));
}

void syncStartFrame(OP_Node &node, OP_EventType reason, void *data) {
  if (reason != OP_PARM_CHANGED || OPgetDirector()->isLoading())
    return;
  // Setting one field from the other changes it too, and comes back here.
  static thread_local bool busy = false;
  if (busy)
    return;

  PRM_ParmList *list = node.getParmList();
  PRM_Parm *phase = list ? list->getParmPtr(parm::Phase) : nullptr;
  PRM_Parm *start = list ? list->getParmPtr(parm::StartFrame) : nullptr;
  if (!phase || !start)
    return;
  // Animated, or following another node's fields as the SOP inside the object does: the
  // fields are whatever their channels say, and the node they follow keeps them in step.
  if (phase->getChannelCount() > 0 || start->getChannelCount() > 0)
    return;

  const int changed = int(intptr_t(data));
  const bool fromStart = changed >= 0 && changed == list->getParmIndex(parm::StartFrame);
  const bool fromPhase = changed < 0 || changed == list->getParmIndex(parm::Phase) ||
                         changed == list->getParmIndex(parm::File);
  if (!fromStart && !fromPhase)
    return;

  const fpreal time = CHgetEvalTime();
  Timing timing;
  if (!captureTiming(capturePath(node, time), timing))
    return;

  busy = true;
  fitStartFrameRange(node, timing.lastFrame);
  fpreal value = node.evalFloat(parm::Phase, 0, time);
  if (fromStart && timing.lastFrame > 0) {
    const exint frame = node.evalInt(parm::StartFrame, 0, time);
    value = std::clamp(fpreal(frame) / fpreal(timing.lastFrame), fpreal(0), fpreal(1));
    node.setFloat(parm::Phase, 0, time, value);
  }
  // Also puts a frame past the end back on the last one.
  const exint frame = exint(std::lround(value * timing.lastFrame));
  if (node.evalInt(parm::StartFrame, 0, time) != frame)
    node.setInt(parm::StartFrame, 0, time, frame);
  busy = false;
}

bool fitScene(OP_Node &node, fpreal time, UT_String &message) {
  Timing timing;
  if (!captureTiming(capturePath(node, time), timing)) {
    message = "VGS: no capture to fit the scene to";
    return false;
  }
  CH_Manager *channels = OPgetDirector()->getChannelManager();
  if (timing.frameRate > 0)
    channels->setSamplesPerSec(timing.frameRate);
  fpreal speed = SYSabs(node.evalFloat(parm::Speed, 0, time));
  if (speed <= 0)
    speed = 1;
  const exint frames = exint(std::lround(timing.duration * channels->getSamplesPerSec() / speed));
  const fpreal start = channels->getGlobalStart();
  const fpreal end = channels->getTime(channels->getGlobalStartFrame() + std::max<exint>(frames, 0));
  channels->setGlobalTime(start, end);
  return true;
}

// ---- templates ---------------------------------------------------------------------------

namespace {

PRM_Name fileName(parm::File, "Capture");
PRM_Name reloadName(parm::Reload, "Reload");
PRM_Name phaseName(parm::Phase, "Phase");
PRM_Name startFrameName(parm::StartFrame, "Start Frame");
PRM_Name speedName(parm::Speed, "Speed");
PRM_Name loopModeName(parm::LoopMode, "Loop");
PRM_Name densityName(parm::Density, "Viewport Density");
PRM_Name upAxisName(parm::UpAxis, "Up Axis");
PRM_Name useShName(parm::UseSh, "Harmonics");
PRM_Name noShPlayingName(parm::NoShPlaying, "Off While Playing");
PRM_Name linearizeName(parm::Linearize, "Linearize Color");
PRM_Name castShadowsName(parm::CastShadows, "Cast Shadows in Karma");
PRM_Name fitSceneName(parm::FitScene, "Fit Scene");

PRM_Name loopModeItems[] = {
    PRM_Name("none", "No Loop"),
    PRM_Name("loop", "Loop"),
    PRM_Name("pingpong", "Ping-Pong"),
    PRM_Name(),
};
PRM_ChoiceList loopModeMenu(PRM_CHOICELIST_SINGLE, loopModeItems);

PRM_Name upAxisItems[] = {
    PRM_Name("y", "Y Up"),
    PRM_Name("z", "Z Up"),
    PRM_Name("negy", "Y Down"),
    PRM_Name(),
};
PRM_ChoiceList upAxisMenu(PRM_CHOICELIST_SINGLE, upAxisItems);

PRM_Default speedDefault(1.0);
PRM_Default loopModeDefault{fpreal(LoopMode::Loop)};
PRM_Default densityDefault(1.0);

PRM_Range phaseRange(PRM_RANGE_RESTRICTED, 0.0, PRM_RANGE_RESTRICTED, 1.0);
PRM_Range startFrameRange(PRM_RANGE_RESTRICTED, 0, PRM_RANGE_UI, 300);
PRM_Range speedRange(PRM_RANGE_UI, -4.0, PRM_RANGE_UI, 4.0);
PRM_Range densityRange(PRM_RANGE_RESTRICTED, 0.01, PRM_RANGE_RESTRICTED, 1.0);

PRM_SpareData filePattern(PRM_SpareArgs()
                          << PRM_SpareToken(PRM_SpareData::getFileChooserPatternToken(),
                                            "*.vgs,*.pgs")
                          << PRM_SpareToken(PRM_SpareData::getFileChooserModeToken(),
                                            PRM_SpareData::getFileChooserModeValRead()));

PRM_Conditional disableWithoutSh("{ usesh == 0 }");

} // namespace

PRM_Template captureTemplates[] = {
    PRM_Template(PRM_Type(PRM_FILE) | PRM_TYPE_JOIN_NEXT, 1, &fileName, nullptr, nullptr, nullptr, nullptr,
                 &filePattern, 1, "The .vgs or .pgs capture to play."),
    PRM_Template(PRM_CALLBACK, 1, &reloadName, nullptr, nullptr, nullptr, onReload, nullptr, 1,
                 "Reopen the capture, for when the file has changed on disk."),
    PRM_Template(PRM_FLT_J, 1, &phaseName, PRMzeroDefaults, nullptr, &phaseRange, nullptr,
                 nullptr, 1,
                 "Where in the capture the scene's first frame falls: 0 its first instant, 1 "
                 "its last. The same thing as Start Frame, as a fraction."),
    PRM_Template(PRM_INT_J, 1, &startFrameName, PRMzeroDefaults, nullptr, &startFrameRange,
                 nullptr, nullptr, 1,
                 "The capture's frame shown on the scene's first frame, counting from 0. The "
                 "same thing as Phase, in frames."),
    PRM_Template(PRM_FLT_J, 1, &speedName, &speedDefault, nullptr, &speedRange, nullptr, nullptr, 1,
                 "Playback speed. Negative plays it backwards from its phase."),
    PRM_Template(PRM_ORD, 1, &loopModeName, &loopModeDefault, &loopModeMenu, nullptr, nullptr,
                 nullptr, 1,
                 "What happens past either end: hold there, start over, or turn around."),
    PRM_Template(PRM_FLT_J, 1, &densityName, &densityDefault, nullptr, &densityRange, nullptr,
                 nullptr, 1,
                 "How many splats the viewport draws, for lighter playback. Renders always "
                 "draw them all."),
    PRM_Template(PRM_ORD, 1, &upAxisName, PRMzeroDefaults, &upAxisMenu, nullptr, nullptr, nullptr,
                 1,
                 "Which axis of the capture points up. Captures are written Y up, as Houdini "
                 "is."),
    PRM_Template(PRM_TOGGLE_J, 1, &useShName, PRMoneDefaults, nullptr, nullptr, nullptr, nullptr, 1,
                 "Decode the spherical harmonics: view-dependent colour."),
    PRM_Template(PRM_TOGGLE, 1, &noShPlayingName, PRMoneDefaults, nullptr, nullptr, nullptr,
                 nullptr, 1,
                 "Leave the harmonics out while the playbar plays, for faster playback. They "
                 "come back when it stops, and renders always have them.",
                 &disableWithoutSh),
    PRM_Template(PRM_TOGGLE, 1, &linearizeName, PRMoneDefaults, nullptr, nullptr, nullptr,
                 nullptr, 1,
                 "Convert the capture's sRGB colour to scene linear, as Bake GSplats does."),
    PRM_Template(PRM_TOGGLE, 1, &castShadowsName, PRMzeroDefaults, nullptr, nullptr, nullptr,
                 nullptr, 1,
                 "Let the splats cast shadows in Karma. Off, as Bake GSplats leaves it: "
                 "shadows from splats are slow and seldom wanted."),
    PRM_Template(PRM_CALLBACK, 1, &fitSceneName, nullptr, nullptr, nullptr, onFitScene, nullptr, 1,
                 "Set the scene's frame rate to the capture's, and its frame range to play it "
                 "once at this speed."),
    PRM_Template(),
};

const char *const linkedParms[] = {
    parm::File,  parm::Phase,  parm::StartFrame,  parm::Speed,     parm::LoopMode,    parm::Density,
    parm::UpAxis, parm::UseSh, parm::NoShPlaying, parm::Linearize, parm::CastShadows, nullptr,
};

} // namespace vgs
