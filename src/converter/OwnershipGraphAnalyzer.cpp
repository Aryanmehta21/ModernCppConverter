#include "converter/OwnershipGraphAnalyzer.h"

#include "converter/StructuralAnalyzers.h"

#include <algorithm>
#include <regex>
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

bool isCompileTimeExtent(const std::string& expression)
{
    return std::regex_match(trim(expression), std::regex(R"(\d+[uUlL]*)"));
}

bool hasPointerToPointerOwnership(const std::string& code,
                                  const std::string& elementType,
                                  const std::string& storageName)
{
    const std::string storage = escapeRegex(storageName);
    const std::string type = escapeRegex(elementType);
    const std::regex innerAllocation(storage + R"(\s*\[[^\]]+\]\s*=\s*new\s+)" + type + R"(\b)");
    const std::regex innerDelete(R"(delete\s+)" + storage + R"(\s*\[[^\]]+\]\s*;)");
    const std::regex outerDelete(R"(delete\s*\[\s*\]\s*)" + storage + R"(\s*;)");
    return std::regex_search(code, innerAllocation)
        && std::regex_search(code, innerDelete)
        && std::regex_search(code, outerDelete);
}

bool hasFixedPointerArrayOwnership(const std::string& code,
                                   const std::string& elementType,
                                   const std::string& storageName)
{
    const std::string storage = escapeRegex(storageName);
    const std::string type = escapeRegex(elementType);
    const std::regex innerAllocation(storage + R"(\s*\[[^\]]+\]\s*=\s*new\s+)" + type + R"(\b)");
    const std::regex innerDelete(R"(delete\s+)" + storage + R"(\s*\[[^\]]+\]\s*;)");
    return std::regex_search(code, innerAllocation) && std::regex_search(code, innerDelete);
}

bool hasVectorPointerOwnership(const std::string& code,
                               const std::string& elementType,
                               const std::string& storageName)
{
    const std::string storage = escapeRegex(storageName);
    const std::string type = escapeRegex(elementType);
    const std::regex pushNew(storage + R"(\s*\.\s*(?:push_back|emplace_back)\s*\(\s*new\s+)" + type + R"(\b)");
    const std::regex deleteLoop(R"(delete\s+[^;\n]*;)");
    return std::regex_search(code, pushNew) && std::regex_search(code, deleteLoop);
}

bool nodeExists(const std::vector<OwnershipGraphNode>& nodes, const std::string& storageName)
{
    return std::any_of(nodes.begin(), nodes.end(), [&storageName](const OwnershipGraphNode& node) {
        return node.storageName == storageName;
    });
}

void addNode(std::vector<OwnershipGraphNode>& nodes, OwnershipGraphNode node)
{
    if (!node.storageName.empty() && !nodeExists(nodes, node.storageName)) {
        nodes.push_back(std::move(node));
    }
}

std::string classNameForPosition(const std::vector<ClassBlock>& classes, const std::size_t position, bool& isMember)
{
    for (const ClassBlock& block : classes) {
        if (position >= block.start && position < block.end) {
            isMember = true;
            return block.name;
        }
    }
    isMember = false;
    return {};
}
} // namespace

