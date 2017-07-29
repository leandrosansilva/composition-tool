//------------------------------------------------------------------------------
//
// Based in a public domain work of Eli Bendersky (eliben@gmail.com)
// https://github.com/eliben/llvm-clang-samples/
//
//------------------------------------------------------------------------------

/*
 * TODO: Refactor all this code, extracting each thing to a resusable 
 * and self-contained function.
*/

#include <sstream>
#include <string>

#include <clang/AST/AST.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/ASTConsumers.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FormatVariadic.h>
#include <clang/Lex/Lexer.h>

namespace {
  const llvm::StringRef PROVIDE_TAG("__provide__");

  const auto objCCategoryHeaderFormatBegin = "@interface {0} ({1}__{2})\n";
  
  const auto objCCategoryImplementationFormatBegin = "@implementation {0} ({1}__{2})\n";
  
  const auto objcDeclarationEnd = "@end\n\n";

  const auto objCSelectorImplementationFormat = R"+-+(
{0} {
  return [{1} {2}];
}
)+-+";

  const auto objcClassSelectorForwardingFormat = "{0}";
  const auto objcInstanceSelectorForwardingFormat = "self.{0}";

  const auto objCSelectorSignatureFormat = R"+=+({0} ({1}){2})+=+";

  struct CodeGeneratorContext
  {
    clang::CompilerInstance* compilerInstance;
    llvm::StringRef inputFilename;
  };

  struct KnownDeclarations
  {
    template<typename T>
    using LookupByInterfaceName = std::map<llvm::StringRef, T>;

    LookupByInterfaceName<clang::ObjCInterfaceDecl*> interfaces;
    LookupByInterfaceName<clang::ObjCInterfaceDecl*> parent;
  };

  template<typename F, typename D>
  auto runIfInMainFile(clang::ASTContext& context, F function, D decl) -> bool
  {
    if (context.getSourceManager().isInMainFile(decl->getLocStart())) {
      return function(decl);
    }
    
    return true;
  }
  
  enum struct ProvidedItemType
  {
    InstanceMethod,
    ClassMethod,
    Property,
    Unknown,
  };
  
  struct ProvidedItem
  {
    ProvidedItemType type;
    llvm::StringRef value;
  };
  
  // FIXME: why 10? Can it grow? OMG, 
  auto extractProvidedItems(const std::vector<clang::AnnotateAttr*>& attrs) -> std::vector<ProvidedItem>
  {
    auto splitList = llvm::SmallVector<llvm::StringRef, 10>();
    
    // TODO: handle errors on parsing provided items?
    for (const auto attr: attrs) {
      attr->getAnnotation().trim().substr(PROVIDE_TAG.size()).split(splitList, " ", -1, false);
    }
    
    auto items = std::vector<ProvidedItem>{splitList.size()};
    
    std::transform(std::begin(splitList), std::end(splitList), begin(items), [](StringRef s) -> ProvidedItem {
      assert(s.size() > 0);
      
      const auto type = [=]() -> ProvidedItemType {
        switch(s[0]) {
          case '@': return ProvidedItemType::Property;
          case '-': return ProvidedItemType::InstanceMethod;
          case '+': return ProvidedItemType::ClassMethod;
        }
        
        assert(false && "Invalid Type!!!! FIXME: handle error");
        
        return ProvidedItemType::Unknown;
      }();
      
      const auto value = s.substr(1);
      
      assert(value.size() > 0 && "Invalid Value!!! FIXME: handle error");
      
      return {type, value};
    });
    
    return items;
  }
  
  template<typename NodeType>
  auto getParent(const CodeGeneratorContext& context, const NodeType o) -> const clang::ObjCContainerDecl*
  {
    assert(o != nullptr);
    
    auto& astContext = context.compilerInstance->getASTContext();
    
    for (const auto p: astContext.getParents(*o)) {
        const auto parent = p.template get<clang::ObjCContainerDecl>();
        
        if (parent != nullptr) {
          return parent;
        }
      }
      
      return nullptr;
  }
  
  template<typename NodeType>
  auto getInterfaceForMember(const CodeGeneratorContext& context, const NodeType member) -> const clang::ObjCInterfaceDecl*
  {
    const auto parent = getParent(context, member);
    
    assert(parent != nullptr);
    
    if (const auto interface = llvm::dyn_cast<clang::ObjCInterfaceDecl>(parent)) {
      return interface;
    }
    
    if (const auto category = llvm::dyn_cast<clang::ObjCCategoryDecl>(parent)) {
      return category->getClassInterface();
    }
    
    return nullptr;
  }
  
  template<
    typename Result,
    typename Filter,
    typename InterfaceExtractor,
    typename CategoryExtractor, 
    typename ProtocolExtractor>
  auto getMemberForInterface(clang::ObjCInterfaceDecl* propertyInterfaceDecl,
                       Filter filter,
                       InterfaceExtractor interfaceExtractor,
                       CategoryExtractor categoryExtractor,
                       ProtocolExtractor protocolExtractor) -> Result
  {
    const auto categories = propertyInterfaceDecl->known_categories();
    
    // TODO: check the other protocols linked in the interface (no only ::protocols())
    const auto protocols = propertyInterfaceDecl->protocols();
    
    const auto membersInInterface = interfaceExtractor(propertyInterfaceDecl);
    
    // FIXME: there's no need for building the list of pairs!
    auto pairs = std::vector<std::pair<decltype(membersInInterface.begin()), decltype(membersInInterface.end())>>{};
    pairs.emplace_back(membersInInterface.begin(), membersInInterface.end());
    
    for (const auto& category: categories) {
      const auto membersInCategory = categoryExtractor(category);
      pairs.emplace_back(membersInCategory.begin(), membersInCategory.end());
    }
    
    // TODO: go up in the protocol hierarchy until find the member or skip
    for (const auto& protocol: protocols) {
      const auto membersInProtocol = protocolExtractor(protocol);
      pairs.emplace_back(membersInProtocol.begin(), membersInProtocol.end());
    }
    
    for (const auto pairIt: pairs) {
      const auto selectorIt = std::find_if(pairIt.first, pairIt.second, filter);

      if (selectorIt != pairIt.second) {
        return *selectorIt;
      }
    }
    
    // TODO: Go up to parents until finds the member or give up
    
    return nullptr;   
  }
  
  template<typename Extractor>
  auto getSelectorForInterface(clang::ObjCInterfaceDecl* propertyInterfaceDecl, const StringRef selectorName, Extractor extractor) -> clang::ObjCMethodDecl*
  {
    const auto filter = [=](const clang::ObjCMethodDecl* methodDecl) {
      //llvm::outs() << "Comparing " << methodDecl->getSelector().getAsString() << " with " << selectorName << '\n';
      return methodDecl->getSelector().getAsString() == selectorName;
    };
    
    return getMemberForInterface<clang::ObjCMethodDecl*>(propertyInterfaceDecl, filter, extractor, extractor, extractor);
  }
  
  auto getInstanceSelectorForInterface(clang::ObjCInterfaceDecl* propertyInterfaceDecl, const StringRef selectorName) -> clang::ObjCMethodDecl*
  {
    const auto extractor = [](clang::ObjCContainerDecl* container) {
      return container->instance_methods();
    };
    
    return getSelectorForInterface(propertyInterfaceDecl, selectorName, extractor);
  }
  
  auto getClassSelectorForInterface(clang::ObjCInterfaceDecl* propertyInterfaceDecl, const StringRef selectorName) -> clang::ObjCMethodDecl*
  {
    const auto extractor = [](clang::ObjCContainerDecl* container) {
      return container->class_methods();
    };
    
    return getSelectorForInterface(propertyInterfaceDecl, selectorName, extractor);
  }
  
  template<typename Extractor>
  auto getPropertyForInterface(clang::ObjCInterfaceDecl* propertyInterfaceDecl, const StringRef propertyName, Extractor extractor) -> clang::ObjCPropertyDecl*
  {
    const auto filter = [=](const clang::ObjCPropertyDecl* propertyDecl) {
      return propertyDecl->getName() == propertyName;
    };
    
    return getMemberForInterface<clang::ObjCPropertyDecl*>(propertyInterfaceDecl, filter, extractor, extractor, extractor);
  }
  
  auto getInstancePropertyForInterface(clang::ObjCInterfaceDecl* propertyInterfaceDecl, const StringRef propertyName) -> clang::ObjCPropertyDecl*
  {
    const auto extractor = [](clang::ObjCContainerDecl* container) {
      return container->instance_properties();
    };
    
    return getPropertyForInterface(propertyInterfaceDecl, propertyName, extractor);
  }
  
  auto getClassPropertyForInterface(clang::ObjCInterfaceDecl* propertyInterfaceDecl, const StringRef propertyName) -> clang::ObjCPropertyDecl*
  {
    const auto extractor = [](clang::ObjCContainerDecl* container) {
      return container->class_properties();
    };
    
    return getPropertyForInterface(propertyInterfaceDecl, propertyName, extractor);
  }
  
  auto generateCallBody(clang::ObjCMethodDecl* selectorInProperty) -> std::string
  {
     const auto selectorNameAsStdString = selectorInProperty->getSelector().getAsString();
          
     if (selectorInProperty->getSelector().getNumArgs() == 0) {
       return selectorNameAsStdString;
     }
     
     // yep, it's a view of the std::string version. Not so fancy, but works
     const auto selectorName = llvm::StringRef{selectorNameAsStdString};
     
     const auto parameters = selectorInProperty->parameters();

     auto paramLabels = llvm::SmallVector<llvm::StringRef, 10>{};
     selectorName.split(paramLabels, ":", -1, false);

     // TODO: use stream instead
     auto callBody = std::string{};

     // FIXME: it fails for methods with no arguments!
     assert(paramLabels.size() == parameters.size());

     for (size_t size = parameters.size(), i = 0u; i < size; i++) {
       callBody += paramLabels[i];
       callBody += ":";
       callBody += parameters[i]->getName();
       if (i < size - 1) {
         callBody += " ";
       }
       llvm::outs() << paramLabels[i] << ":" << parameters[i]->getName() << " " << "\n";
     }

     return callBody;
  }
  
  auto generateSelectorDefinition(clang::ObjCMethodDecl* selectorInProperty,
                                  const std::string& selectorSignature,
                                  const std::string& memberTODORenameMeForSomethingThatRefersToClassToo) -> std::string
  {
    return llvm::formatv(objCSelectorImplementationFormat,
                         selectorSignature,
                         memberTODORenameMeForSomethingThatRefersToClassToo,
                         generateCallBody(selectorInProperty));
  }
  
  auto generateSelectorSignature(clang::ObjCMethodDecl* selector) -> std::string
  {
    const auto returnType = selector->getReturnType();
    
    const auto signatureCore = [&]() -> std::string {
      const auto parameters = selector->parameters();
      
      // FIXME: this is copy@paste
      const auto selectorNameAsStdString = selector->getSelector().getAsString();
      
      llvm::outs() << "analysing selector " << selector->getSelector().getAsString() << '\n';
      
      if (parameters.empty()) {
        return selectorNameAsStdString;
      }
      
      const auto selectorName = llvm::StringRef{selectorNameAsStdString.data(), selectorNameAsStdString.size()};
      
      auto paramLabels = llvm::SmallVector<llvm::StringRef, 10>{};
      selectorName.split(paramLabels, ":", -1, false);

      // FIXME: this is copy&paste code
      
      // TODO: use stream instead
      auto callBody = std::string{};

      // FIXME: it fails for methods with no arguments!
      assert(paramLabels.size() == parameters.size());

      for (size_t size = parameters.size(), i = 0u; i < size; i++) {
        callBody += llvm::formatv("{0}:({1}){2} ", paramLabels[i], parameters[i]->getType().getAsString(), parameters[i]->getName());
      }
      
      return callBody;
    }();
    
    const auto methodType = selector->isClassMethod() ? "+" : "-";
    
    return llvm::formatv(objCSelectorSignatureFormat, methodType, returnType.getAsString(), signatureCore);
  }
  
  std::map<ProvidedItemType, std::function<void(CodeGeneratorContext&, clang::ObjCInterfaceDecl*, clang::ObjCPropertyDecl*, ProvidedItem, llvm::raw_ostream&, llvm::raw_ostream&)>> generators {
    {ProvidedItemType::InstanceMethod, [](CodeGeneratorContext&, clang::ObjCInterfaceDecl* propertyInterfaceDecl, clang::ObjCPropertyDecl* propertyDecl, ProvidedItem item, llvm::raw_ostream& headerStream, llvm::raw_ostream& implStream) {
      const auto selectorName = item.value;
      const auto selectorInProperty = getInstanceSelectorForInterface(propertyInterfaceDecl, selectorName);
      assert(selectorInProperty != nullptr);
      const auto selectorSignature = generateSelectorSignature(selectorInProperty);
      headerStream << selectorSignature << ";\n";
      const auto memberDef = llvm::formatv(objcInstanceSelectorForwardingFormat, propertyDecl->getName());
      implStream << generateSelectorDefinition(selectorInProperty, selectorSignature, memberDef);
    }},
    {ProvidedItemType::ClassMethod, [](CodeGeneratorContext&, clang::ObjCInterfaceDecl* propertyInterfaceDecl, clang::ObjCPropertyDecl*, ProvidedItem item, llvm::raw_ostream& headerStream, llvm::raw_ostream& implStream) {
      const auto selectorName = item.value;
      const auto selectorInProperty = getClassSelectorForInterface(propertyInterfaceDecl, selectorName);
      assert(selectorInProperty != nullptr);
      const auto selectorSignature = generateSelectorSignature(selectorInProperty);
      headerStream << selectorSignature << ";\n";
      const auto memberDef = llvm::formatv(objcClassSelectorForwardingFormat, propertyInterfaceDecl->getName());
      implStream << generateSelectorDefinition(selectorInProperty, selectorSignature, memberDef);
    }},
    {ProvidedItemType::Property, [](CodeGeneratorContext& context, clang::ObjCInterfaceDecl* propertyInterfaceDecl, clang::ObjCPropertyDecl* propertyDecl, ProvidedItem item, llvm::raw_ostream& headerStream, llvm::raw_ostream& implStream) {
      const auto locStart = propertyDecl->getLocStart();
      // FIXME: locEnd is pointing to the beginning of the property name :-(
      const auto locEnd = propertyDecl->getLocEnd();

      const auto codeBegin = context.compilerInstance->getSourceManager().getCharacterData(locStart);
      const auto codeEnd = context.compilerInstance->getSourceManager().getCharacterData(locEnd);
      const auto propertySignature = llvm::StringRef(codeBegin, codeEnd - codeBegin);

      llvm::outs() << "Property signature: " << propertySignature << '\n';
      
      headerStream << propertySignature;
      headerStream << propertyDecl->getName() << ";\n";
      
      const auto f = [&]() -> std::string {
        if (propertyDecl->isInstanceProperty()) {
          return llvm::formatv(objcInstanceSelectorForwardingFormat, propertyDecl->getName());
        }
        
        return llvm::formatv(objcClassSelectorForwardingFormat, propertyInterfaceDecl->getName());
      }();

      if (auto getterMethodDecl = propertyDecl->getGetterMethodDecl()) {
        const auto selectorSignature = generateSelectorSignature(getterMethodDecl);
        implStream << generateSelectorDefinition(getterMethodDecl, selectorSignature, f);
      }
      
      if (auto setterMethodDecl = propertyDecl->getSetterMethodDecl()) {
        const auto selectorSignature = generateSelectorSignature(setterMethodDecl);
        implStream << generateSelectorDefinition(setterMethodDecl, selectorSignature, f);
      }
    }},
  };
  
  // FIXME: no needs to say this function is way too large, doing too much and with a lot of copy&paste, right?
  auto generateExtension(clang::ObjCPropertyDecl* o,
                         CodeGeneratorContext& context,
                         const std::vector<clang::AnnotateAttr*>& attrs,
                         const KnownDeclarations& knownDeclarations,
                         llvm::raw_string_ostream& headerStream,
                         llvm::raw_string_ostream& implStream) -> void
  {
    const auto providedItems = extractProvidedItems(attrs);

    if (providedItems.empty()) {
      return;
    }
    
    const auto parent = getParent(context, o);
    
    assert(parent != nullptr);
    
    const auto interfaceDecl = llvm::dyn_cast<clang::ObjCInterfaceDecl>(parent);
    
    assert(interfaceDecl != nullptr);

    const auto propertyInterfaceDecl = [=]() -> clang::ObjCInterfaceDecl* {
      const auto t = llvm::dyn_cast<clang::ObjCObjectPointerType>(o->getTypeSourceInfo()->getType().getTypePtrOrNull());
      
      assert(t != nullptr);
      
      t->dump();
      
      const auto pointee = t->getPointeeType();
      
      const auto interface = pointee.split().Ty->getAs<clang::ObjCInterfaceType>();
      
      if (interface != nullptr) {
        return interface->getDecl();
      }
      
      // when it not a member of an interface, it can be id or a pointer to a class
      const auto nonInterfacePointer = pointee.split().Ty->getAs<clang::ObjCObjectType>();
      llvm::outs() << "found a id pointer that implements " << nonInterfacePointer->getNumProtocols() << " protocols" << '\n';
      nonInterfacePointer->dump();
      
      return nullptr;
    }();
    
    assert(propertyInterfaceDecl != nullptr);
    
    if (propertyInterfaceDecl != nullptr) {
      llvm::outs() << "property type: " << propertyInterfaceDecl->getName() << '\n';
    }
    
    for (const auto& attr: attrs) {
      const auto annotationValue = attr->getAnnotation();
      llvm::outs() << "Found a property annotation!!! " << annotationValue << '\n';
      //o->dump();
    }
    
    headerStream << llvm::formatv(objCCategoryHeaderFormatBegin,
                                              interfaceDecl->getName(),
                                              propertyInterfaceDecl->getName(),
                                              o->getName());
    
    implStream << llvm::formatv(objCCategoryImplementationFormatBegin,
                                                      interfaceDecl->getName(),
                                                      propertyInterfaceDecl->getName(),
                                                      o->getName());
    
    for (const auto item: providedItems) {
      assert(item.type != ProvidedItemType::Unknown && "FIXME: handle error");
      
      const auto generator = generators[item.type];
      
      llvm::outs() << "Processing item: " << item.value << '\n';
      generator(context, propertyInterfaceDecl, o, item, headerStream, implStream);
    }
    
    headerStream << objcDeclarationEnd;
    implStream << objcDeclarationEnd;
  }
}

