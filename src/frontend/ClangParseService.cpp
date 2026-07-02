#include "frontend/ClangParseService.h"

#include "frontend/ClangExperimentalFrontend.h"
#include "parser/LightweightCppParser.h"
#include "utils/ClangRuntimeSafety.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#include <sstream>
#include <utility>

namespace
{
constexpr int clangParseTimeoutMs = 10000;

FrontendEntityCounts entityCountsFor(const ParsedDocument& document)
{
    return FrontendEntityCounts{
        document.aggregates.size(),
        document.functions.size(),
        document.enums.size(),
        document.memberVariables.size() + document.globalVariables.size() + document.localVariables.size(),
    };
}

QJsonObject positionToJson(const SourcePosition& position)
{
    return {
        {"offset", static_cast<qint64>(position.offset)},
        {"line", static_cast<qint64>(position.line)},
        {"column", static_cast<qint64>(position.column)},
    };
}

SourcePosition positionFromJson(const QJsonObject& object)
{
    SourcePosition position;
    position.offset = static_cast<std::size_t>(object.value("offset").toInteger());
    position.line = static_cast<std::size_t>(object.value("line").toInteger(1));
    position.column = static_cast<std::size_t>(object.value("column").toInteger(1));
    return position;
}

QJsonObject rangeToJson(const SourceRange& range)
{
    QJsonObject object;
    object.insert("start", positionToJson(range.start));
    object.insert("end", positionToJson(range.end));
    object.insert("entityKind", static_cast<int>(range.entityKind));
    object.insert("entityName", QString::fromStdString(range.entityName));
    if (range.parentScopeId) {
        object.insert("parentScopeId", static_cast<qint64>(*range.parentScopeId));
    }
    return object;
}

SourceRange rangeFromJson(const QJsonObject& object)
{
    SourceRange range;
    range.start = positionFromJson(object.value("start").toObject());
    range.end = positionFromJson(object.value("end").toObject());
    range.entityKind = static_cast<SourceEntityKind>(object.value("entityKind").toInt());
    range.entityName = object.value("entityName").toString().toStdString();
    if (object.contains("parentScopeId")) {
        range.parentScopeId = static_cast<std::size_t>(object.value("parentScopeId").toInteger());
    }
    return range;
}

QJsonObject tokenToJson(const CppToken& token)
{
    return {
        {"text", QString::fromStdString(token.text)},
        {"kind", static_cast<int>(token.kind)},
        {"range", rangeToJson(token.range)},
    };
}

CppToken tokenFromJson(const QJsonObject& object)
{
    CppToken token;
    token.text = object.value("text").toString().toStdString();
    token.kind = static_cast<CppTokenKind>(object.value("kind").toInt());
    token.range = rangeFromJson(object.value("range").toObject());
    return token;
}

QJsonObject includeToJson(const ParsedIncludeDirective& include)
{
    return {{"path", QString::fromStdString(include.path)}, {"range", rangeToJson(include.range)}};
}

ParsedIncludeDirective includeFromJson(const QJsonObject& object)
{
    ParsedIncludeDirective include;
    include.path = object.value("path").toString().toStdString();
    include.range = rangeFromJson(object.value("range").toObject());
    return include;
}

QJsonObject macroToJson(const ParsedMacroDirective& macro)
{
    return {{"name", QString::fromStdString(macro.name)}, {"range", rangeToJson(macro.range)}};
}

ParsedMacroDirective macroFromJson(const QJsonObject& object)
{
    ParsedMacroDirective macro;
    macro.name = object.value("name").toString().toStdString();
    macro.range = rangeFromJson(object.value("range").toObject());
    return macro;
}

QJsonObject parameterToJson(const ParsedParameter& parameter)
{
    return {
        {"name", QString::fromStdString(parameter.name)},
        {"type", QString::fromStdString(parameter.type)},
        {"canonicalType", QString::fromStdString(parameter.canonicalType)},
        {"range", rangeToJson(parameter.range)},
        {"nameRange", rangeToJson(parameter.nameRange)},
    };
}

ParsedParameter parameterFromJson(const QJsonObject& object)
{
    ParsedParameter parameter;
    parameter.name = object.value("name").toString().toStdString();
    parameter.type = object.value("type").toString().toStdString();
    parameter.canonicalType = object.value("canonicalType").toString().toStdString();
    parameter.range = rangeFromJson(object.value("range").toObject());
    parameter.nameRange = rangeFromJson(object.value("nameRange").toObject());
    return parameter;
}

QJsonObject aggregateToJson(const ParsedAggregate& aggregate)
{
    QJsonArray bases;
    for (const std::string& base : aggregate.baseNames) {
        bases.append(QString::fromStdString(base));
    }
    return {
        {"kind", static_cast<int>(aggregate.kind)},
        {"name", QString::fromStdString(aggregate.name)},
        {"baseNames", bases},
        {"range", rangeToJson(aggregate.range)},
        {"nameRange", rangeToJson(aggregate.nameRange)},
        {"bodyRange", rangeToJson(aggregate.bodyRange)},
    };
}

ParsedAggregate aggregateFromJson(const QJsonObject& object)
{
    ParsedAggregate aggregate;
    aggregate.kind = static_cast<ParsedAggregateKind>(object.value("kind").toInt());
    aggregate.name = object.value("name").toString().toStdString();
    for (const QJsonValue& base : object.value("baseNames").toArray()) {
        aggregate.baseNames.push_back(base.toString().toStdString());
    }
    aggregate.range = rangeFromJson(object.value("range").toObject());
    aggregate.nameRange = rangeFromJson(object.value("nameRange").toObject());
    aggregate.bodyRange = rangeFromJson(object.value("bodyRange").toObject());
    return aggregate;
}

QJsonObject enumToJson(const ParsedEnum& parsedEnum)
{
    QJsonArray enumerators;
    for (const std::string& enumerator : parsedEnum.enumerators) {
        enumerators.append(QString::fromStdString(enumerator));
    }
    return {
        {"name", QString::fromStdString(parsedEnum.name)},
        {"scoped", parsedEnum.scoped},
        {"underlyingType", QString::fromStdString(parsedEnum.underlyingType)},
        {"enumerators", enumerators},
        {"range", rangeToJson(parsedEnum.range)},
        {"nameRange", rangeToJson(parsedEnum.nameRange)},
        {"bodyRange", rangeToJson(parsedEnum.bodyRange)},
    };
}

ParsedEnum enumFromJson(const QJsonObject& object)
{
    ParsedEnum parsedEnum;
    parsedEnum.name = object.value("name").toString().toStdString();
    parsedEnum.scoped = object.value("scoped").toBool();
    parsedEnum.underlyingType = object.value("underlyingType").toString().toStdString();
    for (const QJsonValue& enumerator : object.value("enumerators").toArray()) {
        parsedEnum.enumerators.push_back(enumerator.toString().toStdString());
    }
    parsedEnum.range = rangeFromJson(object.value("range").toObject());
    parsedEnum.nameRange = rangeFromJson(object.value("nameRange").toObject());
    parsedEnum.bodyRange = rangeFromJson(object.value("bodyRange").toObject());
    return parsedEnum;
}

QJsonObject functionToJson(const ParsedFunction& function)
{
    QJsonArray parameters;
    for (const ParsedParameter& parameter : function.parameters) {
        parameters.append(parameterToJson(parameter));
    }
    return {
        {"name", QString::fromStdString(function.name)},
        {"returnType", QString::fromStdString(function.returnType)},
        {"canonicalReturnType", QString::fromStdString(function.canonicalReturnType)},
        {"parentName", QString::fromStdString(function.parentName)},
        {"parameters", parameters},
        {"range", rangeToJson(function.range)},
        {"nameRange", rangeToJson(function.nameRange)},
        {"bodyRange", rangeToJson(function.bodyRange)},
        {"isMember", function.isMember},
        {"isConst", function.isConst},
        {"hasBody", function.hasBody},
    };
}

ParsedFunction functionFromJson(const QJsonObject& object)
{
    ParsedFunction function;
    function.name = object.value("name").toString().toStdString();
    function.returnType = object.value("returnType").toString().toStdString();
    function.canonicalReturnType = object.value("canonicalReturnType").toString().toStdString();
    function.parentName = object.value("parentName").toString().toStdString();
    for (const QJsonValue& parameter : object.value("parameters").toArray()) {
        function.parameters.push_back(parameterFromJson(parameter.toObject()));
    }
    function.range = rangeFromJson(object.value("range").toObject());
    function.nameRange = rangeFromJson(object.value("nameRange").toObject());
    function.bodyRange = rangeFromJson(object.value("bodyRange").toObject());
    function.isMember = object.value("isMember").toBool();
    function.isConst = object.value("isConst").toBool();
    function.hasBody = object.value("hasBody").toBool();
    return function;
}

QJsonObject variableToJson(const ParsedVariable& variable)
{
    return {
        {"name", QString::fromStdString(variable.name)},
        {"type", QString::fromStdString(variable.type)},
        {"canonicalType", QString::fromStdString(variable.canonicalType)},
        {"parentName", QString::fromStdString(variable.parentName)},
        {"range", rangeToJson(variable.range)},
        {"nameRange", rangeToJson(variable.nameRange)},
        {"isMember", variable.isMember},
    };
}

ParsedVariable variableFromJson(const QJsonObject& object)
{
    ParsedVariable variable;
    variable.name = object.value("name").toString().toStdString();
    variable.type = object.value("type").toString().toStdString();
    variable.canonicalType = object.value("canonicalType").toString().toStdString();
    variable.parentName = object.value("parentName").toString().toStdString();
    variable.range = rangeFromJson(object.value("range").toObject());
    variable.nameRange = rangeFromJson(object.value("nameRange").toObject());
    variable.isMember = object.value("isMember").toBool();
    return variable;
}

QJsonObject symbolToJson(const ParsedSymbol& symbol)
{
    QJsonObject object{
        {"id", static_cast<qint64>(symbol.id)},
        {"kind", static_cast<int>(symbol.kind)},
        {"name", QString::fromStdString(symbol.name)},
        {"qualifiedName", QString::fromStdString(symbol.qualifiedName)},
        {"type", QString::fromStdString(symbol.type)},
        {"canonicalType", QString::fromStdString(symbol.canonicalType)},
        {"range", rangeToJson(symbol.range)},
        {"nameRange", rangeToJson(symbol.nameRange)},
        {"isDefinition", symbol.isDefinition},
    };
    if (symbol.parentId) {
        object.insert("parentId", static_cast<qint64>(*symbol.parentId));
    }
    return object;
}

ParsedSymbol symbolFromJson(const QJsonObject& object)
{
    ParsedSymbol symbol;
    symbol.id = static_cast<ParsedSymbolId>(object.value("id").toInteger());
    symbol.kind = static_cast<ParsedSymbolKind>(object.value("kind").toInt());
    symbol.name = object.value("name").toString().toStdString();
    symbol.qualifiedName = object.value("qualifiedName").toString().toStdString();
    symbol.type = object.value("type").toString().toStdString();
    symbol.canonicalType = object.value("canonicalType").toString().toStdString();
    if (object.contains("parentId")) {
        symbol.parentId = static_cast<ParsedSymbolId>(object.value("parentId").toInteger());
    }
    symbol.range = rangeFromJson(object.value("range").toObject());
    symbol.nameRange = rangeFromJson(object.value("nameRange").toObject());
    symbol.isDefinition = object.value("isDefinition").toBool();
    return symbol;
}

template <typename T, typename ToJson>
QJsonArray toJsonArray(const std::vector<T>& values, ToJson toJson)
{
    QJsonArray array;
    for (const T& value : values) {
        array.append(toJson(value));
    }
    return array;
}

template <typename T, typename FromJson>
std::vector<T> fromJsonArray(const QJsonArray& array, FromJson fromJson)
{
    std::vector<T> values;
    values.reserve(static_cast<std::size_t>(array.size()));
    for (const QJsonValue& value : array) {
        values.push_back(fromJson(value.toObject()));
    }
    return values;
}

QJsonObject documentToJson(const ParsedDocument& document)
{
    return {
        {"parseSucceeded", document.parseSucceeded},
        {"tokens", toJsonArray(document.tokens, tokenToJson)},
        {"includes", toJsonArray(document.includes, includeToJson)},
        {"macros", toJsonArray(document.macros, macroToJson)},
        {"aggregates", toJsonArray(document.aggregates, aggregateToJson)},
        {"enums", toJsonArray(document.enums, enumToJson)},
        {"functions", toJsonArray(document.functions, functionToJson)},
        {"memberVariables", toJsonArray(document.memberVariables, variableToJson)},
        {"globalVariables", toJsonArray(document.globalVariables, variableToJson)},
        {"localVariables", toJsonArray(document.localVariables, variableToJson)},
        {"symbols", toJsonArray(document.symbols, symbolToJson)},
    };
}

ParsedDocument documentFromJson(const QJsonObject& object, const std::string& originalSource)
{
    ParsedDocument document;
    document.originalSource = originalSource;
    document.parseSucceeded = object.value("parseSucceeded").toBool(true);
    document.tokens = fromJsonArray<CppToken>(object.value("tokens").toArray(), tokenFromJson);
    document.includes = fromJsonArray<ParsedIncludeDirective>(object.value("includes").toArray(), includeFromJson);
    document.macros = fromJsonArray<ParsedMacroDirective>(object.value("macros").toArray(), macroFromJson);
    document.aggregates = fromJsonArray<ParsedAggregate>(object.value("aggregates").toArray(), aggregateFromJson);
    document.enums = fromJsonArray<ParsedEnum>(object.value("enums").toArray(), enumFromJson);
    document.functions = fromJsonArray<ParsedFunction>(object.value("functions").toArray(), functionFromJson);
    document.memberVariables = fromJsonArray<ParsedVariable>(object.value("memberVariables").toArray(), variableFromJson);
    document.globalVariables = fromJsonArray<ParsedVariable>(object.value("globalVariables").toArray(), variableFromJson);
    document.localVariables = fromJsonArray<ParsedVariable>(object.value("localVariables").toArray(), variableFromJson);
    document.symbols = fromJsonArray<ParsedSymbol>(object.value("symbols").toArray(), symbolFromJson);
    return document;
}

ModernizationFrontendResult fallbackResult(const std::string& source, std::string reason)
{
    ModernizationFrontendResult result;
    result.kind = ModernizationFrontendKind::ClangExperimental;
    result.frontendName = "ClangExperimentalFrontend";
    result.document = LightweightCppParser{}.parse(source);
    result.parseSucceeded = result.document.parseSucceeded;
    result.entityCounts = entityCountsFor(result.document);
    result.diagnostics.push_back("FRONTEND clang_experiment=enabled default=LightweightFrontend");
    result.diagnostics.push_back("FRONTEND used=ClangExperimentalFrontend experimental=true parse=fallback classes="
                                 + std::to_string(result.entityCounts.classes)
                                 + " functions=" + std::to_string(result.entityCounts.functions)
                                 + " enums=" + std::to_string(result.entityCounts.enums)
                                 + " variables=" + std::to_string(result.entityCounts.variables));
    result.diagnostics.push_back("FRONTEND clang_parse=failure fallback=LightweightFrontend");
    result.diagnostics.push_back("CLANG DIAGNOSTIC severity=fatal message=\"" + std::move(reason) + "\"");
    return result;
}

QString helperExecutablePath()
{
    const QString executableName =
#if defined(Q_OS_WIN)
        QStringLiteral("ModernCppClangParseHelper.exe");
#else
        QStringLiteral("ModernCppClangParseHelper");
#endif

    if (QCoreApplication::instance() != nullptr) {
        const QString sibling = QDir(QCoreApplication::applicationDirPath()).filePath(executableName);
        if (QFileInfo::exists(sibling)) {
            return sibling;
        }
    }

    return QStandardPaths::findExecutable(executableName);
}
} // namespace

