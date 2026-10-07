// The VGS Capture SOP: a capture's splats at the current time, as Houdini GSplats - once,
// where it is, or a copy on every point of a scatter, each at its own time.

#ifndef SOP_VGSCAPTURE_H
#define SOP_VGSCAPTURE_H

#include "vgsblender.h"

#include <SOP/SOP_Node.h>
#include <UT/UT_String.h>

#include <map>
#include <string>

namespace vgs {

class SOP_VGSCapture : public SOP_Node {
public:
  static constexpr const char *TypeName = "vgs_capture";

  static OP_Node *create(OP_Network *net, const char *name, OP_Operator *op);

  SOP_VGSCapture(OP_Network *net, const char *name, OP_Operator *op);
  ~SOP_VGSCapture() override;

  void getDescriptiveParmName(UT_String &name) const override;
  void getNodeSpecificInfoText(OP_Context &context, OP_NodeInfoParms &parms) override;

  void opChanged(OP_EventType reason, void *data = nullptr) override;

  /** Closes every capture so that the next cook opens them again. */
  void reload();

  /**
   * Reloads the capture behind `node`: the node itself when it is this SOP, or every one
   * of these SOPs inside it when it is the VGS Capture object.
   */
  static void reloadFrom(OP_Node &node);

  /** Watches for ROPs, so that renders draw every splat. Once, as the plugin loads. */
  static void installHooks();

  /** One open capture: its player and what it says about itself. */
  struct Opened {
    vgsb_player *player = nullptr;
    vgsb_info info{};
    int slots = 0;
  };

protected:
  OP_ERROR cookMySop(OP_Context &context) override;

private:
  /**
   * The capture at `path`, opened with room for at least `slots` decoded frames, or null
   * with the reason in `failures`. A capture that failed is not retried until Reload.
   */
  Opened *open(const std::string &path, bool includeSh, int slots);
  void closeAll();

  // Every capture this node plays, by path: one for a single capture, one per file for a
  // scatter that mixes them through vgs_path.
  std::map<std::string, Opened> players;
  std::map<std::string, std::string> failures;

  // What the last cook showed, for the node's info.
  std::string mainPath;
  double shownSeconds = -1;
  exint shownSplats = 0;
  exint shownCopies = 0;
  exint shownTimelines = 0;
};

} // namespace vgs

#endif
