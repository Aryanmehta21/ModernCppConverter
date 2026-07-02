#include "frontend/ClangExperimentalFrontend.h"

#include "frontend/ClangParseService.h"
#include "parser/LightweightCppParser.h"
#include "utils/CrashBreadcrumb.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallString.h>

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
FrontendEntityCounts entityCountsFor(const ParsedDocument& document)
{
    return FrontendEntityCounts{
        document.aggregates.size(),
        document.functions.size(),
        document.enums.size(),
        document.memberVariables.size() + document.globalVariables.size() + document.localVariables.size(),
    };
}

bool containsArgument(const std::vector<std::string>& arguments, const std::string& value)
{
    return std::find(arguments.begin(), arguments.end(), value) != arguments.end();
}

std::string joinStrings(const std::vector<std::string>& values, const char separator)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0U) {
            output << separator;
        }
        output << values[index];
    }
    return output.str();
}

QString findHostCxxCompiler()
{
    const QStringList candidates{
        QStringLiteral("clang++"),
        QStringLiteral("g++"),
        QStringLiteral("c++"),
    };
    for (const QString& candidate : candidates) {
        const QString found = QStandardPaths::findExecutable(candidate);
        if (!found.isEmpty()) {
            return found;
        }
    }
    return {};
}

QString runCompilerProbe(const QString& compiler, const QStringList& arguments, const QByteArray& input = {})
{
    if (compiler.isEmpty()) {
        return {};
    }

    QProcess process;
    process.setProgram(compiler);
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(2000)) {
        return {};
    }
    if (!input.isEmpty()) {
        process.write(input);
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished();
        return {};
    }
    return QString::fromUtf8(process.readAllStandardError())
        + QString::fromUtf8(process.readAllStandardOutput());
}

std::vector<std::string> probeSystemIncludePaths(const std::string& languageStandard)
{
    const QString compiler = findHostCxxCompiler();
    const QString output = runCompilerProbe(compiler,
                                            {QStringLiteral("-std=%1").arg(QString::fromStdString(languageStandard)),
                                             QStringLiteral("-E"),
                                             QStringLiteral("-x"),
                                             QStringLiteral("c++"),
                                             QStringLiteral("-"),
                                             QStringLiteral("-v")},
                                            QByteArrayLiteral("\n"));
    std::vector<std::string> includePaths;
    bool inSearchList = false;
    const QStringList lines = output.split('\n');
    for (QString line : lines) {
        line = line.trimmed();
        if (line.contains(QStringLiteral("#include <...> search starts here:"))) {
            inSearchList = true;
            continue;
        }
        if (inSearchList && line.contains(QStringLiteral("End of search list."))) {
            break;
        }
        if (!inSearchList || line.isEmpty() || line.startsWith(QStringLiteral("("))) {
            continue;
        }
        includePaths.push_back(line.toStdString());
    }
    return includePaths;
}

std::string probeResourceDir()
{
    const QString compiler = findHostCxxCompiler();
    if (QFileInfo(compiler).fileName().startsWith(QStringLiteral("clang"))) {
        return runCompilerProbe(compiler, {QStringLiteral("-print-resource-dir")}).trimmed().toStdString();
    }
    const QString clang = QStandardPaths::findExecutable(QStringLiteral("clang++"));
    if (!clang.isEmpty()) {
        return runCompilerProbe(clang, {QStringLiteral("-print-resource-dir")}).trimmed().toStdString();
    }
    return {};
}

ClangParseConfig effectiveClangConfig(ClangParseConfig config)
{
    config.virtualFileName = config.virtualFileName.empty() ? "input.cpp" : config.virtualFileName;
    config.languageStandard = config.languageStandard.empty() ? "c++20" : config.languageStandard;
    if (!containsArgument(config.compileArguments, "-std=" + config.languageStandard)) {
        config.compileArguments.insert(config.compileArguments.begin(), "-std=" + config.languageStandard);
    }
    if (!containsArgument(config.compileArguments, "-fsyntax-only")) {
        config.compileArguments.push_back("-fsyntax-only");
    }
    if (!containsArgument(config.compileArguments, "-x")) {
        config.compileArguments.push_back("-x");
        config.compileArguments.push_back("c++");
    }
    if (config.includePaths.empty()) {
        config.includePaths = probeSystemIncludePaths(config.languageStandard);
    }
    if (config.resourceDir.empty()) {
        config.resourceDir = probeResourceDir();
    }
    if (!config.resourceDir.empty() && !containsArgument(config.compileArguments, "-resource-dir")) {
        config.compileArguments.push_back("-resource-dir");
        config.compileArguments.push_back(config.resourceDir);
    }
    if (!config.systemRoot.empty() && !containsArgument(config.compileArguments, "-isysroot")) {
        config.compileArguments.push_back("-isysroot");
        config.compileArguments.push_back(config.systemRoot);
    }
    for (const std::string& includePath : config.includePaths) {
        if (includePath.empty()) {
            continue;
        }
        config.compileArguments.push_back("-isystem");
        config.compileArguments.push_back(includePath);
    }
    return config;
}

std::string clangParseConfigSummary(const ClangParseConfig& config)
{
    std::ostringstream output;
    output << "virtual_file=" << config.virtualFileName
           << " standard=" << config.languageStandard
           << " args=" << joinStrings(config.compileArguments, ',')
           << " include_paths=" << joinStrings(config.includePaths, ',')
           << " resource_dir=" << config.resourceDir
           << " system_root=" << config.systemRoot;
    return output.str();
}

std::string typeString(clang::QualType type)
{
    if (type.isNull()) {
        return {};
    }
    clang::PrintingPolicy policy(clang::LangOptions{});
    policy.SuppressTagKeyword = true;
    policy.Bool = true;
    return type.getAsString(policy);
}

std::string canonicalTypeString(clang::QualType type)
{
    if (type.isNull()) {
        return {};
    }
    return typeString(type.getCanonicalType());
}

SourceEntityKind entityKindForAggregate(const clang::CXXRecordDecl& declaration)
{
    return declaration.isStruct() ? SourceEntityKind::Struct : SourceEntityKind::Class;
}

ScopeKind scopeKindForAggregate(const clang::CXXRecordDecl& declaration)
{
    return declaration.isStruct() ? ScopeKind::Struct : ScopeKind::Class;
}

bool isInMainFile(const clang::SourceManager& sourceManager, clang::SourceLocation location)
{
    if (location.isInvalid()) {
        return false;
    }
    const clang::SourceLocation spellingLocation = sourceManager.getSpellingLoc(location);
    return spellingLocation.isValid() && sourceManager.isWrittenInMainFile(spellingLocation);
}

