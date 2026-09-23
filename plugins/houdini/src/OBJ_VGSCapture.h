// The VGS Capture object: a geometry object with the capture's controls on it, and a
// VGS Capture SOP inside that they drive.

#ifndef OBJ_VGSCAPTURE_H
#define OBJ_VGSCAPTURE_H

#include <OBJ/OBJ_Geometry.h>
#include <OP/OP_OperatorPair.h>

namespace vgs {

class OBJ_VGSCapture : public OBJ_Geometry {
public:
  static constexpr const char *TypeName = "vgs_capture";

  static OP_Node *create(OP_Network *net, const char *name, OP_Operator *op);
  static OP_TemplatePair *buildTemplatePair(OP_TemplatePair *previous);

  OBJ_VGSCapture(OP_Network *net, const char *name, OP_Operator *op);

  void opChanged(OP_EventType reason, void *data = nullptr) override;

  /** Creates the SOP and ties its parameters to this object's. */
  bool runCreateScript() override;

protected:
  // The capture's controls come before the geometry object's own parameters, which moves
  // those; the base class caches their positions in a table shared by every geometry
  // object, so this class needs a table of its own.
  int *getIndirect() const override { return indirect; }

private:
  static int *indirect;
};

} // namespace vgs

#endif
