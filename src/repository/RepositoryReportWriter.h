#pragma once

#include "models/RepositoryModernizationModels.h"

class RepositoryReportWriter
{
public:
    bool writeReports(RepositoryModernizationResult& result) const;
};
