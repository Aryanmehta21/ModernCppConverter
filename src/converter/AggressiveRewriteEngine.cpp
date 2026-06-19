#include "converter/AggressiveRewriteEngine.h"

#include <algorithm>
#include <cctype>
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

std::string ensureInclude(std::string code, const std::string& includeLine, std::vector<ConversionChange>* changes)
{
    if (code.find(includeLine) != std::string::npos) {
        return code;
    }

    static const std::regex includePattern(R"(^#include\s+<[^>]+>\s*$)");
    std::stringstream input(code);
    std::ostringstream output;
    std::string line;
    bool inserted = false;
    bool sawInclude = false;
    bool firstLine = true;

    while (std::getline(input, line)) {
        if (!firstLine) {
            output << '\n';
        }
        firstLine = false;

        if (!inserted && sawInclude && !std::regex_match(line, includePattern)) {
            output << includeLine << '\n';
            inserted = true;
        }

        output << line;
        if (std::regex_match(line, includePattern)) {
            sawInclude = true;
        }
    }

    if (!inserted) {
        if (sawInclude) {
            output << '\n' << includeLine;
        } else {
            output.str({});
            output.clear();
            output << includeLine << '\n' << code;
        }
    }

    if (!code.empty() && code.back() == '\n') {
        output << '\n';
    }

    if (changes != nullptr) {
        addAppliedChange(*changes,
                         "Add required include",
                         "",
                         includeLine,
                         "Added a standard library include required by the aggressive rewrite output.");
    }
    return output.str();
}

bool isCpp20(const ModernizationOptions& options)
{
    return options.targetStandard == CppStandard::Cpp20;
}

std::string replaceIdentifier(std::string text, const std::string& from, const std::string& to)
{
    return std::regex_replace(text, std::regex("\\b" + from + "\\b"), to);
}

std::string lambdaNameForAccumulator(std::string accumulator)
{
    if (accumulator.empty()) {
        return "computeValue";
    }
    accumulator[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(accumulator[0])));
    return "compute" + accumulator;
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

struct ClassRegion
{
    std::string keyword;
    std::string name;
    std::size_t start = 0;
    std::size_t openBrace = 0;
    std::size_t closeBrace = 0;
    std::size_t end = 0;
};

std::vector<ClassRegion> findClassRegions(const std::string& code)
{
    static const std::regex classHeader(R"(\b(class|struct)\s+([A-Za-z_]\w*)[^;{]*\{)");
    std::vector<ClassRegion> regions;
    std::string remaining = code;
    std::size_t offset = 0;
    std::smatch match;

    while (std::regex_search(remaining, match, classHeader)) {
        const std::size_t start = offset + static_cast<std::size_t>(match.position());
        const std::size_t matchEnd = start + static_cast<std::size_t>(match.length());
        const std::size_t openBrace = code.rfind('{', matchEnd - 1);
        if (openBrace == std::string::npos) {
            offset = matchEnd;
            remaining = code.substr(offset);
            continue;
        }

        int depth = 1;
        std::size_t position = openBrace + 1;
        for (; position < code.size(); ++position) {
            if (code[position] == '{') {
                ++depth;
            } else if (code[position] == '}') {
                --depth;
                if (depth == 0) {
                    break;
                }
            }
        }

        if (position >= code.size()) {
            break;
        }

        std::size_t semicolon = position + 1;
        while (semicolon < code.size() && std::isspace(static_cast<unsigned char>(code[semicolon]))) {
            ++semicolon;
        }
        if (semicolon >= code.size() || code[semicolon] != ';') {
            offset = position + 1;
            remaining = code.substr(offset);
            continue;
        }

        regions.push_back(ClassRegion{
            match[1].str(),
            match[2].str(),
            start,
            openBrace,
            position,
            semicolon + 1,
        });
        offset = semicolon + 1;
        remaining = code.substr(offset);
    }

    return regions;
}

struct MethodRegion
{
    std::size_t start = 0;
    std::size_t openBrace = 0;
    std::size_t closeBrace = 0;
    std::size_t end = 0;
};

bool findDestructorRegion(const std::string& classText, const std::string& className, MethodRegion& region)
{
    const std::regex destructorHeader("~" + escapeRegex(className) + R"(\s*\(\s*\)\s*(?:noexcept\s*)?\{)");
    std::smatch match;
    if (!std::regex_search(classText, match, destructorHeader)) {
        return false;
    }

    const std::size_t start = static_cast<std::size_t>(match.position());
    const std::size_t openBrace = classText.find('{', start);
    if (openBrace == std::string::npos) {
        return false;
    }

    int depth = 1;
    std::size_t position = openBrace + 1;
    for (; position < classText.size(); ++position) {
        if (classText[position] == '{') {
            ++depth;
        } else if (classText[position] == '}') {
            --depth;
            if (depth == 0) {
                break;
            }
        }
    }
    if (position >= classText.size()) {
        return false;
    }

    region = MethodRegion{start, openBrace, position, position + 1};
    return true;
}

bool findFunctionRegion(const std::string& classText,
                        const std::regex& headerPattern,
                        MethodRegion& region)
{
    std::smatch match;
    if (!std::regex_search(classText, match, headerPattern)) {
        return false;
    }

    const std::size_t start = static_cast<std::size_t>(match.position());
    const std::size_t openBrace = classText.find('{', start);
    if (openBrace == std::string::npos) {
        return false;
    }

    int depth = 1;
    std::size_t position = openBrace + 1;
    for (; position < classText.size(); ++position) {
        if (classText[position] == '{') {
            ++depth;
        } else if (classText[position] == '}') {
            --depth;
            if (depth == 0) {
                break;
            }
        }
    }
    if (position >= classText.size()) {
        return false;
    }

    region = MethodRegion{start, openBrace, position, position + 1};
    return true;
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    if (!text.empty() && text.back() == '\n') {
        lines.emplace_back();
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            output << '\n';
        }
        output << lines[index];
    }
    return output.str();
}

std::string lowercaseCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool isObserverGetterName(const std::string& functionName)
{
    const std::string lowered = lowercaseCopy(functionName);
    return lowered.find("get") != std::string::npos
        || lowered.find("view") != std::string::npos
        || lowered.find("observe") != std::string::npos
        || lowered.find("peek") != std::string::npos
        || lowered.find("data") != std::string::npos
        || lowered.find("ptr") != std::string::npos
        || lowered.find("pointer") != std::string::npos;
}

bool isOwnershipTransferName(const std::string& functionName)
{
    const std::string lowered = lowercaseCopy(functionName);
    return lowered.find("release") != std::string::npos
        || lowered.find("take") != std::string::npos
        || lowered.find("steal") != std::string::npos
        || lowered.find("detach") != std::string::npos
        || lowered.find("create") != std::string::npos;
}

