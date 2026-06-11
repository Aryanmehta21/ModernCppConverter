#include "converter/DestructorDeclarationValidationPass.h"

#include "converter/ClassContextResolver.h"

#include <algorithm>
#include <map>
#include <regex>
#include <set>
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

bool hasVirtualDestructor(const ClassContext& context)
{
    return context.destructor.exists && context.destructor.isVirtual;
}

struct DestructorMatch
{
    std::size_t position = 0;
    std::size_t length = 0;
    std::string text;
    bool isDefaulted = false;
};

std::vector<DestructorMatch> destructorMatchesForClass(const ClassContext& context)
{
    std::vector<DestructorMatch> matches;
    if (!context.confident) {
        return matches;
    }

    const std::regex destructorPattern(
        R"(\n?[ \t]*(?:virtual\s+)?~)" + escapeRegex(context.name) + R"(\s*\(\s*\)\s*(?:override\s*)?(?:=\s*default\s*)?(?:;|\{))",
        std::regex::ECMAScript | std::regex::multiline);
    for (std::sregex_iterator iterator(context.block.text.begin(), context.block.text.end(), destructorPattern), end; iterator != end; ++iterator) {
        DestructorMatch match;
        match.position = static_cast<std::size_t>(iterator->position());
        match.length = static_cast<std::size_t>(iterator->length());
        match.text = iterator->str();
        match.isDefaulted = match.text.find("= default") != std::string::npos;
        matches.push_back(std::move(match));
    }
    return matches;
}

std::set<std::string> polymorphicBasesWithVirtualDestructors(const std::vector<ClassContext>& contexts)
{
    std::map<std::string, ClassContext> byName;
    for (const ClassContext& context : contexts) {
        if (context.confident) {
            byName[context.name] = context;
        }
    }

    std::set<std::string> bases;
    for (const ClassContext& context : contexts) {
        for (const std::string& base : context.baseNames) {
            const auto found = byName.find(base);
            if (found != byName.end() && hasVirtualDestructor(found->second)) {
                bases.insert(context.name);
            }
        }
    }
    return bases;
}

std::string repairWrongDestructorName(std::string code,
                                      const ClassContext& context,
                                      std::vector<ConversionChange>& changes)
{
    if (!context.confident || !context.destructor.exists || context.destructor.name == context.name) {
        return code;
    }

    const std::regex wrongNamePattern("~" + escapeRegex(context.destructor.name) + R"(\s*\()");
    const std::string replacement = "~" + context.name + "(";
    const std::string before = code.substr(context.destructor.headerStart,
                                           context.destructor.headerEnd - context.destructor.headerStart);
    std::string header = before;
    header = std::regex_replace(header, wrongNamePattern, replacement, std::regex_constants::format_first_only);
    code.replace(context.destructor.headerStart,
                 context.destructor.headerEnd - context.destructor.headerStart,
                 header);
    addAppliedChange(changes,
                     "Destructor declaration validation",
                     trim(before),
                     trim(header),
                     "Repaired a destructor declaration whose name did not match the enclosing class.");
    return code;
}

std::string removeDuplicateDefaultDestructor(std::string code,
                                             const ClassContext& context,
                                             std::vector<ConversionChange>& changes)
{
    std::vector<DestructorMatch> destructors = destructorMatchesForClass(context);
    if (destructors.size() < 2U) {
        return code;
    }

    const bool hasNonDefaultDestructor = std::any_of(destructors.begin(), destructors.end(), [](const DestructorMatch& match) {
        return !match.isDefaulted;
    });
    std::size_t defaultDestructorsKept = 0;
    std::sort(destructors.begin(), destructors.end(), [](const DestructorMatch& left, const DestructorMatch& right) {
        return left.position > right.position;
    });

    for (const DestructorMatch& destructor : destructors) {
        if (!destructor.isDefaulted) {
            continue;
        }
        if (!hasNonDefaultDestructor && defaultDestructorsKept++ == 0U) {
            continue;
        }

        const std::size_t absolute = context.block.start + destructor.position;
        const std::string before = code.substr(absolute, destructor.length);
        code.erase(absolute, destructor.length);
        addAppliedChange(changes,
                         "Destructor declaration validation",
                         trim(before),
                         "removed",
                         "Removed a duplicate generated default destructor because another destructor already exists for the class.");
    }
    return code;
}

std::string derivedDestructorHeaderWithOverride(std::string header)
{
    header = std::regex_replace(header, std::regex(R"(^([ \t]*)virtual\s+)"), "$1");
    if (header.find("override") != std::string::npos) {
        return header;
    }

    if (header.find("= default") != std::string::npos) {
        return std::regex_replace(header,
                                  std::regex(R"(\s*=\s*default\s*;)"),
                                  " override = default;");
    }

    return std::regex_replace(header, std::regex(R"(\s*(\{|;)\s*$)"), " override $1");
}

std::string enforceDerivedDestructorOverride(std::string code,
                                             const ClassContext& context,
                                             const std::set<std::string>& derivedWithVirtualBaseDestructor,
                                             std::vector<ConversionChange>& changes)
{
    if (!context.confident
        || !context.destructor.exists
        || context.destructor.name != context.name
        || !derivedWithVirtualBaseDestructor.contains(context.name)
        || context.destructor.hasOverride) {
        return code;
    }

    const std::string before = code.substr(context.destructor.headerStart,
                                           context.destructor.headerEnd - context.destructor.headerStart);
    std::string after = derivedDestructorHeaderWithOverride(before);
    if (after == before) {
        return code;
    }

    code.replace(context.destructor.headerStart,
                 context.destructor.headerEnd - context.destructor.headerStart,
                 after);
    addAppliedChange(changes,
                     "Derived destructor override validation",
                     trim(before),
                     trim(after),
                     "Marked a derived destructor override explicitly and removed redundant virtual.");
    return code;
}
} // namespace

std::string DestructorDeclarationValidationPass::validateAndRepair(const std::string& code,
                                                                   std::vector<ConversionChange>& changes) const
{
    std::string updated = code;
    bool changed = true;
    for (int iteration = 0; iteration < 3 && changed; ++iteration) {
        changed = false;
        const std::vector<ClassContext> contexts = ClassContextResolver().resolve(updated);
        const std::set<std::string> derivedWithVirtualBaseDestructor = polymorphicBasesWithVirtualDestructors(contexts);
        std::vector<ClassContext> reverseContexts = contexts;
        std::sort(reverseContexts.begin(), reverseContexts.end(), [](const ClassContext& left, const ClassContext& right) {
            return left.block.start > right.block.start;
        });

        const std::string beforePass = updated;
        for (const ClassContext& context : reverseContexts) {
            updated = repairWrongDestructorName(std::move(updated), context, changes);
            updated = enforceDerivedDestructorOverride(std::move(updated), context, derivedWithVirtualBaseDestructor, changes);
            updated = removeDuplicateDefaultDestructor(std::move(updated), context, changes);
        }
        changed = updated != beforePass;
    }
    return updated;
}