SourcePosition positionFor(const clang::SourceManager& sourceManager, clang::SourceLocation location)
{
    SourcePosition position;
    if (!isInMainFile(sourceManager, location)) {
        return position;
    }

    const clang::SourceLocation spellingLocation = sourceManager.getSpellingLoc(location);
    position.offset = sourceManager.getFileOffset(spellingLocation);
    position.line = sourceManager.getSpellingLineNumber(spellingLocation);
    position.column = sourceManager.getSpellingColumnNumber(spellingLocation);
    return position;
}

clang::SourceLocation endOfToken(const clang::SourceManager& sourceManager,
                                 const clang::LangOptions& langOptions,
                                 clang::SourceLocation location)
{
    if (location.isInvalid()) {
        return location;
    }

    const clang::SourceLocation spellingLocation = sourceManager.getSpellingLoc(location);
    const clang::SourceLocation tokenEnd =
        clang::Lexer::getLocForEndOfToken(spellingLocation, 0, sourceManager, langOptions);
    return tokenEnd.isValid() ? tokenEnd : spellingLocation;
}

SourceRange sourceRangeFor(const clang::SourceManager& sourceManager,
                           const clang::LangOptions& langOptions,
                           clang::SourceRange clangRange,
                           SourceEntityKind kind,
                           std::string name,
                           std::optional<std::size_t> parentScopeId = std::nullopt)
{
    SourceRange range;
    if (clangRange.isInvalid() || !isInMainFile(sourceManager, clangRange.getBegin())) {
        return range;
    }

    const clang::SourceLocation begin = sourceManager.getSpellingLoc(clangRange.getBegin());
    const clang::SourceLocation end = endOfToken(sourceManager, langOptions, clangRange.getEnd());
    if (end.isInvalid() || sourceManager.getFileID(begin) != sourceManager.getFileID(sourceManager.getSpellingLoc(end))) {
        return range;
    }

    range.start = positionFor(sourceManager, begin);
    range.end = positionFor(sourceManager, end);
    if (range.end.offset < range.start.offset) {
        range.end = range.start;
    }
    range.entityKind = kind;
    range.entityName = std::move(name);
    range.parentScopeId = parentScopeId;
    return range;
}

SourceRange sourceRangeForToken(clang::SourceManager& sourceManager,
                                const clang::LangOptions& langOptions,
                                clang::SourceLocation location,
                                std::string tokenText,
                                SourceEntityKind kind,
                                std::optional<std::size_t> parentScopeId = std::nullopt)
{
    SourceRange range;
    if (!isInMainFile(sourceManager, location)) {
        return range;
    }

    const clang::SourceLocation spellingLocation = sourceManager.getSpellingLoc(location);
    range.start = positionFor(sourceManager, spellingLocation);
    range.end = positionFor(sourceManager, endOfToken(sourceManager, langOptions, spellingLocation));
    if (range.end.offset <= range.start.offset && !tokenText.empty()) {
        range.end.offset = range.start.offset + tokenText.size();
        range.end.line = range.start.line;
        range.end.column = range.start.column + tokenText.size();
    }
    range.entityKind = kind;
    range.entityName = std::move(tokenText);
    range.parentScopeId = parentScopeId;
    return range;
}

std::string declarationName(const clang::NamedDecl& declaration)
{
    if (const auto* destructor = llvm::dyn_cast<clang::CXXDestructorDecl>(&declaration)) {
        return "~" + destructor->getParent()->getNameAsString();
    }
    return declaration.getNameAsString();
}

std::string recordNameForContext(const clang::DeclContext* context)
{
    while (context != nullptr && !llvm::isa<clang::TranslationUnitDecl>(context)) {
        if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(context)) {
            return record->getNameAsString();
        }
        context = context->getParent();
    }
    return {};
}

const clang::FunctionDecl* functionForContext(const clang::DeclContext* context)
{
    while (context != nullptr && !llvm::isa<clang::TranslationUnitDecl>(context)) {
        if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(context)) {
            return function;
        }
        context = context->getParent();
    }
    return nullptr;
}

std::string functionNameForContext(const clang::DeclContext* context)
{
    const clang::FunctionDecl* function = functionForContext(context);
    return function != nullptr ? declarationName(*function) : std::string{};
}

std::string namespaceName(const clang::NamespaceDecl& declaration)
{
    return declaration.isAnonymousNamespace() ? "<anonymous namespace>" : declaration.getNameAsString();
}

ParsedSymbolKind symbolKindFor(const clang::NamedDecl& declaration)
{
    if (llvm::isa<clang::NamespaceDecl>(&declaration)) {
        return ParsedSymbolKind::Namespace;
    }
    if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&declaration)) {
        return record->isStruct() ? ParsedSymbolKind::Struct : ParsedSymbolKind::Class;
    }
    if (llvm::isa<clang::EnumDecl>(&declaration)) {
        return ParsedSymbolKind::Enum;
    }
    if (llvm::isa<clang::EnumConstantDecl>(&declaration)) {
        return ParsedSymbolKind::EnumConstant;
    }
    if (llvm::isa<clang::CXXConstructorDecl>(&declaration)) {
        return ParsedSymbolKind::Constructor;
    }
    if (llvm::isa<clang::CXXDestructorDecl>(&declaration)) {
        return ParsedSymbolKind::Destructor;
    }
    if (llvm::isa<clang::CXXMethodDecl>(&declaration)) {
        return ParsedSymbolKind::Method;
    }
    if (llvm::isa<clang::FunctionDecl>(&declaration)) {
        return ParsedSymbolKind::Function;
    }
    if (llvm::isa<clang::ParmVarDecl>(&declaration)) {
        return ParsedSymbolKind::Parameter;
    }
    if (llvm::isa<clang::FieldDecl>(&declaration)) {
        return ParsedSymbolKind::Field;
    }
    if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(&declaration)) {
        return (variable->isLocalVarDecl() || variable->isStaticLocal())
                   ? ParsedSymbolKind::LocalVariable
                   : ParsedSymbolKind::GlobalVariable;
    }
    if (llvm::isa<clang::TypeAliasDecl>(&declaration)) {
        return ParsedSymbolKind::Alias;
    }
    if (llvm::isa<clang::TypedefDecl>(&declaration)) {
        return ParsedSymbolKind::Typedef;
    }
    return ParsedSymbolKind::LocalVariable;
}

