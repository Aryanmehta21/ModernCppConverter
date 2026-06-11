#include "converter/FileIoModernizationPass.h"

#include "converter/FilePointerModernizationPass.h"

std::string FileIoModernizationPass::rewrite(const std::string& code,
                                             std::vector<ConversionChange>& changes) const
{
    const FilePointerModernizationPass filePointerPass;
    return filePointerPass.rewrite(code, changes);
}
