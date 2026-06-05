#include "converter/ModernCppConverter.h"

#include "converter/RuleBasedConverterEngine.h"

#include <stdexcept>
#include <utility>

ModernCppConverter::ModernCppConverter()
    : ModernCppConverter(std::make_unique<RuleBasedConverterEngine>())
{
}

ModernCppConverter::ModernCppConverter(std::unique_ptr<IConverterEngine> engine)
    : engine_(std::move(engine))
{
    if (!engine_) {
        throw std::invalid_argument("ModernCppConverter requires a converter engine.");
    }
}

ConversionResult ModernCppConverter::convert(const std::string& legacyCode) const
{
    return engine_->convert(legacyCode);
}

ConversionResult ModernCppConverter::convert(const std::string& legacyCode,
                                             const ModernizationOptions& options) const
{
    return engine_->convert(legacyCode, options);
}
