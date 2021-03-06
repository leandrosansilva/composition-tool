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

  const auto objCCategoryHeaderFormatBegin = "@interface {0} ({1}__{2})\n\n";
  
  const auto objCCategoryImplementationFormatBegin = "@implementation {0} ({1}__{2})\n\n";
  
  const auto objcDeclarationEnd = "@end\n\n";

  const auto objCSelectorImplementationFormat = R"+-+(
{0} {
  return [{1} {2}];
}
)+-+";

  const auto objcClassSelectorForwardingFormat = "{0}";
  const auto objcInstanceSelectorForwardingFormat = "self.{0}";

  const auto objCSelectorSignatureFormat = R"+=+({0} ({1}){2})+=+";
  
  const auto objCIncludeFileFormat = "#include \"{0}\"\n";

  struct CodeGeneratorContext
  {
    CodeGeneratorContext(clang::CompilerInstance* compilerInstance,
                         llvm::StringRef inputFilename,
                         llvm::raw_ostream& headerStream,
                         llvm::raw_ostream& implStream):
      compilerInstance(compilerInstance),
      inputFilename(inputFilename),
      headerStream(headerStream),
      implStream(implStream)
    {
    }
    
    clang::CompilerInstance* compilerInstance;
    llvm::StringRef inputFilename;
    llvm::raw_ostream& headerStream;
    llvm::raw_ostream& implStream;
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
    typename ProtocolExtractor>
  auto getMemberForProtocols(llvm::iterator_range<clang::ObjCProtocolDecl*const*> protocols, Filter filter, ProtocolExtractor protocolExtractor) -> Result
  {
    for (const auto protocol: protocols) {
      llvm::outs() << "checking member in protocol " << protocol->getName() << '\n';
      const auto membersInProtocol = protocolExtractor(protocol);
      
      const auto it = std::find_if(std::begin(membersInProtocol), std::end(membersInProtocol), filter);
      
      if (it != std::end(membersInProtocol)) {
        return *it;
      }
    }
    
    // Depth first search on referenced protocols tree!
    for (const auto protocol: protocols) {
      llvm::outs() << "entering recursively in " << protocol->getName() << '\n';
      const auto referencedProtocols = protocol->protocols();
      const auto memberInReferencedProtocol = getMemberForProtocols<Result>(referencedProtocols, filter, protocolExtractor);
      
      if (memberInReferencedProtocol != Result()) {
        return memberInReferencedProtocol;
      }
    }
    
    return Result();
  }
  
  // FIXME: this method is way too big! Split it in small pieces!
  template<
    typename Result,
    typename Filter,
    typename InterfaceExtractor,
    typename CategoryExtractor, 
    typename ProtocolExtractor>
  auto getMemberForObjectInterfaceType(const clang::ObjCInterfaceType* interfaceType,
                       Filter filter,
                       InterfaceExtractor interfaceExtractor,
                       CategoryExtractor categoryExtractor,
                       ProtocolExtractor protocolExtractor) -> Result
  {
    const auto propertyInterfaceDecl = interfaceType->getDecl();
    const auto categories = propertyInterfaceDecl->known_categories();
    const auto membersInInterface = interfaceExtractor(propertyInterfaceDecl);
    
    // FIXME: there's no need for building the list of pairs!
    auto pairs = std::vector<std::pair<decltype(membersInInterface.begin()), decltype(membersInInterface.end())>>{};
    pairs.emplace_back(membersInInterface.begin(), membersInInterface.end());
    
    for (const auto& category: categories) {
      const auto membersInCategory = categoryExtractor(category);
      pairs.emplace_back(membersInCategory.begin(), membersInCategory.end());
    }
    
    for (const auto pairIt: pairs) {
      const auto it = std::find_if(pairIt.first, pairIt.second, filter);
  
      if (it != pairIt.second) {
        return *it;
      }
    }
    
    const auto protocols = propertyInterfaceDecl->protocols();
    
    // For sure it is in some protocol!
    const auto memberInProtocols = getMemberForProtocols<Result>(protocols, filter, protocolExtractor);
    
    if (memberInProtocols != Result()) {
      return memberInProtocols;
    }
    
    const auto superClassType = propertyInterfaceDecl->getSuperClassType();
    
    const auto isRootClass = (superClassType == nullptr);
    
    if (isRootClass) {
      llvm::outs() << "Could not find something in " << propertyInterfaceDecl->getName() << " neither on it's parent :-(\n";
      return Result();
    }
    
    // Look for the member in the parent, in case we are not in a root class (NSObject or similar)
    // It's recursive, but as the class hierarchy is not deep (what are you doing?!),
    // there is no problems of stack overflow
    return getMemberForObjectInterfaceType<Result>(llvm::dyn_cast<clang::ObjCInterfaceType>(superClassType),
                                                  filter, interfaceExtractor, categoryExtractor, protocolExtractor);
  }
  
  template<
    typename Result,
    typename Filter,
    typename InterfaceExtractor,
    typename CategoryExtractor, 
    typename ProtocolExtractor>
  auto getMemberForObjectType(const clang::ObjCObjectPointerType* pointerType,
                       Filter filter,
                       InterfaceExtractor interfaceExtractor,
                       CategoryExtractor categoryExtractor,
                       ProtocolExtractor protocolExtractor) -> Result
  {
    const auto type = pointerType->getObjectType();
    
    llvm::outs() << "Dumping type \n";
    type->dump();
    
    const auto resultInInterface = [=] {
      if (const auto interfaceType = llvm::dyn_cast<clang::ObjCInterfaceType>(type)) {
        return getMemberForObjectInterfaceType<Result>(interfaceType, filter, interfaceExtractor, categoryExtractor, protocolExtractor);
      }

      return Result();
    }();
    
    if (resultInInterface != nullptr) {
      return resultInInterface;
    }
    
    assert(!type->isObjCClass() && "FIXME: Sorry, not support for properties of type Class yet :-(");
   
    // Could not find anything in the property interface type or any of its extensions or protocols.
    // Let's now look at the protocols specified in the property type
    // e.g: in `@property NSString<Prot1, Prot2>* prop;` look at Prot1 and Prot2
    for (auto i = 0u, numberOfProtocols = pointerType->getNumProtocols(); i < numberOfProtocols; i++) {
      const auto protocol = pointerType->getProtocol(i);
      
      // FIXME: this is copy&paste from above
      const auto membersInProtocol = protocolExtractor(protocol);
      
      const auto it = std::find_if(membersInProtocol.begin(), membersInProtocol.end(), filter);
      
      if (it != membersInProtocol.end()) {
        return *it;
      }
    }
    
    return nullptr;
  }
  
  template<typename Extractor>
  auto getSelectorForInterface(const clang::ObjCObjectPointerType* type, const StringRef selectorName, Extractor extractor) -> clang::ObjCMethodDecl*
  {
    const auto filter = [=](const clang::ObjCMethodDecl* methodDecl) {
      llvm::outs() << "Comparing " << methodDecl->getSelector().getAsString() << " with " << selectorName << '\n';
      return methodDecl->getSelector().getAsString() == selectorName;
    };
    
    return getMemberForObjectType<clang::ObjCMethodDecl*>(type, filter, extractor, extractor, extractor);
  }
  
  auto getInstanceSelectorForObjectType(const clang::ObjCObjectPointerType* type, const StringRef selectorName) -> clang::ObjCMethodDecl*
  {
    const auto extractor = [](const clang::ObjCContainerDecl* container) {
      return container->instance_methods();
    };
    
    return getSelectorForInterface(type, selectorName, extractor);
  }
  
  auto getClassSelectorForObjectType(const clang::ObjCObjectPointerType* type, const StringRef selectorName) -> clang::ObjCMethodDecl*
  {
    const auto extractor = [](const clang::ObjCContainerDecl* container) {
      return container->class_methods();
    };
    
    return getSelectorForInterface(type, selectorName, extractor);
  }
  
  template<typename Extractor>
  auto getPropertyForObjectType(const clang::ObjCObjectPointerType* type, const StringRef propertyName, Extractor extractor) -> clang::ObjCPropertyDecl*
  {
    const auto filter = [=](const clang::ObjCPropertyDecl* propertyDecl) {
      llvm::outs() << "comparing " << propertyDecl->getName() << " with " << propertyName << '\n';
      return propertyDecl->getName() == propertyName;
    };
    
    return getMemberForObjectType<clang::ObjCPropertyDecl*>(type, filter, extractor, extractor, extractor);
  }
  
  auto getInstancePropertyForObjectType(const clang::ObjCObjectPointerType* type, const StringRef propertyName) -> clang::ObjCPropertyDecl*
  {
    const auto extractor = [](const clang::ObjCContainerDecl* container) {
      return container->instance_properties();
    };
    
    return getPropertyForObjectType(type, propertyName, extractor);
  }
  
  auto getClassPropertyForObjectType(const clang::ObjCObjectPointerType* type, const StringRef propertyName) -> clang::ObjCPropertyDecl*
  {
    const auto extractor = [](const clang::ObjCContainerDecl* container) {
      return container->class_properties();
    };
    
    return getPropertyForObjectType(type, propertyName, extractor);
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
    
  auto getPropertyPointerType(clang::ObjCPropertyDecl* o)
  {
    return llvm::dyn_cast<clang::ObjCObjectPointerType>(o->getTypeSourceInfo()->getType().getTypePtrOrNull());
  }
  
  auto formatObjectType(const clang::ObjCObjectType* type) -> std::string
  {
    // An interface with no protocols or generic types
    if (const auto interfaceType = llvm::dyn_cast<clang::ObjCInterfaceType>(type)) {
      return interfaceType->getDecl()->getName();
    }
    
    const auto objType = llvm::dyn_cast<clang::ObjCObjectType>(type);
    
    if (objType == nullptr) {
      assert(false && "Unknown type!!!");
      return "";
    }
    
    const auto interface = objType->getInterface();
    
    if (interface != nullptr) {
      return std::string{interface->getName()} + "_FIXME_EXTRACT_PROTOCOL_AND_GENERIC_ARGUMENTS";
    }
    
    assert(type->isObjCId() && "This type should be id!!!!");
    
    return "id_FIXME_EXTRACT_PROTOCOL_ARGUMENTS";
  }
  
  auto generateCodeForInstanceMethod(CodeGeneratorContext& context, clang::ObjCPropertyDecl* propertyDecl, ProvidedItem item) -> void
  {
    const auto selectorName = item.value;
    const auto type = getPropertyPointerType(propertyDecl);
    const auto selectorInProperty = getInstanceSelectorForObjectType(type, selectorName);
    assert(selectorInProperty != nullptr);
    const auto selectorSignature = generateSelectorSignature(selectorInProperty);
    context.headerStream << selectorSignature << ";\n\n";
    const auto memberDef = llvm::formatv(objcInstanceSelectorForwardingFormat, propertyDecl->getName());
    context.implStream << generateSelectorDefinition(selectorInProperty, selectorSignature, memberDef);
  }
  
  auto generateCodeForClassMethod(CodeGeneratorContext& context, clang::ObjCPropertyDecl* propertyDecl, ProvidedItem item) -> void
  {
    const auto selectorName = item.value;
    const auto type = getPropertyPointerType(propertyDecl);
    const auto selectorInProperty = getClassSelectorForObjectType(type, selectorName);
    assert(selectorInProperty != nullptr);
    const auto selectorSignature = generateSelectorSignature(selectorInProperty);
    context.headerStream << selectorSignature << ";\n\n";
    const auto memberDef = llvm::formatv(objcClassSelectorForwardingFormat, formatObjectType(type->getObjectType()));
    context.implStream << generateSelectorDefinition(selectorInProperty, selectorSignature, memberDef);
  }
  
  auto generateCodeForProperty(CodeGeneratorContext& context, clang::ObjCPropertyDecl* propertyDecl, ProvidedItem item) -> void
  {
    const auto memberPropertyName = item.value;
    const auto type = getPropertyPointerType(propertyDecl);
    
    const auto propertyDeclInMember = [&]() -> clang::ObjCPropertyDecl* {
      if (const auto instanceProperty = getInstancePropertyForObjectType(type, memberPropertyName)) {
        return instanceProperty;
      }
    
      return getClassPropertyForObjectType(type, memberPropertyName);
    }();
    
    assert(propertyDeclInMember != nullptr);
    
    const auto locStart = propertyDeclInMember->getLocStart();
    // FIXME: locEnd is pointing to the beginning of the property name :-(
    const auto locEnd = propertyDeclInMember->getLocEnd();
    
    const auto codeBegin = context.compilerInstance->getSourceManager().getCharacterData(locStart);
    const auto codeEnd = context.compilerInstance->getSourceManager().getCharacterData(locEnd);
    const auto propertySignature = llvm::StringRef(codeBegin, codeEnd - codeBegin);
    
    llvm::outs() << "Property signature: " << propertySignature << '\n';
    
    context.headerStream << propertySignature;
    context.headerStream << propertyDeclInMember->getName() << ";\n\n";
    
    const auto f = [&]() -> std::string {
      if (propertyDeclInMember->isInstanceProperty()) {
        return llvm::formatv(objcInstanceSelectorForwardingFormat, propertyDeclInMember->getName());
      }
      
      return llvm::formatv(objcClassSelectorForwardingFormat, formatObjectType(type->getObjectType()));
    }();
    
    if (auto getterMethodDecl = propertyDeclInMember->getGetterMethodDecl()) {
      const auto selectorSignature = generateSelectorSignature(getterMethodDecl);
      context.implStream << generateSelectorDefinition(getterMethodDecl, selectorSignature, f);
    }
    
    if (auto setterMethodDecl = propertyDeclInMember->getSetterMethodDecl()) {
      const auto selectorSignature = generateSelectorSignature(setterMethodDecl);
      context.implStream << generateSelectorDefinition(setterMethodDecl, selectorSignature, f);
    }
  }
  
  using ProvidedItemGenerator = std::add_pointer<void(CodeGeneratorContext&, clang::ObjCPropertyDecl*, ProvidedItem)>::type;
  
  const std::map<ProvidedItemType, ProvidedItemGenerator> generators {
    {ProvidedItemType::InstanceMethod, generateCodeForInstanceMethod},
    {ProvidedItemType::ClassMethod, generateCodeForClassMethod},
    {ProvidedItemType::Property, generateCodeForProperty},
  };
 
  // FIXME: no needs to say this function is way too large, doing too much and with a lot of copy&paste, right?
  auto generateExtension(clang::ObjCPropertyDecl* o,
                         CodeGeneratorContext& context,
                         const std::vector<clang::AnnotateAttr*>& attrs,
                         const KnownDeclarations& knownDeclarations) -> void
  {
    const auto providedItems = extractProvidedItems(attrs);

    if (providedItems.empty()) {
      return;
    }
    
    const auto parent = getParent(context, o);
    
    assert(parent != nullptr);
    
    const auto interfaceDecl = llvm::dyn_cast<clang::ObjCInterfaceDecl>(parent);
    
    assert(interfaceDecl != nullptr);
    
    for (const auto& attr: attrs) {
      const auto annotationValue = attr->getAnnotation();
      llvm::outs() << "Found a property annotation!!! " << annotationValue << '\n';
      o->dump();
    }
    
    const auto propertyType = getPropertyPointerType(o);
    
    assert(propertyType != nullptr);
    
    const auto formattedInterfaceName = formatObjectType(propertyType->getObjectType());
    
    context.headerStream << llvm::formatv(objCCategoryHeaderFormatBegin,
                                              interfaceDecl->getName(),
                                              formattedInterfaceName,
                                              o->getName());
    
    context.implStream << llvm::formatv(objCCategoryImplementationFormatBegin,
                                                      interfaceDecl->getName(),
                                                      formattedInterfaceName,
                                                      o->getName());
    
    for (const auto item: providedItems) {
      assert(item.type != ProvidedItemType::Unknown && "FIXME: handle error");
      const auto generator = generators.at(item.type);
      llvm::outs() << "Processing item: " << item.value << '\n';
      generator(context, o, item);
    }
    
    context.headerStream << objcDeclarationEnd;
    context.implStream << objcDeclarationEnd;
  }
}

struct ObjCVisitor: public clang::RecursiveASTVisitor<ObjCVisitor>
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
      
      generateExtension(o, _context, provideAnnotationAttrs, _knownDeclarations);
      
      return true;
    }, o);
  }
};

