// The capture's controls, shared by the VGS Capture object and the SOP inside it.
//
// Both node types carry the same parameters under the same names. The SOP does the work;
// the object is what File > Import creates, and its parameters drive the SOP's through
// channel references, so the controls sit on the object the way they sit on the object's
// data in Blender. A SOP placed on its own - in a SOP network of any object, or in a SOP
// Create LOP - is driven by its own parameters.

#ifndef VGS_PARAMETERS_H
#define VGS_PARAMETERS_H

#include "vgsblender.h"

#include <OP/OP_Value.h>
#include <PRM/PRM_Template.h>
#include <UT/UT_String.h>

class OP_Node;

namespace vgs {

// Parameter names, one place for the templates, the SOP and the object.
namespace parm {
constexpr const char *File = "file";
constexpr const char *Reload = "reload";
constexpr const char *Phase = "phase";
constexpr const char *StartFrame = "startframe";
constexpr const char *Speed = "speed";
constexpr const char *LoopMode = "loopmode";
constexpr const char *Density = "density";
constexpr const char *UpAxis = "upaxis";
constexpr const char *UseSh = "usesh";
constexpr const char *NoShPlaying = "noshplaying";
constexpr const char *Linearize = "linearize";
constexpr const char *CastShadows = "castshadows";
constexpr const char *FitScene = "fitscene";
} // namespace parm

enum class LoopMode { None = 0, Loop = 1, PingPong = 2 };
enum class UpAxis { Y = 0, Z = 1, NegY = 2 };

/** The capture's controls, terminated by an empty template. */
extern PRM_Template captureTemplates[];

/** The parameters the object hands down to its SOP by channel reference: all but buttons. */
extern const char *const linkedParms[];

/**
 * What a capture says about its timing, cached by path so that the callbacks behind the
 * Phase and Start Frame fields, which fire on every step of a drag, do not reopen it.
 */
struct Timing {
  double duration = 0;
  double frameRate = 0;
  int lastFrame = 0;
};

/** The timing of the capture at `path`, opening it once. False when it cannot be read. */
bool captureTiming(const UT_StringHolder &path, Timing &out);

/** Forgets what captureTiming read of `path`, for a reload. */
void forgetTiming(const UT_StringHolder &path);

/** The index of the capture's last frame: 0 is the first. */
int lastFrameOf(double duration, double frameRate);

/**
 * The viewport density field runs from 0.01 to 1 and the fraction of splats drawn too,
 * but not in step: the field is mapped onto [cbrt(0.01), 1] and cubed. Thinning only
 * pays off from about a quarter of the splats down, and a linear field put all of that in
 * its last stretch; cubed, the lower half of the field covers it.
 */
float splatFraction(float field);

/**
 * The frame rate and frame range that play the capture on `node` once from the scene's
 * start, at its speed. Returns false, with a message, when there is no capture to fit.
 */
bool fitScene(OP_Node &node, fpreal time, UT_String &message);

/**
 * Keeps Start Frame showing Phase in the capture's frames, whichever of the two changed,
 * or the capture. Only the phase decides anything; Start Frame is the same value in
 * frames, so setting it sets the phase. Called from the nodes' opChanged, so it holds
 * however a parameter was set: by hand, from Python, or by loading a preset.
 */
void syncStartFrame(OP_Node &node, OP_EventType reason, void *data);

/**
 * Stretches Start Frame's slider over the capture on `node`, 0 to its last frame. The
 * template's range cannot know it, so each node gets its own once the capture is read.
 */
void fitStartFrameRange(OP_Node &node, int lastFrame);

/** Environment variables standing in for the Blender add-on's preferences. */
int envSlots();
int envThreads();
double envPlaybackWaitMs();

} // namespace vgs

#endif
