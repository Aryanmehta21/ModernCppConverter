#include "frontend/ClangExperimentalFrontend.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/Tooling.h>

#include <memory>
#include <sstream>
#include <string>

namespace
{
class EntityCountingVisitor final : public clang::RecursiveASTVisitor<EntityCountingVisitor>
{
public:
    explicit EntityCountingVisitor(FrontendEntityCounts& counts)
        : counts_(counts)
    {
    }

    bool VisitCXXRecordDecl(clang::CXXRecordDecl* declaration)
    {
        if (declaration != nullptr && declaration->isThisDeclarationADefinition() && !declaration->isImplicit()) {
            ++counts_.classes;
        }
        return true;
    }

    bool VisitFunctionDecl(clang::FunctionDecl* declaration)
    {
        if (declaration != nullptr && declaration->isThisDeclarationADefinition() && !declaration->isImplicit()) {
            ++counts_.functions;
        }
        return true;
    }

    bool VisitEnumDecl(clang::EnumDecl* declaration)
    {
        if (declaration != nullptr && declaration->isThisDeclarationADefinition() && !declaration->isImplicit()) {
            ++counts_.enums;
        }
        return true;
    }

    bool VisitVarDecl(clang::VarDecl* declaration)
    {
        if (declaration != nullptr && !declaration->isImplicit()) {
            ++counts_.variables;
        }
        return true;
    }

private:
    FrontendEntityCounts& counts_;
};

class EntityCountingConsumer final : public clang::ASTConsumer
{
public:
    explicit EntityCountingConsumer(FrontendEntityCounts& counts)
        : visitor_(counts)
    {
    }

    void HandleTranslationUnit(clang::ASTContext& context) override
    {
        visitor_.TraverseDecl(context.getTranslationUnitDecl());
    }

private:
    EntityCountingVisitor visitor_;
};

class EntityCountingAction final : public clang::ASTFrontendAction
{
public:
    explicit EntityCountingAction(FrontendEntityCounts& counts)
        : counts_(counts)
    {
    }

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) override
    {
        return std::make_unique<EntityCountingConsumer>(counts_);
    }

private:
    FrontendEntityCounts& counts_;
};

std::string clangSummaryMessage(const ModernizationFrontendResult& result)
{
    std::ostringstream output;
    output << "FRONTEND used=" << result.frontendName
           << " experimental=true"
           << " parse=" << (result.parseSucceeded ? "success" : "failure")
           << " classes=" << result.entityCounts.classes
           << " functions=" << result.entityCounts.functions
           << " enums=" << result.entityCounts.enums
           << " variables=" << result.entityCounts.variables;
    return output.str();
}
} // namespace

std::string ClangExperimentalFrontend::name() const
{
    return "ClangExperimentalFrontend";
}

ModernizationFrontendKind ClangExperimentalFrontend::kind() const
{
    return ModernizationFrontendKind::ClangExperimental;
}

bool ClangExperimentalFrontend::isExperimental() const
{
    return true;
}

ModernizationFrontendResult ClangExperimentalFrontend::analyze(const std::string& source) const
{
    ModernizationFrontendResult result;
    result.kind = kind();
    result.frontendName = name();
    result.document.originalSource = source;

    FrontendEntityCounts counts;
    auto action = std::make_unique<EntityCountingAction>(counts);
    const bool parseSucceeded = clang::tooling::runToolOnCodeWithArgs(
        std::move(action),
        source,
        {"-std=c++20"},
        "modernization-input.cpp");

    result.parseSucceeded = parseSucceeded;
    if (parseSucceeded) {
        result.entityCounts = counts;
    } else {
        result.diagnostics.push_back("FRONTEND clang_parse=failure action=diagnostics-only");
    }
    result.diagnostics.insert(result.diagnostics.begin(), clangSummaryMessage(result));
    result.diagnostics.insert(result.diagnostics.begin(), "FRONTEND clang_experiment=enabled default=LightweightFrontend");
    return result;
}