bool rawMemberReturnsAreObserverGetters(const std::string& classText,
                                        const std::string& memberType,
                                        const std::string& memberName)
{
    const std::string escapedType = escapeRegex(memberType);
    const std::string escapedMember = escapeRegex(memberName);
    const std::regex returnPattern(R"(\breturn\s+)" + escapedMember + R"(\s*;)");
    if (!std::regex_search(classText, returnPattern)) {
        return true;
    }

    const std::regex getterPattern(
        std::string("((?:const\\s+)?)") + escapedType
            + R"(\s*\*\s*([A-Za-z_]\w*)\s*\([^)]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{[^{}]*\breturn\s+)"
            + escapedMember
            + R"(\s*;\s*[^{}]*\})",
        std::regex::ECMAScript);
    std::string withoutKnownGetters = classText;
    bool foundGetterReturn = false;
    std::smatch match;
    while (std::regex_search(withoutKnownGetters, match, getterPattern)) {
        const std::string functionName = match[2].str();
        if (!isObserverGetterName(functionName) || isOwnershipTransferName(functionName)) {
            return false;
        }
        foundGetterReturn = true;
        withoutKnownGetters.replace(static_cast<std::size_t>(match.position()),
                                    static_cast<std::size_t>(match.length()),
                                    "");
    }

    return foundGetterReturn && !std::regex_search(withoutKnownGetters, returnPattern);
}

bool hasSpecialMemberBusinessLogic(const std::string& functionText)
{
    static const std::regex sideEffectPattern(R"(\b(?:std::cout|std::cerr|printf|fprintf|throw|log|callback|telemetry|lock|unlock|open|close)\b)");
    return std::regex_search(functionText, sideEffectPattern);
}

bool copySpecialMemberLooksResourceOnly(const std::string& classText,
                                        const std::regex& headerPattern,
                                        const std::string& memberType,
                                        const std::string& memberName)
{
    MethodRegion region;
    if (!findFunctionRegion(classText, headerPattern, region)) {
        return false;
    }

    const std::string functionText = classText.substr(region.start, region.end - region.start);
    if (hasSpecialMemberBusinessLogic(functionText)) {
        return false;
    }

    const std::string escapedMember = escapeRegex(memberName);
    const std::string escapedType = escapeRegex(memberType);
    const bool mentionsMember = std::regex_search(functionText, std::regex(R"(\b)" + escapedMember + R"(\b)"));
    const bool allocatesMember = std::regex_search(functionText,
                                                   std::regex(escapedMember + R"(\s*(?:=|\()\s*new\s+)" + escapedType));
    const bool deletesMember = std::regex_search(functionText, std::regex(R"(delete\s+)" + escapedMember + R"(\s*;)"));
    return mentionsMember && (allocatesMember || deletesMember);
}

bool hasDeletedCopyConstructor(const std::string& classText, const std::string& className)
{
    const std::regex deletedPattern(escapeRegex(className) + R"(\s*\(\s*const\s+)" + escapeRegex(className)
                                    + R"(\s*&\s*(?:[A-Za-z_]\w*)?\s*\)\s*=\s*delete\s*;)");
    return std::regex_search(classText, deletedPattern);
}

bool hasDeletedCopyAssignment(const std::string& classText, const std::string& className)
{
    const std::regex deletedPattern(escapeRegex(className) + R"(\s*&\s*operator=\s*\(\s*const\s+)"
                                    + escapeRegex(className) + R"(\s*&\s*(?:[A-Za-z_]\w*)?\s*\)\s*=\s*delete\s*;)");
    return std::regex_search(classText, deletedPattern);
}

bool hasMoveConstructor(const std::string& classText, const std::string& className)
{
    const std::regex movePattern(escapeRegex(className) + R"(\s*\(\s*)" + escapeRegex(className) + R"(\s*&&)");
    return std::regex_search(classText, movePattern);
}

bool hasMoveAssignment(const std::string& classText, const std::string& className)
{
    const std::regex movePattern(R"(operator=\s*\(\s*)" + escapeRegex(className) + R"(\s*&&)");
    return std::regex_search(classText, movePattern);
}

std::string deleteCopyOperation(std::string classText,
                                const std::regex& headerPattern,
                                const std::string& replacement,
                                const std::string& ruleName,
                                std::vector<ConversionChange>& changes,
                                bool& changed)
{
    MethodRegion region;
    if (!findFunctionRegion(classText, headerPattern, region)) {
        return classText;
    }

    const std::string before = classText.substr(region.start, region.end - region.start);
    addAppliedChange(changes,
                     ruleName,
                     trim(before),
                     trim(replacement),
                     "Deleted obsolete raw-resource copy semantics because std::unique_ptr expresses exclusive ownership.");
    classText.replace(region.start, region.end - region.start, replacement);
    changed = true;
    return classText;
}

std::size_t publicInsertionPoint(const std::string& classText)
{
    const std::size_t publicPosition = classText.find("public:");
    if (publicPosition == std::string::npos) {
        return classText.find('{') == std::string::npos ? 0 : classText.find('{') + 1;
    }
    const std::size_t lineEnd = classText.find('\n', publicPosition);
    return lineEnd == std::string::npos ? publicPosition + std::string("public:").size() : lineEnd + 1;
}

std::string defaultMoveOperationsAfterUniqueOwnership(std::string classText,
                                                      const std::string& className,
                                                      const std::string& indent,
                                                      std::vector<ConversionChange>& changes)
{
    if (hasMoveConstructor(classText, className) && hasMoveAssignment(classText, className)) {
        return classText;
    }

    std::ostringstream declarations;
    if (!hasMoveConstructor(classText, className)) {
        declarations << indent << className << "(" << className << "&&) noexcept = default;\n";
    }
    if (!hasMoveAssignment(classText, className)) {
        declarations << indent << className << "& operator=(" << className << "&&) noexcept = default;\n";
    }

    const std::string moveDeclarations = declarations.str();
    if (moveDeclarations.empty()) {
        return classText;
    }

    std::size_t insertionPoint = std::string::npos;
    const std::regex copyAssignmentDeletePattern("(^[ \\t]*)" + escapeRegex(className)
                                                 + R"(\s*&\s*operator=\s*\(\s*const\s+)"
                                                 + escapeRegex(className)
                                                 + R"(\s*&\s*(?:[A-Za-z_]\w*)?\s*\)\s*=\s*delete\s*;)",
                                                 std::regex::ECMAScript | std::regex::multiline);
    std::smatch match;
    if (std::regex_search(classText, match, copyAssignmentDeletePattern)) {
        insertionPoint = static_cast<std::size_t>(match.position() + match.length());
    } else {
        const std::regex copyConstructorDeletePattern("(^[ \\t]*)" + escapeRegex(className)
                                                      + R"(\s*\(\s*const\s+)"
                                                      + escapeRegex(className)
                                                      + R"(\s*&\s*(?:[A-Za-z_]\w*)?\s*\)\s*=\s*delete\s*;)",
                                                      std::regex::ECMAScript | std::regex::multiline);
        if (std::regex_search(classText, match, copyConstructorDeletePattern)) {
            insertionPoint = static_cast<std::size_t>(match.position() + match.length());
        }
    }
    if (insertionPoint == std::string::npos) {
        insertionPoint = publicInsertionPoint(classText);
    }

    classText.insert(insertionPoint, "\n" + moveDeclarations);
    addAppliedChange(changes,
                     "Default move operations after unique ownership modernization",
                     className,
                     trim(moveDeclarations),
                     "Defaulted move operations after deleting raw-resource copy operations for exclusive std::unique_ptr ownership.");
    return classText;
}