SourceEntityKind sourceEntityKindForSymbolKind(ParsedSymbolKind kind)
{
    switch (kind) {
    case ParsedSymbolKind::Namespace:
        return SourceEntityKind::Scope;
    case ParsedSymbolKind::Class:
        return SourceEntityKind::Class;
    case ParsedSymbolKind::Struct:
        return SourceEntityKind::Struct;
    case ParsedSymbolKind::Enum:
        return SourceEntityKind::Enum;
    case ParsedSymbolKind::EnumConstant:
        return SourceEntityKind::Enumerator;
    case ParsedSymbolKind::Function:
    case ParsedSymbolKind::Method:
    case ParsedSymbolKind::Constructor:
    case ParsedSymbolKind::Destructor:
        return SourceEntityKind::Function;
    case ParsedSymbolKind::Field:
        return SourceEntityKind::Member;
    case ParsedSymbolKind::LocalVariable:
        return SourceEntityKind::Local;
    case ParsedSymbolKind::GlobalVariable:
    case ParsedSymbolKind::Parameter:
    case ParsedSymbolKind::Typedef:
    case ParsedSymbolKind::Alias:
        return SourceEntityKind::Variable;
    }
    return SourceEntityKind::Unknown;
}

bool declarationIsDefinition(const clang::NamedDecl& declaration)
{
    if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&declaration)) {
        return record->isThisDeclarationADefinition();
    }
    if (const auto* parsedEnum = llvm::dyn_cast<clang::EnumDecl>(&declaration)) {
        return parsedEnum->isThisDeclarationADefinition();
    }
    if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&declaration)) {
        return function->isThisDeclarationADefinition();
    }
    if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(&declaration)) {
        if (variable->isLocalVarDecl() || variable->isStaticLocal()) {
            return true;
        }
        return variable->hasInit();
    }
    if (llvm::isa<clang::NamespaceDecl>(&declaration)
        || llvm::isa<clang::EnumConstantDecl>(&declaration)
        || llvm::isa<clang::FieldDecl>(&declaration)
        || llvm::isa<clang::ParmVarDecl>(&declaration)
        || llvm::isa<clang::TypedefNameDecl>(&declaration)) {
        return true;
    }
    return false;
}

std::string symbolTypeString(const clang::NamedDecl& declaration)
{
    if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&declaration)) {
        return typeString(function->getType());
    }
    if (const auto* value = llvm::dyn_cast<clang::ValueDecl>(&declaration)) {
        return typeString(value->getType());
    }
    if (const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(&declaration)) {
        return typeString(alias->getUnderlyingType());
    }
    if (const auto* parsedEnum = llvm::dyn_cast<clang::EnumDecl>(&declaration)) {
        return typeString(parsedEnum->getIntegerType());
    }
    if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&declaration)) {
        return record->getQualifiedNameAsString();
    }
    return {};
}

std::string symbolCanonicalTypeString(const clang::NamedDecl& declaration)
{
    if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&declaration)) {
        return canonicalTypeString(function->getType());
    }
    if (const auto* value = llvm::dyn_cast<clang::ValueDecl>(&declaration)) {
        return canonicalTypeString(value->getType());
    }
    if (const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(&declaration)) {
        return canonicalTypeString(alias->getUnderlyingType());
    }
    if (const auto* parsedEnum = llvm::dyn_cast<clang::EnumDecl>(&declaration)) {
        return canonicalTypeString(parsedEnum->getIntegerType());
    }
    if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&declaration)) {
        return record->getQualifiedNameAsString();
    }
    return {};
}

std::set<std::string> aggregateNames(const ParsedDocument& document)
{
    std::set<std::string> names;
    for (const ParsedAggregate& aggregate : document.aggregates) {
        names.insert(aggregate.name);
    }
    return names;
}

std::set<std::string> functionNames(const ParsedDocument& document)
{
    std::set<std::string> names;
    for (const ParsedFunction& function : document.functions) {
        names.insert(function.name);
    }
    return names;
}

std::set<std::string> enumNames(const ParsedDocument& document)
{
    std::set<std::string> names;
    for (const ParsedEnum& parsedEnum : document.enums) {
        names.insert(parsedEnum.name);
    }
    return names;
}

void appendMissingNameDiagnostics(std::vector<std::string>& diagnostics,
                                  const std::string& label,
                                  const std::set<std::string>& expected,
                                  const std::set<std::string>& actual)
{
    std::vector<std::string> missing;
    std::set_difference(expected.begin(),
                        expected.end(),
                        actual.begin(),
                        actual.end(),
                        std::back_inserter(missing));
    if (missing.empty()) {
        return;
    }

    std::ostringstream output;
    output << "FRONTEND_COMPARE missing_" << label << '=';
    for (std::size_t index = 0; index < missing.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << missing[index];
    }
    diagnostics.push_back(output.str());
}

void appendRangeMismatchDiagnostics(std::vector<std::string>& diagnostics,
                                    const ParsedDocument& lightweight,
                                    const ParsedDocument& clangDocument)
{
    std::size_t mismatches = 0;
    for (const ParsedAggregate& lightweightAggregate : lightweight.aggregates) {
        const auto clangIt = std::find_if(clangDocument.aggregates.begin(),
                                          clangDocument.aggregates.end(),
                                          [&](const ParsedAggregate& aggregate) {
                                              return aggregate.name == lightweightAggregate.name;
                                          });
        if (clangIt != clangDocument.aggregates.end()
            && clangIt->range.start.offset != lightweightAggregate.range.start.offset) {
            ++mismatches;
        }
    }

    if (mismatches != 0) {
        diagnostics.push_back("FRONTEND_COMPARE source_range_mismatches=" + std::to_string(mismatches));
    }
}

std::string comparisonSummary(const ParsedDocument& lightweight, const ParsedDocument& clangDocument)
{
    const FrontendEntityCounts lightweightCounts = entityCountsFor(lightweight);
    const FrontendEntityCounts clangCounts = entityCountsFor(clangDocument);

    std::ostringstream output;
    output << "FRONTEND_COMPARE lightweight_classes=" << lightweightCounts.classes
           << " clang_classes=" << clangCounts.classes
           << " lightweight_functions=" << lightweightCounts.functions
           << " clang_functions=" << clangCounts.functions
           << " lightweight_enums=" << lightweightCounts.enums
           << " clang_enums=" << clangCounts.enums
           << " lightweight_variables=" << lightweightCounts.variables
           << " clang_variables=" << clangCounts.variables;
    return output.str();
}

