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

using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;

static llvm::cl::OptionCategory ToolingSampleCategory("Mixin With Steroids");

struct ObjCVisitor : public RecursiveASTVisitor<ObjCVisitor> {
  ObjCVisitor(Rewriter &R) : TheRewriter(R) {}
  
  auto VisitObjCInterfaceDecl(ObjCInterfaceDecl* o) -> bool
  {
    return true;
  }
  
  auto VisitObjCProtocolDecl(ObjCProtocolDecl* o) -> bool
  {
    return true;
  }
  
  auto VisitObjCImplementationDecl(ObjCImplementationDecl* o) -> bool
  {
    return true;
  }
  
  auto VisitObjCIvarDeclaration(ObjCIvarDecl* o) -> bool
  {
    return true;
  }

  Rewriter &TheRewriter;
};

struct ObjCASTConsumer : public ASTConsumer {
  ObjCASTConsumer(Rewriter &R) : Visitor(R) {}

  auto HandleTopLevelDecl(DeclGroupRef DR) -> bool override
  {
    for (auto b = DR.begin(), e = DR.end(); b != e; ++b) {
      Visitor.TraverseDecl(*b);
      (*b)->dump();
    }
    return true;
  }

  ObjCVisitor Visitor;
};

// For each source file provided to the tool, a new FrontendAction is created.
struct MyFrontendAction : public ASTFrontendAction {
  MyFrontendAction() {}
  
  auto EndSourceFileAction() -> void override 
  {
    SourceManager &SM = TheRewriter.getSourceMgr();
    llvm::errs() << "** EndSourceFileAction for: "
                 << SM.getFileEntryForID(SM.getMainFileID())->getName() << "\n";

    TheRewriter.getEditBuffer(SM.getMainFileID()).write(llvm::outs());
  }

  auto CreateASTConsumer(CompilerInstance &CI, StringRef file) -> std::unique_ptr<ASTConsumer> override
  {
    llvm::errs() << "** Creating AST consumer for: " << file << "\n";
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return llvm::make_unique<ObjCASTConsumer>(TheRewriter);
  }

  Rewriter TheRewriter;
};

auto main(int argc, const char **argv) -> int {
  CommonOptionsParser op(argc, argv, ToolingSampleCategory);
  ClangTool Tool(op.getCompilations(), op.getSourcePathList());

  return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}