std::string updateCopyAndMoveForUniqueMember(std::string classText,
                                             const std::string& className,
                                             std::vector<ConversionChange>& changes)
{
    const std::regex copyConstructorHeader(
        "(^[ \\t]*)" + escapeRegex(className) + R"(\s*\(\s*const\s+)" + escapeRegex(className)
            + R"(\s*&\s*(?:[A-Za-z_]\w*)?\s*\)\s*(?:[^{]*)?\{)",
        std::regex::ECMAScript | std::regex::multiline);
    const std::regex copyAssignmentHeader(
        "(^[ \\t]*)" + escapeRegex(className) + R"(\s*&\s*operator=\s*\(\s*const\s+)"
            + escapeRegex(className) + R"(\s*&\s*(?:[A-Za-z_]\w*)?\s*\)\s*\{)",
        std::regex::ECMAScript | std::regex::multiline);

    std::smatch match;
    std::string indent = "    ";
    bool deletedCopy = false;
    if (std::regex_search(classText, match, copyConstructorHeader)) {
        indent = match[1].str();
        classText = deleteCopyOperation(std::move(classText),
                                        copyConstructorHeader,
                                        indent + className + "(const " + className + "&) = delete;",
                                        "Delete copy constructor after unique ownership modernization",
                                        changes,
                                        deletedCopy);
    }
    if (std::regex_search(classText, match, copyAssignmentHeader)) {
        indent = match[1].str();
        classText = deleteCopyOperation(std::move(classText),
                                        copyAssignmentHeader,
                                        indent + className + "& operator=(const " + className + "&) = delete;",
                                        "Delete copy assignment after unique ownership modernization",
                                        changes,
                                        deletedCopy);
    }

    if (deletedCopy || hasDeletedCopyConstructor(classText, className) || hasDeletedCopyAssignment(classText, className)) {
        classText = defaultMoveOperationsAfterUniqueOwnership(std::move(classText), className, indent, changes);
    }

    return classText;
}

std::string removeUniquePtrMemberCleanup(std::string destructorBody,
                                         const std::string& memberName,
                                         std::vector<ConversionChange>& changes)
{
    const std::string escapedMember = escapeRegex(memberName);

    const std::regex oneLineConditionalDeletePattern(
        R"(^[ \t]*if\s*\(\s*)" + escapedMember + R"(\s*(?:(?:!=|==)\s*(?:nullptr|NULL|0))?\s*\)\s*delete\s+)"
            + escapedMember + R"(\s*;\s*\n?)",
        std::regex::ECMAScript | std::regex::multiline);
    if (std::regex_search(destructorBody, oneLineConditionalDeletePattern)) {
        destructorBody = std::regex_replace(destructorBody, oneLineConditionalDeletePattern, "");
        addAppliedChange(changes,
                         "Remove redundant manual delete",
                         "if (...) delete " + memberName + ";",
                         "removed",
                         "Manual conditional delete is no longer needed because std::unique_ptr destroys the member automatically.");
    }

    const std::regex deleteLinePattern(R"(^[ \t]*delete\s+)" + escapedMember + R"(\s*;\s*\n?)",
                                       std::regex::ECMAScript | std::regex::multiline);
    if (std::regex_search(destructorBody, deleteLinePattern)) {
        destructorBody = std::regex_replace(destructorBody, deleteLinePattern, "");
        addAppliedChange(changes,
                         "Remove redundant manual delete",
                         "delete " + memberName + ";",
                         "removed",
                         "Manual delete is no longer needed because std::unique_ptr destroys the member automatically.");
    }

    const std::regex nullAssignmentPattern(
        R"(^[ \t]*)" + escapedMember + R"(\s*=\s*(?:nullptr|NULL|0)\s*;\s*\n?)",
        std::regex::ECMAScript | std::regex::multiline);
    if (std::regex_search(destructorBody, nullAssignmentPattern)) {
        destructorBody = std::regex_replace(destructorBody, nullAssignmentPattern, "");
        addAppliedChange(changes,
                         "Remove redundant manual delete",
                         memberName + " = nullptr;",
                         "removed",
                         "Resetting a member after destructor cleanup is redundant once std::unique_ptr owns it.");
    }

    const std::regex emptyIfBlockPattern(
        R"(^[ \t]*if\s*\(\s*)" + escapedMember + R"(\s*(?:!=|==)\s*(?:nullptr|NULL|0)\s*\)\s*\{\s*\}\s*\n?)",
        std::regex::ECMAScript | std::regex::multiline);
    if (std::regex_search(destructorBody, emptyIfBlockPattern)) {
        destructorBody = std::regex_replace(destructorBody, emptyIfBlockPattern, "");
        addAppliedChange(changes,
                         "Rule of Zero destructor cleanup",
                         "empty cleanup block for " + memberName,
                         "removed",
                         "Removed an empty pointer cleanup block left after std::unique_ptr modernization.");
    }

    return destructorBody;
}

} // namespace

std::string AggressiveRewriteEngine::rewrite(const std::string& code,
                                             const ModernizationOptions& options,
                                             std::vector<ConversionChange>& changes) const
{
    std::string updated = rewriteOwnershipModernizations(code, changes);
    updated = rewriteLocalComputationBlocks(updated, changes);
    updated = rewriteHelperFunctionUsedOnce(updated, changes);
    updated = rewriteFunctorPredicate(updated, options, changes);
    updated = rewriteOutputLoopsToAlgorithms(updated, options, changes);
    updated = ensureModernIncludes(updated, options, &changes);
    updated = lightlyFormat(updated);

    if (updated == code) {
        addSuggestion(changes,
                      "AI-style aggressive rewrite",
                      "Input snippet",
                      "No safe aggressive rewrite pattern was recognized. The code was left unchanged.");
    }
    return updated;
}

std::string AggressiveRewriteEngine::rewriteOwnershipModernizations(const std::string& code,
                                                                    std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    updated = rewriteAliasedPointerOwnership(updated, changes);
    updated = rewriteClassMemberOwnership(updated, changes);
    updated = rewriteStringViewStringOwnership(updated, changes);
    return updated;
}

