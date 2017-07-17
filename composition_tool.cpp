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

  const auto objCCategoryHeaderFormat = R"+-+(
@interface {0} ({1}__{2})

{3}

@end
)+-+";

  const auto objCCategoryImplementationFormat = R"+-+(
@implementation {0} ({1}__{2})
{3}
@end
)+-+";

  const auto objCSelectorImplementationFormat = R"+-+(
{0} {
  return [{1}{2} {3}];
}
)+-+";

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
    LookupByInterfaceName<clang::ObjCProtocolDecl*> protocols;
    LookupByInterfaceName<clang::ObjCInterfaceDecl*> parent;

    // More than one category for an interface can be declared
    LookupByInterfaceName<std::vector<clang::ObjCCategoryDecl*>> categories;
  };

  template<typename F, typename D>
  auto runIfInMainFile(clang::ASTContext& context, F function, D decl) -> bool
  {
    if (context.getSourceManager().isInMainFile(decl->getLocStart())) {
      return function(decl);
    }
    
    return true;
  }
  
  // FIXME: why 10? Can it grow? OMG, 
  auto extractProvidedItems(const std::vector<clang::AnnotateAttr*>& attrs) -> llvm::SmallVector<llvm::StringRef, 10>
  {
    auto items = llvm::SmallVector<llvm::StringRef, 10>();
    
    // TODO: handle errors on parsing provided items?
    for (const auto attr: attrs) {
      attr->getAnnotation().trim().substr(PROVIDE_TAG.size()).split(items, " ", -1, false);
    }
    
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

  auto getSelectorForInterface(clang::ObjCInterfaceDecl* propertyInterfaceDecl, const StringRef selectorName) -> clang::ObjCMethodDecl*
  {
    const auto methods = propertyInterfaceDecl->instance_methods();

    const auto itInMainInterfaceDecl = std::find_if(methods.begin(), methods.end(),
                                               [=](const clang::ObjCMethodDecl* methodDecl) -> bool {
                                                 return methodDecl->getSelector().getAsString() == selectorName;
                                               });

    if (itInMainInterfaceDecl != methods.end()) {
      return *itInMainInterfaceDecl;
    }

    // Search on the known categories
    const auto categories = propertyInterfaceDecl->known_categories();

    // TODO: refactor this for to reuse the code above (use a list of begin/end pairs and do a find_if?
    for (const auto& category: categories) {
      const auto methods = category->instance_methods();

      const auto itInCategoryDecl = std::find_if(methods.begin(), methods.end(),
                                               [=](const clang::ObjCMethodDecl* methodDecl) -> bool {
                                                 return methodDecl->getSelector().getAsString() == selectorName;
                                               });
      if (itInCategoryDecl != methods.end()) {
        return *itInCategoryDecl ;
      }
    }

    const auto protocols = propertyInterfaceDecl->protocols();

    // TODO: refactor this for to reuse the code above (use a list of begin/end pairs and do a find_if?
    for (const auto& protocol: protocols) {
      const auto methods = protocol->instance_methods();

      const auto itInProtocolDecl = std::find_if(methods.begin(), methods.end(),
                                               [=](const clang::ObjCMethodDecl* methodDecl) -> bool {
                                                 return methodDecl->getSelector().getAsString() == selectorName;
                                               });
      if (itInProtocolDecl != methods.end()) {
        return *itInProtocolDecl ;
      }
    }

    return nullptr;
  }

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

    // TODO: find all the protocols the type implements.
    // The interface type is just a "plus" for the property type
    
    const auto propertyInterfaceDecl = [=]() -> clang::ObjCInterfaceDecl* {
      const auto t = llvm::dyn_cast<clang::ObjCObjectPointerType>(o->getTypeSourceInfo()->getType().getTypePtrOrNull());
      
      assert(t != nullptr);
      
      t->dump();
      
      llvm::outs() << "lalala" << '\n';
      
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
    
    auto providedBodyForHeader = std::string{};
    auto providedBodyForImplementation = std::string{};
    
    for (const auto item: providedItems) {
      llvm::outs() << "Processing item: " << item << '\n';
      
      // instance methods (FIXME: will fail with wildcard -*)
      if (item.startswith("-")) {
        const auto selectorName = item.substr(1);

        const auto selectorInProperty = getSelectorForInterface(propertyInterfaceDecl, selectorName);
        
        assert(selectorInProperty != nullptr);

        const auto locStart = selectorInProperty->getLocStart();
        const auto locEnd = selectorInProperty->getLocEnd();
        
        const auto codeBegin = context.compilerInstance->getSourceManager().getCharacterData(locStart);
        const auto codeEnd = context.compilerInstance->getSourceManager().getCharacterData(locEnd);
        const auto c = llvm::StringRef(codeBegin, codeEnd - codeBegin);
        
        providedBodyForHeader += c;
        providedBodyForHeader += ";\n";

        llvm::outs() << "before params\n";

        const auto& parameters = selectorInProperty->parameters();

        const auto callBody = [&]() -> std::string {
          if (parameters.size() == 0) {
            return selectorName;
          }

          auto paramLabels = llvm::SmallVector<llvm::StringRef, 10>{};
          selectorName.split(paramLabels, ":", -1, false);

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
        }();

        const auto selectorDefinition = llvm::formatv(objCSelectorImplementationFormat,
                                                      c,
                                                      "self.",
                                                      o->getName(),
                                                      callBody);

        providedBodyForImplementation += selectorDefinition;

        llvm::outs() << "body: " << selectorDefinition << '\n';

        llvm::outs() << "after params\n";
      }
      
      //if (item.startswith("@")) {
      //  const auto propertyName = item.substr(1);

      //  const auto propertyDecl = [&]() -> clang::ObjCPropertyDecl* {
      //    const auto properties = propertyInterfaceDecl->properties();

      //  }();

      //  const auto propertyIt = std::find_if(std::begin(properties->second),
      //                                               std::end(properties->second),
      //                                               [=](const clang::ObjCPropertyDecl* propertyDecl) -> bool {
      //                                                 return propertyDecl->getName() == propertyName;
      //                                               });

      //  assert(propertyIt != std::end(properties->second));

      //  const auto propertyDecl = *propertyIt;

      //  assert(propertyDecl != nullptr);

      //  const auto locStart = propertyDecl->getLocStart();
      //  const auto locEnd = propertyDecl->getLocEnd();

      //  const auto codeBegin = context.compilerInstance->getSourceManager().getCharacterData(locStart);
      //  const auto codeEnd = context.compilerInstance->getSourceManager().getCharacterData(locEnd);
      //  const auto c = llvm::StringRef(codeBegin, codeEnd - codeBegin);

      //  llvm::outs() << "Property code: " << c << '\n';

      //  const auto getterMethodDecl = propertyDecl->getGetterMethodDecl();
      //  const auto setterMethodDecl = propertyDecl->getSetterMethodDecl();
      //}
    }
    
    const auto formattedHeader = llvm::formatv(objCCategoryHeaderFormat,
                                               interfaceDecl->getName(),
                                               propertyInterfaceDecl->getName(),
                                               o->getName(),
                                               providedBodyForHeader);

    const auto formattedImplementation = llvm::formatv(objCCategoryImplementationFormat,
                                                       interfaceDecl->getName(),
                                                       propertyInterfaceDecl->getName(),
                                                       o->getName(),
                                                       providedBodyForImplementation);
      
    llvm::outs() << "generated header: " << formattedHeader << '\n';
    llvm::outs() << "generated implementation: " << formattedImplementation << '\n';

    llvm::outs() << "known interfaces: " << knownDeclarations.interfaces.size() << '\n';
    llvm::outs() << "known protocols: " << knownDeclarations.protocols.size() << '\n';
    llvm::outs() << "known categories: " << knownDeclarations.categories.size() << '\n';
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
    llvm::outs() << "New interface: " << o->getName() << '\n';
    o->getLocation().print(llvm::outs(), _context.compilerInstance->getSourceManager());
    
    visit([](clang::ObjCInterfaceDecl* o) {
      return true;
    }, o);
    
    _knownDeclarations.interfaces[o->getName()] = o;
    return true;
  }
  
  auto VisitObjCProtocolDecl(clang::ObjCProtocolDecl* o) -> bool
  {
    _knownDeclarations.protocols[o->getName()] = o;
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
    _knownDeclarations.categories[interface->getName()].push_back(o);
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
    llvm::outs() << "Visiting property " << o->getName() << '\n';
    
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

      generateExtension(o, _context, provideAnnotationAttrs, _knownDeclarations);

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

