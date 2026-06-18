#include "converter/ScopeAwareSymbolTable.h"

#include "converter/StructuralAnalyzers.h"

#include <algorithm>
#include <regex>
#include <sstream>
#include <string_view>

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

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

int braceDelta(const std::string& line)
{
    int delta = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (const char character : line) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"' && !inCharacter) {
            inString = !inString;
            continue;
        }
        if (character == '\'' && !inString) {
            inCharacter = !inCharacter;
            continue;
        }
        if (inString || inCharacter) {
            continue;
        }
        if (character == '{') {
            ++delta;
        } else if (character == '}') {
            --delta;
        }
    }
    return delta;
}

bool isAccessSpecifier(const std::string& stripped)
{
    return stripped == "public:" || stripped == "private:" || stripped == "protected:";
}

bool looksLikeMemberDeclaration(const std::string& stripped)
{
    return stripped.ends_with(';')
        && stripped.find('(') == std::string::npos
        && !stripped.starts_with("using ")
        && !stripped.starts_with("typedef ")
        && !isAccessSpecifier(stripped);
}

bool parseDeclaration(const std::string& stripped, std::string& type, std::string& name)
{
    std::smatch match;
    static const std::regex arrayDeclaration(R"(^(.+?)\s+([A-Za-z_]\w*)\s*\[[^\]]+\]\s*;)");
    static const std::regex pointerDeclaration(R"(^(.+?\*)\s*([A-Za-z_]\w*)\s*(?:=\s*[^;]+)?\s*;)");
    static const std::regex valueDeclaration(R"(^(.+?)\s+([A-Za-z_]\w*)\s*(?:=\s*[^;]+)?\s*;)");

    if (std::regex_match(stripped, match, arrayDeclaration)
        || std::regex_match(stripped, match, pointerDeclaration)
        || std::regex_match(stripped, match, valueDeclaration)) {
        type = trim(match[1].str());
        name = match[2].str();
        return !type.empty() && !name.empty();
    }
    return false;
}

bool positionInsideAnyClass(const std::vector<ClassBlock>& classes, const std::size_t position)
{
    return std::any_of(classes.begin(), classes.end(), [position](const ClassBlock& block) {
        return position >= block.start && position < block.end;
    });
}

std::size_t findMatchingBrace(const std::string& code, const std::size_t openBracePosition)
{
    int depth = 0;
    bool inString = false;
    bool inCharacter = false;
    bool escaped = false;
    for (std::size_t index = openBracePosition; index < code.size(); ++index) {
        const char character = code[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\' && (inString || inCharacter)) {
            escaped = true;
            continue;
        }
        if (!inCharacter && character == '"') {
            inString = !inString;
            continue;
        }
        if (!inString && character == '\'') {
            inCharacter = !inCharacter;
            continue;
        }
        if (inString || inCharacter) {
            continue;
        }
        if (character == '{') {
            ++depth;
        } else if (character == '}') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}
} // namespace

