// Registers the VGS Capture object and SOP with Houdini.

#include "OBJ_VGSCapture.h"
#include "SOP_VGSCapture.h"
#include "VGS_Parameters.h"

#include <OP/OP_Operator.h>
#include <OP/OP_OperatorTable.h>
#include <UT/UT_DSOVersion.h>

using namespace vgs;

namespace {
// Named, not literal: a literal 0 would also read as a null child table name.
constexpr unsigned noInputs = 0;
constexpr unsigned oneInput = 1;
} // namespace

void newSopOperator(OP_OperatorTable *table) {
  SOP_VGSCapture::installHooks();
  auto *op = new OP_Operator(SOP_VGSCapture::TypeName, "VGS Capture", SOP_VGSCapture::create,
                             captureTemplates, noInputs, noInputs);
  op->setOpTabSubMenuPath("Import");
  table->addOperator(op);
}

void newObjectOperator(OP_OperatorTable *table) {
  auto *op = new OP_Operator(OBJ_VGSCapture::TypeName, "VGS Capture", OBJ_VGSCapture::create,
                             OBJ_VGSCapture::buildTemplatePair(nullptr),
                             OBJ_VGSCapture::theChildTableName, noInputs, oneInput);
  op->setOpTabSubMenuPath("Import");
  table->addOperator(op);
}