std::string AggressiveRewriteEngine::rewriteClassMemberOwnership(const std::string& code,
                                                                 std::vector<ConversionChange>& changes) const
{
    struct MemberCandidate
    {
        std::string indent;
        std::string type;
        std::string name;
        std::string declaration;
    };

    std::string updated = code;
    const std::vector<ClassRegion> regions = findClassRegions(code);
    if (regions.empty()) {
        return updated;
    }

    for (auto regionIterator = regions.rbegin(); regionIterator != regions.rend(); ++regionIterator) {
        const ClassRegion& region = *regionIterator;
        std::string classText = updated.substr(region.start, region.end - region.start);
        const std::string originalClassText = classText;

        std::vector<MemberCandidate> members;
        static const std::regex memberPattern(
            R"((^[ \t]*)([A-Za-z_:][A-Za-z0-9_:]*(?:\s*<[^;\n{}]+>)?)\s*\*\s*([A-Za-z_]\w*)\s*(?:=\s*nullptr)?\s*;)",
            std::regex::ECMAScript | std::regex::multiline);
        std::string memberSearch = classText;
        std::smatch memberMatch;
        while (std::regex_search(memberSearch, memberMatch, memberPattern)) {
            members.push_back(MemberCandidate{
                memberMatch[1].str(),
                trim(memberMatch[2].str()),
                memberMatch[3].str(),
                memberMatch[0].str(),
            });
            memberSearch = memberMatch.suffix().str();
        }

        if (members.empty()) {
            continue;
        }

        MethodRegion destructorRegion;
        const bool hasDestructor = findDestructorRegion(classText, region.name, destructorRegion);
        const std::string destructorText = hasDestructor
            ? classText.substr(destructorRegion.start, destructorRegion.end - destructorRegion.start)
            : std::string{};
        const std::string destructorBody = hasDestructor
            ? classText.substr(destructorRegion.openBrace + 1, destructorRegion.closeBrace - destructorRegion.openBrace - 1)
            : std::string{};

        std::vector<std::string> convertedMembers;

        for (const MemberCandidate& member : members) {
            const std::string escapedName = escapeRegex(member.name);
            const std::string escapedType = escapeRegex(member.type);
            const std::regex assignmentPattern("\\b" + escapedName + R"(\s*=\s*new\s+)" + escapedType + R"(\s*(?:\(([^;]*)\))?\s*;)");
            const std::regex initializerPattern("\\b" + escapedName + R"(\s*\(\s*new\s+)" + escapedType + R"(\s*(?:\(([^)]*)\))?\s*\))");
            const std::regex deletePattern("\\bdelete\\s+" + escapedName + R"(\s*;)");
            const std::regex returnPattern("\\breturn\\s+" + escapedName + R"(\s*;)");
            const std::regex copyConstructorPattern("\\b" + escapeRegex(region.name) + R"(\s*\(\s*(?:const\s+)?)" + escapeRegex(region.name) + R"(\s*&)");
            const std::regex copyAssignmentPattern(R"(operator\s*=\s*\(\s*(?:const\s+)?)" + escapeRegex(region.name) + R"(\s*&)");
            const std::regex copyConstructorHeader(
                "(^[ \\t]*)" + escapeRegex(region.name) + R"(\s*\(\s*const\s+)" + escapeRegex(region.name)
                    + R"(\s*&\s*(?:[A-Za-z_]\w*)?\s*\)\s*(?:[^{]*)?\{)",
                std::regex::ECMAScript | std::regex::multiline);
            const std::regex copyAssignmentHeader(
                "(^[ \\t]*)" + escapeRegex(region.name) + R"(\s*&\s*operator=\s*\(\s*const\s+)"
                    + escapeRegex(region.name) + R"(\s*&\s*(?:[A-Za-z_]\w*)?\s*\)\s*\{)",
                std::regex::ECMAScript | std::regex::multiline);

            const bool hasAssignmentAllocation = std::regex_search(classText, assignmentPattern);
            const bool hasInitializerAllocation = std::regex_search(classText, initializerPattern);
            const bool hasDestructorDelete = hasDestructor && std::regex_search(destructorBody, deletePattern);
            const bool hasUnsafeRawReturn = std::regex_search(classText, returnPattern)
                && !rawMemberReturnsAreObserverGetters(classText, member.type, member.name);
            const bool hasCopyOperation = std::regex_search(classText, copyConstructorPattern)
                || std::regex_search(classText, copyAssignmentPattern);
            const bool copyConstructorDeleted = hasDeletedCopyConstructor(classText, region.name);
            const bool copyAssignmentDeleted = hasDeletedCopyAssignment(classText, region.name);
            const bool hasCopyConstructorBody = std::regex_search(classText, copyConstructorHeader);
            const bool hasCopyAssignmentBody = std::regex_search(classText, copyAssignmentHeader);
            const bool copyConstructorSafe = !hasCopyConstructorBody
                || copyConstructorDeleted
                || copySpecialMemberLooksResourceOnly(classText, copyConstructorHeader, member.type, member.name);
            const bool copyAssignmentSafe = !hasCopyAssignmentBody
                || copyAssignmentDeleted
                || copySpecialMemberLooksResourceOnly(classText, copyAssignmentHeader, member.type, member.name);

            if (!hasAssignmentAllocation && !hasInitializerAllocation) {
                addSuggestion(changes,
                              "Class member raw pointer to std::unique_ptr",
                              trim(member.declaration),
                              "Raw pointer member appears borrowed or ownership is unclear because no matching constructor allocation was found.");
                continue;
            }

            if (!hasDestructorDelete || hasUnsafeRawReturn || (hasCopyOperation && (!copyConstructorSafe || !copyAssignmentSafe))) {
                addSuggestion(changes,
                              "Class member raw pointer to std::unique_ptr",
                              trim(member.declaration),
                              "Raw pointer member was not converted because ownership transfer may escape, deletion is unclear, or custom copy semantics need manual review.");
                continue;
            }

            const std::string modernDeclaration = member.indent + "std::unique_ptr<" + member.type + "> " + member.name + ";";
            const std::size_t declarationPosition = classText.find(member.declaration);
            if (declarationPosition != std::string::npos) {
                classText.replace(declarationPosition, member.declaration.size(), modernDeclaration);
                addAppliedChange(changes,
                                 "Class member raw pointer to std::unique_ptr",
                                 trim(member.declaration),
                                 trim(modernDeclaration),
                                 "The member is allocated by the class constructor and deleted by the destructor, so std::unique_ptr expresses single ownership directly.");
            }

            std::smatch allocationMatch;
            if (std::regex_search(classText, allocationMatch, assignmentPattern)) {
                const std::string arguments = trim(allocationMatch[1].matched ? allocationMatch[1].str() : "");
                const std::string replacement = member.name + " = std::make_unique<" + member.type + ">(" + arguments + ");";
                addAppliedChange(changes,
                                 "Constructor allocation to std::make_unique",
                                 trim(allocationMatch[0].str()),
                                 replacement,
                                 "Constructor allocation now creates the owned member with std::make_unique for exception-safe RAII initialization.");
                classText.replace(static_cast<std::size_t>(allocationMatch.position()),
                                  static_cast<std::size_t>(allocationMatch.length()),
                                  replacement);
            }

            if (std::regex_search(classText, allocationMatch, initializerPattern)) {
                const std::string arguments = trim(allocationMatch[1].matched ? allocationMatch[1].str() : "");
                const std::string replacement = member.name + "(std::make_unique<" + member.type + ">(" + arguments + "))";
                addAppliedChange(changes,
                                 "Constructor allocation to std::make_unique",
                                 trim(allocationMatch[0].str()),
                                 replacement,
                                 "Initializer-list allocation now creates the owned member with std::make_unique.");
                classText.replace(static_cast<std::size_t>(allocationMatch.position()),
                                  static_cast<std::size_t>(allocationMatch.length()),
                                  replacement);
            }

            std::smatch returnMatch;
            while (std::regex_search(classText, returnMatch, returnPattern)) {
                const std::string replacement = "return " + member.name + ".get();";
                addAppliedChange(changes,
                                 "Unique_ptr getter return to raw observer",
                                 trim(returnMatch[0].str()),
                                 replacement,
                                 "The member is now owned by std::unique_ptr, so raw pointer getter APIs return a non-owning view with get().");
                classText.replace(static_cast<std::size_t>(returnMatch.position()),
                                  static_cast<std::size_t>(returnMatch.length()),
                                  replacement);
            }

            classText = updateCopyAndMoveForUniqueMember(std::move(classText), region.name, changes);
            convertedMembers.push_back(member.name);
        }

        if (!convertedMembers.empty()) {
            MethodRegion updatedDestructorRegion;
            if (findDestructorRegion(classText, region.name, updatedDestructorRegion)) {
                std::string updatedDestructorText = classText.substr(updatedDestructorRegion.start,
                                                                     updatedDestructorRegion.end - updatedDestructorRegion.start);
                std::string updatedDestructorBody = classText.substr(updatedDestructorRegion.openBrace + 1,
                                                                     updatedDestructorRegion.closeBrace - updatedDestructorRegion.openBrace - 1);
                std::string cleanedDestructorBody = updatedDestructorBody;

                for (const std::string& memberName : convertedMembers) {
                    cleanedDestructorBody = removeUniquePtrMemberCleanup(std::move(cleanedDestructorBody), memberName, changes);
                }

                if (cleanedDestructorBody != updatedDestructorBody) {
                    if (trim(cleanedDestructorBody).empty()) {
                        addAppliedChange(changes,
                                         "Rule of Zero destructor removal",
                                         trim(updatedDestructorText),
                                         "removed",
                                         "The destructor only performed smart-pointer cleanup, so it was removed to prefer Rule of Zero class design.");
                        classText.erase(updatedDestructorRegion.start, updatedDestructorRegion.end - updatedDestructorRegion.start);
                    } else {
                        std::string replacement = updatedDestructorText;
                        replacement.replace(updatedDestructorRegion.openBrace + 1 - updatedDestructorRegion.start,
                                            updatedDestructorRegion.closeBrace - updatedDestructorRegion.openBrace - 1,
                                            cleanedDestructorBody);
                        classText.replace(updatedDestructorRegion.start,
                                          updatedDestructorRegion.end - updatedDestructorRegion.start,
                                          replacement);
                        addSuggestion(changes,
                                      "Rule of Zero destructor removal",
                                      trim(updatedDestructorText),
                                      "The destructor still contains non-cleanup logic, so only redundant delete statements were removed. Review whether it can be defaulted later.");
                    }
                }
            }
        }

        if (classText != originalClassText) {
            updated.replace(region.start, region.end - region.start, classText);
        }
    }

    return updated;
}

