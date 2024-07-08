//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnostic.h"

#include "revng/Model/Processing.h"
#include "revng/Support/Debug.h"

#include "revng-c/Pipes/Ranks.h"
#include "revng-c/Support/ModelHelpers.h"
#include "revng-c/Support/PTMLC.h"
#include "revng-c/TypeNames/ModelTypeNames.h"

#include "HeaderToModel.h"

using namespace model;
using namespace revng;

static Logger<> Log("header-to-model");
static constexpr std::string_view InputCFile = "revng-input.c";
static constexpr std::string_view PrimitiveTypeHeader = "primitive-types.h";
static constexpr llvm::StringRef RawABIPrefix = "raw_";

static constexpr const char *ABIAnnotatePrefix = "abi:";
static constexpr size_t
  ABIAnnotatePrefixLength = std::char_traits<char>::length(ABIAnnotatePrefix);

static constexpr const char *RegAnnotatePrefix = "reg:";
static constexpr size_t
  RegAnnotatePrefixLength = std::char_traits<char>::length(RegAnnotatePrefix);

static constexpr const char *EnumAnnotatePrefix = "enum_underlying_type:";
static constexpr size_t
  EnumAnnotatePrefixLength = std::char_traits<char>::length(EnumAnnotatePrefix);

template<typename T>
concept HasCustomName = requires(const T &Element) {
  { Element.CustomName() } -> std::same_as<const model::Identifier &>;
  { Element.name() } -> std::same_as<model::Identifier>;
};

template<HasCustomName T>
static void setCustomName(T &Element, llvm::StringRef NewName) {
  if (Element.name() != NewName)
    Element.CustomName() = NewName;
}

namespace clang {
namespace tooling {

class HeaderToModel : public ASTConsumer {
public:
  HeaderToModel(TupleTree<model::Binary> &Model,
                std::optional<model::TypeDefinition::Key> Type,
                MetaAddress FunctionEntry,
                std::optional<ParseCCodeError> &Error,
                enum ImportFromCOption AnalysisOption) :
    Model(Model),
    Type(Type),
    FunctionEntry(FunctionEntry),
    Error(Error),
    AnalysisOption(AnalysisOption) {
    // Either one of these two should be null, since the editing features are
    // exclusive.
    revng_assert(not Type or not FunctionEntry.isValid());
  }

  virtual void HandleTranslationUnit(ASTContext &Context) override;

private:
  TupleTree<model::Binary> &Model;
  std::optional<model::TypeDefinition::Key> Type;
  MetaAddress FunctionEntry;
  std::optional<ParseCCodeError> &Error;
  enum ImportFromCOption AnalysisOption;
};

class DeclVisitor : public clang::RecursiveASTVisitor<DeclVisitor> {
private:
  TupleTree<model::Binary> &Model;
  ASTContext &Context;
  std::optional<model::TypeDefinition::Key> Type;
  MetaAddress FunctionEntry;
  std::optional<revng::ParseCCodeError> &Error;
  enum ImportFromCOption AnalysisOption;

  // These are used for reporting source location of an error, if any.
  unsigned CurrentLineNumber = 0;
  unsigned CurrentColumnNumber = 0;

  // Used to remember return values locations when parsing struct representing
  // the multi-reg return value. Represents register ID and model::Type.
  using RawLocation = std::pair<model::Register::Values, model::UpcastableType>;
  std::optional<llvm::SmallVector<RawLocation, 4>> MultiRegisterReturnValue;

public:
  explicit DeclVisitor(TupleTree<model::Binary> &Model,
                       ASTContext &Context,
                       std::optional<model::TypeDefinition::Key> Type,
                       MetaAddress FunctionEntry,
                       std::optional<ParseCCodeError> &Error,
                       enum ImportFromCOption AnalysisOption);

  void run(clang::TranslationUnitDecl *TUD);
  bool TraverseDecl(clang::Decl *D);

  bool VisitFunctionDecl(const clang::FunctionDecl *FD);
  bool VisitRecordDecl(const clang::RecordDecl *RD);
  bool VisitEnumDecl(const EnumDecl *D);
  bool VisitTypedefDecl(const TypedefDecl *D);
  bool VisitFunctionPrototype(const FunctionProtoType *FP,
                              std::optional<llvm::StringRef> TheABI);

private:
  // This checks that the declaration is the one user provided as input.
  bool comesFromInternalFile(const clang::Decl *D);

  // This checks that the declaration comes from primitive-types.header
  // file.
  bool comesFromPrimitiveTypesHeader(const clang::RecordDecl *RD);

  // Set up line and column for the declaratrion.
  void setupLineAndColumn(const clang::Decl *D);

  // Handle clang's Struct type.
  bool handleStructType(const clang::RecordDecl *RD);
  // Handle clang's Union type.
  bool handleUnionType(const clang::RecordDecl *RD);

  // Convert clang::type to model::type.
  model::UpcastableType makePrimitive(const BuiltinType *UnderlyingBuiltin,
                                      QualType Type);

  // Get model type for clang::RecordType (Struct/Unoion).
  model::UpcastableType
  getTypeForRecordType(const clang::RecordType *RecordType,
                       const QualType &ClangType);

  // Get model type for clang::EnumType.
  model::UpcastableType getTypeForEnumType(const clang::EnumType *EnumType);

  template<NonBaseDerived<model::TypeDefinition> T>
  model::UpcastableType makeTypeByNameOrID(llvm::StringRef Name);

