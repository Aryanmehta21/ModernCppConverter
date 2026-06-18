#pragma once

#include <string>
#include <vector>

enum class SymbolScopeKind
{
    Global,
    ClassMember,
    FunctionLocal,
};

struct SymbolInfo
{
    std::string name;
    std::string type;
    std::string ownerName;
    SymbolScopeKind scopeKind = SymbolScopeKind::Global;
    bool isGenerated = false;
};

class ScopeAwareSymbolTable
{
public:
    [[nodiscard]] static ScopeAwareSymbolTable build(const std::string& code);

    [[nodiscard]] std::vector<SymbolInfo> classMembers(const std::string& className) const;
    [[nodiscard]] std::vector<SymbolInfo> functionLocals(const std::string& functionName) const;
    [[nodiscard]] std::vector<SymbolInfo> visibleSymbols(const std::string& ownerName) const;
    [[nodiscard]] bool hasClassMember(const std::string& className, const std::string& symbolName) const;
    [[nodiscard]] bool hasFunctionLocal(const std::string& functionName, const std::string& symbolName) const;
    [[nodiscard]] bool hasGlobal(const std::string& symbolName) const;

private:
    std::vector<SymbolInfo> symbols_;
};
