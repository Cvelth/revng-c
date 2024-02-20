//
// Copyright (c) rev.ng Labs Srl. See LICENSE.md for details.
//

#include "revng-c/Backend/DecompiledCCodeIndentation.h"
#include "revng-c/HeadersGeneration/ModelToHeader.h"
#include "revng-c/HeadersGeneration/ModelTypeDefinition.h"
#include "revng-c/TypeNames/ModelToPTMLTypeHelpers.h"

static Logger<> Log{ "model-type-definition" };

std::string dumpModelTypeDefinition(const model::Binary &Model,
                                    model::TypeDefinition::Key Key) {
  std::string Result;

  llvm::raw_string_ostream Out(Result);
  ptml::PTMLIndentedOstream PTMLOut(Out, DecompiledCCodeIndentation, true);
  ptml::PTMLCBuilder B(true);

  std::map<model::QualifiedType, std::string> AdditionalNames;
  const std::set<const model::TypeDefinition *> TypesToInline;

  printDefinition(Log,
                  *Model.TypeDefinitions().at(Key),
                  PTMLOut,
                  B,
                  Model,
                  AdditionalNames,
                  TypesToInline,
                  {},
                  {},
                  true);

  return Result;
}
