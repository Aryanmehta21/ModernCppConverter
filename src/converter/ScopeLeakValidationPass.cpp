#include "converter/ScopeLeakValidationPass.h"

#include "converter/ScopeAwareSymbolTable.h"
#include "converter/StructuralAnalyzers.h"

#include <algorithm>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{
std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string escapeRegex(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() * 2);
    for (const char character : text) {
        if (std::string_view(R"(\.^$|()[]{}*+?)").find(character) != std::string_view::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

void addAppliedChange(std::vector<ConversionChange>& changes,
                      std::string ruleName,
                      std::string before,
                      std::string after,
                      std::string reason)
{
    changes.push_back(ConversionChange{
        std::move(ruleName),
        std::move(before),
        std::move(after),
        std::move(reason),
        true,
        false,
    });
}

void addSuggestion(std::vector<ConversionChange>& changes,
                   std::string ruleName,
                   std::string before,
                   std::string reason)
{
    changes.push_back(ConversionChange{
        std::move(ruleName),
        std::move(before),
        {},
        std::move(reason),
        false,
        false,
    });
}

std::size_t findMatchingBrace(const std::string& code, const std::size_t openBracePosition)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = openBracePosition; index < code.size(); ++index) {
        const char current = code[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (current == '\\') {
            escaped = true;
            continue;
        }
        if (current == '"' && !inCharacter) {
            inString = !inString;
            continue;
        }
        if (current == '\'' && !inString) {
            inCharacter = !inCharacter;
            continue;
        }
        if (inString || inCharacter) {
            continue;
        }
        if (current == '{') {
            ++depth;
        } else if (current == '}') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}

bool isVectorLikeRecord(const TypeChangeRecord& record)
{
    return record.isClassMember
        && !record.scopeName.empty()
        && (record.newType.starts_with("std::vector<") || record.newType.starts_with("std::array<"));
}

std::vector<std::string> localSymbolsInBody(const std::string& body)
{
    std::vector<std::string> symbols;
    const std::regex declarationPattern(
        R"((?:^|[;\n]\s*)(?:auto|const\s+auto|std::[A-Za-z_][A-Za-z0-9_:<> ,&*]*|[A-Za-z_:][A-Za-z0-9_:<>]*)(?:\s+|\s*[*&]\s*)([A-Za-z_]\w*)\s*(?:=|\{|;|\)))",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(body.begin(), body.end(), declarationPattern), end; iterator != end; ++iterator) {
        symbols.push_back((*iterator)[1].str());
    }
    return symbols;
}

bool containsSymbol(const std::vector<std::string>& symbols, const std::string& symbol)
{
    return std::find(symbols.begin(), symbols.end(), symbol) != symbols.end();
}

const TypeChangeRecord* singleVectorCandidateForClass(const TransformationContext& context, const std::string& className)
{
    const TypeChangeRecord* candidate = nullptr;
    for (const TypeChangeRecord& record : context.typeChanges()) {
        if (!isVectorLikeRecord(record) || record.scopeName != className) {
            continue;
        }
        if (candidate != nullptr) {
            return nullptr;
        }
        candidate = &record;
    }
    return candidate;
}

bool compilerMentionsUndeclared(const std::string& compilerOutput)
{
    return compilerOutput.find("undeclared") != std::string::npos
        || compilerOutput.find("not declared") != std::string::npos
        || compilerOutput.find("was not declared") != std::string::npos;
}

std::string validateClassMethods(std::string classText,
                                 const std::string& className,
                                 const ScopeAwareSymbolTable& symbolTable,
                                 const TransformationContext& context,
                                 const std::string& compilerOutput,
                                 std::vector<ConversionChange>& changes,
                                 bool& changed)
{
    const std::regex methodHeader(
        R"((^[ \t]*)(?:[A-Za-z_:][A-Za-z0-9_:<>,&*\s]*\s+)?([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?\{)",
        std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    std::string search = classText;
    std::size_t consumed = 0;
    while (std::regex_search(search, match, methodHeader)) {
        const std::size_t position = consumed + static_cast<std::size_t>(match.position());
        const std::size_t openBrace = position + static_cast<std::size_t>(match.length()) - 1;
        const std::size_t closeBrace = findMatchingBrace(classText, openBrace);
        if (closeBrace == std::string::npos) {
            break;
        }

        const std::string methodText = classText.substr(position, closeBrace - position + 1);
        std::string body = classText.substr(openBrace + 1, closeBrace - openBrace - 1);
        const std::vector<std::string> locals = localSymbolsInBody(body);
        const TypeChangeRecord* fallbackMember = singleVectorCandidateForClass(context, className);

        const std::regex suspiciousReferencePattern(R"(\b([A-Za-z_]\w*)\s*\.\s*(size|data|empty)\s*\(\s*\))");
        std::smatch referenceMatch;
        std::string bodySearch = body;
        bool methodChanged = false;
        while (std::regex_search(bodySearch, referenceMatch, suspiciousReferencePattern)) {
            const std::string symbol = referenceMatch[1].str();
            const bool visible = containsSymbol(locals, symbol)
                || symbolTable.hasClassMember(className, symbol)
                || symbolTable.hasGlobal(symbol);
            if (visible) {
                bodySearch = referenceMatch.suffix().str();
                continue;
            }

            if (fallbackMember != nullptr) {
                const std::string before = referenceMatch[0].str();
                const std::string after = fallbackMember->symbolName + "." + referenceMatch[2].str() + "()";
                body = std::regex_replace(body,
                                          std::regex("\\b" + escapeRegex(symbol) + R"(\s*\.\s*)"
                                                         + referenceMatch[2].str() + R"(\s*\(\s*\))"),
                                          after);
                addAppliedChange(changes,
                                 "Scope leak validation",
                                 before,
                                 after,
                                 "Repaired an out-of-scope generated reference by using the vector member visible inside the class.");
                methodChanged = true;
                changed = true;
                bodySearch = body;
                continue;
            }

            addSuggestion(changes,
                          "Scope leak validation",
                          symbol,
                          "Detected a generated reference to a symbol that is not visible in this class/member scope. Manual review is required before trusting this rewrite.");
            bodySearch = referenceMatch.suffix().str();
        }

        const std::regex returnBareSymbolPattern(R"(\breturn\s+([A-Za-z_]\w*)\s*;)");
        bodySearch = body;
        while (std::regex_search(bodySearch, referenceMatch, returnBareSymbolPattern)) {
            const std::string symbol = referenceMatch[1].str();
            const bool visible = containsSymbol(locals, symbol)
                || symbolTable.hasClassMember(className, symbol)
                || symbolTable.hasGlobal(symbol);
            if (!visible && compilerMentionsUndeclared(compilerOutput)) {
                addSuggestion(changes,
                              "Scope leak validation",
                              trim(referenceMatch[0].str()),
                              "Compiler diagnostics mention an undeclared symbol in a return statement. The rewrite was flagged as scope-invalid.");
            }
            bodySearch = referenceMatch.suffix().str();
        }

        if (methodChanged) {
            std::string replacement = methodText;
            replacement.replace(openBrace + 1 - position, closeBrace - openBrace - 1, body);
            classText.replace(position, closeBrace - position + 1, replacement);
            consumed = position + replacement.size();
            search = classText.substr(consumed);
            continue;
        }

        consumed = closeBrace + 1;
        search = classText.substr(consumed);
    }

    return classText;
}
} // namespace

std::string ScopeLeakValidationPass::validate(const std::string& code,
                                              const TransformationContext& context,
                                              const std::string& compilerOutput,
                                              std::vector<ConversionChange>& changes) const
{
    if (context.empty()) {
        return code;
    }

    std::string updated = code;
    const ScopeAwareSymbolTable symbolTable = ScopeAwareSymbolTable::build(updated);
    const ClassResourceAnalyzer classAnalyzer;
    const std::vector<ClassBlock> classes = classAnalyzer.analyzeClasses(updated);
    bool changed = false;

    for (auto iterator = classes.rbegin(); iterator != classes.rend(); ++iterator) {
        std::string classText = updated.substr(iterator->start, iterator->end - iterator->start);
        const std::string before = classText;
        classText = validateClassMethods(std::move(classText),
                                         iterator->name,
                                         symbolTable,
                                         context,
                                         compilerOutput,
                                         changes,
                                         changed);
        if (classText != before) {
            updated.replace(iterator->start, iterator->end - iterator->start, classText);
        }
    }

    if (changed) {
        addAppliedChange(changes,
                         "Scope leak validation pass",
                         "generated scope references",
                         "validated in-scope member references",
                         "Validated generated references after ownership and API cascading so local symbols do not leak across scopes.");
    }

    return updated;
}