struct ObjCASTConsumer: public clang::ASTConsumer
{
  KnownDeclarations _knownDeclarations;
  ObjCVisitor _visitor;

  ObjCASTConsumer(CodeGeneratorContext& context):
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

namespace {
  llvm::cl::OptionCategory commandLineCategory{"Mixin With Steroids Options"};
  
  llvm::cl::opt<std::string> headerFilename{"header-file",
    llvm::cl::desc("Generated header file"),
    llvm::cl::cat(commandLineCategory)};
    
  llvm::cl::opt<std::string> implementationFilename{"implementation-file",
    llvm::cl::desc("Generated implementation file"),
    llvm::cl::cat(commandLineCategory)};
    
  // FIXME: this is a huge workaround for not being able to get all the files passed on the command line
  std::vector<std::string> sourcePathList;
  bool isFirstParse = true;
}

// For each source file provided to the tool, a new FrontendAction is created.
struct MyFrontendAction: public clang::ASTFrontendAction
{
  std::unique_ptr<llvm::raw_fd_ostream> _headerStream;
  std::unique_ptr<llvm::raw_fd_ostream> _implStream;
  
  std::unique_ptr<CodeGeneratorContext> _codeGeneratorContext;
  
  MyFrontendAction()
  {
  }
  
  virtual ~MyFrontendAction()
  {
  }
  