static llvm::cl::OptionCategory commandLineCategory("Mixin With Steroids");

struct ObjCVisitor : public clang::RecursiveASTVisitor<ObjCVisitor>
{
  CodeGeneratorContext _context;
  KnownDeclarations& _knownDeclarations;

  template<typename F, typename D>
  auto visit(F function, D decl) const -> bool
  {
    return ::runIfInMainFile(_context.compilerInstance->getASTContext(), function, decl);
  }
  
  ObjCVisitor(CodeGeneratorContext context,
              KnownDeclarations& knownDeclarations):
  _context(context),
  _knownDeclarations(knownDeclarations)
  {
  }

  auto VisitObjCInterfaceDecl(clang::ObjCInterfaceDecl* o) -> bool
  {
    //llvm::outs() << "New interface: " << o->getName() << '\n';
    //o->getLocation().print(llvm::outs(), _context.compilerInstance->getSourceManager());
    
    visit([](clang::ObjCInterfaceDecl* o) {
      return true;
    }, o);
    
    _knownDeclarations.interfaces[o->getName()] = o;
    return true;
  }
  
  auto VisitObjCProtocolDecl(clang::ObjCProtocolDecl* o) -> bool
  {
    return true;
  }
  
  auto VisitObjCMethodDecl(clang::ObjCMethodDecl* s) -> bool
  {
    //llvm::outs() << "Analysing selector " << s->getSelector().getAsString() << '\n';

   return true;
  }
    
