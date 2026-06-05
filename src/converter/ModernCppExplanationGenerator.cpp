#include "converter/ModernCppExplanationGenerator.h"

#include <algorithm>
#include <sstream>

namespace
{
bool containsRule(const std::vector<ConversionChange>& changes, const std::string& ruleText)
{
    return std::any_of(changes.begin(), changes.end(), [&ruleText](const ConversionChange& change) {
        return change.ruleName.find(ruleText) != std::string::npos;
    });
}

std::size_t countApplied(const std::vector<ConversionChange>& changes)
{
    return static_cast<std::size_t>(std::count_if(changes.begin(), changes.end(), [](const ConversionChange& change) {
        return change.applied;
    }));
}

std::size_t countSkipped(const std::vector<ConversionChange>& changes)
{
    return static_cast<std::size_t>(std::count_if(changes.begin(), changes.end(), [](const ConversionChange& change) {
        return change.skipped;
    }));
}

void writePlural(std::ostringstream& output, std::size_t count, const std::string& singular)
{
    output << count << ' ' << singular;
    if (count != 1) {
        output << 's';
    }
}
} // namespace

std::string ModernCppExplanationGenerator::generate(const std::string& modernCode,
                                                    const std::vector<ConversionChange>& changes,
                                                    const ModernizationOptions& options) const
{
    const std::size_t appliedCount = countApplied(changes);
    const std::size_t skippedCount = countSkipped(changes);
    const std::size_t suggestionCount = changes.size() - appliedCount - skippedCount;

    std::ostringstream explanation;
    explanation << "Summary of changes\n";
    explanation << "==================\n\n";

    if (modernCode.empty()) {
        explanation << "No input code was provided.\n\n";
        explanation << "Modern C++ concepts used\n";
        explanation << "========================\n\n";
        explanation << "No concepts were applied because there was no code to convert.\n\n";
        explanation << "Suggested future improvements\n";
        explanation << "=============================\n\n";
        explanation << "Paste legacy C++ code and run Convert to see modernization guidance.\n";
        return explanation.str();
    }

    if (changes.empty()) {
        explanation << "No automatic changes or suggestions were produced. The code did not match the current conservative rule set.\n\n";
    } else {
        writePlural(explanation, appliedCount, "automatic change");
        explanation << " applied and ";
        writePlural(explanation, suggestionCount, "suggestion");
        explanation << " generated, and ";
        writePlural(explanation, skippedCount, "rule");
        explanation << " skipped because an option was disabled.\n\n";
    }

    if (!options.customInstruction.empty()) {
        explanation << "Custom modernization instruction\n";
        explanation << "================================\n\n";
        explanation << options.customInstruction << "\n\n";
        explanation << "This instruction is stored with the conversion options for future engines. The rule-based engine does not interpret free-form instructions except where a specific rule explicitly supports them.\n\n";
    }

    explanation << "Modern C++ concepts used\n";
    explanation << "========================\n\n";

    bool wroteConcept = false;

    if (options.useNullptr && containsRule(changes, "NULL to nullptr")) {
        explanation << "- nullptr is preferred over NULL because it is type-safe and avoids overload ambiguity.\n";
        wroteConcept = true;
    }

    if (options.useUsingAliases && containsRule(changes, "typedef to using")) {
        explanation << "- using aliases are the modern way to give a type a readable name. They are easier to read and work better with templates.\n";
        wroteConcept = true;
    }

    if ((options.useSmartPointers || options.useMakeUnique)
        && (containsRule(changes, "Raw pointer ownership") || containsRule(changes, "Raw pointer to std::unique_ptr"))) {
        explanation << "- Raw pointers created with new and cleaned up with delete can make ownership unclear. Smart pointers express ownership directly and clean up automatically.\n";
        wroteConcept = true;
    }

    if ((options.useRangeBasedFor || options.useRanges) && containsRule(changes, "Range-based loop")) {
        explanation << "- Range-based for loops let you visit each item in a collection without manually managing an index when the index is not important.\n";
        wroteConcept = true;
    }

    if (options.useEnumClass && containsRule(changes, "enum class")) {
        explanation << "- enum class keeps enum values scoped, which helps prevent accidental name collisions and implicit integer conversions.\n";
        wroteConcept = true;
    }

    if (options.useOptional && containsRule(changes, "std::optional")) {
        explanation << "- std::optional is useful when a value may be absent and you want that absence to be explicit in the type.\n";
        wroteConcept = true;
    }

    if (options.useStringView && containsRule(changes, "std::string_view")) {
        explanation << "- std::string_view can describe read-only string text without owning or copying it.\n";
        wroteConcept = true;
    }

    if (!wroteConcept) {
        explanation << "No modern C++ concepts were applied by the current rule set.\n";
    }

    explanation << "\nSuggested future improvements\n";
    explanation << "=============================\n\n";

    bool wroteImprovement = false;

    if ((options.useSmartPointers || options.useMakeUnique)
        && (containsRule(changes, "Raw pointer ownership") || containsRule(changes, "Raw pointer to std::unique_ptr"))) {
        explanation << "- Consider std::unique_ptr for exclusive ownership, std::shared_ptr for shared ownership, or std::make_unique/std::make_shared to construct smart pointers safely.\n";
        wroteImprovement = true;
    }

    if ((options.useSpan || options.customInstruction.find("vector") != std::string::npos) && containsRule(changes, "C-style array")) {
        explanation << "- Consider std::array for fixed-size arrays or std::vector when the number of elements can change.\n";
        wroteImprovement = true;
    }

    if ((options.useRangeBasedFor || options.useRanges) && containsRule(changes, "Range-based loop")) {
        explanation << "- Review index-based loops and use range-based loops when you only need each element, not its position.\n";
        wroteImprovement = true;
    }

    if (containsRule(changes, "Old-style cast")) {
        explanation << "- Review old-style casts and choose static_cast, const_cast, or reinterpret_cast based on the exact conversion needed.\n";
        wroteImprovement = true;
    }

    if (!wroteImprovement) {
        explanation << "No extra follow-up improvements were detected beyond the safe automatic changes already applied.\n";
    }

    return explanation.str();
}