  auto BeginSourceFileAction(clang::CompilerInstance &compilerInstance, llvm::StringRef file) -> bool final
  {
    std::error_code error;
    
    _headerStream = std::make_unique<llvm::raw_fd_ostream>(headerFilename, error, llvm::sys::fs::F_RW);
    assert(!error && "You must pass -header-file command line argument");
    
    _implStream = std::make_unique<llvm::raw_fd_ostream>(implementationFilename, error, llvm::sys::fs::F_RW);
    assert(!error && "You must pass -implementation-file command line argument");
    
    _codeGeneratorContext = std::make_unique<CodeGeneratorContext>(&compilerInstance, file, *_headerStream.get(), *_implStream.get());
    
    if (isFirstParse) {
      isFirstParse = false;
      for (const auto& filePath: sourcePathList) {
        *_headerStream.get() << llvm::formatv(objCIncludeFileFormat, filePath);
      }
    }
    
    *_implStream.get() << llvm::formatv(objCIncludeFileFormat, headerFilename.getValue());
    
    return true;
  }
  
  auto CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) -> std::unique_ptr<clang::ASTConsumer> final
  {
    assert(_codeGeneratorContext);
    return llvm::make_unique<ObjCASTConsumer>(*_codeGeneratorContext.get());
  }
  
  auto EndSourceFileAction() -> void final
  {
  }
};

auto main(int argc, const char **argv) -> int
{
  auto op = clang::tooling::CommonOptionsParser(argc, argv, commandLineCategory);
  
  sourcePathList = op.getSourcePathList();
  
  auto tool = clang::tooling::ClangTool(op.getCompilations(), sourcePathList);
  
  return tool.run(clang::tooling::newFrontendActionFactory<MyFrontendAction>().get());
}

