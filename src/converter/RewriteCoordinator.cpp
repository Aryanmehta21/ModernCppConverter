#include "converter/RewriteCoordinator.h"

#include <algorithm>
#include <cassert>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace
{
bool rangesOverlap(const SourceRange& left, const SourceRange& right)
{
    return left.start.offset < right.end.offset && right.start.offset < left.end.offset;
}

auto editIdentity(const RewriteEdit& edit)
{
    return std::make_tuple(edit.range.start.offset,
                           edit.range.end.offset,
                           edit.replacementText,
                           edit.passName,
                           edit.affectedSymbol);
}

void skipEdit(RewriteApplicationResult& result, RewriteEdit edit, std::string reason)
{
    result.skippedEdits.push_back(SkippedRewriteEdit{std::move(edit), std::move(reason)});
}
} // namespace

RewriteApplicationResult RewriteCoordinator::apply(const std::string& source,
                                                   const std::vector<RewriteEdit>& edits) const
{
    RewriteApplicationResult result;
    result.code = source;

    std::vector<RewriteEdit> candidates = edits;
    std::sort(candidates.begin(), candidates.end(), [](const RewriteEdit& left, const RewriteEdit& right) {
        if (left.range.start.offset != right.range.start.offset) {
            return left.range.start.offset < right.range.start.offset;
        }
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        return left.range.end.offset < right.range.end.offset;
    });

    std::set<decltype(editIdentity(std::declval<RewriteEdit>()))> seen;
    std::vector<RewriteEdit> accepted;
    for (RewriteEdit edit : candidates) {
        if (!edit.range.isValidFor(source.size())) {
            ++result.invalidRanges;
            skipEdit(result, std::move(edit), "source range invalid");
            continue;
        }

        const auto identity = editIdentity(edit);
        if (!seen.insert(identity).second) {
            ++result.duplicateEdits;
            skipEdit(result, std::move(edit), "duplicate edit");
            continue;
        }

        auto conflict = std::find_if(accepted.begin(), accepted.end(), [&](const RewriteEdit& acceptedEdit) {
            return rangesOverlap(edit.range, acceptedEdit.range) && !edit.allowOverlap && !acceptedEdit.allowOverlap;
        });
        if (conflict != accepted.end()) {
            ++result.overlapConflicts;
            skipEdit(result, std::move(edit), "overlapping edit conflict");
            continue;
        }

        accepted.push_back(std::move(edit));
    }

    std::sort(accepted.begin(), accepted.end(), [](const RewriteEdit& left, const RewriteEdit& right) {
        if (left.range.start.offset != right.range.start.offset) {
            return left.range.start.offset > right.range.start.offset;
        }
        return left.priority > right.priority;
    });

    for (const RewriteEdit& edit : accepted) {
        assert(edit.range.isValidFor(result.code.size()) && "Rewrite edit range must remain valid before application");
        result.code.replace(edit.range.start.offset, edit.range.length(), edit.replacementText);
        result.appliedEdits.push_back(edit);
    }

    return result;
}