std::string AggressiveRewriteEngine::rewriteAliasedPointerOwnership(const std::string& code,
                                                                    std::vector<ConversionChange>& changes) const
{
    std::vector<std::string> lines = splitLines(code);
    bool changed = false;
    bool warned = false;

    static const std::regex allocationPattern(
        R"(^([ \t]*)([A-Za-z_:][A-Za-z0-9_:]*(?:\s*<[^;\n{}]+>)?)\s*\*\s*([A-Za-z_]\w*)\s*=\s*new\s+([A-Za-z_:][A-Za-z0-9_:]*(?:\s*<[^;\n{}]+>)?)\s*(?:\((.*)\))?\s*;\s*$)");

    for (std::size_t allocationIndex = 0; allocationIndex < lines.size(); ++allocationIndex) {
        std::smatch allocationMatch;
        if (!std::regex_match(lines[allocationIndex], allocationMatch, allocationPattern)) {
            continue;
        }

        const std::string indent = allocationMatch[1].str();
        const std::string type = trim(allocationMatch[2].str());
        const std::string owner = allocationMatch[3].str();
        const std::string allocatedType = trim(allocationMatch[4].str());
        const std::string arguments = trim(allocationMatch[5].matched ? allocationMatch[5].str() : "");
        const std::string escapedType = escapeRegex(type);
        const std::string escapedOwner = escapeRegex(owner);
        const std::regex aliasPattern(R"(^[ \t]*)" + escapedType + R"(\s*\*\s*([A-Za-z_]\w*)\s*=\s*)" + escapedOwner + R"(\s*;\s*$)");
        const std::regex deleteOwnerPattern(R"(^[ \t]*delete\s+)" + escapedOwner + R"(\s*;\s*$)");
        const std::regex ownerReturnPattern("\\breturn\\s+" + escapedOwner + R"(\s*;)");
        const std::regex ownerReassignmentPattern("\\b" + escapedOwner + R"(\s*=\s*[^;]+;)");

        std::vector<std::pair<std::size_t, std::string>> aliases;
        for (std::size_t index = allocationIndex + 1; index < lines.size(); ++index) {
            std::smatch aliasMatch;
            if (std::regex_match(lines[index], aliasMatch, aliasPattern)) {
                aliases.emplace_back(index, aliasMatch[1].str());
                continue;
            }
            if (std::regex_search(lines[index], deleteOwnerPattern)) {
                break;
            }
        }
        if (aliases.empty()) {
            continue;
        }

        std::vector<std::regex> deleteAliasPatterns;
        std::vector<std::regex> aliasReturnPatterns;
        std::vector<std::regex> aliasReassignmentPatterns;
        for (const auto& [index, alias] : aliases) {
            (void)index;
            const std::string escapedAlias = escapeRegex(alias);
            deleteAliasPatterns.emplace_back(R"(^[ \t]*delete\s+)" + escapedAlias + R"(\s*;\s*$)");
            aliasReturnPatterns.emplace_back("\\breturn\\s+" + escapedAlias + R"(\s*;)");
            aliasReassignmentPatterns.emplace_back("\\b" + escapedAlias + R"(\s*=\s*[^;]+;)");
        }
        const std::regex externalAliasPattern(R"(=\s*)" + escapedOwner + R"(\s*;)");

        std::size_t deleteIndex = lines.size();
        bool ambiguous = false;
        for (std::size_t index = allocationIndex + 1; index < lines.size(); ++index) {
            const bool isAliasDeclaration = std::regex_match(lines[index], aliasPattern);
            if (std::regex_search(lines[index], ownerReturnPattern)) {
                ambiguous = true;
            }
            for (const std::regex& aliasReturnPattern : aliasReturnPatterns) {
                if (std::regex_search(lines[index], aliasReturnPattern)) {
                    ambiguous = true;
                }
            }
            for (const std::regex& deleteAliasPattern : deleteAliasPatterns) {
                if (std::regex_search(lines[index], deleteAliasPattern)) {
                    ambiguous = true;
                }
            }
            for (const std::regex& aliasReassignmentPattern : aliasReassignmentPatterns) {
                if (!isAliasDeclaration && std::regex_search(lines[index], aliasReassignmentPattern)) {
                    ambiguous = true;
                }
            }
            if (std::regex_search(lines[index], ownerReassignmentPattern)) {
                ambiguous = true;
            }
            if (std::regex_search(lines[index], externalAliasPattern)
                && !std::regex_match(lines[index], aliasPattern)) {
                ambiguous = true;
            }
            if (std::regex_match(lines[index], deleteOwnerPattern)) {
                deleteIndex = index;
                break;
            }
        }

        if (deleteIndex == lines.size() || ambiguous) {
            if (!warned) {
                std::string aliasSummary;
                for (const auto& [index, alias] : aliases) {
                    (void)alias;
                    aliasSummary += lines[index] + "\n";
                }
                addSuggestion(changes,
                              "Dangling pointer risk detected",
                              trim(lines[allocationIndex] + "\n" + aliasSummary),
                              "Potential aliasing/dangling pointer risk detected. Manual ownership review required.");
                warned = true;
            }
            continue;
        }

        auto compactType = [](std::string value) {
            value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char character) {
                            return std::isspace(character) != 0;
                        }),
                        value.end());
            return value;
        };
        const bool allocatedAsDeclaredType = compactType(type) == compactType(allocatedType);
        const std::string allocationReplacement = allocatedAsDeclaredType
            ? indent + "auto " + owner + " = std::make_unique<" + type + ">(" + arguments + ");"
            : indent + "std::unique_ptr<" + type + "> " + owner + " = std::make_unique<" + allocatedType + ">(" + arguments + ");";
        std::string beforeAliases;
        std::string afterAliases;
        for (const auto& [index, alias] : aliases) {
            beforeAliases += lines[index] + "\n";
            afterAliases += indent + type + "* " + alias + " = " + owner + ".get();\n";
        }
        addAppliedChange(changes,
                         "Owning raw pointer with non-owning observer to std::unique_ptr",
                         trim(lines[allocationIndex] + "\n" + beforeAliases),
                         trim(allocationReplacement + "\n" + afterAliases),
                         "Classified the allocated pointer as the owner and preserved the raw alias as a non-owning observer with get().");
        lines[allocationIndex] = allocationReplacement;
        for (const auto& [index, alias] : aliases) {
            lines[index] = indent + type + "* " + alias + " = " + owner + ".get();";
        }

        addAppliedChange(changes,
                         "Remove redundant manual delete",
                         trim(lines[deleteIndex]),
                         "removed",
                         "Manual delete is no longer needed because std::unique_ptr releases the allocation automatically.");
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(deleteIndex));
        changed = true;
        break;
    }

    return changed ? joinLines(lines) : code;
}

