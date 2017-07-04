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

static llvm::cl::OptionCategory ToolingSampleCategory("Mixin With Steroids");

struct ObjCVisitor : public clang::RecursiveASTVisitor<ObjCVisitor>
{
  ObjCVisitor(clang::Rewriter &R):
  _rewriter(R)
  {
  }
  
  auto VisitObjCInterfaceDecl(clang::ObjCInterfaceDecl* o) -> bool
  {
    llvm::outs() << "visiting interface decl" << '\n';
    return true;
  }
  
  auto VisitObjCProtocolDecl(clang::ObjCProtocolDecl* o) -> bool
  {
    llvm::outs() << "visiting protocol decl" << '\n';
    return true;
  }
  
  auto VisitObjCImplementationDecl(clang::ObjCImplementationDecl* o) -> bool
  {
    llvm::outs() << "visiting impl decl" << '\n';
    return true;
  }
  
  auto VisitObjCIvarDeclaration(clang::ObjCIvarDecl* o) -> bool
  {
    llvm::outs() << "visiting ivar decl" << '\n';
    return true;
  }

  clang::Rewriter &_rewriter;
};

struct ObjCASTConsumer : public clang::ASTConsumer
{
  ObjCASTConsumer(clang::Rewriter &rewriter):
  _visitor(rewriter)
  {
  }

  auto HandleTopLevelDecl(clang::DeclGroupRef declGroup) -> bool override
  {
    for (auto &b: declGroup) {
      _visitor.TraverseDecl(b);
      //b->dump();
    }
    
    return true;
  }

  ObjCVisitor _visitor;
};

// For each source file provided to the tool, a new FrontendAction is created.
struct MyFrontendAction : public clang::ASTFrontendAction
{
  MyFrontendAction()
  {
  }
  
  auto EndSourceFileAction() -> void override 
  {
    return;
    
    auto &srcManager = _rewriter.getSourceMgr();
    
    llvm::errs() << "** EndSourceFileAction for: "
                 << srcManager.getFileEntryForID(srcManager.getMainFileID())->getName() << "\n";

    _rewriter.getEditBuffer(srcManager.getMainFileID()).write(llvm::outs());
  }

  auto CreateASTConsumer(clang::CompilerInstance &compilerInstance, llvm::StringRef file) -> std::unique_ptr<clang::ASTConsumer> override
  {
    llvm::errs() << "** Creating AST consumer for: " << file << "\n";
    _rewriter.setSourceMgr(compilerInstance.getSourceManager(), compilerInstance.getLangOpts());
        
    return llvm::make_unique<ObjCASTConsumer>(_rewriter);
  }

  clang::Rewriter _rewriter;
};

auto main(int argc, const char **argv) -> int
{
  auto op = clang::tooling::CommonOptionsParser(argc, argv, ToolingSampleCategory);
  auto tool = clang::tooling::ClangTool(op.getCompilations(), op.getSourcePathList());

  return tool.run(clang::tooling::newFrontendActionFactory<MyFrontendAction>().get());
}
