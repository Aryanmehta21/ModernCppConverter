#include "parser/LightweightCppParser.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace
{
bool isIdentifierStart(char value)
{
    const auto ch = static_cast<unsigned char>(value);
    return std::isalpha(ch) != 0 || value == '_';
}

bool isIdentifierBody(char value)
{
    const auto ch = static_cast<unsigned char>(value);
    return std::isalnum(ch) != 0 || value == '_';
}

bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string trim(std::string_view text)
{
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }

    return std::string(text.substr(begin, end - begin));
}

bool isKeywordText(std::string_view text)
{
    static constexpr std::string_view keywords[] = {
        "alignas", "alignof", "asm", "auto", "bool", "break", "case", "catch", "char", "char8_t",
        "char16_t", "char32_t", "class", "concept", "const", "consteval", "constexpr", "constinit",
        "continue", "decltype", "default", "delete", "do", "double", "else", "enum", "explicit",
        "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int",
        "long", "mutable", "namespace", "new", "noexcept", "nullptr", "operator", "private",
        "protected", "public", "register", "requires", "return", "short", "signed", "sizeof",
        "static", "static_assert", "struct", "switch", "template", "this", "thread_local", "throw",
        "true", "try", "typedef", "typename", "union", "unsigned", "using", "virtual", "void",
        "volatile", "wchar_t", "while"
    };
    return std::find(std::begin(keywords), std::end(keywords), text) != std::end(keywords);
}

bool isControlKeyword(std::string_view text)
{
    static constexpr std::string_view controls[] = {
        "if", "for", "while", "switch", "catch", "sizeof", "alignof", "decltype", "return", "new",
        "delete", "static_cast", "reinterpret_cast", "const_cast", "dynamic_cast"
    };
    return std::find(std::begin(controls), std::end(controls), text) != std::end(controls);
}

bool isAccessSpecifier(std::string_view text)
{
    return text == "public" || text == "private" || text == "protected";
}

bool isSymbolNoSpaceBefore(std::string_view text)
{
    return text == "::" || text == ";" || text == "," || text == ")" || text == "]" || text == ">"
        || text == "." || text == "->";
}

bool isSymbolNoSpaceAfter(std::string_view text)
{
    return text == "::" || text == "(" || text == "[" || text == "<" || text == "*" || text == "&"
        || text == "~" || text == "." || text == "->";
}

bool isDeclarationStop(std::string_view text)
{
    return text == "=" || text == "{" || text == "(" || text == "[" || text == ";";
}

bool isIgnorableForParsing(const CppToken& token)
{
    return token.kind == CppTokenKind::Comment || token.kind == CppTokenKind::Preprocessor;
}

std::string joinTokenText(const std::vector<CppToken>& tokens, std::size_t begin, std::size_t end)
{
    std::string result;
    for (std::size_t index = begin; index < end; ++index) {
        const std::string& text = tokens[index].text;
        if (result.empty()) {
            result += text;
            continue;
        }

        const std::string& previous = tokens[index - 1].text;
        if (!isSymbolNoSpaceBefore(text) && !isSymbolNoSpaceAfter(previous)) {
            result += ' ';
        }
        result += text;
    }
    return trim(result);
}

SourceRange rangeFromTokens(const std::vector<CppToken>& tokens, std::size_t begin, std::size_t endInclusive)
{
    SourceRange range;
    if (begin >= tokens.size() || endInclusive >= tokens.size() || endInclusive < begin) {
        return range;
    }
    range.start = tokens[begin].range.start;
    range.end = tokens[endInclusive].range.end;
    return range;
}

SourceRange annotateRange(SourceRange range,
                          SourceEntityKind kind,
                          std::string name = {},
                          std::optional<std::size_t> parentScopeId = std::nullopt)
{
    range.entityKind = kind;
    range.entityName = std::move(name);
    range.parentScopeId = parentScopeId;
    return range;
}

class ParserImpl
{
public:
    explicit ParserImpl(std::string source)
        : source_(std::move(source))
    {
    }

