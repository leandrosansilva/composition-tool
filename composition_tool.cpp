//------------------------------------------------------------------------------
//
// Based in a public domain work of Eli Bendersky (eliben@gmail.com)
// https://github.com/eliben/llvm-clang-samples/
//
//------------------------------------------------------------------------------

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
#pragma once
#include "{0}"

@interface {1} ({2}__{3})

@end
)+-+";

  // TODO: implement!!!!
  const auto objCCategoryImplementationFormat = R"+-+(
#include "{0}"

@implementation {1} ({2}__{3})

@end

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

    // More than one category for an interface can be declared
    LookupByInterfaceName<std::vector<clang::ObjCCategoryDecl*>> categories;

    // a class can obviously have more than one selector,
    LookupByInterfaceName<std::vector<clang::ObjCMethodDecl*>> instanceMethods;
    LookupByInterfaceName<std::vector<clang::ObjCMethodDecl*>> classMethods;
  };

  struct GeneratedCodeForInterface
  {
    GeneratedCodeForInterface(const GeneratedCodeForInterface&) = default;
    GeneratedCodeForInterface(GeneratedCodeForInterface&&) = default;
    auto operator=(const GeneratedCodeForInterface&) -> GeneratedCodeForInterface& = default;

    std::string _buffer;
    clang::Rewriter _rewriter;
  };

  // TODO: rename it to manager? I hate manager suffixes!
  struct GeneratedCodeForInterfaces
  {
    // interface name, property name
    using PropertyPair = std::pair<llvm::StringRef, llvm::StringRef>;
    std::map<PropertyPair, GeneratedCodeForInterface> _storage;

    auto getContext(const clang::ObjCInterfaceDecl* interfaceDecl,
                    clang::ObjCPropertyDecl* propertyDecl,
                    clang::ObjCInterfaceDecl* propertyTypeDecl,
                    CodeGeneratorContext& context) -> GeneratedCodeForInterface&
    {
      const auto interfaceName = interfaceDecl->getName();
      const auto propertyName = propertyDecl->getName();
      const auto propertyTypeName = propertyTypeDecl->getName();

      const auto pair = std::make_pair(interfaceName, propertyName);

      const auto found = _storage.find(pair);

      if (found != std::end(_storage)) {
        return found->second;
      }
      
      const auto formattedHeader = llvm::formatv(objCCategoryHeaderFormat,
                                                 context.inputFilename,
                                                 interfaceName,
                                                 propertyName,
                                                 propertyTypeName);
      
      llvm::outs() << "output header: " << formattedHeader << '\n';
      
      _storage.emplace(pair, GeneratedCodeForInterface{
        formattedHeader, 
        clang::Rewriter{context.compilerInstance->getSourceManager(),
          context.compilerInstance->getLangOpts()}});

      return _storage.at(pair);
    }
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

  auto generateExtension(clang::ObjCPropertyDecl* o,
                         CodeGeneratorContext& context,
                         const std::vector<clang::AnnotateAttr*>& attrs,
                         const KnownDeclarations& knownDeclarations,
                         GeneratedCodeForInterfaces& genManager) -> void
  {
    const auto providedItems = extractProvidedItems(attrs);

    if (providedItems.empty()) {
      return;
    }
    
    auto& astContext = context.compilerInstance->getASTContext();
    
    const auto interfaceDecl = [&]() -> const clang::ObjCInterfaceDecl* {
      for (const auto p: astContext.getParents(*o)) {
        const auto interfaceParent = p.get<clang::ObjCInterfaceDecl>();
        const auto implementationParent = p.get<clang::ObjCImplementationDecl>();
        
        if (interfaceParent != nullptr) {
          return interfaceParent;
        }
      }
      
      return nullptr;
    }();

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
    
    const auto& generationContext = genManager.getContext(interfaceDecl, o, propertyInterfaceDecl, context);
    
    assert(propertyInterfaceDecl != nullptr);
    
    if (propertyInterfaceDecl != nullptr) {
      llvm::outs() << "property type: " << propertyInterfaceDecl->getName() << '\n';
    }
    
    for (const auto& attr: attrs) {
      const auto annotationValue = attr->getAnnotation();
      llvm::outs() << "Found a property annotation!!! " << annotationValue << '\n';
      //o->dump();
    }
    
    const auto instanceSelectors = knownDeclarations.instanceMethods.find(propertyInterfaceDecl->getName());

    // FIXME: error handling
    assert(instanceSelectors != knownDeclarations.instanceMethods.end());

    for (const auto item: providedItems) {
      // instance methods (FIXME: will fail with wildcard -*)
      if (item.startswith("-")) {
        const auto selectorName = item.substr(1);

        llvm::outs() << "Processing item: " << selectorName << '\n';

        const auto selectorInPropertyIt = std::find_if(std::begin(instanceSelectors->second),
                                                     std::end(instanceSelectors->second),
                                                     [=](const clang::ObjCMethodDecl* methodDecl) -> bool {
                                                       return methodDecl->getSelector().getAsString() == selectorName;
                                                     });

        assert(selectorInPropertyIt != std::end(instanceSelectors->second));
        
        const auto selectorInProperty = *selectorInPropertyIt;
        
        const auto locStart = selectorInProperty->getLocStart();
        const auto locEnd = selectorInProperty->getDeclaratorEndLoc();
        
        const auto buffer = context.compilerInstance->getSourceManager().getBuffer(context.compilerInstance->getSourceManager().getMainFileID());
        
        //clang::Lexer::getLocForEndOfToken(locEnd)
        
        const auto codeBegin = context.compilerInstance->getSourceManager().getCharacterData(locStart);
        const auto codeEnd = context.compilerInstance->getSourceManager().getCharacterData(locEnd);
        
        const auto c = llvm::StringRef(codeBegin, codeEnd - codeBegin);
        
        llvm::outs() << "Selector code: " << c << '\n';
        
        selectorInProperty->dump();
      }
    }

    llvm::outs() << "known interfaces: " << knownDeclarations.interfaces.size() << '\n';
    llvm::outs() << "known protocols: " << knownDeclarations.protocols.size() << '\n';
    llvm::outs() << "known categories: " << knownDeclarations.categories.size() << '\n';
    llvm::outs() << "known classes with instance methods: " << knownDeclarations.instanceMethods.size() << '\n';
    llvm::outs() << "known classes with class methods: " << knownDeclarations.classMethods.size() << '\n';
  }
}