  auto VisitObjCCategoryDecl(clang::ObjCCategoryDecl* o) -> bool
  {
    const auto interface = o->getClassInterface();
    //llvm::outs() << "LALALA category decl: " << o->getName() << " for interface: " << interface->getName() << '\n';
    return true;
  }
 
  auto VisitObjCImplementationDecl(clang::ObjCImplementationDecl* o) -> bool
  {
    // FIXME: do we really need to visit implementations of classes?
    return visit([](clang::ObjCImplementationDecl *o) {
        llvm::outs() << "visiting impl decl " << o->getName() << '\n';
        return true;
    }, o);
  }
  
  auto VisitObjCIvarDecl(clang::ObjCIvarDecl* o) -> bool
  {
    // TODO: do the same logic as for properties
    return visit([](clang::ObjCIvarDecl*) {
        //llvm::outs() << "visiting ivar decl " << o->getName() << '\n';
        return true;
    }, o);
  }
 
  auto VisitObjCPropertyDecl(clang::ObjCPropertyDecl* o) -> bool
  {
    //llvm::outs() << "Visiting property " << o->getName() << '\n';
    
    return visit([this](clang::ObjCPropertyDecl *o) {
      const auto typeInfo = o->getTypeSourceInfo();
      const auto type = typeInfo->getType().getTypePtrOrNull();
    
      auto objcObjectType = llvm::dyn_cast<clang::ObjCObjectPointerType>(type);
    
      // Only properties of objc types can provide...
      // TODO: emit error in case properties of other types try to provide something
      if (objcObjectType == nullptr) {
        return true;
      }
      
      //llvm::outs() << "Found a object property!!! " << objcObjectType->getTypeClassName()
      //             << ":" << o->getName() << '\n';

      auto provideAnnotationAttrs = std::vector<clang::AnnotateAttr*>{};

      // I know, I know, but copy_if does not cast, so a simple for does the job
      for (auto& attr: o->getAttrs()) {
        auto annotateAttr = llvm::dyn_cast<clang::AnnotateAttr>(attr);

        if (annotateAttr != nullptr && annotateAttr->getAnnotation().startswith(PROVIDE_TAG)) {
          provideAnnotationAttrs.push_back(annotateAttr);
        }
      }
      
      if (provideAnnotationAttrs.empty()) {
        return true;
      }
      
      auto providedBodyForHeader = std::string{};
      auto providedBodyForImplementation = std::string{};
      
      {
        llvm::raw_string_ostream headerStream{providedBodyForHeader};
        llvm::raw_string_ostream implStream{providedBodyForImplementation};
        generateExtension(o, _context, provideAnnotationAttrs, _knownDeclarations, headerStream, implStream);
      }
      
      llvm::outs() << "Header: \n" << providedBodyForHeader;
      llvm::outs() << "Implementation: \n" << providedBodyForImplementation;

      return true;
    }, o);
  }
};

