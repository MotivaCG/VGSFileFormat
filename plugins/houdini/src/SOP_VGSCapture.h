// The VGS Capture SOP: a capture's splats at the current time, as Houdini GSplats.

#ifndef SOP_VGSCAPTURE_H
#define SOP_VGSCAPTURE_H

#include "vgsblender.h"

#include <SOP/SOP_Node.h>
#include <UT/UT_String.h>

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

  /** Closes the capture so that the next cook opens it again. */
  void reload();

  /**
   * Reloads the capture behind `node`: the node itself when it is this SOP, or every one
   * of these SOPs inside it when it is the VGS Capture object.
   */
  static void reloadFrom(OP_Node &node);

  /** Watches for ROPs, so that renders draw every splat. Once, as the plugin loads. */
  static void installHooks();

protected:
  OP_ERROR cookMySop(OP_Context &context) override;

private:
  bool ensurePlayer(const UT_StringHolder &path, bool includeSh);
  void closePlayer();

  vgsb_player *player = nullptr;
  vgsb_info info{};
  UT_StringHolder openPath;
  /** Why the capture at `failedPath` did not open; not retried until the path changes. */
  UT_StringHolder failedPath;
  std::string failure;
  /** The capture time of the frame last written, for the node's info. */
  double shownSeconds = -1;
  exint shownSplats = 0;
};

} // namespace vgs

#endif
