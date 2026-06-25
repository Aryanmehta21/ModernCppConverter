#include "converter/IndexLoopModernizationPass.h"

#include "converter/RewriteCoordinator.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <string_view>
#include <utility>
#include <vector>

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
                      std::string before,
                      std::string after,
                      std::string reason)
{
    changes.push_back(ConversionChange{
        "Index loop to range-based for",
        std::move(before),
        std::move(after),
        std::move(reason),
        true,
        false,
    });
}

void addDiagnostic(std::vector<ConversionChange>& changes,
                   const int candidates,
                   const int converted,
                   const int skipped)
{
    changes.push_back(ConversionChange{
        "Functional diagnostics: IndexLoopModernizationPass",
        "pass started",
        {},
        "candidates found: " + std::to_string(candidates)
            + ", candidates converted: " + std::to_string(converted)
            + ", candidates skipped: " + std::to_string(skipped),
        false,
        true,
    });
}

void addSkippedDiagnostic(std::vector<ConversionChange>& changes, std::string reason)
{
    changes.push_back(ConversionChange{
        "Functional diagnostics: IndexLoopModernizationPass skip",
        "candidate skipped",
        {},
        std::move(reason),
        false,
        true,
    });
}

bool containsIdentifier(const std::string& text, const std::string& identifier)
{
    return std::regex_search(text, std::regex(R"(\b)" + escapeRegex(identifier) + R"(\b)"));
}

std::vector<std::pair<std::size_t, std::size_t>> protectedSourceRanges(const std::string& text)
{
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    enum class State
    {
        Code,
        LineComment,
        BlockComment,
        StringLiteral,
        CharLiteral,
    };

    State state = State::Code;
    std::size_t rangeStart = 0;
    for (std::size_t index = 0; index < text.size();) {
        const char current = text[index];
        const char next = index + 1 < text.size() ? text[index + 1] : '\0';

        switch (state) {
        case State::Code:
            if (current == '/' && next == '/') {
                state = State::LineComment;
                rangeStart = index;
                index += 2;
            } else if (current == '/' && next == '*') {
                state = State::BlockComment;
                rangeStart = index;
                index += 2;
            } else if (current == '"') {
                state = State::StringLiteral;
                rangeStart = index++;
            } else if (current == '\'') {
                state = State::CharLiteral;
                rangeStart = index++;
            } else {
                ++index;
            }
            break;
        case State::LineComment:
            if (current == '\n') {
                ranges.push_back({rangeStart, index});
                state = State::Code;
            }
            ++index;
            break;
        case State::BlockComment:
            if (current == '*' && next == '/') {
                index += 2;
                ranges.push_back({rangeStart, index});
                state = State::Code;
            } else {
                ++index;
            }
            break;
        case State::StringLiteral:
        case State::CharLiteral:
            if (current == '\\' && index + 1 < text.size()) {
                index += 2;
                break;
            }
            if ((state == State::StringLiteral && current == '"')
                || (state == State::CharLiteral && current == '\'')) {
                ++index;
                ranges.push_back({rangeStart, index});
                state = State::Code;
            } else {
                ++index;
            }
            break;
        }
    }

    if (state != State::Code) {
        ranges.push_back({rangeStart, text.size()});
    }
    return ranges;
}

bool isInsideProtectedRange(const std::vector<std::pair<std::size_t, std::size_t>>& ranges,
                            std::size_t start,
                            std::size_t end)
{
    return std::any_of(ranges.begin(), ranges.end(), [start, end](const auto& range) {
        return start < range.second && range.first < end;
    });
}

std::string maskProtectedSource(const std::string& text)
{
    std::string masked = text;
    for (const auto& range : protectedSourceRanges(text)) {
        for (std::size_t index = range.first; index < range.second && index < masked.size(); ++index) {
            if (masked[index] != '\n') {
                masked[index] = ' ';
            }
        }
    }
    return masked;
}

std::string baseNameForCollection(std::string collection)
{
    const std::size_t separator = collection.find_last_of(".>");
    if (separator != std::string::npos) {
        collection = collection.substr(separator + 1);
    }
    collection.erase(std::remove_if(collection.begin(), collection.end(), [](unsigned char character) {
                         return !std::isalnum(character) && character != '_';
                     }),
                     collection.end());
    if (collection.size() > 1 && collection.back() == 's') {
        collection.pop_back();
    }
    return collection.empty() ? "item" : collection;
}