ClangParseService::ClangParseService(ClangParseConfig config)
    : config_(std::move(config))
{
}

ModernizationFrontendResult ClangParseService::parse(const std::string& source) const
{
    if (ClangRuntimeSafety::inProcessClangAllowed()) {
        return parseInProcess(source);
    }
    return parseOutOfProcess(source);
}

ModernizationFrontendResult ClangParseService::parseInProcess(const std::string& source) const
{
    ClangExperimentalFrontend frontend(config_);
    ModernizationFrontendResult result = frontend.analyzeInProcess(source);
    result.diagnostics.push_back("FRONTEND clang_parse_enabled=true isolated_process=false timeout_ms=0");
    return result;
}

ModernizationFrontendResult ClangParseService::parseOutOfProcess(const std::string& source) const
{
    const QString helperPath = helperExecutablePath();
    if (helperPath.isEmpty()) {
        return fallbackResult(source, "Clang parse helper executable unavailable");
    }

    QStringList arguments;
    arguments << QStringLiteral("--standard") << QString::fromStdString(config_.languageStandard)
              << QStringLiteral("--virtual-file") << QString::fromStdString(config_.virtualFileName);
    if (!config_.resourceDir.empty()) {
        arguments << QStringLiteral("--resource-dir") << QString::fromStdString(config_.resourceDir);
    }
    if (!config_.systemRoot.empty()) {
        arguments << QStringLiteral("--system-root") << QString::fromStdString(config_.systemRoot);
    }
    for (const std::string& includePath : config_.includePaths) {
        arguments << QStringLiteral("--include") << QString::fromStdString(includePath);
    }
    for (const std::string& compileArgument : config_.compileArguments) {
        arguments << QStringLiteral("--arg") << QString::fromStdString(compileArgument);
    }

    QProcess process;
    process.setProgram(helperPath);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(2000)) {
        return fallbackResult(source, "Clang parse helper failed to start");
    }

    process.write(QByteArray::fromStdString(source));
    process.closeWriteChannel();
    if (!process.waitForFinished(clangParseTimeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return fallbackResult(source, "Clang parse helper timed out");
    }

    const QByteArray standardOutput = process.readAllStandardOutput();
    const QByteArray standardError = process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        std::ostringstream reason;
        reason << "Clang parse helper failed";
        if (process.exitStatus() == QProcess::CrashExit) {
            reason << " after crash";
        }
        if (!standardError.isEmpty()) {
            reason << ": " << standardError.left(500).toStdString();
        }
        return fallbackResult(source, reason.str());
    }

    std::optional<ModernizationFrontendResult> parsed = deserializeClangFrontendResult(standardOutput.toStdString(),
                                                                                       source);
    if (!parsed) {
        return fallbackResult(source, "Clang parse helper returned invalid payload");
    }

    parsed->diagnostics.push_back("FRONTEND clang_parse_enabled=true isolated_process=true timeout_ms="
                                  + std::to_string(clangParseTimeoutMs));
    return *parsed;
}