std::string symbolResolutionSummary(const ParsedDocument& document)
{
    std::size_t classSymbols = 0;
    std::size_t functionSymbols = 0;
    std::size_t variableSymbols = 0;
    std::size_t sourceRangesAvailable = 0;

    for (const ParsedSymbol& symbol : document.symbols) {
        if (symbol.range.isValidFor(document.originalSource.size())) {
            ++sourceRangesAvailable;
        }
        switch (symbol.kind) {
        case ParsedSymbolKind::Class:
        case ParsedSymbolKind::Struct:
            ++classSymbols;
            break;
        case ParsedSymbolKind::Function:
        case ParsedSymbolKind::Method:
        case ParsedSymbolKind::Constructor:
        case ParsedSymbolKind::Destructor:
            ++functionSymbols;
            break;
        case ParsedSymbolKind::Parameter:
        case ParsedSymbolKind::Field:
        case ParsedSymbolKind::LocalVariable:
        case ParsedSymbolKind::GlobalVariable:
            ++variableSymbols;
            break;
        default:
            break;
        }
    }

    std::size_t resolvedReferences = 0;
    std::size_t unresolvedReferences = 0;
    for (const ParsedSymbolReference& reference : document.symbolReferences) {
        if (reference.resolved) {
            ++resolvedReferences;
        } else {
            ++unresolvedReferences;
        }
    }

    std::ostringstream output;
    output << "FRONTEND_SYMBOLS total=" << document.symbols.size()
           << " classes=" << classSymbols
           << " functions=" << functionSymbols
           << " variables=" << variableSymbols
           << " resolved=" << resolvedReferences
           << " unresolved=" << unresolvedReferences
           << " ranges=" << sourceRangesAvailable;
    return output.str();
}

std::string clangSummaryMessage(const ModernizationFrontendResult& result)
{
    std::ostringstream output;
    output << "FRONTEND used=" << result.frontendName
           << " experimental=true"
           << " parse=" << (result.parseSucceeded ? "success" : "fallback")
           << " classes=" << result.entityCounts.classes
           << " functions=" << result.entityCounts.functions
           << " enums=" << result.entityCounts.enums
           << " variables=" << result.entityCounts.variables;
    return output.str();
}

struct ClangCollectionState
{
    ParsedDocument document;
    std::vector<std::string> clangDiagnostics;
    bool hadDiagnosticsError = false;
    bool translationUnitAlive = false;
};

class CapturingDiagnosticConsumer final : public clang::DiagnosticConsumer
{
public:
    explicit CapturingDiagnosticConsumer(ClangCollectionState& state)
        : state_(state)
    {
    }

    void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                          const clang::Diagnostic& diagnostic) override
    {
        llvm::SmallString<256> message;
        diagnostic.FormatDiagnostic(message);

        std::ostringstream output;
        output << "CLANG DIAGNOSTIC severity=" << severityName(level);
        if (diagnostic.hasSourceManager() && diagnostic.getLocation().isValid()) {
            const clang::SourceManager& sourceManager = diagnostic.getSourceManager();
            const clang::FullSourceLoc fullLocation(diagnostic.getLocation(), sourceManager);
            if (fullLocation.isValid()) {
                output << " line=" << fullLocation.getSpellingLineNumber()
                       << " column=" << fullLocation.getSpellingColumnNumber();
            }
        }
        output << " message=\"" << std::string(message.str()) << "\"";
        state_.clangDiagnostics.push_back(output.str());
    }

private:
    static const char* severityName(clang::DiagnosticsEngine::Level level)
    {
        switch (level) {
        case clang::DiagnosticsEngine::Ignored:
            return "ignored";
        case clang::DiagnosticsEngine::Note:
            return "note";
        case clang::DiagnosticsEngine::Remark:
            return "remark";
        case clang::DiagnosticsEngine::Warning:
            return "warning";
        case clang::DiagnosticsEngine::Error:
            return "error";
        case clang::DiagnosticsEngine::Fatal:
            return "fatal";
        }
        return "unknown";
    }

    ClangCollectionState& state_;
};

class PreprocessorEntityCollector final : public clang::PPCallbacks
{
public:
    PreprocessorEntityCollector(ClangCollectionState& state,
                                clang::SourceManager& sourceManager,
                                const clang::LangOptions& langOptions)
        : state_(state),
          sourceManager_(sourceManager),
          langOptions_(langOptions)
    {
    }

    void InclusionDirective(clang::SourceLocation hashLocation,
                            const clang::Token&,
                            llvm::StringRef fileName,
                            bool isAngled,
                            clang::CharSourceRange filenameRange,
                            clang::OptionalFileEntryRef,
                            llvm::StringRef,
                            llvm::StringRef,
                            const clang::Module*,
                            bool,
                            clang::SrcMgr::CharacteristicKind) override
    {
        if (!isInMainFile(sourceManager_, hashLocation)) {
            return;
        }

        ParsedIncludeDirective include;
        include.path = isAngled ? "<" + fileName.str() + ">" : "\"" + fileName.str() + "\"";
        include.range = sourceRangeFor(sourceManager_,
                                       langOptions_,
                                       filenameRange.getAsRange(),
                                       SourceEntityKind::Include,
                                       include.path);
        if (!include.range.isValidFor(state_.document.originalSource.size())) {
            include.range = sourceRangeForToken(sourceManager_,
                                               langOptions_,
                                               hashLocation,
                                               include.path,
                                               SourceEntityKind::Include);
        }
        state_.document.includes.push_back(std::move(include));
    }

    void MacroDefined(const clang::Token& macroNameToken, const clang::MacroDirective*) override
    {
        const clang::SourceLocation location = macroNameToken.getLocation();
        if (!isInMainFile(sourceManager_, location)) {
            return;
        }

        ParsedMacroDirective macro;
        macro.name = macroNameToken.getIdentifierInfo() != nullptr
                         ? macroNameToken.getIdentifierInfo()->getName().str()
                         : std::string{};
        macro.range = sourceRangeForToken(sourceManager_,
                                         langOptions_,
                                         location,
                                         macro.name,
                                         SourceEntityKind::Macro);
        state_.document.macros.push_back(std::move(macro));
    }

private:
    ClangCollectionState& state_;
    clang::SourceManager& sourceManager_;
    const clang::LangOptions& langOptions_;
};

class ClangEntityVisitor final : public clang::RecursiveASTVisitor<ClangEntityVisitor>
{
public:
    ClangEntityVisitor(ClangCollectionState& state,
                       clang::SourceManager& sourceManager,
                       const clang::LangOptions& langOptions)
        : state_(state),
          sourceManager_(sourceManager),
          langOptions_(langOptions)
    {
        ScopeInfo globalScope;
        globalScope.kind = ScopeKind::Global;
        globalScope.name = "<global>";
        globalScope.range.start = {0, 1, 1};
        globalScope.range.end.offset = state_.document.originalSource.size();
        globalScope.range.entityKind = SourceEntityKind::Scope;
        globalScope.range.entityName = globalScope.name;
        state_.document.scopes.push_back(globalScope);
    }