std::string finalIdentifierFromCollection(std::string collection)
{
    collection = trim(std::move(collection));
    std::smatch match;
    const std::regex trailingIdentifier(R"(([A-Za-z_]\w*)\s*(?:\[[^\]]+\])?\s*$)");
    if (std::regex_search(collection, match, trailingIdentifier)) {
        return match[1].str();
    }
    return {};
}

std::string variableNameForCollection(const std::string& collection, const std::string& body)
{
    std::vector<std::string> candidates;
    candidates.push_back(baseNameForCollection(collection));
    candidates.push_back("item");
    candidates.push_back("element");
    candidates.push_back("value");
    candidates.push_back("entry");

    const std::string collectionIdentifier = finalIdentifierFromCollection(collection);
    for (const std::string& candidate : candidates) {
        if (candidate.empty() || candidate == collectionIdentifier || containsIdentifier(body, candidate)) {
            continue;
        }
        return candidate;
    }
    return collectionIdentifier == "item" ? "element" : "item";
}

struct RedundantReferenceAlias
{
    std::string name;
    bool mutableReference = false;
};

std::optional<RedundantReferenceAlias> removeRedundantReferenceAlias(std::string& body,
                                                                     const std::string& indexedExpression)
{
    const std::regex aliasPattern(
        R"((^|\n)([ \t]*(?:const\s+)?[A-Za-z_:][A-Za-z0-9_:<>, \t]*(?:\s+const)?\s*&\s*([A-Za-z_]\w*)\s*=\s*)"
            + indexedExpression
            + R"(\s*;\s*(?:\n|$)))",
        std::regex::ECMAScript);

    std::smatch match;
    if (!std::regex_search(body, match, aliasPattern)) {
        return std::nullopt;
    }

    RedundantReferenceAlias alias;
    alias.name = match[3].str();
    const std::string declarationPrefix = match[2].str();
    alias.mutableReference = !std::regex_search(declarationPrefix, std::regex(R"(\bconst\b)"));

    const std::string linePrefix = match[1].str();
    const std::string replacement = linePrefix.empty() ? std::string{} : "\n";
    body.replace(static_cast<std::size_t>(match.position()),
                 static_cast<std::size_t>(match.length()),
                 replacement);
    return alias;
}

RewriteApplicationResult replaceIndexedExpressionsWithRangeEdits(const std::string& body,
                                                                  const std::string& indexedExpression,
                                                                  const std::string& itemName)
{
    std::vector<RewriteEdit> edits;
    const std::regex expressionPattern(indexedExpression, std::regex::ECMAScript);
    const std::vector<std::pair<std::size_t, std::size_t>> protectedRanges = protectedSourceRanges(body);

    for (std::sregex_iterator it(body.begin(), body.end(), expressionPattern), end; it != end; ++it) {
        const std::size_t start = static_cast<std::size_t>(it->position());
        const std::size_t finish = start + static_cast<std::size_t>(it->length());
        if (isInsideProtectedRange(protectedRanges, start, finish)) {
            continue;
        }

        SourceRange range;
        range.start.offset = start;
        range.start.column = start + 1;
        range.end.offset = finish;
        range.end.column = finish + 1;
        range.entityKind = SourceEntityKind::Expression;
        range.entityName = it->str();

        RewriteEdit edit;
        edit.range = std::move(range);
        edit.replacementText = itemName;
        edit.passName = "IndexLoopModernizationPass";
        edit.reason = "Replace index expression with the generated range-for variable.";
        edit.affectedSymbol = itemName;
        edits.push_back(std::move(edit));
    }

    return RewriteCoordinator{}.apply(body, edits);
}
} // namespace

