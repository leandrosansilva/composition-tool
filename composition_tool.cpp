//------------------------------------------------------------------------------
//
// Based in a public domain work of Eli Bendersky (eliben@gmail.com)
// https://github.com/eliben/llvm-clang-samples/
//
//------------------------------------------------------------------------------

#include <sstream>
#include <string>

#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"

namespace {
  const llvm::StringRef PROVIDE_TAG("__provide__");

  struct KnownDeclarations
  {
    template<typename T>
    using LookupByName = std::map<llvm::StringRef, T>;

    LookupByName<clang::ObjCInterfaceDecl*> interfaces;
    LookupByName<clang::ObjCProtocolDecl*> protocols;

    // More than one category for an interface can be declared
    LookupByName<std::vector<clang::ObjCCategoryDecl*>> categories;

    // a class can obviously have more than one selector,
    LookupByName<std::vector<clang::ObjCMethodDecl*>> instanceMethods;
    LookupByName<std::vector<clang::ObjCMethodDecl*>> classMethods;
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
                         clang::Rewriter& rewriter,
                         clang::ASTContext& context,
                         const std::vector<clang::AnnotateAttr*>& attrs,
                         const KnownDeclarations& knownDeclarations) -> void
  {
    const auto providedItems = extractProvidedItems(attrs);
    
    const auto parent = [&]() -> const clang::ObjCInterfaceDecl* {
      for (const auto p: context.getParents(*o)) {
        const auto interfaceParent = p.get<clang::ObjCInterfaceDecl>();
        const auto implementationParent = p.get<clang::ObjCImplementationDecl>();
        
        if (interfaceParent != nullptr) {
          return interfaceParent;
        }
      }
      
      return nullptr;
    }();
    
    llvm::outs() << "property type!!!" << '\n';
    
    // TODO: find all the protocols the type implements.
    // The interface type is just a "plus" for the property type
    
    const auto interfaceDecl = [=]() -> clang::ObjCInterfaceDecl* {
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
    
    assert(interfaceDecl != nullptr);
    
    if (interfaceDecl != nullptr) {
      llvm::outs() << "property type: " << interfaceDecl->getName() << '\n';
    }
    
    for (const auto& attr: attrs) {
      const auto annotationValue = attr->getAnnotation();
      llvm::outs() << "Found a property annotation!!! " << annotationValue << '\n';
      //o->dump();
    }
    
    const auto instanceSelectors = knownDeclarations.instanceMethods.find(interfaceDecl->getName());

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

static llvm::cl::OptionCategory ToolingSampleCategory("Mixin With Steroids");

struct ObjCVisitor : public clang::RecursiveASTVisitor<ObjCVisitor>
{
  clang::Rewriter& _rewriter;
  clang::ASTContext& _context;
  KnownDeclarations& _knownDeclarations;

  template<typename F, typename D>
  auto visit(F function, D decl) const -> bool
  {
    return ::runIfInMainFile(_context, function, decl);
  }
  
  ObjCVisitor(clang::Rewriter &R, clang::ASTContext& context, KnownDeclarations& knownDeclarations):
  _rewriter(R),
  _context(context),
  _knownDeclarations(knownDeclarations)
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

    const auto parents = _context.getParents(*s);

    if (parents.empty()) {
      llvm::outs() << "  Method with no parent!: " << s->getSelector().getAsString() << '\n';
    }

    for (const auto p: parents) {
      const auto interfaceParent = p.get<clang::ObjCInterfaceDecl>();
      const auto implementationParent = p.get<clang::ObjCImplementationDecl>();
      const auto protocolParent = p.get<clang::ObjCProtocolDecl>();
      const auto categoryParent = p.get<clang::ObjCCategoryDecl>();

      assert(interfaceParent != nullptr || protocolParent != nullptr
             || categoryParent != nullptr || implementationParent != nullptr);
      
      if (interfaceParent != nullptr) {
        llvm::outs() << "  interface parent: " << interfaceParent->getName() << '\n';
      }
      
      if (protocolParent != nullptr) {
        llvm::outs() << "  protocol parent: " << protocolParent->getName() << '\n';
      }

      if (categoryParent != nullptr) {
        llvm::outs() << "  category parent: " << categoryParent->getName() << '\n';
      }

      if (implementationParent != nullptr) {
        llvm::outs() << "  implementation parent: " << implementationParent->getName() << '\n';
      }
    }
    
    if (interface != nullptr) {
      llvm::outs() << "  selector interface: " << interface->getName() << '\n';
    } else {
      llvm::outs() << "  selector has no interface" << '\n';
    }
    
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

      generateExtension(o, _rewriter, _context, provideAnnotationAttrs, _knownDeclarations);

      return true;
    }, o);
  }
};

struct ObjCASTConsumer : public clang::ASTConsumer
{
  KnownDeclarations _knownDeclarations;
  ObjCVisitor _visitor;

  ObjCASTConsumer(clang::Rewriter &rewriter, clang::ASTContext& context):
  _visitor({rewriter, context, _knownDeclarations})
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
  clang::Rewriter _rewriter;

  MyFrontendAction()
  {
  }
  
  auto EndSourceFileAction() -> void final 
  {
    return;
    
    auto &srcManager = _rewriter.getSourceMgr();
    
    llvm::errs() << "** EndSourceFileAction for: "
                 << srcManager.getFileEntryForID(srcManager.getMainFileID())->getName() << "\n";

    _rewriter.getEditBuffer(srcManager.getMainFileID()).write(llvm::outs());
  }

  auto CreateASTConsumer(clang::CompilerInstance &compilerInstance, llvm::StringRef file) -> std::unique_ptr<clang::ASTConsumer> final
  {
    llvm::errs() << "** Creating AST consumer for: " << file << "\n";
    _rewriter.setSourceMgr(compilerInstance.getSourceManager(), compilerInstance.getLangOpts());
    
    auto& context = compilerInstance.getASTContext();
    
    return llvm::make_unique<ObjCASTConsumer>(_rewriter, context);
  }
};

auto main(int argc, const char **argv) -> int
{
  auto op = clang::tooling::CommonOptionsParser(argc, argv, ToolingSampleCategory);
  auto tool = clang::tooling::ClangTool(op.getCompilations(), op.getSourcePathList());

  return tool.run(clang::tooling::newFrontendActionFactory<MyFrontendAction>().get());
}