    bool VisitNamespaceDecl(clang::NamespaceDecl* declaration)
    {
        if (declaration == nullptr || !isInMainFile(sourceManager_, declaration->getBeginLoc())) {
            return true;
        }

        ensureSymbolForNamedDecl(declaration);
        ensureScopeForContext(declaration);
        return true;
    }

    bool VisitCXXRecordDecl(clang::CXXRecordDecl* declaration)
    {
        if (declaration == nullptr
            || declaration->isImplicit()
            || declaration->isLambda()
            || !isInMainFile(sourceManager_, declaration->getLocation())) {
            return true;
        }

        ensureSymbolForNamedDecl(declaration);
        if (!declaration->isThisDeclarationADefinition()) {
            return true;
        }

        const clang::Decl* canonical = declaration->getCanonicalDecl();
        if (!seenAggregates_.insert(canonical).second) {
            return true;
        }

        ParsedAggregate aggregate;
        aggregate.kind = declaration->isStruct() ? ParsedAggregateKind::Struct : ParsedAggregateKind::Class;
        aggregate.name = declaration->getNameAsString();
        const std::optional<std::size_t> parentScopeId = ensureScopeForContext(declaration->getDeclContext());
        aggregate.range = sourceRangeFor(sourceManager_,
                                        langOptions_,
                                        declaration->getSourceRange(),
                                        entityKindForAggregate(*declaration),
                                        aggregate.name,
                                        parentScopeId);
        aggregate.nameRange = sourceRangeForToken(sourceManager_,
                                                 langOptions_,
                                                 declaration->getLocation(),
                                                 aggregate.name,
                                                 entityKindForAggregate(*declaration),
                                                 parentScopeId);
        aggregate.bodyRange = aggregate.range;
        for (const clang::CXXBaseSpecifier& base : declaration->bases()) {
            aggregate.baseNames.push_back(typeString(base.getType()));
        }

        state_.document.aggregates.push_back(std::move(aggregate));
        ensureScopeForContext(declaration);
        return true;
    }

    bool VisitEnumDecl(clang::EnumDecl* declaration)
    {
        if (declaration == nullptr
            || !isInMainFile(sourceManager_, declaration->getLocation())) {
            return true;
        }

        ensureSymbolForNamedDecl(declaration);
        if (!declaration->isThisDeclarationADefinition()) {
            return true;
        }

        const clang::Decl* canonical = declaration->getCanonicalDecl();
        if (!seenEnums_.insert(canonical).second) {
            return true;
        }

        ParsedEnum parsedEnum;
        parsedEnum.name = declaration->getNameAsString();
        parsedEnum.scoped = declaration->isScoped();
        if (!declaration->getIntegerType().isNull()) {
            parsedEnum.underlyingType = typeString(declaration->getIntegerType());
        }
        const std::optional<std::size_t> parentScopeId = ensureScopeForContext(declaration->getDeclContext());
        parsedEnum.range = sourceRangeFor(sourceManager_,
                                         langOptions_,
                                         declaration->getSourceRange(),
                                         SourceEntityKind::Enum,
                                         parsedEnum.name,
                                         parentScopeId);
        parsedEnum.nameRange = sourceRangeForToken(sourceManager_,
                                                  langOptions_,
                                                  declaration->getLocation(),
                                                  parsedEnum.name,
                                                  SourceEntityKind::Enum,
                                                  parentScopeId);
        parsedEnum.bodyRange = parsedEnum.range;
        for (const clang::EnumConstantDecl* enumerator : declaration->enumerators()) {
            parsedEnum.enumerators.push_back(enumerator->getNameAsString());
            ensureSymbolForNamedDecl(enumerator);
        }
        state_.document.enums.push_back(std::move(parsedEnum));
        return true;
    }

    bool VisitFunctionDecl(clang::FunctionDecl* declaration)
    {
        if (declaration == nullptr || declaration->isImplicit() || !isInMainFile(sourceManager_, declaration->getLocation())) {
            return true;
        }

        const clang::SourceRange clangRange = declaration->getSourceRange();
        if (clangRange.isInvalid()) {
            return true;
        }

        const std::optional<ParsedSymbolId> functionSymbolId = ensureSymbolForNamedDecl(declaration);
        ParsedFunction function;
        function.name = declarationName(*declaration);
        if (!llvm::isa<clang::CXXConstructorDecl>(declaration) && !llvm::isa<clang::CXXDestructorDecl>(declaration)) {
            function.returnType = typeString(declaration->getReturnType());
            function.canonicalReturnType = canonicalTypeString(declaration->getReturnType());
        }
        function.parentName = recordNameForContext(declaration->getDeclContext());
        function.isMember = !function.parentName.empty();
        if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(declaration)) {
            function.isConst = method->isConst();
        }
        function.hasBody = declaration->hasBody();

        const std::optional<std::size_t> parentScopeId = ensureScopeForContext(declaration->getDeclContext());
        function.range = sourceRangeFor(sourceManager_,
                                       langOptions_,
                                       declaration->getSourceRange(),
                                       SourceEntityKind::Function,
                                       function.name,
                                       parentScopeId);
        function.nameRange = sourceRangeFor(sourceManager_,
                                           langOptions_,
                                           declaration->getNameInfo().getSourceRange(),
                                           SourceEntityKind::Function,
                                           function.name,
                                           parentScopeId);
        if (!function.nameRange.isValidFor(state_.document.originalSource.size())) {
            function.nameRange = sourceRangeForToken(sourceManager_,
                                                    langOptions_,
                                                    declaration->getLocation(),
                                                    function.name,
                                                    SourceEntityKind::Function,
                                                    parentScopeId);
        }
        if (const clang::Stmt* body = declaration->getBody()) {
            function.bodyRange = sourceRangeFor(sourceManager_,
                                               langOptions_,
                                               body->getSourceRange(),
                                               SourceEntityKind::Scope,
                                               function.name,
                                               parentScopeId);
        }

        for (const clang::ParmVarDecl* parameterDeclaration : declaration->parameters()) {
            ensureSymbolForNamedDecl(parameterDeclaration);
            ParsedParameter parameter;
            parameter.name = parameterDeclaration->getNameAsString();
            parameter.type = typeString(parameterDeclaration->getType());
            parameter.canonicalType = canonicalTypeString(parameterDeclaration->getType());
            parameter.range = sourceRangeFor(sourceManager_,
                                            langOptions_,
                                            parameterDeclaration->getSourceRange(),
                                            SourceEntityKind::Variable,
                                            parameter.name,
                                            functionSymbolId);
            parameter.nameRange = sourceRangeForToken(sourceManager_,
                                                     langOptions_,
                                                     parameterDeclaration->getLocation(),
                                                     parameter.name,
                                                     SourceEntityKind::Variable,
                                                     functionSymbolId);
            function.parameters.push_back(std::move(parameter));
        }

