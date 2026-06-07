#pragma once

#include <string>

enum class OfflineModernizationLevel
{
    Conservative,
    Balanced,
    AggressiveSafe,
    AiStyleAggressiveRewrite,
};

enum class CppStandard
{
    Cpp17,
    Cpp20,
};

enum class OfflineRewriteStyle
{
    SafeModernization,
    AggressiveAiLikeRewrite,
};

struct ModernizationOptions
{
    bool useNullptr = true;
    bool useUsingAliases = true;
    bool useAuto = false;
    bool useRangeBasedFor = true;
    bool useLambdas = false;
    bool useOverrideFinal = true;
    bool useConstexpr = false;
    bool useSmartPointers = true;
    bool useMoveSemantics = false;
    bool useEnumClass = true;

    bool useGenericLambdas = false;
    bool useMakeUnique = true;

    bool useStructuredBindings = false;
    bool useIfConstexpr = false;
    bool useOptional = true;
    bool useVariant = false;
    bool useStringView = true;
    bool useInlineVariables = false;

    bool useConcepts = false;
    bool useRanges = false;
    bool useSpan = false;
    bool useDesignatedInitializers = false;
    bool useConstevalConstinit = false;
    bool useSpaceshipOperator = false;
    bool useStdFormatForStreams = false;
    bool applySafeOwnershipModernization = true;
    bool applyStringViewWhenSafe = false;
    bool compileVerificationEnabled = false;
    OfflineModernizationLevel offlineModernizationLevel = OfflineModernizationLevel::Balanced;
    OfflineRewriteStyle offlineRewriteStyle = OfflineRewriteStyle::SafeModernization;
    CppStandard targetStandard = CppStandard::Cpp20;

    std::string customInstruction;
};
