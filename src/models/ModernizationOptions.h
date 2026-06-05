#pragma once

#include <string>

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
    bool applySafeOwnershipModernization = true;
    bool applyStringViewWhenSafe = false;

    std::string customInstruction;
};