std::string AggressiveRewriteEngine::rewriteStringViewStringOwnership(const std::string& code,
                                                                      std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    const std::vector<ClassRegion> regions = findClassRegions(code);
    if (regions.empty()) {
        return updated;
    }

    for (auto regionIterator = regions.rbegin(); regionIterator != regions.rend(); ++regionIterator) {
        const ClassRegion& region = *regionIterator;
        std::string classText = updated.substr(region.start, region.end - region.start);
        const std::string originalClassText = classText;

        std::vector<std::string> stringMembers;
        static const std::regex stringMemberPattern(R"(\bstd::string\s+([A-Za-z_]\w*)\s*;)");
        std::string memberSearch = classText;
        std::smatch memberMatch;
        while (std::regex_search(memberSearch, memberMatch, stringMemberPattern)) {
            stringMembers.push_back(memberMatch[1].str());
            memberSearch = memberMatch.suffix().str();
        }
        if (stringMembers.empty()) {
            continue;
        }

        std::vector<std::string> viewParameters;
        static const std::regex viewParameterPattern(R"(\bstd::string_view\s+([A-Za-z_]\w*)\b)");
        std::string parameterSearch = classText;
        std::smatch parameterMatch;
        while (std::regex_search(parameterSearch, parameterMatch, viewParameterPattern)) {
            viewParameters.push_back(parameterMatch[1].str());
            parameterSearch = parameterMatch.suffix().str();
        }
        if (viewParameters.empty()) {
            continue;
        }

        for (const std::string& member : stringMembers) {
            for (const std::string& parameter : viewParameters) {
                const std::regex initializerPattern("\\b" + escapeRegex(member) + R"(\s*\(\s*)" + escapeRegex(parameter) + R"(\s*\))");
                std::smatch initializerMatch;
                if (std::regex_search(classText, initializerMatch, initializerPattern)) {
                    const std::string replacement = member + "(std::string{" + parameter + "})";
                    addAppliedChange(changes,
                                     "Explicit string ownership from string_view",
                                     trim(initializerMatch[0].str()),
                                     replacement,
                                     "The constructor accepts a non-owning view but the class stores an owning std::string, so ownership is made explicit.");
                    classText.replace(static_cast<std::size_t>(initializerMatch.position()),
                                      static_cast<std::size_t>(initializerMatch.length()),
                                      replacement);
                }

                const std::regex assignmentPattern("\\b" + escapeRegex(member) + R"(\s*=\s*)" + escapeRegex(parameter) + R"(\s*;)");
                std::smatch assignmentMatch;
                if (std::regex_search(classText, assignmentMatch, assignmentPattern)) {
                    const std::string replacement = member + " = std::string{" + parameter + "};";
                    addAppliedChange(changes,
                                     "Explicit string ownership from string_view",
                                     trim(assignmentMatch[0].str()),
                                     replacement,
                                     "The function accepts a non-owning view but the class stores an owning std::string, so assignment now creates an explicit owned string.");
                    classText.replace(static_cast<std::size_t>(assignmentMatch.position()),
                                      static_cast<std::size_t>(assignmentMatch.length()),
                                      replacement);
                }
            }
        }

        if (classText != originalClassText) {
            updated.replace(region.start, region.end - region.start, classText);
        }
    }

    return updated;
}