        state_.document.functions.push_back(std::move(function));
        ensureScopeForContext(declaration);
        return true;
    }

    bool VisitFieldDecl(clang::FieldDecl* declaration)
    {
        if (declaration == nullptr || declaration->isImplicit() || !isInMainFile(sourceManager_, declaration->getLocation())) {
            return true;
        }

        ensureSymbolForNamedDecl(declaration);
        ParsedVariable variable;
        variable.name = declaration->getNameAsString();
        variable.type = typeString(declaration->getType());
        variable.canonicalType = canonicalTypeString(declaration->getType());
        variable.parentName = recordNameForContext(declaration->getDeclContext());
        variable.isMember = true;
        const std::optional<std::size_t> parentScopeId = ensureScopeForContext(declaration->getDeclContext());
        variable.range = sourceRangeFor(sourceManager_,
                                       langOptions_,
                                       declaration->getSourceRange(),
                                       SourceEntityKind::Member,
                                       variable.name,
                                       parentScopeId);
        variable.nameRange = sourceRangeForToken(sourceManager_,
                                                langOptions_,
                                                declaration->getLocation(),
                                                variable.name,
                                                SourceEntityKind::Member,
                                                parentScopeId);
        state_.document.memberVariables.push_back(std::move(variable));
        return true;
    }

    bool VisitVarDecl(clang::VarDecl* declaration)
    {
        if (declaration == nullptr
            || llvm::isa<clang::ParmVarDecl>(declaration)
            || declaration->isImplicit()
            || !isInMainFile(sourceManager_, declaration->getLocation())) {
            return true;
        }

        const bool isLocalVariable = declaration->isLocalVarDecl() || declaration->isStaticLocal();
        ensureSymbolForNamedDecl(declaration);
        ParsedVariable variable;
        variable.name = declaration->getNameAsString();
        variable.type = typeString(declaration->getType());
        variable.canonicalType = canonicalTypeString(declaration->getType());
        variable.parentName = isLocalVariable ? functionNameForContext(declaration->getDeclContext()) : std::string{};
        variable.isMember = false;
        const std::optional<std::size_t> parentScopeId = ensureScopeForContext(declaration->getDeclContext());
        variable.range = sourceRangeFor(sourceManager_,
                                       langOptions_,
                                       declaration->getSourceRange(),
                                       isLocalVariable ? SourceEntityKind::Local : SourceEntityKind::Variable,
                                       variable.name,
                                       parentScopeId);
        variable.nameRange = sourceRangeForToken(sourceManager_,
                                                langOptions_,
                                                declaration->getLocation(),
                                                variable.name,
                                                isLocalVariable ? SourceEntityKind::Local : SourceEntityKind::Variable,
                                                parentScopeId);
        if (isLocalVariable) {
            state_.document.localVariables.push_back(std::move(variable));
        } else {
            state_.document.globalVariables.push_back(std::move(variable));
        }
        return true;
    }

    bool VisitTypedefNameDecl(clang::TypedefNameDecl* declaration)
    {
        if (declaration == nullptr || !isInMainFile(sourceManager_, declaration->getLocation())) {
            return true;
        }

        ensureSymbolForNamedDecl(declaration);
        return true;
    }

    bool VisitCallExpr(clang::CallExpr* expression)
    {
        if (expression == nullptr || !isInMainFile(sourceManager_, expression->getBeginLoc())) {
            return true;
        }

        const clang::FunctionDecl* directCallee = expression->getDirectCallee();
        if (directCallee == nullptr) {
            return true;
        }

        ParsedCallExpression call;
        call.callee = declarationName(*directCallee);
        call.range = sourceRangeFor(sourceManager_,
                                    langOptions_,
                                    expression->getSourceRange(),
                                    SourceEntityKind::Expression,
                                    call.callee,
                                    std::nullopt);
        call.nameRange = sourceRangeForToken(sourceManager_,
                                            langOptions_,
                                            expression->getBeginLoc(),
                                            call.callee,
                                            SourceEntityKind::Expression);
        state_.document.callExpressions.push_back(std::move(call));
        return true;
    }

    bool VisitDeclRefExpr(clang::DeclRefExpr* expression)
    {
        if (expression == nullptr || !isInMainFile(sourceManager_, expression->getLocation())) {
            return true;
        }

        recordSymbolReference(expression->getFoundDecl(),
                              expression->getSourceRange(),
                              expression->getLocation());
        return true;
    }

    bool VisitMemberExpr(clang::MemberExpr* expression)
    {
        if (expression == nullptr || !isInMainFile(sourceManager_, expression->getMemberLoc())) {
            return true;
        }

        recordSymbolReference(expression->getMemberDecl(),
                              expression->getSourceRange(),
                              expression->getMemberLoc());
        return true;
    }

    bool TraverseFunctionDecl(clang::FunctionDecl* declaration)
    {
        const std::optional<ParsedSymbolId> previousFunctionSymbolId = currentFunctionSymbolId_;
        if (declaration != nullptr && !declaration->isImplicit() && isInMainFile(sourceManager_, declaration->getLocation())) {
            currentFunctionSymbolId_ = ensureSymbolForNamedDecl(declaration);
        }
        const bool result = clang::RecursiveASTVisitor<ClangEntityVisitor>::TraverseFunctionDecl(declaration);
        currentFunctionSymbolId_ = previousFunctionSymbolId;
        return result;
    }

