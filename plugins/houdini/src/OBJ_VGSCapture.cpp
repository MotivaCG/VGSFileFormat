#include "OBJ_VGSCapture.h"

#include "SOP_VGSCapture.h"
#include "VGS_Parameters.h"

#include <CH/CH_ExprLanguage.h>
#include <OP/OP_Operator.h>
#include <PRM/PRM_Parm.h>
#include <UT/UT_WorkBuffer.h>

namespace vgs {

int *OBJ_VGSCapture::indirect = nullptr;

OP_Node *OBJ_VGSCapture::create(OP_Network *net, const char *name, OP_Operator *op) {
  return new OBJ_VGSCapture(net, name, op);
}

OP_TemplatePair *OBJ_VGSCapture::buildTemplatePair(OP_TemplatePair *previous) {
  // The capture first, then everything a geometry object has.
  OP_TemplatePair *geometry =
      new OP_TemplatePair(OBJ_Geometry::getTemplateList(OBJ_PARMS_PLAIN), previous);
  return new OP_TemplatePair(captureTemplates, geometry);
}

OBJ_VGSCapture::OBJ_VGSCapture(OP_Network *net, const char *name, OP_Operator *op)
    : OBJ_Geometry(net, name, op) {
  if (!indirect)
    indirect = allocIndirect(I_N_GEO_INDICES);
}

void OBJ_VGSCapture::opChanged(OP_EventType reason, void *data) {
  OBJ_Geometry::opChanged(reason, data);
  syncStartFrame(*this, reason, data);
}

bool OBJ_VGSCapture::runCreateScript() {
  const bool ran = OBJ_Geometry::runCreateScript();

  // Only when the object is new: a loaded one brings its SOP along from the file.
  OP_Node *sop = createNode(SOP_VGSCapture::TypeName, "capture");
  if (!sop)
    return ran;
  sop->setDisplay(true);
  sop->setRender(true);

  // Each parameter of the SOP follows the object's of the same name, so the controls live
  // on the object and the SOP still cooks from its own parameters.
  for (const char *const *name = linkedParms; *name; ++name) {
    PRM_Parm *parm = sop->getParmList()->getParmPtr(*name);
    if (!parm)
      continue;
    UT_WorkBuffer expression;
    if (parm->getType().isStringType())
      expression.sprintf("chs(\"../%s\")", *name);
    else
      expression.sprintf("ch(\"../%s\")", *name);
    parm->setExpression(0, expression.buffer(), CH_OLD_EXPR_LANGUAGE, 0);
  }
  return ran;
}

} // namespace vgs