struct ObjCASTConsumer : public clang::ASTConsumer
{
  KnownDeclarations _knownDeclarations;
  ObjCVisitor _visitor;

  ObjCASTConsumer(CodeGeneratorContext context):
  _visitor({context, _knownDeclarations})
  {
  }

  auto HandleTopLevelDecl(clang::DeclGroupRef declGroup) -> bool final
  {
    for (auto &b: declGroup) {
      _visitor.TraverseDecl(b);
    }
    
    return true;
  }

};

// For each source file provided to the tool, a new FrontendAction is created.
struct MyFrontendAction : public clang::ASTFrontendAction
{
  MyFrontendAction()
  {
  }
  
  auto CreateASTConsumer(clang::CompilerInstance &compilerInstance,
                         llvm::StringRef file) -> std::unique_ptr<clang::ASTConsumer> final
  {
    return llvm::make_unique<ObjCASTConsumer>(CodeGeneratorContext{&compilerInstance, file});
  }
};

auto main(int argc, const char **argv) -> int
{
  auto op = clang::tooling::CommonOptionsParser(argc, argv, commandLineCategory);
  auto tool = clang::tooling::ClangTool(op.getCompilations(), op.getSourcePathList());

  return tool.run(clang::tooling::newFrontendActionFactory<MyFrontendAction>().get());
}