std::string AggressiveRewriteEngine::rewriteLocalComputationBlocks(const std::string& code,
                                                                   std::vector<ConversionChange>& changes) const
{
    static const std::regex pattern(
        R"(((?:unsigned\s+)?(?:long\s+long|long|int|short|double|float)|[A-Za-z_:]\w*(?:::\w+)*(?:<[^;\n{}]+>)?)\s+([A-Za-z_]\w*)\s*\{\s*([A-Za-z_]\w*)\s*\}\s*;\s*\n\s*\1\s+([A-Za-z_]\w*)\s*\{\s*([^{};]+)\s*\}\s*;\s*\n\s*while\s*\(\s*\3\s*([!<>=]+)\s*([^)]*)\)\s*\n\s*\{\s*\n\s*\4\s*=\s*([^;]+);\s*\n\s*\3\s*([/%*+\-]?=)\s*([^;]+);\s*\n\s*\}\s*\n\s*return\s+\2\s*==\s*\4\s*;)",
        std::regex::ECMAScript);

    std::string updated = code;
    std::smatch match;
    if (!std::regex_search(updated, match, pattern)) {
        return updated;
    }

    const std::string valueType = match[1].str();
    const std::string sourceVariable = match[3].str();
    const std::string accumulator = match[4].str();
    const std::string initialValue = trim(match[5].str());
    const std::string conditionOperator = match[6].str();
    const std::string conditionRight = replaceIdentifier(trim(match[7].str()), sourceVariable, "value");
    const std::string accumulatorExpression = replaceIdentifier(trim(match[8].str()), sourceVariable, "value");
    const std::string mutationOperator = match[9].str();
    const std::string mutationExpression = replaceIdentifier(trim(match[10].str()), sourceVariable, "value");
    const std::string lambdaName = lambdaNameForAccumulator(accumulator);
    const std::string replacement =
        "const auto " + lambdaName + " = [](" + valueType + " value)\n"
        "{\n"
        "    " + valueType + " " + accumulator + "{" + initialValue + "};\n\n"
        "    while (value " + conditionOperator + " " + conditionRight + ")\n"
        "    {\n"
        "        " + accumulator + " = " + accumulatorExpression + ";\n"
        "        value " + mutationOperator + " " + mutationExpression + ";\n"
        "    }\n\n"
        "    return " + accumulator + ";\n"
        "};\n\n"
        "return " + sourceVariable + " == " + lambdaName + "(" + sourceVariable + ");";

    addAppliedChange(changes,
                     "AI-style local computation to lambda",
                     trim(match[0].str()),
                     trim(replacement),
                     "Converted self-contained local computation into a lambda to improve locality and modern C++ style.");
    updated.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), replacement);
    return updated;
}

std::string AggressiveRewriteEngine::rewriteHelperFunctionUsedOnce(const std::string& code,
                                                                   std::vector<ConversionChange>& changes) const
{
    static const std::regex helperPattern(
        R"(bool\s+([A-Za-z_]\w*)\s*\(\s*int\s+([A-Za-z_]\w*)\s*\)\s*\n?\{\s*\n\s*return\s+([^;\{\}]+);\s*\n\s*\}\s*\n\s*void\s+([A-Za-z_]\w*)\s*\(([^\)]*std::vector<int>[^\)]*)\)\s*\n?\{\s*\n)",
        std::regex::ECMAScript);

    std::string updated = code;
    std::smatch match;
    if (!std::regex_search(updated, match, helperPattern)) {
        static const std::regex helperOnlyPattern(
            R"(bool\s+([A-Za-z_]\w*)\s*\(\s*int\s+([A-Za-z_]\w*)\s*\)\s*\n?\{\s*\n\s*return\s+([^;\{\}]+);\s*\n\s*\})",
            std::regex::ECMAScript);
        std::smatch helperMatch;
        if (!std::regex_search(updated, helperMatch, helperOnlyPattern)) {
            return updated;
        }

        const std::string helperName = helperMatch[1].str();
        const std::string argumentName = helperMatch[2].str();
        const std::string expression = helperMatch[3].str();
        const std::string suffix = updated.substr(static_cast<std::size_t>(helperMatch.position() + helperMatch.length()));
        const std::string callNeedle = helperName + "(";
        std::size_t callCount = 0;
        std::size_t searchPosition = 0;
        while ((searchPosition = suffix.find(callNeedle, searchPosition)) != std::string::npos) {
            ++callCount;
            searchPosition += callNeedle.size();
        }
        if (callCount != 1) {
            return updated;
        }

        const std::size_t absoluteCall = static_cast<std::size_t>(helperMatch.position() + helperMatch.length()) + suffix.find(callNeedle);
        static const std::regex functionStartPattern(R"(void\s+[A-Za-z_]\w*\s*\([^\)]*\)\s*\n?\{)");
        std::string between = updated.substr(static_cast<std::size_t>(helperMatch.position() + helperMatch.length()),
                                             absoluteCall - static_cast<std::size_t>(helperMatch.position() + helperMatch.length()));
        std::smatch functionMatch;
        std::size_t insertionPosition = std::string::npos;
        std::string remaining = between;
        std::size_t consumed = 0;
        while (std::regex_search(remaining, functionMatch, functionStartPattern)) {
            insertionPosition = static_cast<std::size_t>(helperMatch.position() + helperMatch.length())
                + consumed
                + static_cast<std::size_t>(functionMatch.position() + functionMatch.length());
            consumed += static_cast<std::size_t>(functionMatch.position() + functionMatch.length());
            remaining = functionMatch.suffix().str();
        }
        if (insertionPosition == std::string::npos) {
            return updated;
        }

        const std::string lambda =
            "\n    const auto " + helperName + " = [](int " + argumentName + ")\n"
            "    {\n"
            "        return " + expression + ";\n"
            "    };\n";
        addAppliedChange(changes,
                         "AI-style helper function to lambda",
                         trim(helperMatch[0].str()),
                         trim(lambda),
                         "Moved a small helper used once into the calling function as a local lambda.");
        updated.insert(insertionPosition, lambda);
        updated.erase(static_cast<std::size_t>(helperMatch.position()), static_cast<std::size_t>(helperMatch.length()));
        updated = std::regex_replace(updated, std::regex(R"(for\s*\(\s*int\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*\))"), "for (const auto $1 : $2)");
        updated = std::regex_replace(updated, std::regex(R"(std::cout\s*<<\s*([^;]+?)\s*<<\s*std::endl\s*;)"), "std::cout << $1 << '\\n';");
        return updated;
    }

    const std::string helperName = match[1].str();
    const std::string argumentName = match[2].str();
    const std::string expression = match[3].str();
    const std::string functionName = match[4].str();
    const std::string parameters = match[5].str();
    const std::string replacement =
        "void " + functionName + "(" + parameters + ")\n"
        "{\n"
        "    const auto " + helperName + " = [](int " + argumentName + ")\n"
        "    {\n"
        "        return " + expression + ";\n"
        "    };\n\n";

    addAppliedChange(changes,
                     "AI-style helper function to lambda",
                     trim(match[0].str()),
                     trim(replacement),
                     "Moved a small helper used by local logic into the calling function as a lambda to improve locality.");
    updated.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), replacement);

    updated = std::regex_replace(updated, std::regex(R"(for\s*\(\s*int\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*\))"), "for (const auto $1 : $2)");
    updated = std::regex_replace(updated, std::regex(R"(std::cout\s*<<\s*([^;]+?)\s*<<\s*std::endl\s*;)"), "std::cout << $1 << '\\n';");
    return updated;
}