private:
    std::optional<ParsedSymbolId> ensureSymbolForNamedDecl(const clang::NamedDecl* declaration)
    {
        if (declaration == nullptr || declaration->isImplicit() || !isInMainFile(sourceManager_, declaration->getLocation())) {
            return std::nullopt;
        }

        const auto* canonicalDeclaration = llvm::dyn_cast<clang::NamedDecl>(declaration->getCanonicalDecl());
        if (canonicalDeclaration == nullptr) {
            canonicalDeclaration = declaration;
        }
        const auto existing = symbolIdByDecl_.find(canonicalDeclaration);
        if (existing != symbolIdByDecl_.end()) {
            ParsedSymbol* symbol = symbolForId(existing->second);
            if (symbol != nullptr && declarationIsDefinition(*declaration)) {
                symbol->isDefinition = true;
                if (symbol->range.length() == 0) {
                    symbol->range = symbolRangeFor(*declaration);
                }
            }
            return existing->second;
        }

        ParsedSymbol symbol;
        symbol.id = nextSymbolId_++;
        symbol.kind = symbolKindFor(*declaration);
        symbol.name = declarationName(*declaration);
        if (const auto* namespaceDecl = llvm::dyn_cast<clang::NamespaceDecl>(declaration)) {
            symbol.name = namespaceName(*namespaceDecl);
        }
        symbol.qualifiedName = declaration->getQualifiedNameAsString();
        if (symbol.qualifiedName.empty()) {
            symbol.qualifiedName = symbol.name;
        }
        symbol.type = symbolTypeString(*declaration);
        symbol.canonicalType = symbolCanonicalTypeString(*declaration);
        symbol.parentId = parentSymbolForContext(declaration->getDeclContext());
        symbol.range = symbolRangeFor(*declaration);
        symbol.nameRange = sourceRangeForToken(sourceManager_,
                                               langOptions_,
                                               declaration->getLocation(),
                                               symbol.name,
                                               sourceEntityKindForSymbolKind(symbol.kind),
                                               symbol.parentId);
        symbol.isDefinition = declarationIsDefinition(*declaration);

        const ParsedSymbolId id = symbol.id;
        symbolIdByDecl_[canonicalDeclaration] = id;
        state_.document.symbols.push_back(std::move(symbol));
        return id;
    }

    ParsedSymbol* symbolForId(ParsedSymbolId id)
    {
        for (ParsedSymbol& symbol : state_.document.symbols) {
            if (symbol.id == id) {
                return &symbol;
            }
        }
        return nullptr;
    }

    SourceRange symbolRangeFor(const clang::NamedDecl& declaration)
    {
        return sourceRangeFor(sourceManager_,
                              langOptions_,
                              declaration.getSourceRange(),
                              sourceEntityKindForSymbolKind(symbolKindFor(declaration)),
                              declarationName(declaration),
                              parentSymbolForContext(declaration.getDeclContext()));
    }

    std::optional<ParsedSymbolId> parentSymbolForContext(const clang::DeclContext* context)
    {
        if (context == nullptr || llvm::isa<clang::TranslationUnitDecl>(context)) {
            return std::nullopt;
        }

        if (const auto* namespaceDecl = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
            return ensureSymbolForNamedDecl(namespaceDecl);
        }
        if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(context)) {
            if (!record->isImplicit()) {
                return ensureSymbolForNamedDecl(record);
            }
        }
        if (const auto* parsedEnum = llvm::dyn_cast<clang::EnumDecl>(context)) {
            return ensureSymbolForNamedDecl(parsedEnum);
        }
        if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(context)) {
            return ensureSymbolForNamedDecl(function);
        }

        return parentSymbolForContext(context->getParent());
    }

    void recordSymbolReference(const clang::NamedDecl* targetDeclaration,
                               clang::SourceRange expressionRange,
                               clang::SourceLocation nameLocation)
    {
        if (targetDeclaration == nullptr) {
            return;
        }

        const std::optional<ParsedSymbolId> targetSymbolId = ensureSymbolForNamedDecl(targetDeclaration);
        if (!targetSymbolId.has_value()) {
            return;
        }

        ParsedSymbolReference reference;
        reference.name = declarationName(*targetDeclaration);
        reference.symbolId = targetSymbolId;
        reference.parentSymbolId = currentFunctionSymbolId_;
        reference.range = sourceRangeFor(sourceManager_,
                                         langOptions_,
                                         expressionRange,
                                         SourceEntityKind::Expression,
                                         reference.name,
                                         currentFunctionSymbolId_);
        reference.nameRange = sourceRangeForToken(sourceManager_,
                                                 langOptions_,
                                                 nameLocation,
                                                 reference.name,
                                                 SourceEntityKind::Expression,
                                                 currentFunctionSymbolId_);
        reference.resolved = true;
        state_.document.symbolReferences.push_back(std::move(reference));
    }

    std::optional<std::size_t> ensureScopeForContext(const clang::DeclContext* context)
    {
        if (context == nullptr || llvm::isa<clang::TranslationUnitDecl>(context)) {
            return 0;
        }

        if (const auto* namespaceDecl = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
            const clang::DeclContext* canonicalContext = namespaceDecl->getCanonicalDecl();
            if (const auto existing = lookupScope(canonicalContext)) {
                return existing;
            }

            const std::optional<std::size_t> parentScopeId = ensureScopeForContext(namespaceDecl->getDeclContext());
            ScopeInfo scope;
            scope.kind = ScopeKind::Namespace;
            scope.name = namespaceName(*namespaceDecl);
            scope.parentIndex = parentScopeId;
            scope.range = sourceRangeFor(sourceManager_,
                                        langOptions_,
                                        namespaceDecl->getSourceRange(),
                                        SourceEntityKind::Scope,
                                        scope.name,
                                        parentScopeId);
            scopeByContext_[canonicalContext] = state_.document.scopes.size();
            state_.document.scopes.push_back(std::move(scope));
            return scopeByContext_[canonicalContext];
        }

        if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(context)) {
            const clang::DeclContext* canonicalContext = record->getCanonicalDecl();
            if (const auto existing = lookupScope(canonicalContext)) {
                return existing;
            }

            const std::optional<std::size_t> parentScopeId = ensureScopeForContext(record->getDeclContext());
            ScopeInfo scope;
            scope.kind = scopeKindForAggregate(*record);
            scope.name = record->getNameAsString();
            scope.parentIndex = parentScopeId;
            scope.range = sourceRangeFor(sourceManager_,
                                        langOptions_,
                                        record->getSourceRange(),
                                        SourceEntityKind::Scope,
                                        scope.name,
                                        parentScopeId);
            scopeByContext_[canonicalContext] = state_.document.scopes.size();
            state_.document.scopes.push_back(std::move(scope));
            return scopeByContext_[canonicalContext];
        }

        if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(context)) {
            const clang::DeclContext* canonicalContext = function->getCanonicalDecl();
            if (const auto existing = lookupScope(canonicalContext)) {
                return existing;
            }

            const std::optional<std::size_t> parentScopeId = ensureScopeForContext(function->getDeclContext());
            ScopeInfo scope;
            scope.kind = ScopeKind::Function;
            scope.name = declarationName(*function);
            scope.parentIndex = parentScopeId;
            scope.range = sourceRangeFor(sourceManager_,
                                        langOptions_,
                                        function->getSourceRange(),
                                        SourceEntityKind::Scope,
                                        scope.name,
                                        parentScopeId);
            scopeByContext_[canonicalContext] = state_.document.scopes.size();
            state_.document.scopes.push_back(std::move(scope));
            return scopeByContext_[canonicalContext];
        }

        return ensureScopeForContext(context->getParent());
    }

    std::optional<std::size_t> lookupScope(const clang::DeclContext* context) const
    {
        const auto it = scopeByContext_.find(context);
        if (it == scopeByContext_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    ClangCollectionState& state_;
    clang::SourceManager& sourceManager_;
    const clang::LangOptions& langOptions_;
    std::unordered_map<const clang::DeclContext*, std::size_t> scopeByContext_;
    std::unordered_map<const clang::NamedDecl*, ParsedSymbolId> symbolIdByDecl_;
    ParsedSymbolId nextSymbolId_ = 1;
    std::optional<ParsedSymbolId> currentFunctionSymbolId_;
    std::unordered_set<const clang::Decl*> seenAggregates_;
    std::unordered_set<const clang::Decl*> seenEnums_;
};

class ClangEntityConsumer final : public clang::ASTConsumer
{
public:
    ClangEntityConsumer(ClangCollectionState& state,
                        clang::SourceManager& sourceManager,
                        const clang::LangOptions& langOptions)
        : state_(state),
          visitor_(state, sourceManager, langOptions)
    {
    }

    void HandleTranslationUnit(clang::ASTContext& context) override
    {
        assert(state_.translationUnitAlive && "Clang AST traversal requires a live translation unit");
        visitor_.TraverseDecl(context.getTranslationUnitDecl());
    }

private:
    ClangCollectionState& state_;
    ClangEntityVisitor visitor_;
};

class ClangReadOnlyAction final : public clang::ASTFrontendAction
{
public:
    explicit ClangReadOnlyAction(ClangCollectionState& state)
        : state_(state)
    {
    }

    bool BeginSourceFileAction(clang::CompilerInstance& compiler) override
    {
        state_.translationUnitAlive = true;
        compiler.getPreprocessor().addPPCallbacks(
            std::make_unique<PreprocessorEntityCollector>(state_,
                                                          compiler.getSourceManager(),
                                                          compiler.getLangOpts()));
        return true;
    }

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& compiler,
                                                         llvm::StringRef) override
    {
        return std::make_unique<ClangEntityConsumer>(state_,
                                                    compiler.getSourceManager(),
                                                    compiler.getLangOpts());
    }

    void EndSourceFileAction() override
    {
        state_.hadDiagnosticsError = getCompilerInstance().getDiagnostics().hasErrorOccurred();
        state_.translationUnitAlive = false;
    }

private:
    ClangCollectionState& state_;
};