std::string serializeClangFrontendResult(const ModernizationFrontendResult& result)
{
    QJsonObject root;
    root.insert("kind", static_cast<int>(result.kind));
    root.insert("frontendName", QString::fromStdString(result.frontendName));
    root.insert("parseSucceeded", result.parseSucceeded);
    root.insert("document", documentToJson(result.document));
    root.insert("classes", static_cast<qint64>(result.entityCounts.classes));
    root.insert("functions", static_cast<qint64>(result.entityCounts.functions));
    root.insert("enums", static_cast<qint64>(result.entityCounts.enums));
    root.insert("variables", static_cast<qint64>(result.entityCounts.variables));

    QJsonArray diagnostics;
    for (const std::string& diagnostic : result.diagnostics) {
        diagnostics.append(QString::fromStdString(diagnostic));
    }
    root.insert("diagnostics", diagnostics);
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

std::optional<ModernizationFrontendResult> deserializeClangFrontendResult(const std::string& payload,
                                                                          const std::string& originalSource)
{
    QJsonParseError error;
    const QJsonDocument json = QJsonDocument::fromJson(QByteArray::fromStdString(payload), &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) {
        return std::nullopt;
    }

    const QJsonObject root = json.object();
    ModernizationFrontendResult result;
    result.kind = static_cast<ModernizationFrontendKind>(root.value("kind").toInt(static_cast<int>(ModernizationFrontendKind::ClangExperimental)));
    result.frontendName = root.value("frontendName").toString(QStringLiteral("ClangExperimentalFrontend")).toStdString();
    result.parseSucceeded = root.value("parseSucceeded").toBool();
    result.document = documentFromJson(root.value("document").toObject(), originalSource);
    result.entityCounts.classes = static_cast<std::size_t>(root.value("classes").toInteger());
    result.entityCounts.functions = static_cast<std::size_t>(root.value("functions").toInteger());
    result.entityCounts.enums = static_cast<std::size_t>(root.value("enums").toInteger());
    result.entityCounts.variables = static_cast<std::size_t>(root.value("variables").toInteger());
    for (const QJsonValue& diagnostic : root.value("diagnostics").toArray()) {
        result.diagnostics.push_back(diagnostic.toString().toStdString());
    }
    return result;
}