    ParsedDocument run()
    {
        document_.originalSource = source_;
        tokenize();
        collectDirectives();
        buildSignificantTokens();
        buildTokenMatches();

        ScopeInfo globalScope;
        globalScope.kind = ScopeKind::Global;
        globalScope.name = "<global>";
        globalScope.range.start = {0, 1, 1};
        globalScope.range.end = endPosition();
        globalScope.range.entityKind = SourceEntityKind::Scope;
        globalScope.range.entityName = globalScope.name;
        document_.scopes.push_back(globalScope);

        parseEntities(0, significantTokens_.size(), {}, 0);
        return document_;
    }

private:
    SourcePosition currentPosition() const
    {
        return {offset_, line_, column_};
    }

    SourcePosition endPosition() const
    {
        return {source_.size(), line_, column_};
    }

    void advanceOne()
    {
        if (offset_ >= source_.size()) {
            return;
        }

        if (source_[offset_] == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        ++offset_;
    }

    void addToken(std::size_t startOffset,
                  SourcePosition start,
                  SourcePosition end,
                  CppTokenKind kind)
    {
        CppToken token;
        token.text = source_.substr(startOffset, end.offset - startOffset);
        token.kind = kind;
        token.range.start = start;
        token.range.end = end;
        token.range.entityKind = SourceEntityKind::Token;
        token.range.entityName = token.text;
        document_.tokens.push_back(std::move(token));
    }

    void tokenize()
    {
        while (offset_ < source_.size()) {
            const char ch = source_[offset_];
            if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
                advanceOne();
                continue;
            }

            const std::size_t startOffset = offset_;
            const SourcePosition start = currentPosition();

            if (ch == '#') {
                while (offset_ < source_.size() && source_[offset_] != '\n') {
                    advanceOne();
                }
                addToken(startOffset, start, currentPosition(), CppTokenKind::Preprocessor);
                continue;
            }

            if (ch == '/' && offset_ + 1 < source_.size() && source_[offset_ + 1] == '/') {
                advanceOne();
                advanceOne();
                while (offset_ < source_.size() && source_[offset_] != '\n') {
                    advanceOne();
                }
                addToken(startOffset, start, currentPosition(), CppTokenKind::Comment);
                continue;
            }

            if (ch == '/' && offset_ + 1 < source_.size() && source_[offset_ + 1] == '*') {
                advanceOne();
                advanceOne();
                bool closed = false;
                while (offset_ < source_.size()) {
                    if (source_[offset_] == '*' && offset_ + 1 < source_.size() && source_[offset_ + 1] == '/') {
                        advanceOne();
                        advanceOne();
                        closed = true;
                        break;
                    }
                    advanceOne();
                }
                if (!closed) {
                    markParseWarning("unterminated block comment");
                }
                addToken(startOffset, start, currentPosition(), CppTokenKind::Comment);
                continue;
            }

            if (ch == '"' || ch == '\'') {
                const char quote = ch;
                advanceOne();
                bool closed = false;
                while (offset_ < source_.size()) {
                    if (source_[offset_] == '\\') {
                        advanceOne();
                        advanceOne();
                        continue;
                    }
                    if (source_[offset_] == quote) {
                        advanceOne();
                        closed = true;
                        break;
                    }
                    advanceOne();
                }
                if (!closed) {
                    markParseWarning("unterminated literal");
                }
                addToken(startOffset,
                         start,
                         currentPosition(),
                         quote == '"' ? CppTokenKind::StringLiteral : CppTokenKind::CharLiteral);
                continue;
            }

            if (isIdentifierStart(ch)) {
                advanceOne();
                while (offset_ < source_.size() && isIdentifierBody(source_[offset_])) {
                    advanceOne();
                }
                const std::string_view text(source_.data() + startOffset, offset_ - startOffset);
                addToken(startOffset,
                         start,
                         currentPosition(),
                         isKeywordText(text) ? CppTokenKind::Keyword : CppTokenKind::Identifier);
                continue;
            }

            if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
                advanceOne();
                while (offset_ < source_.size()) {
                    const char next = source_[offset_];
                    if (std::isalnum(static_cast<unsigned char>(next)) == 0 && next != '_' && next != '.'
                        && next != '\'') {
                        break;
                    }
                    advanceOne();
                }
                addToken(startOffset, start, currentPosition(), CppTokenKind::Number);
                continue;
            }

            const std::string two = offset_ + 1 < source_.size() ? source_.substr(offset_, 2) : std::string{};
            const std::string three = offset_ + 2 < source_.size() ? source_.substr(offset_, 3) : std::string{};
            if (three == "..." || three == "->*") {
                advanceOne();
                advanceOne();
                advanceOne();
            } else if (two == "::" || two == "->" || two == "++" || two == "--" || two == "=="
                       || two == "!=" || two == "<=" || two == ">=" || two == "&&" || two == "||"
                       || two == "+=" || two == "-=" || two == "*=" || two == "/=" || two == "%="
                       || two == "<<" || two == ">>") {
                advanceOne();
                advanceOne();
            } else {
                advanceOne();
            }
            addToken(startOffset, start, currentPosition(), CppTokenKind::Symbol);
        }
    }

    void collectDirectives()
    {
        for (const CppToken& token : document_.tokens) {
            if (token.kind != CppTokenKind::Preprocessor) {
                continue;
            }

            const std::string text = trim(token.text);
            if (startsWith(text, "#include")) {
                ParsedIncludeDirective include;
                include.path = trim(std::string_view(text).substr(std::string_view("#include").size()));
                include.range = annotateRange(token.range, SourceEntityKind::Include, include.path);
                document_.includes.push_back(std::move(include));
            } else if (startsWith(text, "#define")) {
                std::string rest = trim(std::string_view(text).substr(std::string_view("#define").size()));
                ParsedMacroDirective macro;
                std::size_t nameEnd = 0;
                while (nameEnd < rest.size() && isIdentifierBody(rest[nameEnd])) {
                    ++nameEnd;
                }
                macro.name = rest.substr(0, nameEnd);
                macro.range = annotateRange(token.range, SourceEntityKind::Macro, macro.name);
                document_.macros.push_back(std::move(macro));
            }
        }
    }

    void buildSignificantTokens()
    {
        significantTokens_.clear();
        for (const CppToken& token : document_.tokens) {
            if (!isIgnorableForParsing(token)) {
                significantTokens_.push_back(token);
            }
        }
    }

    void buildTokenMatches()
    {
        buildMatches("(", ")", parenMatches_, "unmatched parenthesis");
        buildMatches("{", "}", braceMatches_, "unmatched brace");
    }

    void buildMatches(const std::string& open,
                      const std::string& close,
                      std::unordered_map<std::size_t, std::size_t>& matches,
                      const std::string& warning)
    {
        std::vector<std::size_t> stack;
        for (std::size_t index = 0; index < significantTokens_.size(); ++index) {
            const std::string& text = significantTokens_[index].text;
            if (text == open) {
                stack.push_back(index);
            } else if (text == close) {
                if (stack.empty()) {
                    markParseWarning(warning);
                    continue;
                }
                const std::size_t openIndex = stack.back();
                stack.pop_back();
                matches[openIndex] = index;
                matches[index] = openIndex;
            }
        }

        if (!stack.empty()) {
            markParseWarning(warning);
        }
    }

    void markParseWarning(const std::string& warning)
    {
        document_.parseSucceeded = false;
        if (std::find(document_.warnings.begin(), document_.warnings.end(), warning) == document_.warnings.end()) {
            document_.warnings.push_back(warning);
        }
    }

    void parseEntities(std::size_t begin,
                       std::size_t end,
                       const std::string& parentName,
                       std::size_t parentScopeIndex)
    {
        std::size_t index = begin;
        while (index < end) {
            const std::string& text = significantTokens_[index].text;
            if ((text == "class" || text == "struct") && parseAggregate(index, end, parentScopeIndex)) {
                continue;
            }
            if (text == "enum" && parseEnum(index, end)) {
                continue;
            }
            if (text == "(" && parseFunctionAt(index, begin, end, parentName, parentScopeIndex)) {
                continue;
            }
            ++index;
        }
    }

    bool parseAggregate(std::size_t& index, std::size_t end, std::size_t parentScopeIndex)
    {
        if (index + 1 >= end || significantTokens_[index + 1].kind != CppTokenKind::Identifier) {
            return false;
        }

        const bool isStruct = significantTokens_[index].text == "struct";
        const std::size_t nameIndex = index + 1;
        std::optional<std::size_t> bodyOpen;
        std::optional<std::size_t> declarationEnd;
        for (std::size_t scan = nameIndex + 1; scan < end; ++scan) {
            if (significantTokens_[scan].text == ";") {
                declarationEnd = scan;
                break;
            }
            if (significantTokens_[scan].text == "{") {
                bodyOpen = scan;
                break;
            }
        }

        if (!bodyOpen.has_value()) {
            return false;
        }

        const auto match = braceMatches_.find(*bodyOpen);
        if (match == braceMatches_.end() || match->second >= end) {
            return false;
        }

        const std::size_t bodyClose = match->second;
        declarationEnd = bodyClose;
        if (bodyClose + 1 < end && significantTokens_[bodyClose + 1].text == ";") {
            declarationEnd = bodyClose + 1;
        }

        ParsedAggregate aggregate;
        aggregate.kind = isStruct ? ParsedAggregateKind::Struct : ParsedAggregateKind::Class;
        aggregate.name = significantTokens_[nameIndex].text;
        aggregate.nameRange = annotateRange(significantTokens_[nameIndex].range,
                                            isStruct ? SourceEntityKind::Struct : SourceEntityKind::Class,
                                            aggregate.name,
                                            parentScopeIndex);
        aggregate.range = annotateRange(rangeFromTokens(significantTokens_, index, *declarationEnd),
                                        isStruct ? SourceEntityKind::Struct : SourceEntityKind::Class,
                                        aggregate.name,
                                        parentScopeIndex);
        aggregate.bodyRange = annotateRange(rangeFromTokens(significantTokens_, *bodyOpen, bodyClose),
                                           SourceEntityKind::Scope,
                                           aggregate.name,
                                           parentScopeIndex);
        aggregate.baseNames = parseBaseNames(nameIndex + 1, *bodyOpen);
        document_.aggregates.push_back(aggregate);

        ScopeInfo scope;
        scope.kind = isStruct ? ScopeKind::Struct : ScopeKind::Class;
        scope.name = aggregate.name;
        scope.range = aggregate.bodyRange;
        scope.parentIndex = parentScopeIndex;
        const std::size_t scopeIndex = document_.scopes.size();
        document_.scopes.push_back(std::move(scope));

        parseMemberVariables(*bodyOpen + 1, bodyClose, aggregate.name);
        parseEntities(*bodyOpen + 1, bodyClose, aggregate.name, scopeIndex);

        index = *declarationEnd + 1;
        return true;
    }

    std::vector<std::string> parseBaseNames(std::size_t begin, std::size_t end) const
    {
        std::vector<std::string> names;
        bool inBaseClause = false;
        for (std::size_t index = begin; index < end; ++index) {
            const CppToken& token = significantTokens_[index];
            if (token.text == ":") {
                inBaseClause = true;
                continue;
            }
            if (!inBaseClause || token.kind != CppTokenKind::Identifier || isAccessSpecifier(token.text)) {
                continue;
            }
            names.push_back(token.text);
        }
        return names;
    }

    bool parseEnum(std::size_t& index, std::size_t end)
    {
        std::size_t scan = index + 1;
        bool scoped = false;
        if (scan < end && (significantTokens_[scan].text == "class" || significantTokens_[scan].text == "struct")) {
            scoped = true;
            ++scan;
        }
        if (scan >= end || significantTokens_[scan].kind != CppTokenKind::Identifier) {
            return false;
        }

        const std::size_t nameIndex = scan;
        std::optional<std::size_t> bodyOpen;
        for (++scan; scan < end; ++scan) {
            if (significantTokens_[scan].text == ";") {
                return false;
            }
            if (significantTokens_[scan].text == "{") {
                bodyOpen = scan;
                break;
            }
        }

        if (!bodyOpen.has_value()) {
            return false;
        }

        const auto match = braceMatches_.find(*bodyOpen);
        if (match == braceMatches_.end() || match->second >= end) {
            return false;
        }

        const std::size_t bodyClose = match->second;
        std::size_t declarationEnd = bodyClose;
        if (bodyClose + 1 < end && significantTokens_[bodyClose + 1].text == ";") {
            declarationEnd = bodyClose + 1;
        }

        ParsedEnum parsedEnum;
        parsedEnum.name = significantTokens_[nameIndex].text;
        parsedEnum.scoped = scoped;
        parsedEnum.nameRange = annotateRange(significantTokens_[nameIndex].range, SourceEntityKind::Enum, parsedEnum.name);
        parsedEnum.range = annotateRange(rangeFromTokens(significantTokens_, index, declarationEnd),
                                         SourceEntityKind::Enum,
                                         parsedEnum.name);
        parsedEnum.bodyRange = annotateRange(rangeFromTokens(significantTokens_, *bodyOpen, bodyClose),
                                            SourceEntityKind::Scope,
                                            parsedEnum.name);
        parsedEnum.underlyingType = parseEnumUnderlyingType(nameIndex + 1, *bodyOpen);
        parsedEnum.enumerators = parseEnumerators(*bodyOpen + 1, bodyClose);
        document_.enums.push_back(std::move(parsedEnum));

        index = declarationEnd + 1;
        return true;
    }

    std::string parseEnumUnderlyingType(std::size_t begin, std::size_t bodyOpen) const
    {
        for (std::size_t index = begin; index < bodyOpen; ++index) {
            if (significantTokens_[index].text == ":") {
                return joinTokenText(significantTokens_, index + 1, bodyOpen);
            }
        }
        return {};
    }

    std::vector<std::string> parseEnumerators(std::size_t begin, std::size_t end) const
    {
        std::vector<std::string> enumerators;
        for (std::size_t index = begin; index < end; ++index) {
            if (significantTokens_[index].kind == CppTokenKind::Identifier) {
                enumerators.push_back(significantTokens_[index].text);
                while (index < end && significantTokens_[index].text != ",") {
                    ++index;
                }
            }
        }
        return enumerators;
    }

    bool parseFunctionAt(std::size_t& index,
                         std::size_t searchBegin,
                         std::size_t end,
                         const std::string& parentName,
                         std::size_t parentScopeIndex)
    {
        if (index == 0) {
            return false;
        }

        const std::size_t nameIndex = index - 1;
        if (significantTokens_[nameIndex].kind != CppTokenKind::Identifier || isControlKeyword(significantTokens_[nameIndex].text)) {
            return false;
        }

        const auto closeParenIt = parenMatches_.find(index);
        if (closeParenIt == parenMatches_.end()) {
            return false;
        }

        const std::size_t closeParen = closeParenIt->second;
        if (closeParen >= end) {
            return false;
        }

        std::optional<std::size_t> signatureEnd;
        std::optional<std::size_t> bodyOpen;
        for (std::size_t scan = closeParen + 1; scan < end; ++scan) {
            const std::string& text = significantTokens_[scan].text;
            if (text == ";") {
                signatureEnd = scan;
                break;
            }
            if (text == "{") {
                bodyOpen = scan;
                break;
            }
            if (text == "=" && scan + 1 < end && significantTokens_[scan + 1].text == "0") {
                continue;
            }
        }

        if (!signatureEnd.has_value() && !bodyOpen.has_value()) {
            return false;
        }

        std::optional<std::size_t> bodyClose;
        if (bodyOpen.has_value()) {
            const auto closeIt = braceMatches_.find(*bodyOpen);
            if (closeIt == braceMatches_.end() || closeIt->second >= end) {
                return false;
            }
            bodyClose = closeIt->second;
            signatureEnd = *bodyClose;
        }

        const std::size_t declarationStart = functionDeclarationStart(searchBegin, nameIndex);

        ParsedFunction function;
        function.name = significantTokens_[nameIndex].text;
        function.parentName = parentName;
        function.isMember = !parentName.empty();
        function.returnType = cleanLeadingAccess(joinTokenText(significantTokens_, declarationStart, nameIndex));
        function.parameters = parseParameters(index + 1, closeParen);
        function.range = annotateRange(rangeFromTokens(significantTokens_, declarationStart, *signatureEnd),
                                       SourceEntityKind::Function,
                                       function.name,
                                       parentScopeIndex);
        function.nameRange = annotateRange(significantTokens_[nameIndex].range,
                                           SourceEntityKind::Function,
                                           function.name,
                                           parentScopeIndex);
        function.hasBody = bodyOpen.has_value();
        function.isConst = hasQualifier(closeParen + 1, bodyOpen.value_or(*signatureEnd), "const");
        if (bodyOpen.has_value() && bodyClose.has_value()) {
            function.bodyRange = annotateRange(rangeFromTokens(significantTokens_, *bodyOpen, *bodyClose),
                                               SourceEntityKind::Scope,
                                               function.name,
                                               parentScopeIndex);
        }
        document_.functions.push_back(function);

        if (bodyOpen.has_value() && bodyClose.has_value()) {
            ScopeInfo scope;
            scope.kind = ScopeKind::Function;
            scope.name = function.name;
            scope.range = function.bodyRange;
            scope.parentIndex = parentScopeIndex;
            const std::size_t scopeIndex = document_.scopes.size();
            document_.scopes.push_back(std::move(scope));
            parseLocalVariables(*bodyOpen + 1, *bodyClose, function.name);
            parseCallExpressions(*bodyOpen + 1, *bodyClose, function.name);
            addBlockScopes(*bodyOpen + 1, *bodyClose, scopeIndex);
            index = *bodyClose + 1;
        } else {
            index = *signatureEnd + 1;
        }
        return true;
    }

    std::size_t functionDeclarationStart(std::size_t searchBegin, std::size_t nameIndex) const
    {
        std::size_t start = nameIndex;
        while (start > searchBegin) {
            const std::string& previous = significantTokens_[start - 1].text;
            if (previous == ";" || previous == "{" || previous == "}") {
                break;
            }
            --start;
        }
        return start;
    }

    bool hasQualifier(std::size_t begin, std::size_t end, std::string_view qualifier) const
    {
        for (std::size_t index = begin; index < end; ++index) {
            if (significantTokens_[index].text == qualifier) {
                return true;
            }
        }
        return false;
    }

    std::string cleanLeadingAccess(std::string text) const
    {
        text = trim(text);
        for (std::string_view access : {"public:", "private:", "protected:"}) {
            if (startsWith(text, access)) {
                text = trim(std::string_view(text).substr(access.size()));
            }
        }
        return text;
    }

    std::vector<ParsedParameter> parseParameters(std::size_t begin, std::size_t end) const
    {
        std::vector<ParsedParameter> parameters;
        std::size_t paramStart = begin;
        int angleDepth = 0;
        for (std::size_t index = begin; index <= end; ++index) {
            const bool atEnd = index == end;
            if (!atEnd) {
                if (significantTokens_[index].text == "<") {
                    ++angleDepth;
                } else if (significantTokens_[index].text == ">" && angleDepth > 0) {
                    --angleDepth;
                }
            }
            if (atEnd || (angleDepth == 0 && significantTokens_[index].text == ",")) {
                if (paramStart < index) {
                    auto parameter = parseParameter(paramStart, index);
                    if (parameter.has_value()) {
                        parameters.push_back(std::move(*parameter));
                    }
                }
                paramStart = index + 1;
            }
        }
        return parameters;
    }

    std::optional<ParsedParameter> parseParameter(std::size_t begin, std::size_t end) const
    {
        std::size_t stop = end;
        for (std::size_t index = begin; index < end; ++index) {
            if (significantTokens_[index].text == "=") {
                stop = index;
                break;
            }
        }
        if (stop <= begin || (stop - begin == 1 && significantTokens_[begin].text == "void")) {
            return std::nullopt;
        }

        std::optional<std::size_t> nameIndex;
        for (std::size_t index = stop; index > begin; --index) {
            const std::size_t candidate = index - 1;
            if (significantTokens_[candidate].kind == CppTokenKind::Identifier) {
                nameIndex = candidate;
                break;
            }
        }

        ParsedParameter parameter;
        parameter.range = annotateRange(rangeFromTokens(significantTokens_, begin, stop - 1),
                                        SourceEntityKind::Variable);
        if (nameIndex.has_value() && *nameIndex > begin) {
            parameter.name = significantTokens_[*nameIndex].text;
            parameter.nameRange = annotateRange(significantTokens_[*nameIndex].range,
                                                SourceEntityKind::Variable,
                                                parameter.name);
            parameter.type = joinTokenText(significantTokens_, begin, *nameIndex);
        } else {
            parameter.type = joinTokenText(significantTokens_, begin, stop);
        }
        parameter.range.entityName = parameter.name;
        return parameter;
    }

    void parseMemberVariables(std::size_t begin, std::size_t end, const std::string& className)
    {
        parseVariablesInRange(begin, end, className, true);
    }

    void parseLocalVariables(std::size_t begin, std::size_t end, const std::string& functionName)
    {
        parseVariablesInRange(begin, end, functionName, false);
    }

    void parseVariablesInRange(std::size_t begin,
                               std::size_t end,
                               const std::string& parentName,
                               bool memberVariables)
    {
        std::size_t statementStart = begin;
        int parenDepth = 0;
        int braceDepth = 0;
        for (std::size_t index = begin; index < end; ++index) {
            const std::string& text = significantTokens_[index].text;
            if (text == "(") {
                ++parenDepth;
            } else if (text == ")" && parenDepth > 0) {
                --parenDepth;
            } else if (text == "{") {
                ++braceDepth;
                statementStart = index + 1;
            } else if (text == "}" && braceDepth > 0) {
                --braceDepth;
                statementStart = index + 1;
            } else if (text == ";" && parenDepth == 0 && braceDepth == 0) {
                auto variable = parseVariableStatement(statementStart, index, parentName, memberVariables);
                if (variable.has_value()) {
                    if (memberVariables) {
                        document_.memberVariables.push_back(std::move(*variable));
                    } else {
                        document_.localVariables.push_back(std::move(*variable));
                    }
                }
                statementStart = index + 1;
            }
        }
    }

    std::optional<ParsedVariable> parseVariableStatement(std::size_t begin,
                                                         std::size_t semicolonIndex,
                                                         const std::string& parentName,
                                                         bool isMember) const
    {
        while (begin < semicolonIndex && isAccessSpecifier(significantTokens_[begin].text)) {
            begin += (begin + 1 < semicolonIndex && significantTokens_[begin + 1].text == ":") ? 2 : 1;
        }

        if (begin >= semicolonIndex) {
            return std::nullopt;
        }

        const std::string& first = significantTokens_[begin].text;
        if (first == "return" || first == "delete" || first == "using" || first == "typedef" || first == "static_assert"
            || first == "if" || first == "for" || first == "while" || first == "switch" || first == "class"
            || first == "struct" || first == "enum") {
            return std::nullopt;
        }

        if (containsTopLevelToken(begin, semicolonIndex, ",")) {
            return std::nullopt;
        }

        std::size_t stop = semicolonIndex;
        for (std::size_t index = begin; index < semicolonIndex; ++index) {
            if (isDeclarationStop(significantTokens_[index].text)) {
                stop = index;
                break;
            }
        }

        if (stop <= begin) {
            return std::nullopt;
        }

        std::optional<std::size_t> nameIndex;
        for (std::size_t index = stop; index > begin; --index) {
            const std::size_t candidate = index - 1;
            if (significantTokens_[candidate].kind == CppTokenKind::Identifier
                && !isKeywordText(significantTokens_[candidate].text)) {
                nameIndex = candidate;
                break;
            }
        }

        if (!nameIndex.has_value() || *nameIndex == begin) {
            return std::nullopt;
        }

        ParsedVariable variable;
        variable.name = significantTokens_[*nameIndex].text;
        variable.type = cleanLeadingAccess(joinTokenText(significantTokens_, begin, *nameIndex));
        variable.parentName = parentName;
        variable.isMember = isMember;
        variable.range = annotateRange(rangeFromTokens(significantTokens_, begin, semicolonIndex),
                                       isMember ? SourceEntityKind::Member : SourceEntityKind::Local,
                                       variable.name);
        variable.nameRange = annotateRange(significantTokens_[*nameIndex].range,
                                           isMember ? SourceEntityKind::Member : SourceEntityKind::Local,
                                           variable.name);
        if (variable.type.empty()) {
            return std::nullopt;
        }
        return variable;
    }

    bool containsTopLevelToken(std::size_t begin, std::size_t end, std::string_view needle) const
    {
        int parenDepth = 0;
        int angleDepth = 0;
        for (std::size_t index = begin; index < end; ++index) {
            const std::string& text = significantTokens_[index].text;
            if (text == "(") {
                ++parenDepth;
            } else if (text == ")" && parenDepth > 0) {
                --parenDepth;
            } else if (text == "<") {
                ++angleDepth;
            } else if (text == ">" && angleDepth > 0) {
                --angleDepth;
            } else if (parenDepth == 0 && angleDepth == 0 && text == needle) {
                return true;
            }
        }
        return false;
    }

    void parseCallExpressions(std::size_t begin, std::size_t end, const std::string& functionName)
    {
        for (std::size_t index = begin + 1; index < end; ++index) {
            if (significantTokens_[index].text != "(") {
                continue;
            }
            const std::size_t calleeIndex = index - 1;
            if (significantTokens_[calleeIndex].kind != CppTokenKind::Identifier
                || isControlKeyword(significantTokens_[calleeIndex].text)) {
                continue;
            }
            const auto closeIt = parenMatches_.find(index);
            if (closeIt == parenMatches_.end() || closeIt->second > end) {
                continue;
            }
            ParsedCallExpression call;
            call.callee = significantTokens_[calleeIndex].text;
            call.parentFunction = functionName;
            call.nameRange = annotateRange(significantTokens_[calleeIndex].range,
                                           SourceEntityKind::Expression,
                                           call.callee);
            call.range = annotateRange(rangeFromTokens(significantTokens_, calleeIndex, closeIt->second),
                                       SourceEntityKind::Expression,
                                       call.callee);
            document_.callExpressions.push_back(std::move(call));
        }
    }

    void addBlockScopes(std::size_t begin, std::size_t end, std::size_t functionScopeIndex)
    {
        for (std::size_t index = begin; index < end; ++index) {
            if (significantTokens_[index].text != "{") {
                continue;
            }
            const auto closeIt = braceMatches_.find(index);
            if (closeIt == braceMatches_.end() || closeIt->second >= end) {
                continue;
            }
            ScopeInfo scope;
            scope.kind = ScopeKind::Block;
            scope.name = "<block>";
            scope.range = annotateRange(rangeFromTokens(significantTokens_, index, closeIt->second),
                                        SourceEntityKind::Scope,
                                        scope.name,
                                        functionScopeIndex);
            scope.parentIndex = functionScopeIndex;
            document_.scopes.push_back(std::move(scope));
        }
    }

    std::string source_;
    ParsedDocument document_;
    std::size_t offset_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
    std::vector<CppToken> significantTokens_;
    std::unordered_map<std::size_t, std::size_t> parenMatches_;
    std::unordered_map<std::size_t, std::size_t> braceMatches_;
};
}

ParsedDocument LightweightCppParser::parse(const std::string& source) const
{
    ParserImpl parser(source);
    return parser.run();
}