class ClangReadOnlyActionFactory final : public clang::tooling::FrontendActionFactory
{
public:
    explicit ClangReadOnlyActionFactory(ClangCollectionState& state)
        : state_(state)
    {
    }

    std::unique_ptr<clang::FrontendAction> create() override
    {
        return std::make_unique<ClangReadOnlyAction>(state_);
    }

private:
    ClangCollectionState& state_;
};

bool runFrontendActionOnCode(ClangCollectionState& state,
                             const std::string& source,
                             const ClangParseConfig& config)
{
    CrashBreadcrumb::ScopedStage stage("Clang Tool parse invocation");
    clang::tooling::FixedCompilationDatabase compilationDatabase(".", config.compileArguments);
    std::vector<std::string> sourcePaths{config.virtualFileName};
    clang::tooling::ClangTool tool(compilationDatabase, sourcePaths);
    tool.mapVirtualFile(config.virtualFileName, source);
    tool.clearArgumentsAdjusters();
    tool.setPrintErrorMessage(false);

    CapturingDiagnosticConsumer diagnosticConsumer(state);
    tool.setDiagnosticConsumer(&diagnosticConsumer);

    ClangReadOnlyActionFactory factory(state);
    return tool.run(&factory) == 0;
}
} // namespace

ClangExperimentalFrontend::ClangExperimentalFrontend() = default;

ClangExperimentalFrontend::ClangExperimentalFrontend(ClangParseConfig config)
    : config_(std::move(config))
{
}

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
    ClangParseService service(config_);
    return service.parse(source);
}

ModernizationFrontendResult ClangExperimentalFrontend::analyzeInProcess(const std::string& source) const
{
    CrashBreadcrumb::ScopedStage stage("ClangExperimentalFrontend analyze");
    const ClangParseConfig parseConfig = effectiveClangConfig(config_);
    const ParsedDocument lightweightDocument = LightweightCppParser{}.parse(source);

    ClangCollectionState state;
    state.document.originalSource = source;
    state.document.tokens = lightweightDocument.tokens;

    const bool toolSucceeded = runFrontendActionOnCode(state, source, parseConfig);
    const bool clangSucceeded = toolSucceeded && !state.hadDiagnosticsError;

    ModernizationFrontendResult result;
    result.kind = kind();
    result.frontendName = name();

    if (clangSucceeded) {
        state.document.parseSucceeded = true;
        result.document = std::move(state.document);
        result.parseSucceeded = true;
        result.entityCounts = entityCountsFor(result.document);
        result.diagnostics.push_back("FRONTEND clang_parse=success fallback=none");
        result.diagnostics.push_back("FRONTEND clang_parse_config=\"" + clangParseConfigSummary(parseConfig) + "\"");
        result.diagnostics.push_back(comparisonSummary(lightweightDocument, result.document));
        result.diagnostics.push_back(symbolResolutionSummary(result.document));
        appendMissingNameDiagnostics(result.diagnostics,
                                     "classes_in_clang",
                                     aggregateNames(lightweightDocument),
                                     aggregateNames(result.document));
        appendMissingNameDiagnostics(result.diagnostics,
                                     "functions_in_clang",
                                     functionNames(lightweightDocument),
                                     functionNames(result.document));
        appendMissingNameDiagnostics(result.diagnostics,
                                     "enums_in_clang",
                                     enumNames(lightweightDocument),
                                     enumNames(result.document));
        appendRangeMismatchDiagnostics(result.diagnostics, lightweightDocument, result.document);
    } else {
        result.document = lightweightDocument;
        result.parseSucceeded = lightweightDocument.parseSucceeded;
        result.entityCounts = entityCountsFor(result.document);
        result.diagnostics.push_back("FRONTEND clang_parse=failure fallback=LightweightFrontend");
        result.diagnostics.push_back("FRONTEND clang_parse_config=\"" + clangParseConfigSummary(parseConfig) + "\"");
        result.diagnostics.insert(result.diagnostics.end(), state.clangDiagnostics.begin(), state.clangDiagnostics.end());
    }

    result.diagnostics.insert(result.diagnostics.begin(), clangSummaryMessage(result));
    result.diagnostics.insert(result.diagnostics.begin(), "FRONTEND clang_experiment=enabled default=LightweightFrontend");
    return result;
}