std::string AggressiveRewriteEngine::rewriteFunctorPredicate(const std::string& code,
                                                             const ModernizationOptions& options,
                                                             std::vector<ConversionChange>& changes) const
{
    static const std::regex functorPattern(
        R"(struct\s+([A-Za-z_]\w*)\s*\{\s*bool\s+operator\(\)\s*\(\s*int\s+([A-Za-z_]\w*)\s*\)\s*const\s*\{\s*return\s+([^;\{\}]+);\s*\}\s*\};)",
        std::regex::ECMAScript);

    std::string updated = code;
    std::smatch functorMatch;
    if (!std::regex_search(updated, functorMatch, functorPattern)) {
        return updated;
    }

    const std::string functorName = functorMatch[1].str();
    const std::string argumentName = functorMatch[2].str();
    const std::string expression = functorMatch[3].str();
    const std::regex callPattern("std::count_if\\s*\\(\\s*([A-Za-z_]\\w*)\\.begin\\(\\)\\s*,\\s*\\1\\.end\\(\\)\\s*,\\s*" + functorName + "\\s*\\(\\s*\\)\\s*\\)");
    std::smatch callMatch;
    if (!std::regex_search(updated, callMatch, callPattern)) {
        return updated;
    }

    const std::string collection = callMatch[1].str();
    const std::string replacement = isCpp20(options)
        ? "std::ranges::count_if(" + collection + ", [](int " + argumentName + ")\n{\n    return " + expression + ";\n})"
        : "std::count_if(" + collection + ".begin(), " + collection + ".end(), [](int " + argumentName + ")\n{\n    return " + expression + ";\n})";

    addAppliedChange(changes,
                     "AI-style functor predicate to lambda",
                     trim(functorMatch[0].str()) + "\n" + trim(callMatch[0].str()),
                     trim(replacement),
                     "Converted a stateless predicate functor into an inline lambda and modernized the algorithm call.");
    updated.replace(static_cast<std::size_t>(callMatch.position()), static_cast<std::size_t>(callMatch.length()), replacement);
    updated.erase(static_cast<std::size_t>(functorMatch.position()), static_cast<std::size_t>(functorMatch.length()));
    return updated;
}

std::string AggressiveRewriteEngine::rewriteOutputLoopsToAlgorithms(const std::string& code,
                                                                    const ModernizationOptions& options,
                                                                    std::vector<ConversionChange>& changes) const
{
    static const std::regex rangeOutputLoop(
        R"((^[ \t]*)for\s*\(\s*const\s+auto&\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*\)\s*\n\1\{\s*\n([ \t]*)std::cout\s*<<\s*\2\s*<<\s*std::endl\s*;\s*\n\1\})",
        std::regex::ECMAScript | std::regex::multiline);

    std::string remaining = code;
    std::string rewritten;
    std::smatch match;
    bool changed = false;

    while (std::regex_search(remaining, match, rangeOutputLoop)) {
        const std::string indent = match[1].str();
        const std::string value = match[2].str();
        const std::string collection = match[3].str();
        const std::string bodyIndent = match[4].str();
        const std::string replacement = isCpp20(options)
            ? indent + "std::ranges::for_each(" + collection + ", [](const auto& " + value + ")\n"
                + indent + "{\n" + bodyIndent + "std::cout << " + value + " << '\\n';\n" + indent + "});"
            : indent + "std::for_each(" + collection + ".begin(), " + collection + ".end(), [](const auto& " + value + ")\n"
                + indent + "{\n" + bodyIndent + "std::cout << " + value + " << '\\n';\n" + indent + "});";

        addAppliedChange(changes,
                         "AI-style algorithm modernization",
                         trim(match[0].str()),
                         trim(replacement),
                         "Converted a simple output loop into an algorithm call with a local lambda.");
        rewritten += match.prefix().str();
        rewritten += replacement;
        remaining = match.suffix().str();
        changed = true;
    }

    if (!changed) {
        return code;
    }

    rewritten += remaining;
    return rewritten;
}

std::string AggressiveRewriteEngine::ensureModernIncludes(const std::string& code,
                                                          const ModernizationOptions& options,
                                                          std::vector<ConversionChange>* changes) const
{
    std::string updated = code;
    if (updated.find("std::make_unique") != std::string::npos
        || updated.find("std::unique_ptr") != std::string::npos
        || updated.find("std::make_shared") != std::string::npos
        || updated.find("std::shared_ptr") != std::string::npos) {
        updated = ensureInclude(updated, "#include <memory>", changes);
    }
    if (updated.find("std::string") != std::string::npos) {
        updated = ensureInclude(updated, "#include <string>", changes);
    }
    if (updated.find("std::string_view") != std::string::npos) {
        updated = ensureInclude(updated, "#include <string_view>", changes);
    }
    if (updated.find("std::optional") != std::string::npos) {
        updated = ensureInclude(updated, "#include <optional>", changes);
    }
    if (updated.find("std::vector") != std::string::npos) {
        updated = ensureInclude(updated, "#include <vector>", changes);
    }
    if (updated.find("std::move") != std::string::npos) {
        updated = ensureInclude(updated, "#include <utility>", changes);
    }
    if (updated.find("std::for_each") != std::string::npos || updated.find("std::count_if") != std::string::npos || updated.find("std::ranges::") != std::string::npos) {
        updated = ensureInclude(updated, "#include <algorithm>", changes);
    }
    if (isCpp20(options) && updated.find("std::ranges::") != std::string::npos) {
        updated = ensureInclude(updated, "#include <ranges>", changes);
    }
    return updated;
}

std::string AggressiveRewriteEngine::lightlyFormat(const std::string& code) const
{
    std::string updated = std::regex_replace(code, std::regex(R"(\n{4,})"), "\n\n\n");
    return updated;
}
