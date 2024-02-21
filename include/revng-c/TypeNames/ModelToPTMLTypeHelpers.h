#pragma once

//
// Copyright rev.ng Labs Srl. See LICENSE.md for details.
//

#include <unordered_map>

#include "revng/ADT/GenericGraph.h"
#include "revng/Model/Binary.h"
#include "revng/Model/TypeDefinition.h"

#include "revng-c/Pipes/Ranks.h"
#include "revng-c/Support/PTMLC.h"

// This is used to encapsulate all the things necessary for type inlining.
struct TypeInlineHelper {
public:
  struct NodeData {
    model::TypeDefinition *T;
  };

  using Node = BidirectionalNode<NodeData>;
  using Graph = GenericGraph<Node>;

  // This Graph is being used for the purpose of inlining types only.
  struct GraphInfo {
    // The bi-directional graph used to analyze type connections in both
    // directions.
    Graph TypeGraph;
    // This is being used for speeding up the counting of the type references.
    std::map<const model::TypeDefinition *, Node *> TypeToNode;
  };

private:
  GraphInfo TypeGraph;
  std::unordered_map<const model::TypeDefinition *, unsigned> TypeToNumOfRefs;
  std::set<const model::TypeDefinition *> TypesToInline;

public:
  TypeInlineHelper(const model::Binary &Model);

public:
  const GraphInfo &getTypeGraph() const;
  const std::set<const model::TypeDefinition *> &getTypesToInline() const;
  const std::unordered_map<const model::TypeDefinition *, unsigned> &
  getTypeToNumOfRefs() const;

public:
  // Collect stack frame types per model::Function.
  std::unordered_map<const model::Function *,
                     std::set<const model::TypeDefinition *>>
  findStackTypesPerFunction(const model::Binary &Model) const;

  // Collect all stack frame types, since we want to dump them inline in the
  // function body.
  std::set<const model::TypeDefinition *>
  collectStackTypes(const model::Binary &Model) const;

  // Find all nested types of the `RootType` that should be inlined into it.
  std::set<const model::TypeDefinition *>
  getTypesToInlineInTypeTy(const model::Binary &Model,
                           const model::TypeDefinition *RootType) const;

private:
  std::set<const model::TypeDefinition *>
  findTypesToInline(const model::Binary &Model, const GraphInfo &TypeGraph);

  GraphInfo buildTypeGraph(const model::Binary &Model);

  std::unordered_map<const model::TypeDefinition *, unsigned>
  calculateNumOfOccurences(const model::Binary &Model);

  bool isReachableFromRootType(const model::TypeDefinition *Type,
                               const model::TypeDefinition *RootType,
                               const GraphInfo &TypeGraph);

  // Helper function used for finding all nested (into `RootType`) inlinable
  // types.
  std::set<const model::TypeDefinition *>
  getNestedTypesToInline(const model::TypeDefinition *RootType,
                         const model::UpcastableTypeDefinition &NestedTy) const;
};

extern bool declarationIsDefinition(const model::TypeDefinition *T);

extern void printForwardDeclaration(const model::TypeDefinition &T,
                                    ptml::PTMLIndentedOstream &Header,
                                    ptml::PTMLCBuilder &B);

// Print a declaration for a Type. The last three arguments (`TypesToInline`
// `NameOfInlineInstance` and `Qualifiers`) are being used for printing types
// inline. If the `NameOfInlineInstance` and `Qualifiers` are set, it means that
// we should print the type inline. Types that can be inline are structs, unions
// and enums.
extern void
printDeclaration(Logger<> &Log,
                 const model::TypeDefinition &T,
                 ptml::PTMLIndentedOstream &Header,
                 ptml::PTMLCBuilder &B,
                 const model::Binary &Model,
                 std::map<model::QualifiedType, std::string> &AdditionalNames,
                 const std::set<const model::TypeDefinition *> &TypesToInline,
                 llvm::StringRef NameOfInlineInstance = llvm::StringRef(),
                 const std::vector<model::Qualifier> &Qualifiers = {},
                 bool ForEditing = false);

// Print a definition for a Type. The last three arguments (`TypesToInline`
// `NameOfInlineInstance` and `Qualifiers`) are being used for printing types
// inline. If the `NameOfInlineInstance` and `Qualifiers` are set, it means that
// we should print the type inline. Types that can be inline are structs, unions
// and enums.
extern void
printDefinition(Logger<> &Log,
                const model::TypeDefinition &T,
                ptml::PTMLIndentedOstream &Header,
                ptml::PTMLCBuilder &B,
                const model::Binary &Model,
                std::map<model::QualifiedType, std::string> &AdditionalNames,
                const std::set<const model::TypeDefinition *> &TypesToInline,
                llvm::StringRef NameOfInlineInstance = llvm::StringRef(),
                const std::vector<model::Qualifier> &Qualifiers = {},
                bool ForEditing = false);

// Print a definition for a struct. The last three arguments (`TypesToInline`
// `NameOfInlineInstance` and `Qualifiers`) are being used for printing types
// inline. If the `NameOfInlineInstance` and `Qualifiers` are set, it means that
// we should print the type inline.
extern void
printDefinition(Logger<> &Log,
                const model::StructDefinition &S,
                ptml::PTMLIndentedOstream &Header,
                ptml::PTMLCBuilder &B,
                const model::Binary &Model,
                std::map<model::QualifiedType, std::string> &AdditionalNames,
                const std::set<const model::TypeDefinition *> &TypesToInline,
                llvm::StringRef NameOfInlineInstance = llvm::StringRef(),
                const std::vector<model::Qualifier> &Qualifiers = {});

// Checks if a Type is valid candidate to inline. Types that can be inline are
// structs, unions and enums.
extern bool isCandidateForInline(const model::TypeDefinition *T);

// Print a typedef declaration.
extern void printDeclaration(const model::TypedefDefinition &TD,
                             ptml::PTMLIndentedOstream &Header,
                             ptml::PTMLCBuilder &B);