ScopeAwareSymbolTable ScopeAwareSymbolTable::build(const std::string& code)
{
    ScopeAwareSymbolTable table;
    const ClassResourceAnalyzer classAnalyzer;
    const std::vector<ClassBlock> classes = classAnalyzer.analyzeClasses(code);

    for (const ClassBlock& block : classes) {
        const std::vector<std::string> lines = splitLines(block.text);
        int depth = 0;
        for (const std::string& line : lines) {
            const std::string stripped = trim(line);
            if (depth == 1 && looksLikeMemberDeclaration(stripped)) {
                std::string type;
                std::string name;
                if (parseDeclaration(stripped, type, name)) {
                    table.symbols_.push_back(SymbolInfo{
                        name,
                        type,
                        block.name,
                        SymbolScopeKind::ClassMember,
                        false,
                    });
                }
            }
            depth += braceDelta(line);
        }
    }

    const std::vector<std::string> lines = splitLines(code);
    std::size_t position = 0;
    int depth = 0;
    for (const std::string& line : lines) {
        const std::string stripped = trim(line);
        if (depth == 0 && looksLikeMemberDeclaration(stripped) && !positionInsideAnyClass(classes, position)) {
            std::string type;
            std::string name;
            if (parseDeclaration(stripped, type, name)) {
                table.symbols_.push_back(SymbolInfo{
                    name,
                    type,
                    {},
                    SymbolScopeKind::Global,
                    false,
                });
            }
        }
        depth += braceDelta(line);
        position += line.size() + 1;
    }

    const std::regex functionPattern(
        R"((?:^|\n)[ \t]*(?:template\s*<[^;\n{}]+>\s*)?(?:[A-Za-z_:][A-Za-z0-9_:<>,\s*&*]*\s+)+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{)",
        std::regex::ECMAScript);
    for (std::sregex_iterator iterator(code.begin(), code.end(), functionPattern), end; iterator != end; ++iterator) {
        const std::size_t headerPosition = static_cast<std::size_t>(iterator->position());
        if (positionInsideAnyClass(classes, headerPosition)) {
            continue;
        }

        const std::string functionName = (*iterator)[1].str();
        const std::size_t openBrace = code.find('{', headerPosition);
        if (openBrace == std::string::npos) {
            continue;
        }
        const std::size_t closeBrace = findMatchingBrace(code, openBrace);
        if (closeBrace == std::string::npos || closeBrace <= openBrace) {
            continue;
        }

        const std::vector<std::string> bodyLines = splitLines(code.substr(openBrace + 1, closeBrace - openBrace - 1));
        int localDepth = 1;
        for (const std::string& bodyLine : bodyLines) {
            const std::string stripped = trim(bodyLine);
            if (localDepth == 1 && looksLikeMemberDeclaration(stripped)) {
                std::string type;
                std::string name;
                if (parseDeclaration(stripped, type, name)) {
                    table.symbols_.push_back(SymbolInfo{
                        name,
                        type,
                        functionName,
                        SymbolScopeKind::FunctionLocal,
                        false,
                    });
                }
            }
            localDepth += braceDelta(bodyLine);
        }
    }

    return table;
}

std::vector<SymbolInfo> ScopeAwareSymbolTable::classMembers(const std::string& className) const
{
    std::vector<SymbolInfo> members;
    for (const SymbolInfo& symbol : symbols_) {
        if (symbol.scopeKind == SymbolScopeKind::ClassMember && symbol.ownerName == className) {
            members.push_back(symbol);
        }
    }
    return members;
}

std::vector<SymbolInfo> ScopeAwareSymbolTable::functionLocals(const std::string& functionName) const
{
    std::vector<SymbolInfo> locals;
    for (const SymbolInfo& symbol : symbols_) {
        if (symbol.scopeKind == SymbolScopeKind::FunctionLocal && symbol.ownerName == functionName) {
            locals.push_back(symbol);
        }
    }
    return locals;
}

std::vector<SymbolInfo> ScopeAwareSymbolTable::visibleSymbols(const std::string& ownerName) const
{
    std::vector<SymbolInfo> visible;
    for (const SymbolInfo& symbol : symbols_) {
        if (symbol.scopeKind == SymbolScopeKind::Global || symbol.ownerName == ownerName) {
            visible.push_back(symbol);
        }
    }
    return visible;
}

bool ScopeAwareSymbolTable::hasClassMember(const std::string& className, const std::string& symbolName) const
{
    return std::any_of(symbols_.begin(), symbols_.end(), [&className, &symbolName](const SymbolInfo& symbol) {
        return symbol.scopeKind == SymbolScopeKind::ClassMember
            && symbol.ownerName == className
            && symbol.name == symbolName;
    });
}

bool ScopeAwareSymbolTable::hasFunctionLocal(const std::string& functionName, const std::string& symbolName) const
{
    return std::any_of(symbols_.begin(), symbols_.end(), [&functionName, &symbolName](const SymbolInfo& symbol) {
        return symbol.scopeKind == SymbolScopeKind::FunctionLocal
            && symbol.ownerName == functionName
            && symbol.name == symbolName;
    });
}

bool ScopeAwareSymbolTable::hasGlobal(const std::string& symbolName) const
{
    return std::any_of(symbols_.begin(), symbols_.end(), [&symbolName](const SymbolInfo& symbol) {
        return symbol.scopeKind == SymbolScopeKind::Global && symbol.name == symbolName;
    });
}