std::vector<OwnershipGraphNode> OwnershipGraphAnalyzer::analyze(const std::string& code) const
{
    std::vector<OwnershipGraphNode> nodes;
    const ClassResourceAnalyzer classAnalyzer;
    const std::vector<ClassBlock> classes = classAnalyzer.analyzeClasses(code);

    const std::regex pointerToPointerDeclaration(
        R"((^[ \t]*)([A-Za-z_:][A-Za-z0-9_:]*(?:\s*<[^;\n{}]+>)?)\s*\*\s*\*\s*([A-Za-z_]\w*)\s*(?:=\s*new\s+\2\s*\*\s*\[\s*([^\]]+)\s*\])?\s*;)",
        std::regex::ECMAScript | std::regex::multiline);
    for (std::sregex_iterator iterator(code.begin(), code.end(), pointerToPointerDeclaration), end; iterator != end; ++iterator) {
        const std::string elementType = trim((*iterator)[2].str());
        const std::string storageName = (*iterator)[3].str();
        if (!hasPointerToPointerOwnership(code, elementType, storageName)) {
            continue;
        }

        bool isMember = false;
        const std::string scopeName = classNameForPosition(classes, static_cast<std::size_t>(iterator->position()), isMember);
        addNode(nodes,
                OwnershipGraphNode{
                    elementType,
                    storageName,
                    trim((*iterator)[4].matched ? (*iterator)[4].str() : ""),
                    scopeName,
                    OwnershipClassification::SequentialCollectionOwnership,
                    isMember,
                    true,
                    false,
                    false,
                });
    }

    const std::regex fixedPointerArrayDeclaration(
        R"((^[ \t]*)([A-Za-z_:][A-Za-z0-9_:]*(?:\s*<[^;\n{}]+>)?)\s*\*\s*([A-Za-z_]\w*)\s*\[\s*([^\]]+)\s*\]\s*;)",
        std::regex::ECMAScript | std::regex::multiline);
    for (std::sregex_iterator iterator(code.begin(), code.end(), fixedPointerArrayDeclaration), end; iterator != end; ++iterator) {
        const std::string elementType = trim((*iterator)[2].str());
        const std::string storageName = (*iterator)[3].str();
        const std::string extent = trim((*iterator)[4].str());
        if (!isCompileTimeExtent(extent) || !hasFixedPointerArrayOwnership(code, elementType, storageName)) {
            continue;
        }

        bool isMember = false;
        const std::string scopeName = classNameForPosition(classes, static_cast<std::size_t>(iterator->position()), isMember);
        addNode(nodes,
                OwnershipGraphNode{
                    elementType,
                    storageName,
                    extent,
                    scopeName,
                    OwnershipClassification::FixedSizeCollectionOwnership,
                    isMember,
                    false,
                    true,
                    false,
                });
    }

    const std::regex vectorRawPointerDeclaration(
        R"((^[ \t]*)std::vector\s*<\s*([A-Za-z_:][A-Za-z0-9_:]*(?:\s*<[^;\n{}]+>)?)\s*\*\s*>\s+([A-Za-z_]\w*)\s*;)",
        std::regex::ECMAScript | std::regex::multiline);
    for (std::sregex_iterator iterator(code.begin(), code.end(), vectorRawPointerDeclaration), end; iterator != end; ++iterator) {
        const std::string elementType = trim((*iterator)[2].str());
        const std::string storageName = (*iterator)[3].str();
        if (!hasVectorPointerOwnership(code, elementType, storageName)) {
            continue;
        }

        bool isMember = false;
        const std::string scopeName = classNameForPosition(classes, static_cast<std::size_t>(iterator->position()), isMember);
        addNode(nodes,
                OwnershipGraphNode{
                    elementType,
                    storageName,
                    {},
                    scopeName,
                    OwnershipClassification::SequentialCollectionOwnership,
                    isMember,
                    false,
                    false,
                    true,
                });
    }

    return nodes;
}

std::vector<StringOwnershipOpportunity> StringOwnershipPatternDetector::detect(const std::string& code) const
{
    std::vector<StringOwnershipOpportunity> opportunities;
    const ClassResourceAnalyzer classAnalyzer;
    const std::vector<ClassBlock> classes = classAnalyzer.analyzeClasses(code);

    for (const ClassBlock& block : classes) {
        const std::regex charPointerMember(R"(\bchar\s*\*\s*([A-Za-z_]\w*)\s*;)");
        std::smatch memberMatch;
        std::string search = block.text;
        while (std::regex_search(search, memberMatch, charPointerMember)) {
            const std::string memberName = memberMatch[1].str();
            const std::string escaped = escapeRegex(memberName);
            const bool allocatesText = std::regex_search(block.text, std::regex(escaped + R"(\s*=\s*new\s+char\s*\[)"));
            const bool copiesText = std::regex_search(block.text, std::regex(R"((?:std::)?str(?:n?cpy|len|cmp)\s*\()"));
            const bool deletesText = std::regex_search(block.text, std::regex(R"(delete\s*\[\s*\]\s*)" + escaped));
            if (allocatesText && copiesText && deletesText) {
                opportunities.push_back(StringOwnershipOpportunity{
                    block.name,
                    memberName,
                    "Class owns a char* buffer, copies text with C-string APIs, and manually deletes the buffer.",
                });
            }
            search = memberMatch.suffix().str();
        }
    }

    return opportunities;
}
