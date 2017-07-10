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
                         const std::vector<clang::AnnotateAttr*>& attrs) -> void
  {
    const auto provided = extractProvidedItems(attrs);
    
    for (const auto& item: provided) {
      llvm::outs() << "item: " << item << '\n';
    }
    
    const auto typeInfo = o->getTypeSourceInfo();
    const auto type = typeInfo->getType().getTypePtrOrNull();

    if (auto objcObjectType = llvm::dyn_cast<clang::ObjCObjectPointerType>(type)) {
      llvm::outs() << "Found a object property!!! " << objcObjectType->getTypeClassName() << '\n';
    }

    for (const auto& attr: attrs) {
      const auto annotationValue = attr->getAnnotation();
      llvm::outs() << "Found a property annotation!!! " << annotationValue << '\n';
      o->dump();
    }
  }
}

static llvm::cl::OptionCategory ToolingSampleCategory("Mixin With Steroids");

struct ObjCVisitor : public clang::RecursiveASTVisitor<ObjCVisitor>
{
  clang::Rewriter& _rewriter;
  clang::ASTContext& _context;
  
  template<typename T>
  using LookupByName = std::map<llvm::StringRef, T>;
  
  LookupByName<clang::ObjCInterfaceDecl*> _knownInterfaceDecls;
  LookupByName<clang::ObjCProtocolDecl*> _knownPrococolDecls;
  
  // More than one category for an interface can be declared
  LookupByName<std::vector<clang::ObjCCategoryDecl*>> _knownCategoryDecls;
  
  LookupByName<std::vector<clang::ObjCMethodDecl*>> _knownInstanceSelectors;
  LookupByName<std::vector<clang::ObjCMethodDecl*>> _knownClassSelectors;
  
  template<typename F, typename D>
  auto visit(F function, D decl) const -> bool
  {
    return ::runIfInMainFile(_context, function, decl);
  }
  
  ObjCVisitor(clang::Rewriter &R, clang::ASTContext& context):
  _rewriter(R),
  _context(context)
  {
  }

  auto VisitObjCInterfaceDecl(clang::ObjCInterfaceDecl* o) -> bool
  {
    _knownInterfaceDecls[o->getName()] = o;
    return true;
  }
  
  auto VisitObjCProtocolDecl(clang::ObjCProtocolDecl* o) -> bool
  {
    _knownPrococolDecls[o->getName()] = o;
    return true;
  }
  
  auto VisitObjCMethodDecl(clang::ObjCMethodDecl* s) -> bool
  {
    s->isInstanceMethod();
    
    const auto parents = _context.getParents(*s);
    
    for (const auto p: parents) {
      const auto interfaceParent = p.get<clang::ObjCInterfaceDecl>();
      const auto protocolParent = p.get<clang::ObjCProtocolDecl>();
      
      assert(interfaceParent != nullptr || protocolParent != nullptr);
      
      if (interfaceParent != nullptr) {
        llvm::outs() << "LALALA interface parent: " << interfaceParent->getName() << '\n';
      }
      
      if (protocolParent != nullptr) {
        llvm::outs() << "LALALA interface parent: " << protocolParent->getName() << '\n';
      }
    }
    
    if (parents.empty()) {
      llvm::outs() << "Method with no parent: " << s->getSelector().getAsString() << '\n';
    }
    
    const auto interface = s->getClassInterface();
    
    if (interface != nullptr) {
      llvm::outs() << "selector interface: " << interface->getName() << '\n';
    }
    
    //_knownSelectors[interface->getName()].push_back(s);
    return true;
  }
    
  auto VisitObjCCategoryDecl(clang::ObjCCategoryDecl* o) -> bool
  {
    const auto interface = o->getClassInterface();
    //llvm::outs() << "LALALA category decl: " << o->getName() << " for interface: " << interface->getName() << '\n';
    _knownCategoryDecls[interface->getName()].push_back(o);
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
      
      llvm::outs() << "Found a object property!!! " << objcObjectType->getTypeClassName() 
                   << ":" << o->getName() << '\n';

      auto provideAnnotationAttrs = std::vector<clang::AnnotateAttr*>{};

      // I know, I know, but copy_if does not cast, so a simple for does the job
      for (auto& attr: o->getAttrs()) {
        auto annotateAttr = llvm::dyn_cast<clang::AnnotateAttr>(attr);

        if (annotateAttr != nullptr && annotateAttr->getAnnotation().startswith(PROVIDE_TAG)) {
          provideAnnotationAttrs.push_back(annotateAttr);
        }
      }

      generateExtension(o, _rewriter, _context, provideAnnotationAttrs);

      return true;
    }, o);
  }
};

struct ObjCASTConsumer : public clang::ASTConsumer
{
  ObjCVisitor _visitor;
  
  ObjCASTConsumer(clang::Rewriter &rewriter, clang::ASTContext& context):
  _visitor({rewriter, context})
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

  clang::Rewriter _rewriter;
};

auto main(int argc, const char **argv) -> int
{
  auto op = clang::tooling::CommonOptionsParser(argc, argv, ToolingSampleCategory);
  auto tool = clang::tooling::ClangTool(op.getCompilations(), op.getSourcePathList());

  return tool.run(clang::tooling::newFrontendActionFactory<MyFrontendAction>().get());
}