  RecursiveCoroutine<model::UpcastableType>
  getModelTypeForClangType(const QualType &QT);

  model::UpcastableType getEnumUnderlyingType(const std::string &TypeName);
};

DeclVisitor::DeclVisitor(TupleTree<model::Binary> &Model,
                         ASTContext &Context,
                         std::optional<model::TypeDefinition::Key> Type,
                         MetaAddress FunctionEntry,
                         std::optional<ParseCCodeError> &Error,
                         enum ImportFromCOption AnalysisOption) :
  Model(Model),
  Context(Context),
  Type(Type),
  FunctionEntry(FunctionEntry),
  Error(Error),
  AnalysisOption(AnalysisOption) {
}

// Parse ABI from the annotate attribute content.
static std::optional<std::string> getABI(llvm::StringRef ABIAnnotate) {
  if (not ABIAnnotate.startswith(ABIAnnotatePrefix))
    return std::nullopt;
  return std::string(ABIAnnotate.substr(ABIAnnotatePrefixLength));
}

static model::Architecture::Values getRawABIArchitecture(llvm::StringRef ABI) {
  revng_assert(ABI.starts_with(RawABIPrefix));
  return model::Architecture::fromName(ABI.substr(RawABIPrefix.size()));
}

// Parse location of parameter (`reg:` or `stack`).
static std::optional<std::string> getLoc(llvm::StringRef Annotate) {
  if (Annotate == "stack")
    return Annotate.str();

  if (not Annotate.startswith(RegAnnotatePrefix))
    return std::nullopt;
  return std::string(Annotate.substr(RegAnnotatePrefixLength));
}

// Parse enum's underlying type from the annotate attribute content.
static std::optional<std::string>
parseEnumUnderlyingType(llvm::StringRef Annotate) {
  if (not Annotate.startswith(EnumAnnotatePrefix))
    return std::nullopt;
  return std::string(Annotate.substr(EnumAnnotatePrefixLength));
}

model::UpcastableType
DeclVisitor::getEnumUnderlyingType(const std::string &TypeName) {
  auto R = model::PrimitiveType::fromCName(TypeName);
  if (R.empty())
    revng_log(Log, "An enum with a non-primitive underlying type.");

  return R;
}

model::UpcastableType
DeclVisitor::makePrimitive(const BuiltinType *UnderlyingBuiltin,
                           QualType Type) {
  revng_assert(UnderlyingBuiltin);

  auto AsElaboratedType = Type->getAs<ElaboratedType>();
  if (not AsElaboratedType) {
    PrintingPolicy Policy(Context.getLangOpts());
    std::string ErrorMessage = "revng: Builtin type `"
                               + UnderlyingBuiltin->getName(Policy).str()
                               + "` not allowed, please use a revng "
                                 "model::PrimitiveType instead";
    Error = { ErrorMessage, CurrentLineNumber, CurrentColumnNumber };

    return model::UpcastableType::makeEmpty();
  }

  while (auto Typedef = AsElaboratedType->getAs<TypedefType>()) {
    auto TheUnderlyingType = Typedef->getDecl()->getUnderlyingType();
    if (not TheUnderlyingType->getAs<ElaboratedType>())
      break;
    AsElaboratedType = TheUnderlyingType->getAs<ElaboratedType>();
  }

  std::string TypeName = AsElaboratedType->getNamedType().getAsString();
  if (model::PrimitiveType::fromCName(TypeName).empty()) {
    std::string ErrorMessage = "revng: `"
                               + AsElaboratedType->getNamedType().getAsString()
                               + "`, please use a revng model::PrimitiveType "
                                 "instead";
    Error = { ErrorMessage, CurrentLineNumber, CurrentColumnNumber };

    return model::UpcastableType::makeEmpty();
  }

  switch (UnderlyingBuiltin->getKind()) {
  case BuiltinType::UInt128:
    return model::PrimitiveType::makeUnsigned(16);

  case BuiltinType::Int128:
    return model::PrimitiveType::makeSigned(16);

  case BuiltinType::ULongLong:
  case BuiltinType::ULong:
    return model::PrimitiveType::makeUnsigned(8);

  case BuiltinType::LongLong:
  case BuiltinType::Long:
    return model::PrimitiveType::makeSigned(8);

  case BuiltinType::WChar_U:
  case BuiltinType::UInt:
    return model::PrimitiveType::makeUnsigned(4);

  case BuiltinType::WChar_S:
  case BuiltinType::Char32:
  case BuiltinType::Int:
    return model::PrimitiveType::makeSigned(4);

  case BuiltinType::UShort:
    return model::PrimitiveType::makeUnsigned(2);

  case BuiltinType::Char16:
  case BuiltinType::Short:
    return model::PrimitiveType::makeSigned(2);

  case BuiltinType::Char_S:
  case BuiltinType::SChar:
  case BuiltinType::Char8:
  case BuiltinType::Bool:
    return model::PrimitiveType::makeUnsigned(1);

  case BuiltinType::Char_U:
  case BuiltinType::UChar:
    return model::PrimitiveType::makeSigned(1);

  case BuiltinType::Void:
    return model::PrimitiveType::makeVoid();

  case BuiltinType::Float16:
    return model::PrimitiveType::makeFloat(2);

  case BuiltinType::Float:
    return model::PrimitiveType::makeFloat(4);

  case BuiltinType::Double:
    return model::PrimitiveType::makeFloat(8);

  case BuiltinType::Float128:
  case BuiltinType::LongDouble:
    return model::PrimitiveType::makeFloat(16);

  default:
    revng_log(Log, "Unable to handle a primitive type");
  }

  return model::UpcastableType::makeEmpty();
}

template<NonBaseDerived<model::TypeDefinition> T>
model::UpcastableType DeclVisitor::makeTypeByNameOrID(llvm::StringRef Name) {
  // Try to find by name first.
  for (auto &Type : Model->TypeDefinitions())
    if (llvm::isa<T>(Type.get()))
      if (Type->CustomName() == Name)
        return Model->makeType(Type->key());

  // Getting here means we didn't manage to find it,
  // let's try parsing the name.
  size_t LocationOfID = Name.rfind("_");

  if (LocationOfID != std::string::npos) {
    std::string ID = std::string(Name.substr(LocationOfID + 1));
    llvm::Expected<uint64_t> DeserializedID = deserialize<uint64_t>(ID);
    if (DeserializedID) {
      return Model->makeType(model::TypeDefinition::Key{ *DeserializedID,
                                                         T::AssociatedKind });
    } else {
      llvm::logAllUnhandledErrors(DeserializedID.takeError(),
                                  *Log.getAsLLVMStream());
    }
  }

  return model::UpcastableType::makeEmpty();
}

model::UpcastableType
DeclVisitor::getTypeForRecordType(const clang::RecordType *RecordType,
                                  const QualType &ClangType) {
  revng_assert(RecordType);

  // Check if it is a primitive type described with a struct.
  if (comesFromPrimitiveTypesHeader(RecordType->getDecl())) {
    const TypedefType *AsTypedef = ClangType->getAs<TypedefType>();
    if (not AsTypedef) {
      revng_log(Log,
                "There should be a typedef for struct that defines the "
                "primitive type");
      return model::UpcastableType::makeEmpty();
    }
    auto TypeName = AsTypedef->getDecl()->getName();
    auto R = model::PrimitiveType::fromCName(TypeName);
    revng_assert(R);
    return R;
  }

  auto Name = RecordType->getDecl()->getName();
  if (Name.empty()) {
    revng_log(Log, "Unable to find record type without name");
    return model::UpcastableType::makeEmpty();
  }

  if (RecordType->isStructureType()) {
    if (auto Struct = makeTypeByNameOrID<model::StructDefinition>(Name))
      return Struct;

  } else if (RecordType->isUnionType()) {
    if (auto Union = makeTypeByNameOrID<model::UnionDefinition>(Name))
      return Union;
  }

  revng_log(Log, "Unable to find record type " << Name);
  return model::UpcastableType::makeEmpty();
}

model::UpcastableType
DeclVisitor::getTypeForEnumType(const clang::EnumType *EnumType) {
  revng_assert(EnumType);
  revng_assert(AnalysisOption != ImportFromCOption::EditFunctionPrototype);

  auto EnumName = EnumType->getDecl()->getName();
  if (EnumName.empty()) {
    revng_log(Log, "Unable to find enum type without name");
    return model::UpcastableType::makeEmpty();
  }

  if (auto Enum = makeTypeByNameOrID<model::EnumDefinition>(EnumName))
    return Enum;

  revng_log(Log, "Unable to find enum type " << EnumName);
  return model::UpcastableType::makeEmpty();
}

bool DeclVisitor::comesFromInternalFile(const clang::Decl *D) {
  SourceManager &SM = Context.getSourceManager();
  PresumedLoc Loc = SM.getPresumedLoc(D->getLocation());
  if (!Loc.isValid()) {
    revng_log(Log, "Invalid source location found");
    return false;
  }

  StringRef TheFileName(Loc.getFilename());
  // Process the new type only.
  if (TheFileName.contains(InputCFile))
    return true;

  return false;
}

bool DeclVisitor::comesFromPrimitiveTypesHeader(const clang::RecordDecl *RD) {
  SourceManager &SM = Context.getSourceManager();
  PresumedLoc Loc = SM.getPresumedLoc(RD->getLocation());
  if (!Loc.isValid()) {
    revng_log(Log, "Invalid source location found");
    return false;
  }

  StringRef TheFileName(Loc.getFilename());
  if (TheFileName.contains(PrimitiveTypeHeader))
    return true;

  return false;
}

void DeclVisitor::setupLineAndColumn(const clang::Decl *D) {
  SourceManager &SM = Context.getSourceManager();
  PresumedLoc Loc = SM.getPresumedLoc(D->getLocation());
  if (!Loc.isValid()) {
    revng_log(Log, "Invalid source location found");
    return;
  }

  CurrentLineNumber = Loc.getLine();
  CurrentColumnNumber = Loc.getColumn();
}

RecursiveCoroutine<model::UpcastableType>
DeclVisitor::getModelTypeForClangType(const QualType &QT) {
  model::UpcastableType R;

  if (const BuiltinType *AsBuiltinType = QT->getAs<BuiltinType>()) {
    R = makePrimitive(AsBuiltinType, QT);

  } else if (const PointerType *Pointer = QT->getAs<PointerType>()) {
    QualType Pointee = Pointer->getPointeeType();
    R = model::PointerType::make(rc_recur getModelTypeForClangType(Pointee),
                                 Model->Architecture());

  } else if (QT->isArrayType()) {
    if (const auto *CAT = dyn_cast<ConstantArrayType>(QT)) {
      QualType ElementType = Context.getBaseElementType(QT);
      uint64_t NumberOfElements = CAT->getSize().getZExtValue();
      R = model::ArrayType::make(rc_recur getModelTypeForClangType(ElementType),
                                 NumberOfElements);
    } else {
      // Here we can face `clang::VariableArrayType` and
      // `clang::IncompleteArrayType`.
      revng_log(Log, "Unsupported type used as an array");
    }
  } else if (const RecordType *AsRecordType = QT->getAs<RecordType>()) {
    R = getTypeForRecordType(AsRecordType, QT);

  } else if (const EnumType *AsEnum = QT->getAs<EnumType>()) {
    R = getTypeForEnumType(AsEnum);

  } else if (const auto *AsFn = QT->getAs<FunctionProtoType>()) {
    if (const TypedefType *AsTypedef = QT->getAs<TypedefType>()) {
      auto Name = AsTypedef->getDecl()->getName();
      if (auto CFT = makeTypeByNameOrID<model::CABIFunctionDefinition>(Name))
        R = std::move(CFT);
      else if (auto Rw = makeTypeByNameOrID<model::RawFunctionDefinition>(Name))
        R = std::move(Rw);
      else
        revng_log(Log, "Couldn't find function type in the model");
    } else {
      revng_log(Log, "There should be a typedef for function type");
    }

  } else {
    revng_log(Log, "Unsupported QualType");
  }

  if (not R.empty() and QT.isConstQualified())
    R->IsConst() = true;

  rc_return R;
}

bool DeclVisitor::VisitFunctionDecl(const clang::FunctionDecl *FD) {
  if (not comesFromInternalFile(FD))
    return true;

  revng_assert(FD);
  revng_assert(AnalysisOption == ImportFromCOption::EditFunctionPrototype);
  std::optional<std::string> MaybeABI;
  std::vector<AnnotateAttr *> AnnotateAttrs;

  {
    // May be multiple Annotate attributes. One for ABI, one for return value.
    std::for_each(begin(FD->getAttrs()),
                  end(FD->getAttrs()),
                  [&](Attr *Attribute) {
                    if (auto *Annotation = dyn_cast<AnnotateAttr>(Attribute))
                      AnnotateAttrs.push_back(Annotation);
                  });

    for (auto *Annotate : AnnotateAttrs) {
      MaybeABI = getABI(Annotate->getAnnotation());
      if (MaybeABI)
        break;
    }

    if (not MaybeABI or MaybeABI->empty()) {
      revng_log(Log,
                "Functions should have attribute annotate with abi: "
                "specification");
      return false;
    }
  }

  revng_assert(MaybeABI.has_value());
  bool IsRawFunctionType = MaybeABI->starts_with(RawABIPrefix);
  auto NewType = IsRawFunctionType ?
                   makeTypeDefinition<RawFunctionDefinition>() :
                   makeTypeDefinition<CABIFunctionDefinition>();

  if (not IsRawFunctionType) {
    auto TheModelABI = model::ABI::fromName(*MaybeABI);
    if (TheModelABI == model::ABI::Invalid) {
      revng_log(Log, "Invalid ABI provided");
      return false;
    }

    auto &FunctionType = llvm::cast<CABIFunctionDefinition>(*NewType);
    FunctionType.ABI() = TheModelABI;
    auto TheRetClangType = FD->getReturnType();
    model::UpcastableType RetType = getModelTypeForClangType(TheRetClangType);
    if (not RetType) {
      revng_log(Log, "Unsupported type for function return value");
      return false;
    }

    FunctionType.ReturnType() = std::move(RetType);

    // Handle params.
    uint32_t Index = 0;
    for (unsigned I = 0, N = FD->getNumParams(); I != N; ++I) {
      auto QT = FD->getParamDecl(I)->getType();
      model::UpcastableType ParamType = getModelTypeForClangType(QT);
      if (not ParamType) {
        revng_log(Log, "Unsupported type for function parameter");
        return false;
      }

      model::Argument &NewArgument = FunctionType.Arguments()[Index];
      setCustomName(NewArgument, FD->getParamDecl(I)->getName());
      NewArgument.Type() = std::move(ParamType);
      ++Index;
    }
  } else {
    auto TheRetClangType = FD->getReturnType();
    auto &TheRawFunctionType = llvm::cast<RawFunctionDefinition>(*NewType);

    auto Architecture = getRawABIArchitecture(*MaybeABI);
    if (Architecture == model::Architecture::Invalid) {
      revng_log(Log, "Invalid raw abi architecture");
      return false;
    }
    TheRawFunctionType.Architecture() = Architecture;

    auto ReturnValuesInserter = TheRawFunctionType.ReturnValues()
                                  .batch_insert();

    // This represents multiple register location for return values.
    if (TheRetClangType->isStructureType()) {
      if (not MultiRegisterReturnValue) {
        revng_log(Log, "Return value should have already been parsed");
        return false;
      }

      for (auto &[Location, Type] : *MultiRegisterReturnValue) {
        model::NamedTypedRegister &NTR = ReturnValuesInserter.emplace(Location);
        NTR.Type() = Type;
      }
    } else {
      // Return value as single register.
      std::for_each(begin(FD->getAttrs()),
                    end(FD->getAttrs()),
                    [&](Attr *Attribute) {
                      if (auto *Annotation = dyn_cast<AnnotateAttr>(Attribute))
                        AnnotateAttrs.push_back(Annotation);
                    });

      std::optional<std::string> ReturnValue;
      for (auto *Annotate : AnnotateAttrs) {
        ReturnValue = getLoc(Annotate->getAnnotation());
        if (ReturnValue)
          break;
      }

      if (not ReturnValue or ReturnValue->empty()) {
        revng_log(Log,
                  "Return value should have attribute annotate with reg or "
                  "stack");
        return false;
      }

      // TODO: Handle stack location.
      if (*ReturnValue == "stack") {
        revng_log(Log, "We don't support Return value on stack for now");
        return false;
      }

      model::UpcastableType RetType = getModelTypeForClangType(TheRetClangType);
      if (not RetType) {
        revng_log(Log, "Unsupported type for function return value");
        return false;
      }

      auto RegisterID = model::Register::fromCSVName(*ReturnValue,
                                                     Model->Architecture());
      if (RegisterID == model::Register::Invalid) {
        revng_log(Log, "Unsupported register location");
        return false;
      }

      auto &ReturnValueReg = ReturnValuesInserter.emplace(RegisterID);
      ReturnValueReg.Type() = std::move(RetType);
    }

    auto ArgumentsInserter = TheRawFunctionType.Arguments().batch_insert();
    for (unsigned I = 0, N = FD->getNumParams(); I != N; ++I) {
      auto ParamDecl = FD->getParamDecl(I);
      if (ParamDecl->hasAttr<AnnotateAttr>()) {
        auto Annotate = std::find_if(begin(ParamDecl->getAttrs()),
                                     end(ParamDecl->getAttrs()),
                                     [&](Attr *Attribute) {
                                       return isa<AnnotateAttr>(Attribute);
                                     });
        auto Loc = getLoc(cast<AnnotateAttr>(*Annotate)->getAnnotation());
        if (not Loc or Loc->empty()) {
          revng_log(Log,
                    "Parameters should have attribute annotate with reg or "
                    "stack");
          return false;
        }

        // TODO: Handle stack location.
        if (*Loc == "stack") {
          revng_log(Log, "We don't support parameters on stack for now");
          return false;
        }

        auto LocationID = model::Register::fromCSVName(*Loc,
                                                       Model->Architecture());
        if (LocationID == model::Register::Invalid) {
          revng_log(Log, "Unsupported register location");
          return false;
        }

        auto QT = ParamDecl->getType();
        model::UpcastableType ParamType = getModelTypeForClangType(QT);
        if (not ParamType) {
          revng_log(Log, "Unsupported type for raw function parameter");
          return false;
        }

        NamedTypedRegister &ParamReg = ArgumentsInserter.emplace(LocationID);
        ParamReg.Type() = std::move(ParamType);
      }
    }
  }

  // Update the name if in the case it got changed.
  auto &ModelFunction = Model->Functions()[FunctionEntry];
  setCustomName(ModelFunction, FD->getName());

  // TODO: remember/clone StackFrameType as well.

  auto [_, Prototype] = Model->recordNewType(std::move(NewType));
  ModelFunction.Prototype() = Prototype;

  return true;
}

bool DeclVisitor::VisitTypedefDecl(const TypedefDecl *D) {
  if (not comesFromInternalFile(D))
    return true;

  revng_assert(AnalysisOption != ImportFromCOption::EditFunctionPrototype);

  QualType TheType = D->getUnderlyingType();
  if (auto Fn = llvm::dyn_cast<FunctionProtoType>(TheType)) {
    // Parse the ABI from annotate attribute attached to the typedef
    // declaration. Please do note that annotations on the parameters are not
    // attached, so we will use default RawFunctionDefinition from the Model if
    // the abi is raw.
    // TODO: Should we change the annotate attached to function types to have
    // info about parameters in the toplevel annotate attribute attached to
    // the typedef itself?
    std::optional<std::string> TheABI;
    if (D->hasAttr<AnnotateAttr>()) {
      auto TheAnnotateAttr = std::find_if(begin(D->getAttrs()),
                                          end(D->getAttrs()),
                                          [&](Attr *Attribute) {
                                            return isa<AnnotateAttr>(Attribute);
                                          });

      TheABI = getABI(cast<AnnotateAttr>(*TheAnnotateAttr)->getAnnotation());
    }

    if (not TheABI or TheABI->empty()) {
      revng_log(Log, "Unable to parse `abi:` from the annotate attribute");
      return false;
    }

    return VisitFunctionPrototype(Fn, TheABI);
  }

  // Regular, non-function, typedef.
  model::UpcastableType ModelTypedefType = getModelTypeForClangType(TheType);
  if (not ModelTypedefType) {
    revng_log(Log, "Unsupported underlying type for typedef");
    return false;
  }
  auto [ID, Kind] = *Type;
  auto TypeTypedef = model::makeTypeDefinition<model::TypedefDefinition>();
  if (AnalysisOption == ImportFromCOption::EditType)
    TypeTypedef->ID() = ID;

  auto TheTypeTypeDef = cast<model::TypedefDefinition>(TypeTypedef.get());
  TheTypeTypeDef->UnderlyingType() = std::move(ModelTypedefType);
  setCustomName(*TheTypeTypeDef, D->getName());

  if (AnalysisOption == ImportFromCOption::EditType) {
    // Remove old and add new type with the same ID.
    llvm::erase_if(Model->TypeDefinitions(),
                   [&ID](model::UpcastableTypeDefinition &P) {
                     return P.get()->ID() == ID;
                   });

    Model->TypeDefinitions().insert(std::move(TypeTypedef));
  } else {
    Model->recordNewType(std::move(TypeTypedef));
  }

  return true;
}

bool DeclVisitor::VisitFunctionPrototype(const FunctionProtoType *FP,
                                         std::optional<llvm::StringRef> ABI) {
  revng_assert(AnalysisOption != ImportFromCOption::EditFunctionPrototype);

  if (not ABI) {
    revng_log(Log, "No annotate attribute found with `abi:` information");
    return false;
  }

  bool IsRawFunctionType = ABI->starts_with(RawABIPrefix);
  auto NewType = IsRawFunctionType ?
                   makeTypeDefinition<RawFunctionDefinition>() :
                   makeTypeDefinition<CABIFunctionDefinition>();

  auto [ID, Kind] = *Type;
  if (AnalysisOption == ImportFromCOption::EditType)
    NewType->ID() = ID;

  if (not IsRawFunctionType) {
    auto &FunctionType = llvm::cast<CABIFunctionDefinition>(*NewType);
    auto TheModelABI = model::ABI::fromName(*ABI);
    if (TheModelABI == model::ABI::Invalid) {
      revng_log(Log, "An invalid ABI found as an input");
      return false;
    }

    FunctionType.ABI() = TheModelABI;

    auto TheRetClangType = FP->getReturnType();
    model::UpcastableType RetType = getModelTypeForClangType(TheRetClangType);
    if (not RetType) {
      revng_log(Log, "Unsupported type for function return value");
      return false;
    }

    FunctionType.ReturnType() = std::move(RetType);

    // Handle params.
    uint32_t Index = 0;
    for (auto QT : FP->getParamTypes()) {
      model::UpcastableType ParamType = getModelTypeForClangType(QT);
      if (not ParamType) {
        revng_log(Log, "Unsupported type for function parameter");
        return false;
      }

      model::Argument &NewArgument = FunctionType.Arguments()[Index];
      NewArgument.Type() = std::move(ParamType);
      ++Index;
    }
  } else {
    auto Architecture = getRawABIArchitecture(*ABI);
    if (Architecture == model::Architecture::Invalid) {
      revng_log(Log, "Invalid raw abi architecture");
      return false;
    }

    // TODO: Since we do not have info about parameters annotation, we use
    // default raw function.
    auto Default = cast<RawFunctionDefinition>(*Model->defaultPrototype());

    auto &FunctionType = llvm::cast<RawFunctionDefinition>(*NewType);
    FunctionType.Architecture() = Architecture;
    FunctionType.Arguments() = Default.Arguments();
    FunctionType.ReturnValues() = Default.ReturnValues();
    FunctionType.PreservedRegisters() = Default.PreservedRegisters();
    FunctionType.FinalStackOffset() = Default.FinalStackOffset();
  }

  if (AnalysisOption == ImportFromCOption::EditType) {
    // Remove old and add new type with the same ID.
    llvm::erase_if(Model->TypeDefinitions(),
                   [&ID](model::UpcastableTypeDefinition &P) {
                     return P.get()->ID() == ID;
                   });

    Model->TypeDefinitions().insert(std::move(NewType));
  } else {
    Model->recordNewType(std::move(NewType));
  }

  return true;
}

bool DeclVisitor::handleStructType(const clang::RecordDecl *RD) {
  const RecordDecl *Definition = RD->getDefinition();

  auto [ID, Kind] = *Type;
  auto NewType = makeTypeDefinition<model::StructDefinition>();
  if (AnalysisOption == ImportFromCOption::EditType)
    NewType->ID() = ID;

  setCustomName(*NewType, RD->getName());
  auto Struct = cast<model::StructDefinition>(NewType.get());
  uint64_t CurrentOffset = 0;

  //
  // Iterate over the struct fields
  //
  llvm::SmallVector<RawLocation, 4> ReturnValues;
  for (const FieldDecl *Field : Definition->fields()) {
    if (Field->isInvalidDecl()) {
      revng_log(Log, "Invalid declaration for a struct field");
      return false;
    }

    std::optional<model::Register::Values> LocationID;
    if (AnalysisOption == ImportFromCOption::EditFunctionPrototype) {
      if (not Field->hasAttr<AnnotateAttr>()) {
        revng_log(Log,
                  "Struct field representing return value should have annotate "
                  "attribute describing location");
        return false;
      }

      auto Annotate = std::find_if(begin(Field->getAttrs()),
                                   end(Field->getAttrs()),
                                   [&](Attr *Attribute) {
                                     return isa<AnnotateAttr>(Attribute);
                                   });

      auto Loc = getLoc(cast<AnnotateAttr>(*Annotate)->getAnnotation());
      if (not Loc or Loc->empty()) {
        revng_log(Log,
                  "Return value should have attribute annotate with reg or "
                  "stack");
        return false;
      }

      // TODO: Handle stack location.
      if (*Loc == "stack") {
        revng_log(Log, "We don't support Return value on stack for now");
        return false;
      }

      LocationID = model::Register::fromCSVName(*Loc, Model->Architecture());
      if (*LocationID == model::Register::Invalid) {
        revng_log(Log, "Unsupported register location");
        return false;
      }
    }

    std::optional<uint64_t> Size = 0;
    const QualType &ClangFieldType = Field->getType();
    model::UpcastableType ModelField = getModelTypeForClangType(ClangFieldType);

    if (ModelField.empty()) {
      revng_log(Log, "Unsupported type for a struct field");
      return false;
    }

    if (AnalysisOption == ImportFromCOption::EditFunctionPrototype) {
      revng_assert(LocationID);
      ReturnValues.emplace_back(*LocationID, ModelField);
    }

    if (ClangFieldType->isPointerType()) {
      Size = model::Architecture::getPointerSize(Model->Architecture());

    } else if (ClangFieldType->isArrayType()) {
      uint64_t NumberOfElements = 0;
      if (const auto *CAT = dyn_cast<ConstantArrayType>(ClangFieldType)) {
        NumberOfElements = CAT->getSize().getZExtValue();
      } else {
        revng_log(Log, "Unsupported array type");
        return false;
      }

      const model::Type &Element = *ModelField->toArray().ElementType();
      Size = *Element.size() * NumberOfElements;

    } else {
      Size = *ModelField->size();
    }

    if (not Field->getName().starts_with(StructPaddingPrefix)) {
      auto &FieldModelType = Struct->Fields()[CurrentOffset];
      setCustomName(FieldModelType, Field->getName());
      FieldModelType.Type() = std::move(ModelField);
    } else {
      // Do not create fields for padding
    }

    revng_assert(Size);
    CurrentOffset += *Size;
  }

  // TODO: Can this be calculated/fetched automatically?
  Struct->Size() = CurrentOffset;

  switch (AnalysisOption) {
  case ImportFromCOption::EditType:
    // Remove old and add new type with the same ID.
    llvm::erase_if(Model->TypeDefinitions(),
                   [&ID](model::UpcastableTypeDefinition &P) {
                     return P.get()->ID() == ID;
                   });
    Model->TypeDefinitions().insert(std::move(NewType));
    break;

  case ImportFromCOption::EditFunctionPrototype:
    MultiRegisterReturnValue = std::move(ReturnValues);
    break;

  case ImportFromCOption::AddType:
    Model->recordNewType(std::move(NewType));
    break;
  }

  return true;
}

bool DeclVisitor::handleUnionType(const clang::RecordDecl *RD) {
  revng_assert(AnalysisOption != ImportFromCOption::EditFunctionPrototype);

  auto [ID, Kind] = *Type;
  const RecordDecl *Definition = RD->getDefinition();
  auto NewType = makeTypeDefinition<model::UnionDefinition>();
  if (AnalysisOption == ImportFromCOption::EditType)
    NewType->ID() = ID;

  setCustomName(*NewType, RD->getName().str());
  auto Union = cast<model::UnionDefinition>(NewType.get());

  uint64_t CurrentIndex = 0;
  for (const FieldDecl *Field : Definition->fields()) {
    if (Field->isInvalidDecl()) {
      revng_log(Log, "Invalid declaration for a union field");
      return false;
    }

    const QualType &FieldType = Field->getType();
    model::UpcastableType TheFieldType = getModelTypeForClangType(FieldType);

    if (not TheFieldType) {
      revng_log(Log, "Unsupported type for an union field");
      return false;
    }

    auto &FieldModelType = Union->Fields()[CurrentIndex];
    setCustomName(FieldModelType, Field->getName());
    FieldModelType.Type() = std::move(TheFieldType);

    ++CurrentIndex;
  }

  if (AnalysisOption == ImportFromCOption::EditType) {
    // Remove old and add new type with the same ID.
    llvm::erase_if(Model->TypeDefinitions(),
                   [&ID](model::UpcastableTypeDefinition &P) {
                     return P.get()->ID() == ID;
                   });
    Model->TypeDefinitions().insert(std::move(NewType));
  } else {
    Model->recordNewType(std::move(NewType));
  }

  return true;
}

bool DeclVisitor::VisitRecordDecl(const clang::RecordDecl *RD) {
  if (not comesFromInternalFile(RD))
    return true;

  if (AnalysisOption != ImportFromCOption::EditFunctionPrototype
      and not RD->hasAttr<PackedAttr>()) {
    revng_log(Log, "Unions and Structs should have attribute packed");
    return false;
  }

  QualType TheType = Context.getTypeDeclType(RD);
  if (TheType->isStructureType()) {
    return handleStructType(RD);
  } else if (TheType->isUnionType()) {
    return handleUnionType(RD);
  } else {
    revng_log(Log, "Unhandled record type declaration");
    return false;
  }

  return true;
}

bool DeclVisitor::VisitEnumDecl(const EnumDecl *D) {
  if (not comesFromInternalFile(D))
    return true;

  revng_assert(AnalysisOption != ImportFromCOption::EditFunctionPrototype);

  if (not D->hasAttr<PackedAttr>()) {
    revng_log(Log, "Enums should have attribute packed");
    return false;
  }

  // Parse annotate attribute used for specifying underlying type.
  std::optional<std::string> UnderlyingType;
  if (D->hasAttr<AnnotateAttr>()) {
    auto Annotate = std::find_if(begin(D->getAttrs()),
                                 end(D->getAttrs()),
                                 [&](Attr *Attribute) {
                                   return isa<AnnotateAttr>(Attribute);
                                 });

    llvm::StringRef Annotation = cast<AnnotateAttr>(*Annotate)->getAnnotation();
    UnderlyingType = parseEnumUnderlyingType(Annotation);
    if (not UnderlyingType or UnderlyingType->empty()) {
      revng_log(Log,
                "Unable to parse `enum_underlying_type:` from the annotate "
                "attribute");
      return false;
    }
  }

  revng_assert(UnderlyingType.has_value());
  auto TheUnderlyingModelType = getEnumUnderlyingType(*UnderlyingType);
  if (not TheUnderlyingModelType) {
    revng_log(Log,
              "UnderlyingType of a EnumDefinition can only be Signed or "
              "Unsigned");
    return false;
  }

  model::EnumDefinition *NewType = nullptr;
  if (AnalysisOption == ImportFromCOption::EditType) {
    revng_assert(Type != std::nullopt);
    model::TypeDefinition &Definition = *Model->TypeDefinitions().at(*Type);
    if (auto *Enum = llvm::dyn_cast<model::EnumDefinition>(&Definition)) {
      NewType = Enum;
    } else {
      // It seems like the kind of the type got changed. Since it affects
      // the key we need to erase the old type before adding the new one.
      Model->TypeDefinitions().erase(*Type);
      NewType = &Model->makeEnumDefinition().first;
    }
  }

  NewType->UnderlyingType() = std::move(TheUnderlyingModelType);

  auto *Definition = D->getDefinition();
  setCustomName(*NewType, Definition->getName());
  for (const auto *Enum : Definition->enumerators()) {
    auto Value = Enum->getInitVal().getExtValue();
    auto NewIterator = NewType->Entries().emplace(Value).first;
    NewIterator->CustomName() = Enum->getName().str();
  }

  return true;
}

void DeclVisitor::run(clang::TranslationUnitDecl *TUD) {
  this->TraverseDecl(TUD);
}

bool DeclVisitor::TraverseDecl(clang::Decl *D) {
  // This can happen due to an error in the code.
  if (!D)
    return true;

  setupLineAndColumn(D);

  if (isa<EnumDecl>(D))
    VisitEnumDecl(cast<EnumDecl>(D));

  clang::RecursiveASTVisitor<DeclVisitor>::TraverseDecl(D);
  return true;
}

void HeaderToModel::HandleTranslationUnit(ASTContext &Context) {
  clang::TranslationUnitDecl *TUD = Context.getTranslationUnitDecl();
  DeclVisitor(Model, Context, Type, FunctionEntry, Error, AnalysisOption)
    .run(TUD);
}

std::unique_ptr<ASTConsumer> HeaderToModelEditTypeAction::newASTConsumer() {
  return std::make_unique<HeaderToModel>(Model,
                                         Type,
                                         MetaAddress::invalid(),
                                         Error,
                                         AnalysisOption);
}

std::unique_ptr<ASTConsumer> HeaderToModelEditFunctionAction::newASTConsumer() {
  return std::make_unique<HeaderToModel>(Model,
                                         /* Type = */ std::nullopt,
                                         FunctionEntry,
                                         Error,
                                         AnalysisOption);
}

std::unique_ptr<ASTConsumer> HeaderToModelAddTypeAction::newASTConsumer() {
  return std::make_unique<HeaderToModel>(Model,
                                         /* Type = */ std::nullopt,
                                         MetaAddress::invalid(),
                                         Error,
                                         AnalysisOption);
}

std::unique_ptr<ASTConsumer>
HeaderToModelAction::CreateASTConsumer(CompilerInstance &, llvm::StringRef) {
  return newASTConsumer();
}

bool HeaderToModelAction::BeginInvocation(clang::CompilerInstance &CI) {
  DiagConsumer = new HeaderToModelDiagnosticConsumer(CI.getDiagnostics());
  CI.getDiagnostics().setClient(DiagConsumer, /*ShouldOwnClient=*/true);
  return true;
}

void HeaderToModelAction::EndSourceFile() {
  if (DiagConsumer->getError()) {
    Error = DiagConsumer->getError();
  }
}

void HeaderToModelDiagnosticConsumer::EndSourceFile() {
  Client->EndSourceFile();
}

using Level = DiagnosticsEngine::Level;
void HeaderToModelDiagnosticConsumer::HandleDiagnostic(Level DiagLevel,
                                                       const Diagnostic &Info) {
  SmallString<100> OutStr;
  Info.FormatDiagnostic(OutStr);

  llvm::raw_svector_ostream DiagMessageStream(OutStr);

  std::string Text;
  std::string ErrorLocation;
  llvm::raw_string_ostream OS(Text);
  auto *DiagOpts = &Info.getDiags()->getDiagnosticOptions();

  uint64_t StartOfLocationInfo = OS.tell();

  TextDiagnostic::printDiagnosticLevel(OS, DiagLevel, DiagOpts->ShowColors);
  const bool IsSupplemental = DiagLevel == DiagnosticsEngine::Note;
  TextDiagnostic::printDiagnosticMessage(OS,
                                         IsSupplemental,
                                         DiagMessageStream.str(),
                                         OS.tell() - StartOfLocationInfo,
                                         DiagOpts->MessageLength,
                                         DiagOpts->ShowColors);

  unsigned Line = 0;
  unsigned Column = 0;
  std::string FileName;
  if (Info.getLocation().isValid()) {
    FullSourceLoc Location(Info.getLocation(), Info.getSourceManager());
    Line = Location.getLineNumber();
    Column = Location.getColumnNumber();
    FileName = Location.getPresumedLoc().getFilename();
  }

  ErrorLocation = FileName + ":" + std::to_string(Line) + ":"
                  + std::to_string(Column) + ": ";
  Text = ErrorLocation + Text;
  // Report all the messages coming from clang.
  if (Error)
    Text = Error->ErrorMessage + Text;

  Error = { Text, Line, Column };

  OS.flush();
}
} // end namespace tooling
} // end namespace clang
