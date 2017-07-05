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
  template<typename F, typename D>
  auto runIfInMainFile(clang::ASTContext& context, F function, D decl) -> bool
  {
    if (context.getSourceManager().isInMainFile(decl->getLocStart())) {
      return function(decl);
    }
    
    return true;
  }
}

static llvm::cl::OptionCategory ToolingSampleCategory("Mixin With Steroids");

struct ObjCVisitor : public clang::RecursiveASTVisitor<ObjCVisitor>
{
  clang::Rewriter &_rewriter;
  clang::ASTContext& _context;
  
  template<typename F, typename D>
  auto runIfInMainFile(F function, D decl) const -> bool
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
    return runIfInMainFile([](clang::ObjCInterfaceDecl* o){
      llvm::outs() << "visiting interface decl " << o->getName() << '\n';
      return true; 
    }, o);
  }
  
  auto VisitObjCProtocolDecl(clang::ObjCProtocolDecl* o) -> bool
  {
    return runIfInMainFile([](clang::ObjCProtocolDecl* o){
      llvm::outs() << "visiting protocol decl " << o->getName() << '\n';
      return true;
    }, o);
  }
  
  auto VisitObjCImplementationDecl(clang::ObjCImplementationDecl* o) -> bool
  {
    return runIfInMainFile([](clang::ObjCImplementationDecl* o){
      llvm::outs() << "visiting impl decl " << o->getName() << '\n';
      return true;
    }, o);
  }
  
  auto VisitObjCIvarDecl(clang::ObjCIvarDecl* o) -> bool
  {
    return runIfInMainFile([](clang::ObjCIvarDecl* o){
      llvm::outs() << "visiting ivar decl " << o->getName() << '\n';
      return true;
    }, o);
  }
  
  auto VisitObjCPropertyDecl(clang::ObjCPropertyDecl* o) -> bool
  {
    return runIfInMainFile([](clang::ObjCPropertyDecl* o){
      llvm::outs() << "visiting property decl " << o->getName() << '\n';
      const auto attrs = o->getAttrs();
      for (auto attr: attrs) {
        llvm::outs() << "new attr: " << attr->getSpelling() << '\n';
        auto kind = attr->getKind();
        if (kind == clang::attr::Kind::Annotate) {
          llvm::outs() << "Found a property annotation!!!" << '\n';
        }
      }
      o->dump();
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
      //b->dump();
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