std::string IndexLoopModernizationPass::rewrite(const std::string& code,
                                                const ModernizationOptions& options,
                                                std::vector<ConversionChange>& changes) const
{
    if (!options.useRangeBasedFor) {
        return code;
    }

    std::string updated = code;
    const std::string collectionExpression =
        R"((?:this|[A-Za-z_]\w*)(?:\s*\[[^\]\n;]+\])?(?:(?:\.|->)[A-Za-z_]\w*(?:\s*\[[^\]\n;]+\])?)*)";
    const std::regex loopPattern(
        R"((^[ \t]*)for\s*\(\s*(?:std::size_t|size_t|int|auto)\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\2\s*<\s*()"
            + collectionExpression
            + R"()\s*\.\s*size\s*\(\s*\)\s*;\s*(?:\+\+\2|\2\+\+)\s*\)\s*(?:\n\1)?\{\s*\n?([\s\S]*?)\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string search = updated;
    std::size_t consumed = 0;
    int candidates = 0;
    int converted = 0;
    int skipped = 0;
    while (std::regex_search(search, match, loopPattern)) {
        ++candidates;
        const std::string indent = match[1].str();
        const std::string indexName = match[2].str();
        const std::string collection = match[3].str();
        std::string body = match[4].str();
        if (body.find(".erase") != std::string::npos
            || body.find(".insert") != std::string::npos
            || body.find(collection + ".push_back") != std::string::npos
            || body.find(collection + ".emplace_back") != std::string::npos) {
            ++skipped;
            addSkippedDiagnostic(changes, "Index loop was preserved because the loop mutates container size while traversing.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::string indexedExpression = escapeRegex(collection) + R"(\s*\[\s*)" + escapeRegex(indexName) + R"(\s*\])";
        const std::regex anyIndexAccess(R"(\b[A-Za-z_]\w*(?:(?:\.|->)[A-Za-z_]\w*)*\s*\[\s*)"
                                        + escapeRegex(indexName)
                                        + R"(\s*\])");
        const std::string analysisBody = maskProtectedSource(body);
        std::string bodyWithoutTarget = std::regex_replace(analysisBody, std::regex(indexedExpression), "");
        if (std::regex_search(bodyWithoutTarget, anyIndexAccess)
            || std::regex_search(bodyWithoutTarget, std::regex("\\b" + escapeRegex(indexName) + "\\b"))
            || std::regex_search(analysisBody, std::regex("\\b" + escapeRegex(indexName) + R"(\s*(?:\+|-|\*|/|%))"))
            || std::regex_search(analysisBody, std::regex(R"((?:\+|-|\*|/|%)\s*)" + escapeRegex(indexName) + R"(\b)"))) {
            ++skipped;
            addSkippedDiagnostic(changes, "Index loop was preserved because the index has semantic meaning beyond selecting the current element.");
            consumed += static_cast<std::size_t>(match.position() + match.length());
            search = match.suffix().str();
            continue;
        }

        const std::optional<RedundantReferenceAlias> redundantAlias =
            removeRedundantReferenceAlias(body, indexedExpression);
        const std::string analysisBodyAfterAlias = maskProtectedSource(body);

        const bool mutableElement = (redundantAlias.has_value() && redundantAlias->mutableReference)
            || std::regex_search(analysisBodyAfterAlias, std::regex(indexedExpression + R"(\s*(?:=|\+=|-=|\*=|/=|%=|\+\+|--))"))
            || std::regex_search(analysisBodyAfterAlias, std::regex(R"((?:\+\+|--)\s*)" + indexedExpression))
            || std::regex_search(analysisBodyAfterAlias, std::regex(indexedExpression + R"(\s*(?:\.|->)[^;\n]*\s*(?:=|\+=|-=|\*=|/=|%=|\+\+|--|\())"));
        const std::string itemName = redundantAlias.has_value()
            ? redundantAlias->name
            : variableNameForCollection(collection, body);

        const RewriteApplicationResult rewriteResult =
            replaceIndexedExpressionsWithRangeEdits(body, indexedExpression, itemName);
        for (const SkippedRewriteEdit& skippedEdit : rewriteResult.skippedEdits) {
            addSkippedDiagnostic(changes,
                                 "Source-range edit from " + skippedEdit.edit.passName
                                     + " was skipped: " + skippedEdit.reason);
        }
        body = rewriteResult.code;
        body = std::regex_replace(body, std::regex(R"(std::endl)"), "'\\n'");

        const std::string replacement = indent + "for (" + (mutableElement ? "auto& " : "const auto& ")
            + itemName + " : " + collection + ")\n" + indent + "{\n" + body + "\n" + indent + "}";
        updated.replace(consumed + static_cast<std::size_t>(match.position()),
                        static_cast<std::size_t>(match.length()),
                        replacement);
        ++converted;
        addAppliedChange(changes,
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted a size-based index loop to range-based for because the index only selected the current element.");
        consumed += static_cast<std::size_t>(match.position()) + replacement.size();
        search = updated.substr(consumed);
    }

    addDiagnostic(changes, candidates, converted, skipped);
    return updated;
}
