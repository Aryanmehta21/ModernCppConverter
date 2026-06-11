#include "converter/PolymorphicContractPolishPass.h"

#include "converter/PolymorphicContractPass.h"

std::string PolymorphicContractPolishPass::rewrite(const std::string& code,
                                                   std::vector<ConversionChange>& changes) const
{
    const PolymorphicContractPass pass;
    return pass.rewrite(code, changes);
}