static llvm::cl::OptionCategory commandLineCategory("Mixin With Steroids");

struct ObjCVisitor : public clang::RecursiveASTVisitor<ObjCVisitor>
{
  CodeGeneratorContext _context;
  KnownDeclarations& _knownDeclarations;
  GeneratedCodeForInterfaces& _genManager;

  template<typename F, typename D>
  auto visit(F function, D decl) const -> bool
  {
    return ::runIfInMainFile(_context.compilerInstance->getASTContext(), function, decl);
  }
  
  ObjCVisitor(CodeGeneratorContext context,
              KnownDeclarations& knownDeclarations,
              GeneratedCodeForInterfaces& genManager):
  _context(context),
  _knownDeclarations(knownDeclarations),
  _genManager(genManager)
  {
  }

  auto VisitObjCInterfaceDecl(clang::ObjCInterfaceDecl* o) -> bool
  {
    visit([](clang::ObjCInterfaceDecl* o) {
      llvm::outs() << "New interface: " << o->getName() << '\n';
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

    const auto interface = s->getClassInterface();

    if (interface == nullptr) {
      return true;
    }

    auto& knownSelectors = s->isInstanceMethod()
                           ? _knownDeclarations.instanceMethods
                           : _knownDeclarations.classMethods;

    knownSelectors[interface->getName()].push_back(s);

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

      generateExtension(o, _context, provideAnnotationAttrs, _knownDeclarations, _genManager);

      return true;
    }, o);
  }
};

struct ObjCASTConsumer : public clang::ASTConsumer
{
  KnownDeclarations _knownDeclarations;
  ObjCVisitor _visitor;
  GeneratedCodeForInterfaces _genManager;

  ObjCASTConsumer(CodeGeneratorContext context):
  _visitor({context, _knownDeclarations, _genManager})
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

