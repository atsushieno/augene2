#include "compiler_internal.hpp"

#include <algorithm>
#include <any>
#include <iomanip>
#include <cctype>
#include <cmath>
#include <sstream>
#include <ostream>
#include <umppi/details/Common.hpp>
#include <umppi/details/Midi1Writer.hpp>
#include <umppi/details/UmpFactory.hpp>

#include "MugeneParser.h"
#include "MugeneParserBaseVisitor.h"
#include "MugeneLexer.h"

namespace mugene2::detail {

namespace {

class TokenSourceAdapter final : public antlr4::TokenSource {
public:
    explicit TokenSourceAdapter(const std::vector<Token>& tokens) : tokens_(tokens) {}

    std::unique_ptr<antlr4::Token> nextToken() override {
        if (position_ >= tokens_.size())
            return makeToken(static_cast<int>(antlr4::Token::EOF), "", 0, 0, 0);

        const auto& token = tokens_[position_++];
        return makeToken(mapTokenType(token.type), tokenText(token), token.location.line, token.location.column,
                         static_cast<int>(position_ - 1));
    }

    size_t getLine() const override {
        return position_ < tokens_.size() ? static_cast<size_t>(tokens_[position_].location.line) : 0;
    }

    size_t getCharPositionInLine() override {
        return position_ < tokens_.size() ? static_cast<size_t>(tokens_[position_].location.column) : 0;
    }

    antlr4::CharStream* getInputStream() override { return nullptr; }
    std::string getSourceName() override {
        return position_ < tokens_.size() ? tokens_[position_].location.source_name : std::string{};
    }
    antlr4::TokenFactory<antlr4::CommonToken>* getTokenFactory() override { return &factory_; }

private:
    static int mapTokenType(TokenType type) {
        switch (type) {
            case TokenType::identifier: return MugeneParser::Identifier;
            case TokenType::string_literal: return MugeneParser::StringLiteral;
            case TokenType::number_literal: return MugeneParser::NumberLiteral;
            case TokenType::period: return MugeneParser::Dot;
            case TokenType::comma: return MugeneParser::Comma;
            case TokenType::percent: return MugeneParser::Percent;
            case TokenType::open_paren: return MugeneParser::OpenParen;
            case TokenType::close_paren: return MugeneParser::CloseParen;
            case TokenType::open_curly: return MugeneParser::OpenCurly;
            case TokenType::close_curly: return MugeneParser::CloseCurly;
            case TokenType::question: return MugeneParser::Question;
            case TokenType::plus: return MugeneParser::Plus;
            case TokenType::minus: return MugeneParser::Minus;
            case TokenType::asterisk: return MugeneParser::Asterisk;
            case TokenType::slash: return MugeneParser::Slash;
            case TokenType::dollar: return MugeneParser::Dollar;
            case TokenType::colon: return MugeneParser::Colon;
            case TokenType::caret: return MugeneParser::Caret;
            case TokenType::backslash_lesser: return MugeneParser::BackSlashLesser;
            case TokenType::backslash_lesser_equal: return MugeneParser::BackSlashLesserEqual;
            case TokenType::backslash_greater: return MugeneParser::BackSlashGreater;
            case TokenType::backslash_greater_equal: return MugeneParser::BackSlashGreaterEqual;
            case TokenType::keyword_number: return MugeneParser::KeywordNumber;
            case TokenType::keyword_length: return MugeneParser::KeywordLength;
            case TokenType::keyword_string: return MugeneParser::KeywordString;
            case TokenType::keyword_buffer: return MugeneParser::KeywordBuffer;
            case TokenType::none:
            default: return 0;
        }
    }

    static std::string tokenText(const Token& token) {
        if (const auto* number = std::get_if<double>(&token.value)) {
            if (std::isfinite(*number) && std::floor(*number) == *number) {
                std::ostringstream stream;
                stream << std::fixed << std::setprecision(0) << *number;
                return stream.str();
            }
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(12) << *number;
            auto text = stream.str();
            auto dot = text.find('.');
            if (dot != std::string::npos) {
                while (!text.empty() && text.back() == '0')
                    text.pop_back();
                if (!text.empty() && text.back() == '.')
                    text.pop_back();
            }
            return text;
        }
        if (const auto* text = std::get_if<std::string>(&token.value))
            return *text;

        switch (token.type) {
            case TokenType::period: return ".";
            case TokenType::comma: return ",";
            case TokenType::percent: return "%";
            case TokenType::open_paren: return "(";
            case TokenType::close_paren: return ")";
            case TokenType::open_curly: return "{";
            case TokenType::close_curly: return "}";
            case TokenType::question: return "?";
            case TokenType::plus: return "+";
            case TokenType::minus: return "-";
            case TokenType::asterisk: return "*";
            case TokenType::slash: return "/";
            case TokenType::dollar: return "$";
            case TokenType::colon: return ":";
            case TokenType::caret: return "^";
            case TokenType::backslash_lesser: return "\\<";
            case TokenType::backslash_lesser_equal: return "\\<=";
            case TokenType::backslash_greater: return "\\>";
            case TokenType::backslash_greater_equal: return "\\>=";
            default: return {};
        }
    }

    std::unique_ptr<antlr4::CommonToken> makeToken(int type,
                                                   std::string text,
                                                   int line,
                                                   int column,
                                                   int index) {
        auto token = std::make_unique<antlr4::CommonToken>(type, std::move(text));
        token->setLine(static_cast<size_t>(std::max(line, 0)));
        token->setCharPositionInLine(static_cast<size_t>(std::max(column, 0)));
        token->setTokenIndex(index);
        return token;
    }

    const std::vector<Token>& tokens_;
    std::size_t position_{0};
    antlr4::CommonTokenFactory factory_{false};
};

class ParserErrorStrategy final : public antlr4::DefaultErrorStrategy {
public:
    void reportNoViableAlternative(antlr4::Parser* recognizer,
                                   const antlr4::NoViableAltException& e) override {
        DefaultErrorStrategy::reportNoViableAlternative(recognizer, e);
    }
};

Length lengthWithDots(int number, int dots, bool is_value_by_step = false) {
    return Length{.number = number, .dots = dots, .is_value_by_step = is_value_by_step};
}

double lengthDotsToMultiplier(int dots) {
    return 2.0 - std::pow(0.5, static_cast<double>(dots));
}

class ParserVisitor final : public MugeneParserBaseVisitor {
public:
    explicit ParserVisitor(const std::vector<Token>& tokens) : tokens_(tokens) {}

    std::any visitTerminal(antlr4::tree::TerminalNode* node) override {
        const auto* symbol = node->getSymbol();
        const auto index = symbol ? symbol->getTokenIndex() : -1;
        if (index >= 0 && static_cast<std::size_t>(index) < tokens_.size())
            return tokens_[static_cast<std::size_t>(index)];
        return MugeneParserBaseVisitor::visitTerminal(node);
    }

    std::any visitExpressionOrOperationUses(MugeneParser::ExpressionOrOperationUsesContext* ctx) override {
        return visit(ctx->children[0]);
    }

    std::any visitExpression(MugeneParser::ExpressionContext* ctx) override {
        return visit(ctx->children[0]);
    }

    std::any visitOperationUses(MugeneParser::OperationUsesContext* ctx) override {
        std::vector<OperationUse> result;
        for (auto* use_ctx : ctx->operationUse()) {
            auto use = std::any_cast<OperationUse>(visitOperationUse(use_ctx));
            result.push_back(std::move(use));
        }
        return result;
    }

    std::any visitOperationUse(MugeneParser::OperationUseContext* ctx) override {
        const auto token = std::any_cast<Token>(visit(ctx->canBeIdentifier()));
        OperationUse use;
        use.name = std::get<std::string>(token.value);
        use.location = token.location;
        if (auto* args_ctx = ctx->argumentsOptCurly()) {
            auto args = std::any_cast<std::vector<OperationUse::Argument>>(visit(args_ctx));
            use.arguments = std::move(args);
        }
        return use;
    }

    std::any visitCanBeIdentifier(MugeneParser::CanBeIdentifierContext* ctx) override {
        return visit(ctx->children[0]);
    }

    std::any visitArgumentsOptCurly(MugeneParser::ArgumentsOptCurlyContext* ctx) override {
        if (auto* args = ctx->arguments())
            return visit(args);
        return std::vector<OperationUse::Argument>{};
    }

    std::any visitArguments(MugeneParser::ArgumentsContext* ctx) override {
        std::vector<OperationUse::Argument> result;
        const bool has_head = ctx->arguments() != nullptr;
        if (auto* head = ctx->arguments())
            result = std::any_cast<std::vector<OperationUse::Argument>>(visitArguments(head));
        if (auto* commas = ctx->commas()) {
            const auto comma_count = std::any_cast<int>(visit(commas));
            const int skipped_count = has_head ? (comma_count - 1) : comma_count;
            for (int i = 0; i < skipped_count; ++i)
                result.push_back(OperationUse::Argument{.skipped = true, .value = {}});
        }
        result.push_back(std::any_cast<OperationUse::Argument>(visitArgument(ctx->argument())));
        return result;
    }

    std::any visitArgument(MugeneParser::ArgumentContext* ctx) override {
        return OperationUse::Argument{
            .skipped = false,
            .value = std::any_cast<ValueExprPtr>(visit(ctx->children[0])),
        };
    }

    std::any visitConditionalExpr(MugeneParser::ConditionalExprContext* ctx) override {
        if (ctx->children.size() == 1)
            return visit(ctx->children[0]);
        auto condition = std::any_cast<ValueExprPtr>(visit(ctx->children[0]));
        auto true_expr = std::any_cast<ValueExprPtr>(visit(ctx->children[2]));
        auto false_expr = std::any_cast<ValueExprPtr>(visit(ctx->children[4]));
        return ValueExprPtr(std::make_shared<ConditionalExpr>(
            condition->location, std::move(condition), std::move(true_expr), std::move(false_expr)));
    }

    std::any visitComparisonExpr(MugeneParser::ComparisonExprContext* ctx) override {
        if (ctx->children.size() == 1)
            return visit(ctx->children[0]);
        auto left = std::any_cast<ValueExprPtr>(visit(ctx->children[0]));
        auto type = std::any_cast<ComparisonType>(visit(ctx->children[1]));
        auto right = std::any_cast<ValueExprPtr>(visit(ctx->children[2]));
        return ValueExprPtr(std::make_shared<ComparisonExpr>(
            left->location, std::move(left), std::move(right), type));
    }

    std::any visitComparisonOperator(MugeneParser::ComparisonOperatorContext* ctx) override {
        if (ctx->BackSlashLesser())
            return ComparisonType::lesser;
        if (ctx->BackSlashLesserEqual())
            return ComparisonType::lesser_equal;
        if (ctx->BackSlashGreater())
            return ComparisonType::greater;
        return ComparisonType::greater_equal;
    }

    std::any visitAddSubExpr(MugeneParser::AddSubExprContext* ctx) override {
        if (ctx->children.size() == 1)
            return visit(ctx->children[0]);
        auto left = std::any_cast<ValueExprPtr>(visit(ctx->children[0]));
        auto right = std::any_cast<ValueExprPtr>(visit(ctx->children[2]));
        if (ctx->Plus() || ctx->Caret())
            return ValueExprPtr(std::make_shared<AddExpr>(left->location, std::move(left), std::move(right)));
        return ValueExprPtr(std::make_shared<SubtractExpr>(left->location, std::move(left), std::move(right)));
    }

    std::any visitMulDivModExpr(MugeneParser::MulDivModExprContext* ctx) override {
        if (ctx->children.size() == 1)
            return visit(ctx->children[0]);
        auto left = std::any_cast<ValueExprPtr>(visit(ctx->children[0]));
        auto right = std::any_cast<ValueExprPtr>(visit(ctx->children[2]));
        if (ctx->Asterisk())
            return ValueExprPtr(std::make_shared<MultiplyExpr>(left->location, std::move(left), std::move(right)));
        if (ctx->Slash())
            return ValueExprPtr(std::make_shared<DivideExpr>(left->location, std::move(left), std::move(right)));
        return ValueExprPtr(std::make_shared<ModuloExpr>(left->location, std::move(left), std::move(right)));
    }

    std::any visitPrimaryExpr(MugeneParser::PrimaryExprContext* ctx) override {
        if (ctx->variableReference() || ctx->stringConstant() || ctx->stepConstant() || ctx->unaryExpr())
            return visit(ctx->children[0]);
        auto content = std::any_cast<ValueExprPtr>(visit(ctx->expression()));
        return ValueExprPtr(std::make_shared<ParenthesizedExpr>(content->location, std::move(content)));
    }

    std::any visitUnaryExpr(MugeneParser::UnaryExprContext* ctx) override {
        if (ctx->Caret()) {
            auto expr = std::any_cast<ValueExprPtr>(visit(ctx->children[1]));
            auto length = ValueExprPtr(std::make_shared<VariableReferenceExpr>(expr->location, "__length"));
            return ValueExprPtr(std::make_shared<AddExpr>(expr->location, std::move(length), std::move(expr)));
        }
        const auto multiplier = ctx->Minus() ? -1.0 : 1.0;
        auto expr = std::any_cast<ValueExprPtr>(visit(ctx->numberOrLengthConstant()));
        auto constant = ValueExprPtr(std::make_shared<ConstantExpr>(
            expr->location, DataType::number, ConstantExpr::Value(multiplier)));
        return ValueExprPtr(std::make_shared<MultiplyExpr>(expr->location, std::move(constant), std::move(expr)));
    }

    std::any visitVariableReference(MugeneParser::VariableReferenceContext* ctx) override {
        const auto token = std::any_cast<Token>(visit(ctx->children[1]));
        return ValueExprPtr(std::make_shared<VariableReferenceExpr>(
            token.location, std::get<std::string>(token.value)));
    }

    std::any visitStringConstant(MugeneParser::StringConstantContext* ctx) override {
        const auto token = std::any_cast<Token>(visit(ctx->children[0]));
        return ValueExprPtr(std::make_shared<ConstantExpr>(
            token.location, DataType::string, ConstantExpr::Value(std::get<std::string>(token.value))));
    }

    std::any visitStepConstant(MugeneParser::StepConstantContext* ctx) override {
        const auto token = std::any_cast<Token>(visit(ctx->NumberLiteral()));
        const auto multiplier = ctx->Minus() ? -1 : 1;
        const auto value = static_cast<int>(std::get<double>(token.value)) * multiplier;
        return ValueExprPtr(std::make_shared<ConstantExpr>(
            token.location, DataType::length, ConstantExpr::Value(lengthWithDots(value, 0, true))));
    }

    std::any visitNumberOrLengthConstant(MugeneParser::NumberOrLengthConstantContext* ctx) override {
        auto* dots_ctx = ctx->dots();
        if (ctx->NumberLiteral()) {
            const auto token = std::any_cast<Token>(visit(ctx->NumberLiteral()));
            if (!dots_ctx) {
                return ValueExprPtr(std::make_shared<ConstantExpr>(
                    token.location, DataType::number, ConstantExpr::Value(std::get<double>(token.value))));
            }
            const auto dot_count = std::any_cast<int>(visit(dots_ctx));
            return ValueExprPtr(std::make_shared<ConstantExpr>(
                token.location,
                DataType::length,
                ConstantExpr::Value(lengthWithDots(static_cast<int>(std::get<double>(token.value)), dot_count))));
        }
        const auto dot_count = dots_ctx ? std::any_cast<int>(visit(dots_ctx)) : 1;
        auto multiplier = ValueExprPtr(std::make_shared<ConstantExpr>(
            LineInfo{}, DataType::number, ConstantExpr::Value(lengthDotsToMultiplier(dot_count))));
        auto length = ValueExprPtr(std::make_shared<VariableReferenceExpr>(LineInfo{}, "__length"));
        return ValueExprPtr(std::make_shared<MultiplyExpr>(LineInfo{}, std::move(multiplier), std::move(length)));
    }

    std::any visitDots(MugeneParser::DotsContext* ctx) override {
        if (!ctx->dots())
            return 1;
        return std::any_cast<int>(visit(ctx->dots())) + 1;
    }

    std::any visitCommas(MugeneParser::CommasContext* ctx) override {
        if (!ctx->commas())
            return 1;
        return std::any_cast<int>(visit(ctx->commas())) + 1;
    }

    std::any defaultResult() override {
        return {};
    }

private:
    const std::vector<Token>& tokens_;
};

std::string locationScopedName(const std::vector<std::string>& include_stack, std::string_view leaf) {
    if (include_stack.empty())
        return std::string(leaf);
    return include_stack.back();
}

std::string trimLeadingWhitespace(std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t'))
        ++pos;
    return std::string(text.substr(pos));
}

std::optional<std::string> parseQuotedString(std::string_view text) {
    if (text.empty() || text.front() != '"')
        return std::nullopt;
    std::string out;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '"')
            return out;
        if (ch == '\\' && i + 1 < text.size()) {
            const char escaped = text[++i];
            switch (escaped) {
                case '/': out.push_back('/'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'r': out.push_back('\r'); break;
                case 'n': out.push_back('\n'); break;
                default: out.push_back(escaped); break;
            }
            continue;
        }
        out.push_back(ch);
    }
    return std::nullopt;
}

bool isWhitespaceChar(char c) {
    return c == ' ' || c == '\t';
}

bool isIdentifierCharSimple(char c, bool start_char, bool escaped_continue = false) {
    if (c == '\r' || c == '\n')
        return false;
    if (isWhitespaceChar(c))
        return false;
    if (std::isdigit(static_cast<unsigned char>(c)))
        return escaped_continue;

    switch (c) {
        case '?':
        case '+':
        case '-':
        case '^':
        case '#':
            return !start_char || escaped_continue;
        case ':':
        case '/':
        case '%':
        case '(':
        case ')':
            return start_char || escaped_continue;
        case '*':
        case '$':
        case ',':
        case '"':
        case '{':
        case '}':
            return escaped_continue;
        default:
            return true;
    }
}

std::string readIdentifierSimple(std::string_view text, std::size_t& pos) {
    std::string out;
    bool start_char = true;
    while (pos < text.size() && isIdentifierCharSimple(text[pos], start_char)) {
        out.push_back(text[pos++]);
        start_char = false;
    }
    return out;
}

std::optional<std::string> longestMatchingIdentifier(std::string_view text,
                                                     std::size_t pos,
                                                     const std::unordered_set<std::string>& known_identifiers) {
    std::optional<std::string> matched;
    for (const auto& identifier : known_identifiers) {
        if (identifier.empty())
            continue;
        if (identifier.size() <= (matched ? matched->size() : 0))
            continue;
        if (pos + identifier.size() > text.size())
            continue;
        if (text.substr(pos, identifier.size()) == identifier)
            matched = identifier;
    }
    return matched;
}

std::string replaceAliases(std::string text, const std::unordered_map<std::string, std::string>& aliases) {
    for (const auto& [key, value] : aliases) {
        std::size_t pos = 0;
        while ((pos = text.find(key, pos)) != std::string::npos) {
            text.replace(pos, key.size(), value);
            pos += value.size();
        }
    }
    return text;
}

std::string joinLogicalLine(const std::vector<std::string>& lines) {
    std::string result;
    for (const auto& line : lines) {
        if (!result.empty())
            result.push_back(' ');
        result += line;
    }
    return result;
}

std::unordered_set<std::string> defaultPrimitiveIdentifiers() {
    return {
        "__PRINT", "__LET", "__LET_PN", "__PER_NOTE", "__PER_NOTE_RESET",
        "__STORE", "__STORE_FORMAT", "__FORMAT", "__APPLY", "__MIDI",
        "__MIDI_NG", "__SYNC_NOFF_WITH_NEXT", "__ON_MIDI_NOTE_OFF",
        "__MIDI_META", "__FLEX_BINARY", "__FLEX_TEXT", "__SAVE_OPER_BEGIN",
        "__SAVE_OPER_END", "__RESTORE_OPER", "__LOOP_BEGIN", "__LOOP_BREAK",
        "__LOOP_END", "[", ":", "/", "]"
    };
}

ValueExprPtr defaultValueForType(const LineInfo& location, DataType type) {
    switch (type) {
        case DataType::number:
        case DataType::length:
            return std::make_shared<ConstantExpr>(location, type, ConstantExpr::Value(0.0));
        case DataType::string:
            return std::make_shared<ConstantExpr>(location, type, ConstantExpr::Value(std::string{}));
        case DataType::buffer:
            return std::make_shared<ConstantExpr>(location, type, ConstantExpr::Value(std::monostate{}));
        case DataType::any:
        default:
            return {};
    }
}

void fillDefaultValue(SemanticVariable& variable) {
    if (!variable.default_value)
        variable.default_value = defaultValueForType(variable.location, variable.type);
}

struct ResolvedEvent {
    std::string operation{};
    int tick{};
    std::vector<uint8_t> arguments{};
};

using ResolvedValue = std::variant<std::monostate, double, std::string, Length>;
struct ResolveContext;

struct LoopLocation {
    int source{};
    int output{};
    int tick{};
};

struct Loop {
    explicit Loop(const ResolveContext& context);

    std::optional<LoopLocation> begin_at{};
    std::optional<LoopLocation> first_break_at{};
    std::unordered_map<int, LoopLocation> breaks{};
    std::vector<ResolvedEvent> events{};
    std::unordered_map<std::string, ResolvedValue> saved_values{};
    std::unordered_map<int, LoopLocation> end_locations{};
    std::vector<int> current_breaks{};
};

struct ResolveContext {
    SemanticTree& source_tree;
    ResolveContext* global_context{};
    std::unordered_map<std::string, ResolvedValue> macro_arguments{};
    std::unordered_map<std::string, ResolvedValue> values{};
    std::unordered_map<std::string, std::vector<ResolvedValue>> values_per_note{};
    int per_note_context{-1};
    int timeline_position{};
    std::vector<Loop> loops{};
};

Loop::Loop(const ResolveContext& context)
    : saved_values(context.values) {}

int lengthToSteps(const Length& length, int base_count) {
    if (length.is_value_by_step)
        return length.number;
    if (length.number == 0)
        return 0;
    int basis = base_count / length.number;
    int result = basis;
    for (int i = 0; i < length.dots; ++i) {
        basis /= 2;
        result += basis;
    }
    return result;
}

std::string resolvedValueToString(const ResolvedValue& value) {
    if (const auto* text = std::get_if<std::string>(&value))
        return *text;
    if (const auto* number = std::get_if<double>(&value)) {
        std::ostringstream stream;
        if (std::isfinite(*number) && std::floor(*number) == *number)
            stream << static_cast<long long>(*number);
        else
            stream << *number;
        return stream.str();
    }
    if (const auto* length = std::get_if<Length>(&value)) {
        std::ostringstream stream;
        if (length->is_value_by_step)
            stream << '%';
        stream << length->number;
        for (int i = 0; i < length->dots; ++i)
            stream << '.';
        return stream.str();
    }
    return {};
}

ResolvedValue convertResolvedValue(const ResolvedValue& value,
                                   DataType type,
                                   const LineInfo& location,
                                   int base_count,
                                   DiagnosticSink& diagnostics) {
    switch (type) {
        case DataType::any:
            return value;
        case DataType::string:
            return resolvedValueToString(value);
        case DataType::number:
            if (const auto* number = std::get_if<double>(&value))
                return *number;
            if (const auto* length = std::get_if<Length>(&value))
                return static_cast<double>(lengthToSteps(*length, base_count));
            if (std::holds_alternative<std::monostate>(value))
                return 0.0;
            diagnostics.error(location, "Cannot convert value to number.");
            return 0.0;
        case DataType::length:
            if (const auto* length = std::get_if<Length>(&value))
                return *length;
            if (const auto* number = std::get_if<double>(&value))
                return Length{.number = static_cast<int>(*number), .dots = 0, .is_value_by_step = false};
            if (std::holds_alternative<std::monostate>(value))
                return Length{.number = 0, .dots = 0, .is_value_by_step = false};
            diagnostics.error(location, "Cannot convert value to length.");
            return Length{.number = 0, .dots = 0, .is_value_by_step = false};
        case DataType::buffer:
            if (std::holds_alternative<std::monostate>(value))
                return std::monostate{};
            diagnostics.error(location, "Invalid value for buffer.");
            return std::monostate{};
    }
    return {};
}

double resolvedNumberValue(const ResolvedValue& value,
                           const LineInfo& location,
                           int base_count,
                           DiagnosticSink& diagnostics) {
    auto converted = convertResolvedValue(value, DataType::number, location, base_count, diagnostics);
    if (const auto* number = std::get_if<double>(&converted))
        return *number;
    return 0.0;
}

ResolvedValue evaluateValueExpr(const ValueExpr& expr,
                                ResolveContext& context,
                                DataType expected_type,
                                DiagnosticSink& diagnostics);

ResolvedValue ensureDefaultResolvedVariable(SemanticVariable& variable,
                                            ResolveContext& context,
                                            DiagnosticSink& diagnostics) {
    if (context.per_note_context >= 0) {
        auto per_note_it = context.values_per_note.find(variable.name);
        if (per_note_it != context.values_per_note.end()) {
            auto& slot = per_note_it->second[static_cast<std::size_t>(context.per_note_context)];
            if (!std::holds_alternative<std::monostate>(slot))
                return slot;
            fillDefaultValue(variable);
            if (!variable.default_value)
                return {};
            auto value = evaluateValueExpr(*variable.default_value, context, variable.type, diagnostics);
            slot = value;
            return value;
        }
    }

    auto it = context.values.find(variable.name);
    if (it != context.values.end())
        return it->second;

    fillDefaultValue(variable);
    if (!variable.default_value)
        return {};

    auto value = evaluateValueExpr(*variable.default_value, context, variable.type, diagnostics);
    context.values[variable.name] = value;
    return value;
}

ResolvedValue evaluateArithmetic(const ArithmeticExpr& expr,
                                 ResolveContext& context,
                                 DataType expected_type,
                                 DiagnosticSink& diagnostics,
                                 char operation) {
    auto left = evaluateValueExpr(*expr.left, context,
                                  operation == '*' ? DataType::number : expected_type,
                                  diagnostics);
    auto right = evaluateValueExpr(*expr.right, context, expected_type, diagnostics);

    if (expected_type == DataType::length) {
        const auto left_number = resolvedNumberValue(left, expr.location, context.source_tree.base_count, diagnostics);
        const auto right_number = resolvedNumberValue(right, expr.location, context.source_tree.base_count, diagnostics);
        int result = 0;
        switch (operation) {
            case '+': result = static_cast<int>(left_number + right_number); break;
            case '-': result = static_cast<int>(left_number - right_number); break;
            case '*': result = static_cast<int>(left_number * right_number); break;
            case '/': result = static_cast<int>(left_number / right_number); break;
            case '%': result = static_cast<int>(std::fmod(left_number, right_number)); break;
        }
        return Length{.number = result, .dots = 0, .is_value_by_step = true};
    }

    if (operation == '+' &&
        (std::holds_alternative<std::string>(left) || std::holds_alternative<std::string>(right))) {
        return resolvedValueToString(left) + resolvedValueToString(right);
    }

    const auto left_number = resolvedNumberValue(left, expr.location, context.source_tree.base_count, diagnostics);
    const auto right_number = resolvedNumberValue(right, expr.location, context.source_tree.base_count, diagnostics);
    switch (operation) {
        case '+': return left_number + right_number;
        case '-': return left_number - right_number;
        case '*': return left_number * right_number;
        case '/': return left_number / right_number;
        case '%': return std::fmod(left_number, right_number);
    }
    return 0.0;
}

ResolvedValue evaluateValueExpr(const ValueExpr& expr,
                                ResolveContext& context,
                                DataType expected_type,
                                DiagnosticSink& diagnostics) {
    if (const auto* constant = dynamic_cast<const ConstantExpr*>(&expr))
        return convertResolvedValue(constant->value, expected_type, constant->location,
                                    context.source_tree.base_count, diagnostics);

    if (const auto* variable = dynamic_cast<const VariableReferenceExpr*>(&expr)) {
        if (variable->scope == 3 && context.global_context)
            return evaluateValueExpr(expr, *context.global_context, expected_type, diagnostics);

        auto arg = context.macro_arguments.find(variable->name);
        if (variable->scope <= 1 && arg != context.macro_arguments.end())
            return convertResolvedValue(arg->second, expected_type, variable->location,
                                        context.source_tree.base_count, diagnostics);

        auto it = context.source_tree.variables.find(variable->name);
        if (it == context.source_tree.variables.end()) {
            diagnostics.error(variable->location, "Cannot resolve variable '" + variable->name + "'");
            return {};
        }
        return convertResolvedValue(ensureDefaultResolvedVariable(it->second, context, diagnostics),
                                    expected_type, variable->location, context.source_tree.base_count, diagnostics);
    }

    if (const auto* parenthesized = dynamic_cast<const ParenthesizedExpr*>(&expr))
        return evaluateValueExpr(*parenthesized->content, context, expected_type, diagnostics);
    if (const auto* add = dynamic_cast<const AddExpr*>(&expr))
        return evaluateArithmetic(*add, context, expected_type, diagnostics, '+');
    if (const auto* sub = dynamic_cast<const SubtractExpr*>(&expr))
        return evaluateArithmetic(*sub, context, expected_type, diagnostics, '-');
    if (const auto* mul = dynamic_cast<const MultiplyExpr*>(&expr))
        return evaluateArithmetic(*mul, context, expected_type, diagnostics, '*');
    if (const auto* div = dynamic_cast<const DivideExpr*>(&expr))
        return evaluateArithmetic(*div, context, expected_type, diagnostics, '/');
    if (const auto* mod = dynamic_cast<const ModuloExpr*>(&expr))
        return evaluateArithmetic(*mod, context, expected_type, diagnostics, '%');
    if (const auto* conditional = dynamic_cast<const ConditionalExpr*>(&expr)) {
        auto condition = evaluateValueExpr(*conditional->condition, context, DataType::number, diagnostics);
        if (resolvedNumberValue(condition, conditional->location, context.source_tree.base_count, diagnostics) != 0.0)
            return evaluateValueExpr(*conditional->true_expr, context, expected_type, diagnostics);
        return evaluateValueExpr(*conditional->false_expr, context, expected_type, diagnostics);
    }
    if (const auto* comparison = dynamic_cast<const ComparisonExpr*>(&expr)) {
        if (expected_type == DataType::string) {
            auto left = evaluateValueExpr(*comparison->left, context, expected_type, diagnostics);
            auto right = evaluateValueExpr(*comparison->right, context, expected_type, diagnostics);
            const auto l = resolvedValueToString(left);
            const auto r = resolvedValueToString(right);
            bool result = false;
            switch (comparison->comparison_type) {
                case ComparisonType::lesser: result = l < r; break;
                case ComparisonType::lesser_equal: result = l <= r; break;
                case ComparisonType::greater: result = l > r; break;
                case ComparisonType::greater_equal: result = l >= r; break;
            }
            return result ? 1.0 : 0.0;
        }
        auto left = evaluateValueExpr(*comparison->left, context, expected_type, diagnostics);
        auto right = evaluateValueExpr(*comparison->right, context, expected_type, diagnostics);
        const auto l = resolvedNumberValue(left, comparison->location, context.source_tree.base_count, diagnostics);
        const auto r = resolvedNumberValue(right, comparison->location, context.source_tree.base_count, diagnostics);
        bool result = false;
        switch (comparison->comparison_type) {
            case ComparisonType::lesser: result = l < r; break;
            case ComparisonType::lesser_equal: result = l <= r; break;
            case ComparisonType::greater: result = l > r; break;
            case ComparisonType::greater_equal: result = l >= r; break;
        }
        return result ? 1.0 : 0.0;
    }

    diagnostics.error(expr.location, "Unsupported expression node.");
    return {};
}

std::vector<Token> tokenizeInlineText(std::string_view text,
                                      const LineInfo& base_location,
                                      const std::unordered_set<std::string>& known_identifiers,
                                      bool new_identifier_mode,
                                      DiagnosticSink& diagnostics) {
    auto read_number = [&](std::size_t& pos, const LineInfo& location) -> std::optional<double> {
        if (pos >= text.size())
            return std::nullopt;
        if (text[pos] == '#') {
            ++pos;
            std::string hex;
            while (pos < text.size() && std::isxdigit(static_cast<unsigned char>(text[pos])))
                hex.push_back(text[pos++]);
            if (hex.empty()) {
                diagnostics.error(location, "Invalid hexadecimal digits.");
                return std::nullopt;
            }
            return static_cast<double>(std::stoll(hex, nullptr, 16));
        }
        std::size_t start = pos;
        bool seen_dot = false;
        while (pos < text.size()) {
            const char ch = text[pos];
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                ++pos;
                continue;
            }
            if (!seen_dot && ch == '.' && pos + 1 < text.size() &&
                std::isdigit(static_cast<unsigned char>(text[pos + 1]))) {
                seen_dot = true;
                ++pos;
                continue;
            }
            break;
        }
        return std::stod(std::string(text.substr(start, pos - start)));
    };

    auto read_string = [&](std::size_t& pos, const LineInfo& location) -> std::optional<std::string> {
        if (pos >= text.size() || text[pos] != '"')
            return std::nullopt;
        ++pos;
        std::string out;
        while (pos < text.size()) {
            const char ch = text[pos++];
            if (ch == '"')
                return out;
            if (ch == '\\') {
                if (pos >= text.size()) {
                    diagnostics.error(location, "Incomplete string literal.");
                    return std::nullopt;
                }
                const char escaped = text[pos++];
                switch (escaped) {
                    case '/': out.push_back('/'); break;
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'n': out.push_back('\n'); break;
                    default:
                        diagnostics.error(location, std::string("Unexpected string escape sequence: \\") + escaped);
                        return std::nullopt;
                }
                continue;
            }
            out.push_back(ch);
        }
        diagnostics.error(location, "Incomplete string literal.");
        return std::nullopt;
    };

    std::vector<Token> tokens;
    std::size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && isWhitespaceChar(text[pos]))
            ++pos;
        if (pos >= text.size())
            break;

        LineInfo location{base_location.source_name, base_location.line, base_location.column + static_cast<int>(pos)};
        const char ch = text[pos];
        auto push_simple = [&](TokenType type, std::string lexeme = {}) {
            Token token;
            token.type = type;
            token.location = location;
            if (!lexeme.empty())
                token.value = std::move(lexeme);
            tokens.push_back(std::move(token));
            ++pos;
        };

        switch (ch) {
            case '.': push_simple(TokenType::period); continue;
            case ',': push_simple(TokenType::comma, ","); continue;
            case '%': push_simple(TokenType::percent, "%"); continue;
            case '{': push_simple(TokenType::open_curly); continue;
            case '}': push_simple(TokenType::close_curly); continue;
            case '?': push_simple(TokenType::question); continue;
            case '^': push_simple(TokenType::caret); continue;
            case '+': push_simple(TokenType::plus); continue;
            case '-': push_simple(TokenType::minus); continue;
            case '*': push_simple(TokenType::asterisk); continue;
            case '/': push_simple(TokenType::slash, "/"); continue;
            case '$': push_simple(TokenType::dollar); continue;
            case ':': push_simple(TokenType::colon, ":"); continue;
            case '=': push_simple(TokenType::identifier, "="); continue;
            case '\\':
                ++pos;
                if (pos >= text.size()) {
                    diagnostics.error(location, "Unexpected end of stream in the middle of escaped token.");
                    return {};
                }
                if (text[pos] == '<') {
                    ++pos;
                    if (pos < text.size() && text[pos] == '=') {
                        ++pos;
                        tokens.push_back(Token{TokenType::backslash_lesser_equal, {}, location});
                    } else {
                        tokens.push_back(Token{TokenType::backslash_lesser, {}, location});
                    }
                    continue;
                }
                if (text[pos] == '>') {
                    ++pos;
                    if (pos < text.size() && text[pos] == '=') {
                        ++pos;
                        tokens.push_back(Token{TokenType::backslash_greater_equal, {}, location});
                    } else {
                        tokens.push_back(Token{TokenType::backslash_greater, {}, location});
                    }
                    continue;
                }
                diagnostics.error(location, std::string("Unexpected escaped token: '\\") + text[pos] + "'");
                return {};
            case '"': {
                auto text_value = read_string(pos, location);
                if (!text_value)
                    return {};
                tokens.push_back(Token{TokenType::string_literal, *text_value, location});
                continue;
            }
            default: break;
        }

        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '#') {
            auto number = read_number(pos, location);
            if (!number)
                return {};
            tokens.push_back(Token{TokenType::number_literal, *number, location});
            continue;
        }

        if (isIdentifierCharSimple(ch, true)) {
            if (text.substr(pos).starts_with("number")) {
                tokens.push_back(Token{TokenType::keyword_number, std::string("number"), location});
                pos += 6;
                continue;
            }
            if (text.substr(pos).starts_with("length")) {
                tokens.push_back(Token{TokenType::keyword_length, std::string("length"), location});
                pos += 6;
                continue;
            }
            if (text.substr(pos).starts_with("string")) {
                tokens.push_back(Token{TokenType::keyword_string, std::string("string"), location});
                pos += 6;
                continue;
            }
            if (text.substr(pos).starts_with("buffer")) {
                tokens.push_back(Token{TokenType::keyword_buffer, std::string("buffer"), location});
                pos += 6;
                continue;
            }
            std::optional<std::string> ident;
            if (!new_identifier_mode)
                ident = longestMatchingIdentifier(text, pos, known_identifiers);
            if (ident)
                pos += ident->size();
            else
                ident = readIdentifierSimple(text, pos);
            if (ident->empty()) {
                diagnostics.error(location, std::string("The lexer could not read a valid identifier token: '") + ch + "'");
                return {};
            }
            Token token;
            token.location = location;
            token.value = *ident;
            if (*ident == "number")
                token.type = TokenType::keyword_number;
            else if (*ident == "length")
                token.type = TokenType::keyword_length;
            else if (*ident == "string")
                token.type = TokenType::keyword_string;
            else if (*ident == "buffer")
                token.type = TokenType::keyword_buffer;
            else
                token.type = TokenType::identifier;
            tokens.push_back(std::move(token));
            continue;
        }

        diagnostics.error(location, std::string("The lexer could not read a valid token: '") + ch + "'");
        return {};
    }
    return tokens;
}

} // namespace

void DiagnosticSink::error(const LineInfo& location, std::string message) {
    add(DiagnosticSeverity::error, location, std::move(message));
}

void DiagnosticSink::warning(const LineInfo& location, std::string message) {
    add(DiagnosticSeverity::warning, location, std::move(message));
}

void DiagnosticSink::information(const LineInfo& location, std::string message) {
    add(DiagnosticSeverity::information, location, std::move(message));
}

void DiagnosticSink::merge(const std::vector<Diagnostic>& diagnostics) {
    diagnostics_.insert(diagnostics_.end(), diagnostics.begin(), diagnostics.end());
}

bool DiagnosticSink::hasErrors() const {
    return std::any_of(diagnostics_.begin(), diagnostics_.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::error;
    });
}

void DiagnosticSink::add(DiagnosticSeverity severity, const LineInfo& location, std::string message) {
    diagnostics_.push_back(Diagnostic{
        .severity = severity,
        .source_name = location.source_name,
        .line = location.line,
        .column = location.column,
        .message = std::move(message),
    });
}

FrontEnd::FrontEnd(DiagnosticSink& diagnostics,
                   std::span<const SourceText> sources,
                   IncludeResolver resolver)
    : diagnostics_(diagnostics), sources_(sources), resolver_(std::move(resolver)) {
    registerPrimitiveIdentifiers();
}

bool FrontEnd::process() {
    for (const auto& source : sources_) {
        if (!processSource(source))
            return false;
    }

    for (auto& raw_track : raw_tracks_) {
        if (!tokenizeTrack(raw_track))
            continue;
    }

    return !diagnostics_.hasErrors();
}

bool FrontEnd::processSource(const SourceText& source) {
    if (std::find(include_stack_.begin(), include_stack_.end(), source.name) != include_stack_.end()) {
        diagnostics_.error(LineInfo{source.name, 0, 0}, "Recursive inclusion is prohibited.");
        return false;
    }

    include_stack_.push_back(source.name);
    bool continued = false;
    RawTrackLine* continued_track = nullptr;
    std::vector<std::string>* continued_lines = nullptr;
    int line_number = 0;

    std::istringstream stream(source.text);
    std::string physical_line;
    while (std::getline(stream, physical_line)) {
        ++line_number;
        auto trimmed = trimComments(physical_line);
        while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' ' || trimmed.back() == '\t'))
            trimmed.pop_back();
        if (trimmed.empty())
            continue;

        bool line_continues = !trimmed.empty() && trimmed.back() == '\\';
        if (line_continues)
            trimmed.pop_back();

        if (continued) {
            if (!in_comment_mode_) {
                if (continued_track != nullptr)
                    continued_track->physical_lines.push_back(trimmed);
                else if (continued_lines != nullptr)
                    continued_lines->push_back(trimmed);
            }
            continued = line_continues;
            continue;
        }

        LineInfo location{source.name, line_number, 0};
        if (!trimmed.empty() && trimmed.front() == '#') {
            if (!processPragma(trimmed, location, &continued_lines))
                return false;
            continued_track = nullptr;
            if (continued_lines == nullptr)
                continued_track = nullptr;
        } else {
            if (!processTrackLine(trimmed, location, &continued_track))
                return false;
            continued_lines = nullptr;
        }

        continued = line_continues;
    }

    if (continued) {
        diagnostics_.error(LineInfo{source.name, line_number, 0},
                           "Unexpected end of consecutive line by '\\' at the end of file.");
        include_stack_.pop_back();
        return false;
    }

    include_stack_.pop_back();
    return true;
}

bool FrontEnd::processPragma(const std::string& pragma_line,
                             const LineInfo& location,
                             std::vector<std::string>** continued_lines) {
    *continued_lines = nullptr;
    std::size_t pos = 1;
    while (pos < pragma_line.size() && isWhitespace(pragma_line[pos]))
        ++pos;

    std::size_t start = pos;
    while (pos < pragma_line.size() && std::isalpha(static_cast<unsigned char>(pragma_line[pos])))
        ++pos;

    const auto pragma = pragma_line.substr(start, pos - start);
    if (pragma.empty()) {
        diagnostics_.error(location, "Unexpected empty preprocessor directive.");
        return false;
    }

    while (pos < pragma_line.size() && isWhitespace(pragma_line[pos]))
        ++pos;
    const auto rest = pragma_line.substr(pos);

    if (pragma == "include") {
        if (!resolver_) {
            diagnostics_.error(location, "#include requires an include resolver.");
            return false;
        }
        auto resolved = resolver_(locationScopedName(include_stack_, location.source_name), rest);
        if (!resolved) {
            diagnostics_.error(location, "Included source could not be resolved: " + rest);
            return false;
        }
        return processSource(*resolved);
    }

    if (pragma == "comment") {
        in_comment_mode_ = true;
        return true;
    }

    if (pragma == "endcomment") {
        in_comment_mode_ = false;
        return true;
    }

    if (pragma == "basecount" || pragma == "conditional" || pragma == "meta" ||
        pragma == "define") {
        PragmaSource source;
        source.name = pragma;
        source.lines.push_back(rest);
        source.first_location = location;
        pragmas_.push_back(std::move(source));
        *continued_lines = &pragmas_.back().lines;
        return true;
    }

    if (pragma == "variable") {
        VariableSource source;
        source.lines.push_back(rest);
        source.first_location = location;
        std::size_t name_pos = 0;
        while (name_pos < rest.size()) {
            while (name_pos < rest.size() && isWhitespace(rest[name_pos]))
                ++name_pos;
            if (name_pos >= rest.size())
                break;
            auto identifier = readIdentifier(rest, name_pos);
            if (identifier.empty()) {
                diagnostics_.error(location, "Invalid variable declaration.");
                return false;
            }
            registerIdentifier(identifier);
            source.parsed_names.push_back(identifier);
            while (name_pos < rest.size() && rest[name_pos] != ',')
                ++name_pos;
            if (name_pos < rest.size() && rest[name_pos] == ',')
                ++name_pos;
        }
        variables_.push_back(std::move(source));
        *continued_lines = &variables_.back().lines;
        return true;
    }

    if (pragma == "macro") {
        MacroSource source;
        source.lines.push_back(rest);
        source.first_location = location;
        std::size_t macro_pos = 0;
        while (macro_pos < rest.size() && isWhitespace(rest[macro_pos]))
            ++macro_pos;
        if (macro_pos < rest.size() && (std::isdigit(static_cast<unsigned char>(rest[macro_pos])) || rest[macro_pos] == '#')) {
            auto range = parseRange(rest, macro_pos, diagnostics_, location);
            if (!range)
                return false;
            while (macro_pos < rest.size() && isWhitespace(rest[macro_pos]))
                ++macro_pos;
        }
        auto identifier = readIdentifier(rest, macro_pos);
        if (identifier.empty()) {
            diagnostics_.error(location, "Invalid macro definition.");
            return false;
        }
        registerIdentifier(identifier);
        source.parsed_name = identifier;
        macros_.push_back(std::move(source));
        *continued_lines = &macros_.back().lines;
        return true;
    }

    diagnostics_.error(location, "Unexpected preprocessor directive: " + pragma);
    return false;
}

bool FrontEnd::processTrackLine(const std::string& text, const LineInfo& location, RawTrackLine** continued_track) {
    *continued_track = nullptr;
    if (in_comment_mode_)
        return true;

    std::size_t pos = 0;
    std::string block_name = previous_block_name_;
    auto track_numbers = previous_track_numbers_;

    if (!text.empty() && isWhitespace(text.front())) {
        while (pos < text.size() && isWhitespace(text[pos]))
            ++pos;
        if (!raw_tracks_.empty()) {
            raw_tracks_.back().physical_lines.push_back(text.substr(pos));
            *continued_track = &raw_tracks_.back();
            return true;
        }
    } else {
        if (pos < text.size() && isIdentifierChar(text[pos], true)) {
            std::size_t start = pos;
            while (pos < text.size() && isIdentifierChar(text[pos], pos == start))
                ++pos;
            block_name = text.substr(start, pos - start);
            while (pos < text.size() && isWhitespace(text[pos]))
                ++pos;
        }

        if (pos < text.size() && (std::isdigit(static_cast<unsigned char>(text[pos])) || text[pos] == '#')) {
            track_numbers = parseRange(text, pos, diagnostics_, location);
            if (track_numbers) {
                while (pos < text.size() && isWhitespace(text[pos]))
                    ++pos;
            }
        }
    }

    if (!track_numbers || track_numbers->empty()) {
        diagnostics_.error(location,
                           "Current line indicates no track number, and there was no indicated tracks previously.");
        return false;
    }

    previous_block_name_ = block_name;
    previous_track_numbers_ = track_numbers;

    if (!raw_tracks_.empty() &&
        raw_tracks_.back().block_name == block_name &&
        raw_tracks_.back().track_numbers == *track_numbers) {
        raw_tracks_.back().physical_lines.push_back(text.substr(pos));
        *continued_track = &raw_tracks_.back();
        return true;
    }

    RawTrackLine track;
    track.block_name = block_name;
    track.track_numbers = *track_numbers;
    track.physical_lines.push_back(text.substr(pos));
    track.first_location = LineInfo{location.source_name, location.line, static_cast<int>(pos)};
    raw_tracks_.push_back(std::move(track));
    *continued_track = &raw_tracks_.back();
    return true;
}

bool FrontEnd::tokenizeTrack(RawTrackLine& track) {
    auto read_number = [&](std::string_view text, std::size_t& pos, const LineInfo& base_location) -> std::optional<double> {
        if (pos >= text.size())
            return std::nullopt;
        if (text[pos] == '#') {
            ++pos;
            std::string hex;
            while (pos < text.size() && std::isxdigit(static_cast<unsigned char>(text[pos])))
                hex.push_back(text[pos++]);
            if (hex.empty()) {
                diagnostics_.error(base_location, "Invalid hexadecimal digits.");
                return std::nullopt;
            }
            return static_cast<double>(std::stoll(hex, nullptr, 16));
        }

        std::size_t start = pos;
        bool seen_dot = false;
        while (pos < text.size()) {
            const char ch = text[pos];
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                ++pos;
                continue;
            }
            if (!seen_dot && ch == '.' && pos + 1 < text.size() &&
                std::isdigit(static_cast<unsigned char>(text[pos + 1]))) {
                seen_dot = true;
                ++pos;
                continue;
            }
            break;
        }
        return std::stod(std::string(text.substr(start, pos - start)));
    };

    auto read_string = [&](std::string_view text, std::size_t& pos, const LineInfo& base_location) -> std::optional<std::string> {
        if (pos >= text.size() || text[pos] != '"')
            return std::nullopt;
        ++pos;
        std::string out;
        while (pos < text.size()) {
            const char ch = text[pos++];
            if (ch == '"')
                return out;
            if (ch == '\\') {
                if (pos >= text.size()) {
                    diagnostics_.error(base_location, "Incomplete string literal.");
                    return std::nullopt;
                }
                const char escaped = text[pos++];
                switch (escaped) {
                    case '/': out.push_back('/'); break;
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'n': out.push_back('\n'); break;
                    default:
                        diagnostics_.error(base_location, std::string("Unexpected string escape sequence: \\") + escaped);
                        return std::nullopt;
                }
                continue;
            }
            out.push_back(ch);
        }
        diagnostics_.error(base_location, "Incomplete string literal.");
        return std::nullopt;
    };

    TrackSource result_track;
    result_track.track_id = next_track_id_++;
    result_track.block_name = track.block_name;
    result_track.track_numbers = track.track_numbers;

    for (std::size_t line_index = 0; line_index < track.physical_lines.size(); ++line_index) {
        const auto& line = track.physical_lines[line_index];
        std::size_t pos = 0;
        while (pos < line.size()) {
            while (pos < line.size() && isWhitespace(line[pos]))
                ++pos;
            if (pos >= line.size())
                break;

            LineInfo location{track.first_location.source_name,
                              track.first_location.line + static_cast<int>(line_index),
                              static_cast<int>(pos)};
            const char ch = line[pos];

            auto push_simple = [&](TokenType type, std::string lexeme = {}) {
                Token token;
                token.type = type;
                token.location = location;
                if (!lexeme.empty())
                    token.value = std::move(lexeme);
                result_track.tokens.push_back(std::move(token));
                ++pos;
            };

            switch (ch) {
                case '.': push_simple(TokenType::period); continue;
                case ',': push_simple(TokenType::comma, ","); continue;
                case '%': push_simple(TokenType::percent, "%"); continue;
                case '{': push_simple(TokenType::open_curly); continue;
                case '}': push_simple(TokenType::close_curly); continue;
                case '?': push_simple(TokenType::question); continue;
                case '^': push_simple(TokenType::caret); continue;
                case '+': push_simple(TokenType::plus); continue;
                case '-': push_simple(TokenType::minus); continue;
                case '*': push_simple(TokenType::asterisk); continue;
                case '/': push_simple(TokenType::slash, "/"); continue;
                case '$': push_simple(TokenType::dollar); continue;
                case ':': push_simple(TokenType::colon, ":"); continue;
                case '\\':
                    ++pos;
                    if (pos >= line.size()) {
                        diagnostics_.error(location, "Unexpected end of stream in the middle of escaped token.");
                        return false;
                    }
                    if (line[pos] == '<') {
                        ++pos;
                        if (pos < line.size() && line[pos] == '=') {
                            ++pos;
                            result_track.tokens.push_back(Token{TokenType::backslash_lesser_equal, {}, location});
                        } else {
                            result_track.tokens.push_back(Token{TokenType::backslash_lesser, {}, location});
                        }
                        continue;
                    }
                    if (line[pos] == '>') {
                        ++pos;
                        if (pos < line.size() && line[pos] == '=') {
                            ++pos;
                            result_track.tokens.push_back(Token{TokenType::backslash_greater_equal, {}, location});
                        } else {
                            result_track.tokens.push_back(Token{TokenType::backslash_greater, {}, location});
                        }
                        continue;
                    }
                    diagnostics_.error(location, std::string("Unexpected escaped token: '\\") + line[pos] + "'");
                    return false;
                case '"': {
                    auto text_value = read_string(line, pos, location);
                    if (!text_value)
                        return false;
                    result_track.tokens.push_back(Token{TokenType::string_literal, *text_value, location});
                    continue;
                }
                default: break;
            }

            if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '#') {
                auto number = read_number(line, pos, location);
                if (!number)
                    return false;
                result_track.tokens.push_back(Token{TokenType::number_literal, *number, location});
                continue;
            }

            if (isIdentifierChar(ch, true)) {
                auto ident = longestMatchingIdentifier(line, pos);
                if (ident) {
                    pos += ident->size();
                } else {
                    ident = readIdentifier(line, pos);
                }
                if (ident->empty()) {
                    diagnostics_.error(location, std::string("The lexer could not read a valid identifier token: '") + ch + "'");
                    return false;
                }
                Token token;
                token.location = location;
                token.value = *ident;
                if (*ident == "number")
                    token.type = TokenType::keyword_number;
                else if (*ident == "length")
                    token.type = TokenType::keyword_length;
                else if (*ident == "string")
                    token.type = TokenType::keyword_string;
                else if (*ident == "buffer")
                    token.type = TokenType::keyword_buffer;
                else
                    token.type = TokenType::identifier;
                result_track.tokens.push_back(std::move(token));
                continue;
            }

            diagnostics_.error(location, std::string("The lexer could not read a valid token: '") + ch + "'");
            return false;
        }
    }

    tracks_.push_back(std::move(result_track));
    return true;
}

std::string FrontEnd::trimComments(const std::string& text, std::size_t start) {
    const auto idx2 = text.find("//", start);
    if (idx2 == std::string::npos)
        return text;
    const auto idx1 = text.find('"', start);
    if (idx1 == std::string::npos || idx2 < idx1)
        return text.substr(0, idx2);
    const auto idx3 = text.find('"', idx1 + 1);
    if (idx3 == std::string::npos)
        return text.substr(0, idx2);
    if (idx3 > idx2)
        return trimComments(text, idx3 + 1);
    return trimComments(text, idx3 + 1);
}

bool FrontEnd::isWhitespace(char c) {
    return c == ' ' || c == '\t';
}

bool FrontEnd::isIdentifierChar(char c, bool start_char, bool escaped_continue) {
    if (c == '\r' || c == '\n')
        return false;
    if (isWhitespace(c))
        return false;
    if (std::isdigit(static_cast<unsigned char>(c)))
        return escaped_continue;

    switch (c) {
        case '?':
        case '+':
        case '-':
        case '^':
        case '#':
            return !start_char || escaped_continue;
        case ':':
        case '/':
        case '%':
        case '(':
        case ')':
            return start_char || escaped_continue;
        case '*':
        case '$':
        case ',':
        case '"':
        case '{':
        case '}':
            return escaped_continue;
        default:
            return true;
    }
}

std::optional<std::vector<double>> FrontEnd::parseRange(std::string_view text,
                                                        std::size_t& pos,
                                                        DiagnosticSink& diagnostics,
                                                        const LineInfo& location) {
    auto read_number = [&](std::size_t& number_pos) -> std::optional<double> {
        if (number_pos >= text.size())
            return std::nullopt;
        if (text[number_pos] == '#') {
            ++number_pos;
            std::string hex;
            while (number_pos < text.size() && std::isxdigit(static_cast<unsigned char>(text[number_pos])))
                hex.push_back(text[number_pos++]);
            if (hex.empty()) {
                diagnostics.error(location, "Invalid hexadecimal digits.");
                return std::nullopt;
            }
            return static_cast<double>(std::stoll(hex, nullptr, 16));
        }

        std::size_t start = number_pos;
        bool seen_dot = false;
        while (number_pos < text.size()) {
            const char ch = text[number_pos];
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                ++number_pos;
                continue;
            }
            if (!seen_dot && ch == '.' && number_pos + 1 < text.size() &&
                std::isdigit(static_cast<unsigned char>(text[number_pos + 1]))) {
                seen_dot = true;
                ++number_pos;
                continue;
            }
            break;
        }
        return std::stod(std::string(text.substr(start, number_pos - start)));
    };

    std::vector<double> results;
    while (true) {
        auto start = read_number(pos);
        if (!start)
            return std::nullopt;

        if (pos < text.size() && text[pos] == '-') {
            ++pos;
            auto end = read_number(pos);
            if (!end)
                return std::nullopt;
            if (*end < *start) {
                diagnostics.error(location, "Invalid range specification: larger number must appear later.");
                return std::nullopt;
            }
            for (double value = *start; value <= *end; value += 1.0)
                results.push_back(value);
        } else {
            results.push_back(*start);
        }

        if (pos >= text.size() || text[pos] != ',')
            break;
        ++pos;
    }

    return results;
}

std::string FrontEnd::readIdentifier(std::string_view text, std::size_t& pos) const {
    std::string out;
    bool start_char = true;
    while (pos < text.size() && isIdentifierChar(text[pos], start_char)) {
        out.push_back(text[pos++]);
        start_char = false;
    }
    return out;
}

std::optional<std::string> FrontEnd::longestMatchingIdentifier(std::string_view text, std::size_t pos) const {
    std::optional<std::string> matched;
    for (const auto& identifier : known_identifiers_) {
        if (identifier.empty())
            continue;
        if (identifier.size() <= (matched ? matched->size() : 0))
            continue;
        if (pos + identifier.size() > text.size())
            continue;
        if (text.substr(pos, identifier.size()) == identifier)
            matched = identifier;
    }
    return matched;
}

void FrontEnd::registerPrimitiveIdentifiers() {
    static const char* primitives[] = {
        "__PRINT", "__LET", "__LET_PN", "__PER_NOTE", "__PER_NOTE_RESET",
        "__STORE", "__STORE_FORMAT", "__FORMAT", "__APPLY", "__MIDI",
        "__MIDI_NG", "__SYNC_NOFF_WITH_NEXT", "__ON_MIDI_NOTE_OFF",
        "__MIDI_META", "__FLEX_BINARY", "__FLEX_TEXT", "__SAVE_OPER_BEGIN",
        "__SAVE_OPER_END", "__RESTORE_OPER", "__LOOP_BEGIN", "__LOOP_BREAK",
        "__LOOP_END", "[", ":", "/", "]"
    };
    for (const char* primitive : primitives)
        registerIdentifier(primitive);
}

void FrontEnd::registerIdentifier(std::string identifier) {
    known_identifiers_.insert(std::move(identifier));
}

void ParserErrorListener::syntaxError(antlr4::Recognizer*,
                                      antlr4::Token*,
                                      size_t line,
                                      size_t charPositionInLine,
                                      const std::string& msg,
                                      std::exception_ptr) {
    diagnostics_.error(LineInfo{source_name_, static_cast<int>(line), static_cast<int>(charPositionInLine)}, msg);
}

bool parseTokenStream(const std::vector<Token>& tokens, DiagnosticSink& diagnostics) {
    return compileOperationUses(tokens, diagnostics).size() > 0 || tokens.empty() || !diagnostics.hasErrors();
}

bool isOperationIdentifierToken(const Token& token) {
    return token.type == TokenType::identifier || token.type == TokenType::colon || token.type == TokenType::slash;
}

bool canStartOperation(const std::vector<Token>& tokens, std::size_t pos) {
    if (pos >= tokens.size())
        return false;
    if (isOperationIdentifierToken(tokens[pos]))
        return true;
    return pos + 1 < tokens.size() && tokens[pos].type == TokenType::dollar && isOperationIdentifierToken(tokens[pos + 1]);
}

std::vector<Token> rewriteBareIdentifiersAsVariableReferences(const std::vector<Token>& tokens) {
    std::vector<Token> rewritten;
    rewritten.reserve(tokens.size() * 2);
    bool expect_operand = true;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        const bool already_variable_ref =
            !rewritten.empty() && rewritten.back().type == TokenType::dollar;
        if (expect_operand && token.type == TokenType::identifier && !already_variable_ref) {
            Token dollar;
            dollar.type = TokenType::dollar;
            dollar.location = token.location;
            rewritten.push_back(std::move(dollar));
        }
        rewritten.push_back(token);
        switch (token.type) {
            case TokenType::number_literal:
            case TokenType::string_literal:
            case TokenType::identifier:
            case TokenType::close_curly:
                expect_operand = false;
                break;
            case TokenType::dollar:
            case TokenType::open_curly:
            case TokenType::question:
            case TokenType::comma:
            case TokenType::plus:
            case TokenType::minus:
            case TokenType::asterisk:
            case TokenType::slash:
            case TokenType::percent:
            case TokenType::caret:
            case TokenType::backslash_lesser:
            case TokenType::backslash_lesser_equal:
            case TokenType::backslash_greater:
            case TokenType::backslash_greater_equal:
                expect_operand = true;
                break;
            default:
                break;
        }
    }
    return rewritten;
}

std::optional<std::vector<OperationUse>> compileOperationUsesFallback(const std::vector<Token>& tokens,
                                                                     DiagnosticSink& diagnostics) {
    std::vector<OperationUse> operations;
    std::size_t pos = 0;
    while (pos < tokens.size()) {
        if (!canStartOperation(tokens, pos))
            return std::nullopt;

        const bool applied_name = tokens[pos].type == TokenType::dollar;
        const auto& name_token = tokens[applied_name ? pos + 1 : pos];
        OperationUse use;
        use.name = std::get<std::string>(name_token.value);
        use.location = name_token.location;
        pos += applied_name ? 2 : 1;

        auto push_argument = [&](std::vector<Token> argument_tokens) {
            if (argument_tokens.empty()) {
                use.arguments.push_back(OperationUse::Argument{.skipped = true, .value = {}});
                return;
            }
            if (applied_name)
                argument_tokens = rewriteBareIdentifiersAsVariableReferences(argument_tokens);
            use.arguments.push_back(OperationUse::Argument{
                .skipped = false,
                .value = compileExpression(argument_tokens, diagnostics),
            });
        };

        if (pos < tokens.size() && tokens[pos].type == TokenType::open_curly) {
            ++pos;
            int curly_depth = 0;
            std::vector<Token> current;
            while (pos < tokens.size()) {
                const auto& token = tokens[pos++];
                if (token.type == TokenType::open_curly) {
                    ++curly_depth;
                    current.push_back(token);
                    continue;
                }
                if (token.type == TokenType::close_curly) {
                    if (curly_depth == 0) {
                        if (!current.empty() || !use.arguments.empty())
                            push_argument(current);
                        current.clear();
                        break;
                    }
                    --curly_depth;
                    current.push_back(token);
                    continue;
                }
                if (token.type == TokenType::comma && curly_depth == 0) {
                    push_argument(current);
                    current.clear();
                    continue;
                }
                current.push_back(token);
            }
            operations.push_back(std::move(use));
            continue;
        }

        while (pos < tokens.size()) {
            if (tokens[pos].type == TokenType::comma) {
                push_argument({});
                ++pos;
                continue;
            }
            if (!use.arguments.empty() && canStartOperation(tokens, pos))
                break;
            std::vector<Token> current;
            int curly_depth = 0;
            while (pos < tokens.size()) {
                const auto& token = tokens[pos];
                if (token.type == TokenType::open_curly) {
                    ++curly_depth;
                    current.push_back(token);
                    ++pos;
                    continue;
                }
                if (token.type == TokenType::close_curly) {
                    if (curly_depth == 0)
                        break;
                    --curly_depth;
                    current.push_back(token);
                    ++pos;
                    continue;
                }
                if (curly_depth == 0 && token.type == TokenType::comma)
                    break;
                if (curly_depth == 0 && !current.empty() &&
                    (token.type == TokenType::identifier || token.type == TokenType::colon ||
                     (token.type == TokenType::dollar && pos + 1 < tokens.size() &&
                      isOperationIdentifierToken(tokens[pos + 1])))) {
                    break;
                }
                current.push_back(token);
                ++pos;
            }
            if (current.empty())
                break;
            push_argument(current);
            if (pos < tokens.size() && tokens[pos].type == TokenType::comma) {
                ++pos;
                continue;
            }
            break;
        }

        operations.push_back(std::move(use));
    }
    return operations;
}

std::vector<OperationUse> compileOperationUses(const std::vector<Token>& tokens, DiagnosticSink& diagnostics) {
    if (tokens.empty())
        return {};

    std::vector<Diagnostic> parser_diagnostics;
    DiagnosticSink parser_sink(parser_diagnostics);
    TokenSourceAdapter token_source(tokens);
    antlr4::CommonTokenStream stream(&token_source);
    MugeneParser parser(&stream);
    ParserErrorListener listener(parser_sink, tokens.front().location.source_name);
    parser.removeErrorListeners();
    parser.addErrorListener(&listener);
    parser.setErrorHandler(std::make_shared<antlr4::BailErrorStrategy>());
    try {
        auto* tree = parser.operationUses();
        if (parser.getNumberOfSyntaxErrors() != 0) {
            if (auto fallback = compileOperationUsesFallback(tokens, diagnostics))
                return *fallback;
            return {};
        }
        ParserVisitor visitor(tokens);
        return std::any_cast<std::vector<OperationUse>>(visitor.visit(tree));
    } catch (const antlr4::ParseCancellationException&) {
        if (auto fallback = compileOperationUsesFallback(tokens, diagnostics))
            return *fallback;
        if (tokens.front().type == TokenType::identifier &&
            tokens.size() >= 3 &&
            tokens[1].type == TokenType::open_curly &&
            tokens.back().type == TokenType::close_curly) {
            OperationUse use;
            use.name = std::get<std::string>(tokens.front().value);
            use.location = tokens.front().location;

            std::vector<Token> current;
            int curly_depth = 0;
            for (std::size_t i = 2; i + 1 < tokens.size(); ++i) {
                const auto& token = tokens[i];
                if (token.type == TokenType::open_curly)
                    ++curly_depth;
                else if (token.type == TokenType::close_curly)
                    --curly_depth;

                if (token.type == TokenType::comma && curly_depth == 0) {
                    if (!current.empty()) {
                        use.arguments.push_back(OperationUse::Argument{
                            .skipped = false,
                            .value = compileExpression(current, diagnostics)});
                        current.clear();
                    }
                    continue;
                }
                current.push_back(token);
            }
            if (!current.empty()) {
                use.arguments.push_back(OperationUse::Argument{
                    .skipped = false,
                    .value = compileExpression(current, diagnostics)});
            }
            return {std::move(use)};
        }
        diagnostics.merge(parser_diagnostics);
        return {};
    }
}

ValueExprPtr compileExpression(const std::vector<Token>& tokens, DiagnosticSink& diagnostics) {
    if (tokens.empty())
        return {};

    std::vector<Diagnostic> parser_diagnostics;
    DiagnosticSink parser_sink(parser_diagnostics);
    TokenSourceAdapter token_source(tokens);
    antlr4::CommonTokenStream stream(&token_source);
    MugeneParser parser(&stream);
    ParserErrorListener listener(parser_sink, tokens.front().location.source_name);
    parser.removeErrorListeners();
    parser.addErrorListener(&listener);
    parser.setErrorHandler(std::make_shared<antlr4::BailErrorStrategy>());

    try {
        auto* tree = parser.expression();
        if (parser.getNumberOfSyntaxErrors() != 0) {
            diagnostics.merge(parser_diagnostics);
            return {};
        }
        ParserVisitor visitor(tokens);
        return std::any_cast<ValueExprPtr>(visitor.visit(tree));
    } catch (const antlr4::ParseCancellationException&) {
        diagnostics.merge(parser_diagnostics);
        return {};
    }
}

SemanticTree buildTrackSemanticTree(const std::vector<TrackSource>& tracks, DiagnosticSink& diagnostics) {
    SemanticTree tree;
    for (const auto& track : tracks) {
        auto operations = compileOperationUses(track.tokens, diagnostics);
        for (double track_number : track.track_numbers) {
            auto it = std::find_if(tree.tracks.begin(), tree.tracks.end(), [track_number](const SemanticTrack& semantic_track) {
                return semantic_track.number == track_number;
            });
            if (it == tree.tracks.end()) {
                SemanticTrack semantic_track;
                semantic_track.number = track_number;
                semantic_track.data = operations;
                tree.tracks.push_back(std::move(semantic_track));
            } else {
                it->data.insert(it->data.end(), operations.begin(), operations.end());
            }
        }
    }
    return tree;
}

SemanticTree buildSemanticTree(const FrontEnd& front_end, DiagnosticSink& diagnostics) {
    struct CompilationCondition {
        std::vector<std::string> blocks{};
        std::vector<double> tracks{};

        bool shouldCompileBlock(const std::string& name) const {
            return blocks.empty() || std::find(blocks.begin(), blocks.end(), name) != blocks.end();
        }

        bool shouldCompileTrack(double track) const {
            return tracks.empty() || std::find(tracks.begin(), tracks.end(), track) != tracks.end();
        }
    } condition;

    auto parse_conditional_tracks = [&](std::string_view text, const LineInfo& location) -> std::vector<double> {
        std::vector<double> result;
        auto read_number = [&](std::size_t& number_pos) -> std::optional<double> {
            if (number_pos >= text.size())
                return std::nullopt;
            if (text[number_pos] == '#') {
                ++number_pos;
                std::string hex;
                while (number_pos < text.size() && std::isxdigit(static_cast<unsigned char>(text[number_pos])))
                    hex.push_back(text[number_pos++]);
                if (hex.empty()) {
                    diagnostics.error(location, "Invalid hexadecimal digits.");
                    return std::nullopt;
                }
                return static_cast<double>(std::stoll(hex, nullptr, 16));
            }
            std::size_t start = number_pos;
            bool seen_dot = false;
            while (number_pos < text.size()) {
                const char ch = text[number_pos];
                if (std::isdigit(static_cast<unsigned char>(ch))) {
                    ++number_pos;
                    continue;
                }
                if (!seen_dot && ch == '.' && number_pos + 1 < text.size() &&
                    std::isdigit(static_cast<unsigned char>(text[number_pos + 1]))) {
                    seen_dot = true;
                    ++number_pos;
                    continue;
                }
                break;
            }
            return std::stod(std::string(text.substr(start, number_pos - start)));
        };

        std::size_t pos = 0;
        while (pos < text.size()) {
            while (pos < text.size() && isWhitespaceChar(text[pos]))
                ++pos;
            if (pos >= text.size())
                break;
            auto start = read_number(pos);
            if (!start)
                break;
            while (pos < text.size() && isWhitespaceChar(text[pos]))
                ++pos;
            if (pos < text.size() && text[pos] == '-') {
                ++pos;
                while (pos < text.size() && isWhitespaceChar(text[pos]))
                    ++pos;
                auto end = read_number(pos);
                if (!end)
                    break;
                for (double value = *start; value <= *end; value += 1.0)
                    result.push_back(value);
            } else {
                result.push_back(*start);
            }
            while (pos < text.size() && isWhitespaceChar(text[pos]))
                ++pos;
            if (pos < text.size() && text[pos] == ',')
                ++pos;
        }
        return result;
    };

    for (const auto& pragma : front_end.pragmas()) {
        if (pragma.name != "conditional" || pragma.lines.empty())
            continue;
        auto line = trimLeadingWhitespace(pragma.lines.front());
        std::size_t pos = 0;
        while (pos < line.size() && !isWhitespaceChar(line[pos]))
            ++pos;
        const auto category = line.substr(0, pos);
        line = trimLeadingWhitespace(std::string_view(line).substr(pos));
        if (category == "block") {
            std::size_t item_pos = 0;
            while (item_pos < line.size()) {
                while (item_pos < line.size() && isWhitespaceChar(line[item_pos]))
                    ++item_pos;
                std::size_t start = item_pos;
                while (item_pos < line.size() && line[item_pos] != ',')
                    ++item_pos;
                auto name = trimLeadingWhitespace(std::string_view(line).substr(start, item_pos - start));
                while (!name.empty() && isWhitespaceChar(name.back()))
                    name.pop_back();
                if (!name.empty())
                    condition.blocks.push_back(name);
                if (item_pos < line.size() && line[item_pos] == ',')
                    ++item_pos;
            }
        } else if (category == "track") {
            auto tracks = parse_conditional_tracks(line, pragma.first_location);
            condition.tracks.insert(condition.tracks.end(), tracks.begin(), tracks.end());
        } else {
            diagnostics.error(pragma.first_location, "Unexpected compilation condition type '" + category + "'");
        }
    }

    std::vector<TrackSource> filtered_tracks;
    filtered_tracks.reserve(front_end.tracks().size());
    for (const auto& track : front_end.tracks()) {
        if (!condition.shouldCompileBlock(track.block_name))
            continue;
        TrackSource filtered = track;
        filtered.track_numbers.clear();
        for (double track_number : track.track_numbers) {
            if (condition.shouldCompileTrack(track_number))
                filtered.track_numbers.push_back(track_number);
        }
        if (!filtered.track_numbers.empty())
            filtered_tracks.push_back(std::move(filtered));
    }

    SemanticTree tree = buildTrackSemanticTree(filtered_tracks, diagnostics);

    tree.variables.emplace("__timeline_position", SemanticVariable{
        .location = LineInfo{},
        .name = "__timeline_position",
        .type = DataType::number,
        .default_value = defaultValueForType(LineInfo{}, DataType::number),
    });
    tree.variables.emplace("__base_count", SemanticVariable{
        .location = LineInfo{},
        .name = "__base_count",
        .type = DataType::number,
        .default_value = std::make_shared<ConstantExpr>(
            LineInfo{}, DataType::number, ConstantExpr::Value(static_cast<double>(tree.base_count))),
    });

    for (const auto& pragma : front_end.pragmas()) {
        if (pragma.lines.empty())
            continue;

        const auto& line = pragma.lines.front();
        if (pragma.name == "basecount") {
            const auto text = trimLeadingWhitespace(line);
            if (text.empty()) {
                diagnostics.error(pragma.first_location, "Expected basecount value.");
                continue;
            }
            try {
                tree.base_count = std::stoi(text);
                auto it = tree.variables.find("__base_count");
                if (it != tree.variables.end()) {
                    it->second.default_value = std::make_shared<ConstantExpr>(
                        LineInfo{}, DataType::number, ConstantExpr::Value(static_cast<double>(tree.base_count)));
                }
            } catch (...) {
                diagnostics.error(pragma.first_location, "Invalid basecount value.");
            }
            continue;
        }

        if (pragma.name == "define") {
            std::size_t pos = 0;
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
                ++pos;
            std::size_t start = pos;
            while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t')
                ++pos;
            const auto key = line.substr(start, pos - start);
            const auto value = trimLeadingWhitespace(std::string_view(line).substr(pos));
            if (!key.empty())
                tree.aliases[key] = value;
            continue;
        }

        if (pragma.name == "meta") {
            std::size_t pos = 0;
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
                ++pos;
            std::size_t start = pos;
            while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t')
                ++pos;
            const auto meta_key = line.substr(start, pos - start);
            const auto value_text = trimLeadingWhitespace(std::string_view(line).substr(pos));
            auto meta_value = parseQuotedString(value_text);
            if (!meta_value) {
                diagnostics.error(pragma.first_location, "Invalid #meta directive text.");
                continue;
            }
            int meta_type = 0;
            if (meta_key == "text")
                meta_type = 1;
            else if (meta_key == "copyright")
                meta_type = 2;
            else if (meta_key == "title")
                meta_type = 3;
            else {
                diagnostics.error(pragma.first_location, "Invalid #meta directive argument: " + meta_key);
                continue;
            }

            OperationUse use;
            use.name = "__MIDI_META";
            use.location = pragma.first_location;
            use.arguments.push_back(OperationUse::Argument{
                .skipped = false,
                .value = std::make_shared<ConstantExpr>(
                    pragma.first_location, DataType::number, ConstantExpr::Value(static_cast<double>(meta_type)))});
            use.arguments.push_back(OperationUse::Argument{
                .skipped = false,
                .value = std::make_shared<ConstantExpr>(
                    pragma.first_location, DataType::string, ConstantExpr::Value(*meta_value))});

            auto it = std::find_if(tree.tracks.begin(), tree.tracks.end(), [](const SemanticTrack& track) {
                return track.number == -1.0;
            });
            if (it == tree.tracks.end()) {
                SemanticTrack meta_track;
                meta_track.number = -1.0;
                meta_track.data.push_back(std::move(use));
                tree.tracks.insert(tree.tracks.begin(), std::move(meta_track));
            } else {
                it->data.push_back(std::move(use));
            }
        }
    }

    auto known_identifiers = defaultPrimitiveIdentifiers();
    for (const auto& variable_source : front_end.variables())
        for (const auto& name : variable_source.parsed_names)
            known_identifiers.insert(name);
    for (const auto& macro_source : front_end.macros())
        if (macro_source.parsed_name)
            known_identifiers.insert(*macro_source.parsed_name);

    auto parse_data_type = [](const Token& token) -> std::optional<DataType> {
        switch (token.type) {
            case TokenType::keyword_number: return DataType::number;
            case TokenType::keyword_length: return DataType::length;
            case TokenType::keyword_string: return DataType::string;
            case TokenType::keyword_buffer: return DataType::buffer;
            default: return std::nullopt;
        }
    };

    auto parse_variable_declarations =
        [&](const std::vector<Token>& tokens, bool is_variable) -> std::vector<SemanticVariable> {
            std::vector<SemanticVariable> parsed;
            std::size_t pos = 0;
            while (pos < tokens.size()) {
                if (tokens[pos].type == TokenType::comma) {
                    ++pos;
                    continue;
                }
                if (tokens[pos].type != TokenType::identifier) {
                    diagnostics.error(tokens[pos].location, "Variable name is expected.");
                    break;
                }

                SemanticVariable variable;
                variable.location = tokens[pos].location;
                variable.name = std::get<std::string>(tokens[pos].value);
                variable.type = DataType::any;
                ++pos;

                if (pos < tokens.size() && tokens[pos].type == TokenType::colon) {
                    ++pos;
                    if (pos >= tokens.size()) {
                        diagnostics.error(variable.location, "type name is expected after ':' in variable definition");
                        break;
                    }
                    auto type = parse_data_type(tokens[pos]);
                    if (!type) {
                        diagnostics.error(tokens[pos].location, "Data type name is expected.");
                        break;
                    }
                    variable.type = *type;
                    ++pos;
                }

                if (pos < tokens.size() && tokens[pos].type == TokenType::identifier &&
                    std::get<std::string>(tokens[pos].value) == "=") {
                    ++pos;
                    std::vector<Token> default_tokens;
                    while (pos < tokens.size() && tokens[pos].type != TokenType::comma) {
                        default_tokens.push_back(tokens[pos]);
                        ++pos;
                    }
                    if (default_tokens.empty()) {
                        if (!is_variable)
                            diagnostics.error(variable.location, "Incomplete argument default value definition");
                    } else {
                        variable.default_value = compileExpression(default_tokens, diagnostics);
                    }
                }

                parsed.push_back(std::move(variable));
                if (pos < tokens.size() && tokens[pos].type == TokenType::comma)
                    ++pos;
            }
            return parsed;
        };

    for (const auto& variable_source : front_end.variables()) {
        if (variable_source.lines.empty())
            continue;
        const auto text = replaceAliases(joinLogicalLine(variable_source.lines), tree.aliases);
        auto tokens = tokenizeInlineText(text, variable_source.first_location, known_identifiers, true, diagnostics);
        if (tokens.empty())
            continue;
        for (auto& variable : parse_variable_declarations(tokens, true)) {
            fillDefaultValue(variable);
            tree.variables[variable.name] = std::move(variable);
        }
    }

    for (const auto& macro_source : front_end.macros()) {
        if (macro_source.lines.empty())
            continue;
        const auto header_line = replaceAliases(macro_source.lines.front(), tree.aliases);
        std::size_t body_pos = header_line.find('{');
        if (body_pos == std::string::npos) {
            diagnostics.error(macro_source.first_location, "'{' is expected at the end of macro definition");
            continue;
        }

        const auto header = std::string_view(header_line).substr(0, body_pos);

        std::size_t pos = 0;
        std::vector<double> target_tracks;
        auto read_header_number = [&](std::size_t& header_pos) -> std::optional<double> {
            if (header_pos >= header.size())
                return std::nullopt;
            std::size_t start = header_pos;
            bool seen_dot = false;
            while (header_pos < header.size()) {
                const char ch = header[header_pos];
                if (std::isdigit(static_cast<unsigned char>(ch))) {
                    ++header_pos;
                    continue;
                }
                if (!seen_dot && ch == '.' && header_pos + 1 < header.size() &&
                    std::isdigit(static_cast<unsigned char>(header[header_pos + 1]))) {
                    seen_dot = true;
                    ++header_pos;
                    continue;
                }
                break;
            }
            if (start == header_pos)
                return std::nullopt;
            return std::stod(std::string(header.substr(start, header_pos - start)));
        };

        while (pos < header.size() && isWhitespaceChar(header[pos]))
            ++pos;
        if (pos < header.size() && (std::isdigit(static_cast<unsigned char>(header[pos])) || header[pos] == '#')) {
            auto first = read_header_number(pos);
            if (first)
                target_tracks.push_back(*first);
            while (pos < header.size() && header[pos] == ',') {
                ++pos;
                auto next = read_header_number(pos);
                if (!next)
                    break;
                target_tracks.push_back(*next);
            }
            while (pos < header.size() && isWhitespaceChar(header[pos]))
                ++pos;
        }

        std::size_t name_pos = pos;
        auto macro_name = readIdentifierSimple(header, name_pos);
        if (macro_name.empty()) {
            diagnostics.error(macro_source.first_location, "Invalid macro definition.");
            continue;
        }

        SemanticMacro macro;
        macro.location = macro_source.first_location;
        macro.name = macro_name;
        macro.target_tracks = target_tracks;

        const auto argument_text = trimLeadingWhitespace(header.substr(name_pos));
        if (!argument_text.empty()) {
            auto arg_tokens = tokenizeInlineText(argument_text, macro_source.first_location, known_identifiers, true, diagnostics);
            for (auto& variable : parse_variable_declarations(arg_tokens, false)) {
                fillDefaultValue(variable);
                macro.arguments.push_back(std::move(variable));
            }
        }

        auto macro_known_identifiers = known_identifiers;
        for (const auto& argument : macro.arguments)
            macro_known_identifiers.insert(argument.name);

        bool saw_closing_brace = false;
        std::vector<Token> macro_body_tokens;
        for (std::size_t line_index = 0; line_index < macro_source.lines.size(); ++line_index) {
            std::string line = replaceAliases(macro_source.lines[line_index], tree.aliases);
            if (line_index == 0) {
                auto start = line.find('{');
                if (start == std::string::npos)
                    continue;
                line = line.substr(start + 1);
            }
            if (line_index + 1 == macro_source.lines.size()) {
                auto end = line.rfind('}');
                if (end != std::string::npos) {
                    line = line.substr(0, end);
                    saw_closing_brace = true;
                }
            }

            if (trimLeadingWhitespace(line).empty())
                continue;

            LineInfo line_location{
                macro_source.first_location.source_name,
                macro_source.first_location.line + static_cast<int>(line_index),
                macro_source.first_location.column
            };
            auto body_tokens = tokenizeInlineText(line, line_location, macro_known_identifiers, false, diagnostics);
            if (body_tokens.empty())
                continue;
            macro_body_tokens.insert(macro_body_tokens.end(), body_tokens.begin(), body_tokens.end());
        }
        if (!saw_closing_brace) {
            diagnostics.error(macro_source.first_location, "'{' is expected at the end of macro definition for '" + macro.name + "'");
            continue;
        }
        auto operations = compileOperationUses(macro_body_tokens, diagnostics);
        macro.data.insert(macro.data.end(), operations.begin(), operations.end());
        tree.macros.push_back(std::move(macro));
    }

    return tree;
}

bool prepareSemanticTree(SemanticTree& tree, DiagnosticSink& diagnostics) {
    ResolveContext global_context{.source_tree = tree, .global_context = nullptr};

    for (auto& [_, variable] : tree.variables) {
        fillDefaultValue(variable);
        if (!variable.default_value)
            continue;
        (void) ensureDefaultResolvedVariable(variable, global_context, diagnostics);
    }

    for (auto& macro : tree.macros) {
        for (auto& argument : macro.arguments)
            fillDefaultValue(argument);
    }

    return !diagnostics.hasErrors();
}

struct ResolvedTrack {
    double number{};
    std::vector<ResolvedEvent> events{};
    std::unordered_map<std::string, const SemanticMacro*> macros{};
};

struct ResolvedMusic {
    int base_count{192};
    std::vector<ResolvedTrack> tracks{};
};

struct StoredOperations {
    std::vector<OperationUse> operations{};
    std::unordered_map<std::string, ResolvedValue> values{};
    std::unordered_map<std::string, std::vector<ResolvedValue>> values_per_note{};
    std::unordered_map<std::string, ResolvedValue> macro_arguments{};
};

uint8_t resolvedByteValue(const ResolvedValue& value,
                          const LineInfo& location,
                          int base_count,
                          DiagnosticSink& diagnostics) {
    return static_cast<uint8_t>(static_cast<int>(resolvedNumberValue(value, location, base_count, diagnostics)));
}

std::vector<uint8_t> resolvedByteArrayValue(const ResolvedValue& value,
                                            const LineInfo& location,
                                            int base_count,
                                            DiagnosticSink& diagnostics) {
    if (const auto* text = std::get_if<std::string>(&value))
        return std::vector<uint8_t>(text->begin(), text->end());
    if (std::holds_alternative<std::monostate>(value))
        return {};
    return {resolvedByteValue(value, location, base_count, diagnostics)};
}

bool isNoteOffEvent(const ResolvedEvent& event) {
    return ((event.operation == "MIDI" || event.operation == "MIDI_NG") &&
            !event.arguments.empty() && event.arguments[0] == 0x80);
}

class EventStreamGenerator {
public:
    EventStreamGenerator(const SemanticTree& tree, DiagnosticSink& diagnostics, bool is_midi2)
        : tree_(tree),
          diagnostics_(diagnostics),
          global_context_{.source_tree = tree_mutable(), .global_context = nullptr},
          result_{.base_count = tree.base_count},
          is_midi2_(is_midi2) {}

    ResolvedMusic generate() {
        for (const auto& track : tree_.tracks) {
            ResolvedTrack resolved_track;
            resolved_track.number = track.number;
            for (const auto& macro : tree_.macros) {
                if (macro.target_tracks.empty() ||
                    std::find(macro.target_tracks.begin(), macro.target_tracks.end(), track.number) != macro.target_tracks.end()) {
                    resolved_track.macros[macro.name] = &macro;
                }
            }

            ResolveContext context{.source_tree = tree_mutable(), .global_context = &global_context_};
            context.values = global_context_.values;
            context.timeline_position = 0;
            current_output_ = &resolved_track.events;
            processOperations(resolved_track, context, track.data, 0, static_cast<int>(track.data.size()), {});
            if (!context.loops.empty()) {
                const auto& loop = context.loops.back();
                LineInfo location{};
                if (loop.begin_at && loop.begin_at->source >= 0 &&
                    static_cast<std::size_t>(loop.begin_at->source) < track.data.size()) {
                    location = track.data[static_cast<std::size_t>(loop.begin_at->source)].location;
                }
                diagnostics_.error(location, "There is an unclosed loop");
            }

            sortEvents(resolved_track.events);
            result_.tracks.push_back(std::move(resolved_track));
        }
        return result_;
    }

private:
    SemanticTree& tree_mutable() {
        return const_cast<SemanticTree&>(tree_);
    }

    static Loop* currentLoop(ResolveContext& context) {
        return context.loops.empty() ? nullptr : &context.loops.back();
    }

    static bool isNoteOnEvent(const ResolvedEvent& event) {
        return ((event.operation == "MIDI" || event.operation == "MIDI_NG") &&
                !event.arguments.empty() && event.arguments[0] == 0x90);
    }

    std::vector<ValueExprPtr> normalizeArguments(const OperationUse& operation) {
        std::vector<ValueExprPtr> args;
        args.reserve(operation.arguments.size());
        for (const auto& arg : operation.arguments)
            args.push_back(arg.value);
        return args;
    }

    ResolvedValue evaluateArgument(const ValueExprPtr& expr,
                                   ResolveContext& context,
                                   DataType type,
                                   const LineInfo& location) {
        if (!expr)
            return {};
        return evaluateValueExpr(*expr, context, type, diagnostics_);
    }

    void appendCurrentEvent(const ResolvedEvent& event) {
        if (!current_output_)
            return;
        current_output_->push_back(event);
        if (record_next_as_chord_)
            chord_.push_back({current_output_, current_output_->size() - 1});
        record_next_as_chord_ = false;
    }

    void processMacroCall(const ResolvedTrack& track,
                          ResolveContext& context,
                          const OperationUse& operation,
                          const std::vector<ValueExprPtr>& extra_tail_args) {
        auto macro_it = track.macros.find(operation.name);
        if (macro_it == track.macros.end()) {
            diagnostics_.error(operation.location, "Macro " + operation.name + " was not found");
            return;
        }

        const SemanticMacro* macro = macro_it->second;
        if (std::find(expansion_stack_.begin(), expansion_stack_.end(), macro) != expansion_stack_.end()) {
            diagnostics_.error(operation.location, "Illegally recursive macro reference to " + macro->name + " is found");
            return;
        }
        expansion_stack_.push_back(macro);

        std::unordered_map<std::string, ResolvedValue> args;

        auto oper_use_args = normalizeArguments(operation);
        if (!extra_tail_args.empty())
            oper_use_args.insert(oper_use_args.end(), extra_tail_args.begin(), extra_tail_args.end());

        for (std::size_t i = 0; i < macro->arguments.size(); ++i) {
            const auto& argdef = macro->arguments[i];
            ValueExprPtr arg = i < oper_use_args.size() ? oper_use_args[i] : nullptr;
            if (!arg)
                arg = argdef.default_value;
            if (!arg)
                continue;
            auto resolved = evaluateValueExpr(*arg, context, argdef.type, diagnostics_);
            if (args.contains(argdef.name)) {
                diagnostics_.error(operation.location,
                                   "Argument name must be identical to all other argument names. Argument '" +
                                       argdef.name + "' in '" + operation.name + "' macro");
            }
            args[argdef.name] = resolved;
        }

        auto macro_args_backup = context.macro_arguments;
        context.macro_arguments = args;
        std::vector<ValueExprPtr> extra_tail_args_to_call;
        if (macro->arguments.size() < oper_use_args.size()) {
            extra_tail_args_to_call.assign(oper_use_args.begin() + static_cast<std::ptrdiff_t>(macro->arguments.size()),
                                           oper_use_args.end());
        }
        processOperations(track, context, macro->data, 0, static_cast<int>(macro->data.size()), extra_tail_args_to_call);
        context.macro_arguments = std::move(macro_args_backup);

        expansion_stack_.pop_back();
    }

    void processOperations(const ResolvedTrack& track,
                           ResolveContext& context,
                           const std::vector<OperationUse>& operations,
                           int start,
                           int count,
                           const std::vector<ValueExprPtr>& extra_tail_args) {
        int store_index = -1;
        std::vector<ResolvedEvent>* store_current_output = nullptr;
        std::vector<ResolvedEvent> store_dummy;
        StoredOperations* current_stored_operations = nullptr;

        for (int list_index = start; list_index < start + count; ++list_index) {
            const auto& operation = operations[static_cast<std::size_t>(list_index)];
            const auto tail_args = (list_index == start + count - 1) ? extra_tail_args : std::vector<ValueExprPtr>{};

            if (operation.name == "__PRINT") {
                if (!operation.arguments.empty()) {
                    auto value = evaluateArgument(operation.arguments[0].value, context, DataType::string, operation.location);
                    diagnostics_.information(operation.location, resolvedValueToString(value));
                }
                break;
            }

            if (operation.name == "__LET") {
                if (operation.arguments.size() < 2)
                    continue;
                auto name_value = evaluateArgument(operation.arguments[0].value, context, DataType::string, operation.location);
                const auto variable_name = resolvedValueToString(name_value);
                auto variable_it = tree_.variables.find(variable_name);
                if (variable_it == tree_.variables.end()) {
                    diagnostics_.error(operation.location, "Target variable not found: " + variable_name);
                    continue;
                }
                auto value = evaluateArgument(operation.arguments[1].value, context, variable_it->second.type, operation.location);
                context.values[variable_name] = value;
                if (variable_name == "__timeline_position") {
                    context.timeline_position = static_cast<int>(
                        resolvedNumberValue(value, operation.location, tree_.base_count, diagnostics_));
                }
                continue;
            }

            if (operation.name == "__LET_PN") {
                if (operation.arguments.size() < 2)
                    continue;
                auto name_value = evaluateArgument(operation.arguments[0].value, context, DataType::string, operation.location);
                const auto variable_name = resolvedValueToString(name_value);
                auto variable_it = tree_.variables.find(variable_name);
                if (variable_it == tree_.variables.end()) {
                    diagnostics_.error(operation.location, "Target variable not found: " + variable_name);
                    continue;
                }
                auto value = evaluateArgument(operation.arguments[1].value, context, variable_it->second.type, operation.location);
                auto& slots = context.values_per_note[variable_name];
                if (slots.empty())
                    slots.resize(128);
                if (context.per_note_context < 0 || context.per_note_context > 127) {
                    diagnostics_.error(operation.location, "Per-Note context must be between 0 and 127");
                    continue;
                }
                slots[static_cast<std::size_t>(context.per_note_context)] = value;
                continue;
            }

            if (operation.name == "__PER_NOTE") {
                if (operation.arguments.empty())
                    continue;
                auto note_value = evaluateArgument(operation.arguments[0].value, context, DataType::number, operation.location);
                const auto note = static_cast<int>(resolvedNumberValue(
                    note_value, operation.location, tree_.base_count, diagnostics_));
                if (note < 0 || note > 127) {
                    diagnostics_.error(operation.location, "Per-Note context must be between 0 and 127");
                    continue;
                }
                context.per_note_context = note;
                continue;
            }

            if (operation.name == "__PER_NOTE_RESET") {
                context.per_note_context = -1;
                continue;
            }

            if (operation.name == "__STORE") {
                if (operation.arguments.empty())
                    continue;
                auto name_value = evaluateArgument(operation.arguments[0].value, context, DataType::string, operation.location);
                const auto variable_name = resolvedValueToString(name_value);
                auto& mutable_variables = tree_mutable().variables;
                auto variable_it = mutable_variables.find(variable_name);
                if (variable_it == mutable_variables.end()) {
                    diagnostics_.error(operation.location, "Target variable not found: " + variable_name);
                    continue;
                }
                if (variable_it->second.type != DataType::buffer) {
                    diagnostics_.error(operation.location, "Target variable is not a buffer: " + variable_name);
                    continue;
                }
                auto current = ensureDefaultResolvedVariable(variable_it->second, context, diagnostics_);
                std::string buffer = resolvedValueToString(current);
                for (std::size_t arg_index = 1; arg_index < operation.arguments.size(); ++arg_index) {
                    auto piece = evaluateArgument(operation.arguments[arg_index].value, context, DataType::string, operation.location);
                    buffer += resolvedValueToString(piece);
                }
                context.values[variable_name] = buffer;
                continue;
            }

            if (operation.name == "__APPLY") {
                if (operation.arguments.empty())
                    continue;
                auto macro_name = evaluateArgument(operation.arguments[0].value, context, DataType::string, operation.location);
                OperationUse applied;
                applied.name = resolvedValueToString(macro_name);
                applied.location = operation.location;
                for (std::size_t arg_index = 1; arg_index < operation.arguments.size(); ++arg_index)
                    applied.arguments.push_back(operation.arguments[arg_index]);
                processMacroCall(track, context, applied, tail_args);
                continue;
            }

            if (operation.name == "__MIDI") {
                ResolvedEvent event{.operation = "MIDI", .tick = context.timeline_position};
                for (const auto& argument : operation.arguments) {
                    auto value = evaluateArgument(argument.value, context, DataType::any, operation.location);
                    auto bytes = resolvedByteArrayValue(value, operation.location, tree_.base_count, diagnostics_);
                    event.arguments.insert(event.arguments.end(), bytes.begin(), bytes.end());
                }
                appendCurrentEvent(event);
                continue;
            }

            if (operation.name == "__MIDI_NG") {
                ResolvedEvent event{.operation = "MIDI_NG", .tick = context.timeline_position};
                for (const auto& argument : operation.arguments) {
                    auto value = evaluateArgument(argument.value, context, DataType::any, operation.location);
                    event.arguments.push_back(resolvedByteValue(value, operation.location, tree_.base_count, diagnostics_));
                }
                appendCurrentEvent(event);
                continue;
            }

            if (operation.name == "__SYNC_NOFF_WITH_NEXT") {
                record_next_as_chord_ = true;
                continue;
            }

            if (operation.name == "__ON_MIDI_NOTE_OFF") {
                if (operation.arguments.size() < 3)
                    continue;
                auto note_off_length = evaluateArgument(operation.arguments[0].value, context, DataType::number, operation.location);
                const auto ticks = static_cast<int>(resolvedNumberValue(
                    note_off_length, operation.location, tree_.base_count, diagnostics_));
                if (ticks == 0) {
                    record_next_as_chord_ = true;
                } else {
                    for (const auto& [events, index] : chord_) {
                        if (events && index < events->size())
                            (*events)[index].tick += ticks;
                    }
                    chord_.clear();
                }
                continue;
            }

            if (operation.name == "__MIDI_META") {
                std::vector<ResolvedValue> args;
                args.reserve(operation.arguments.size());
                for (const auto& argument : operation.arguments)
                    args.push_back(evaluateArgument(argument.value, context, DataType::any, operation.location));

                int flex_data_status = -1;
                if (!args.empty()) {
                    auto first = resolvedByteArrayValue(args.front(), operation.location, tree_.base_count, diagnostics_);
                    if (!first.empty()) {
                        switch (first[0]) {
                            case umppi::MidiMetaType::COPYRIGHT:
                                flex_data_status = umppi::MetadataTextStatus::COPYRIGHT;
                                break;
                            case umppi::MidiMetaType::TEXT:
                                flex_data_status = umppi::MetadataTextStatus::UNKNOWN;
                                break;
                            case umppi::MidiMetaType::TRACK_NAME:
                                flex_data_status = umppi::MetadataTextStatus::MIDI_CLIP_NAME;
                                break;
                            case umppi::MidiMetaType::INSTRUMENT_NAME:
                                flex_data_status = umppi::MetadataTextStatus::UNKNOWN;
                                break;
                            case umppi::MidiMetaType::MARKER:
                            case umppi::MidiMetaType::CUE_POINT:
                                flex_data_status = umppi::MetadataTextStatus::UNKNOWN;
                                break;
                            default:
                                break;
                        }
                    }
                }

                if (is_midi2_ && flex_data_status >= 0 && args.size() > 1) {
                    std::string text_value = resolvedValueToString(args[1]);
                    auto first = resolvedByteArrayValue(args.front(), operation.location, tree_.base_count, diagnostics_);
                    if (!first.empty()) {
                        switch (first[0]) {
                            case umppi::MidiMetaType::INSTRUMENT_NAME:
                                text_value = "InstrumentName: " + text_value;
                                break;
                            case umppi::MidiMetaType::MARKER:
                                text_value = "Marker: " + text_value;
                                break;
                            case umppi::MidiMetaType::CUE_POINT:
                                text_value = "Cue: " + text_value;
                                break;
                            default:
                                break;
                        }
                    }
                    auto umps = umppi::UmpFactory::metadataText(
                        0, umppi::FlexDataAddress::GROUP, 0, static_cast<uint8_t>(flex_data_status),
                        text_value);
                    ResolvedEvent event{.operation = "FLEX_TEXT", .tick = context.timeline_position};
                    for (const auto& ump : umps) {
                        auto bytes = ump.toPlatformBytes();
                        event.arguments.insert(event.arguments.end(), bytes.begin(), bytes.end());
                    }
                    appendCurrentEvent(event);
                } else if (is_midi2_ && args.size() > 1) {
                    auto first = resolvedByteArrayValue(args.front(), operation.location, tree_.base_count, diagnostics_);
                    if (!first.empty() && first[0] == umppi::MidiMetaType::LYRIC) {
                        auto umps = umppi::UmpFactory::performanceText(
                            0, umppi::FlexDataAddress::GROUP, 0, umppi::PerformanceTextStatus::LYRICS,
                            resolvedValueToString(args[1]));
                        ResolvedEvent event{.operation = "FLEX_TEXT", .tick = context.timeline_position};
                        for (const auto& ump : umps) {
                            auto bytes = ump.toPlatformBytes();
                            event.arguments.insert(event.arguments.end(), bytes.begin(), bytes.end());
                        }
                        appendCurrentEvent(event);
                    } else {
                        ResolvedEvent event{.operation = "META", .tick = context.timeline_position};
                        event.arguments.push_back(0xFF);
                        for (const auto& arg : args) {
                            auto bytes = resolvedByteArrayValue(arg, operation.location, tree_.base_count, diagnostics_);
                            event.arguments.insert(event.arguments.end(), bytes.begin(), bytes.end());
                        }
                        appendCurrentEvent(event);
                    }
                } else {
                    ResolvedEvent event{.operation = "META", .tick = context.timeline_position};
                    event.arguments.push_back(0xFF);
                    for (const auto& arg : args) {
                        auto bytes = resolvedByteArrayValue(arg, operation.location, tree_.base_count, diagnostics_);
                        event.arguments.insert(event.arguments.end(), bytes.begin(), bytes.end());
                    }
                    appendCurrentEvent(event);
                }
                continue;
            }

            if (operation.name == "__FLEX_BINARY") {
                std::vector<ResolvedValue> args;
                args.reserve(operation.arguments.size());
                for (const auto& argument : operation.arguments)
                    args.push_back(evaluateArgument(argument.value, context, DataType::any, operation.location));
                if (args.size() < 3)
                    continue;

                ResolvedEvent event{.operation = "FLEX_BINARY", .tick = context.timeline_position};
                const auto channel = resolvedByteValue(args[0], operation.location, tree_.base_count, diagnostics_);
                const auto address = resolvedByteValue(args[1], operation.location, tree_.base_count, diagnostics_);
                const auto status = resolvedByteValue(args[2], operation.location, tree_.base_count, diagnostics_);
                event.arguments.push_back(0xD0);
                event.arguments.push_back(static_cast<uint8_t>((address << 4) + channel));
                event.arguments.push_back(0);
                event.arguments.push_back(status);
                for (std::size_t index = 3; index < args.size(); ++index) {
                    auto bytes = resolvedByteArrayValue(args[index], operation.location, tree_.base_count, diagnostics_);
                    event.arguments.insert(event.arguments.end(), bytes.begin(), bytes.end());
                }
                if (args.size() < 12)
                    event.arguments.insert(event.arguments.end(), 12 - args.size() + 3, 0);
                appendCurrentEvent(event);
                continue;
            }

            if (operation.name == "__FLEX_TEXT") {
                std::vector<ResolvedValue> args;
                args.reserve(operation.arguments.size());
                for (const auto& argument : operation.arguments)
                    args.push_back(evaluateArgument(argument.value, context, DataType::any, operation.location));
                if (args.size() < 5)
                    continue;

                const auto channel = resolvedByteValue(args[0], operation.location, tree_.base_count, diagnostics_);
                const auto address = resolvedByteValue(args[1], operation.location, tree_.base_count, diagnostics_);
                const auto status_bank = resolvedByteValue(args[2], operation.location, tree_.base_count, diagnostics_);
                const auto status = resolvedByteValue(args[3], operation.location, tree_.base_count, diagnostics_);
                const auto text = resolvedValueToString(args[4]);
                auto umps = status_bank == umppi::FlexDataStatusBank::METADATA_TEXT
                    ? umppi::UmpFactory::metadataText(0, address, channel, status, text)
                    : umppi::UmpFactory::performanceText(0, address, channel, status, text);
                ResolvedEvent event{.operation = "FLEX_TEXT", .tick = context.timeline_position};
                for (const auto& ump : umps) {
                    auto bytes = ump.toPlatformBytes();
                    event.arguments.insert(event.arguments.end(), bytes.begin(), bytes.end());
                }
                appendCurrentEvent(event);
                continue;
            }

            if (operation.name == "__SAVE_OPER_BEGIN") {
                if (store_index >= 0) {
                    diagnostics_.error(operation.location,
                                       "__SAVE_OPER_BEGIN works only within a simple list without nested uses");
                    continue;
                }
                store_index = list_index + 1;
                store_current_output = current_output_;
                current_output_ = &store_dummy;
                stored_operations_scratch_.emplace_back();
                current_stored_operations = &stored_operations_scratch_.back();
                current_stored_operations->values = context.values;
                current_stored_operations->values_per_note = context.values_per_note;
                current_stored_operations->macro_arguments = context.macro_arguments;
                continue;
            }

            if (operation.name == "__SAVE_OPER_END") {
                if (operation.arguments.empty() || !current_stored_operations || store_index < 0 || !store_current_output)
                    continue;
                auto buffer_index_value = evaluateArgument(operation.arguments[0].value, context, DataType::number, operation.location);
                const auto buffer_index = static_cast<int>(resolvedNumberValue(
                    buffer_index_value, operation.location, tree_.base_count, diagnostics_));
                stored_operations_[buffer_index] = *current_stored_operations;
                current_stored_operations->operations.assign(
                    operations.begin() + store_index,
                    operations.begin() + list_index - 1);
                current_output_ = store_current_output;
                store_dummy.clear();
                store_index = -1;
                stored_operations_scratch_.pop_back();
                current_stored_operations = nullptr;
                continue;
            }

            if (operation.name == "__RESTORE_OPER") {
                if (operation.arguments.empty())
                    continue;
                auto buffer_index_value = evaluateArgument(operation.arguments[0].value, context, DataType::number, operation.location);
                const auto buffer_index = static_cast<int>(resolvedNumberValue(
                    buffer_index_value, operation.location, tree_.base_count, diagnostics_));
                auto stored_it = stored_operations_.find(buffer_index);
                if (stored_it == stored_operations_.end())
                    continue;
                const auto& stored = stored_it->second;
                auto values_backup = context.values;
                auto macro_args_backup = context.macro_arguments;
                context.values = stored.values;
                context.macro_arguments = stored.macro_arguments;
                context.values["__timeline_position"] = static_cast<double>(context.timeline_position);
                processOperations(track, context, stored.operations, 0, static_cast<int>(stored.operations.size()), tail_args);
                context.values = std::move(values_backup);
                context.macro_arguments = std::move(macro_args_backup);
                continue;
            }

            if (operation.name == "__LOOP_BEGIN" || operation.name == "[") {
                Loop loop(context);
                loop.begin_at = LoopLocation{list_index, static_cast<int>(current_output_ ? current_output_->size() : 0),
                                             context.timeline_position};
                context.values = loop.saved_values;
                context.loops.push_back(std::move(loop));
                current_output_ = &context.loops.back().events;
                continue;
            }

            if (operation.name == "__LOOP_BREAK" || operation.name == "/" || operation.name == ":") {
                auto* loop = currentLoop(context);
                if (!loop) {
                    diagnostics_.error(operation.location,
                                       "Loop break operation must be inside a pair of loop start and end");
                    continue;
                }
                if (!loop->first_break_at) {
                    loop->first_break_at = LoopLocation{
                        list_index,
                        static_cast<int>(current_output_ ? current_output_->size() : 0),
                        context.timeline_position
                    };
                }
                for (int current_break : loop->current_breaks) {
                    loop->end_locations[current_break] = LoopLocation{
                        list_index,
                        static_cast<int>(current_output_ ? current_output_->size() : 0),
                        context.timeline_position
                    };
                }
                loop->current_breaks.clear();

                if (operation.arguments.empty()) {
                    if (loop->breaks.contains(-1) && loop->breaks.at(-1).source != list_index) {
                        diagnostics_.error(operation.location, "Default loop break is already defined in current loop");
                    }
                    loop->breaks[-1] = LoopLocation{
                        list_index,
                        static_cast<int>(current_output_ ? current_output_->size() : 0),
                        context.timeline_position
                    };
                    loop->current_breaks.push_back(-1);
                } else {
                    for (std::size_t x = 0; x < operation.arguments.size(); ++x) {
                        auto num_value = evaluateArgument(operation.arguments[x].value, context, DataType::number, operation.location);
                        const auto num = static_cast<int>(resolvedNumberValue(
                            num_value, operation.location, tree_.base_count, diagnostics_)) - 1;
                        if (x > 0 && num < 0)
                            break;
                        loop->current_breaks.push_back(num);
                        if (loop->breaks.contains(num) && loop->breaks.at(num).source != list_index) {
                            diagnostics_.error(operation.location,
                                               "Loop section " + std::to_string(num) + " was already defined in current loop");
                            break;
                        }
                        loop->breaks[num] = LoopLocation{
                            list_index,
                            static_cast<int>(current_output_ ? current_output_->size() : 0),
                            context.timeline_position
                        };
                    }
                }
                continue;
            }

            if (operation.name == "__LOOP_END" || operation.name == "]") {
                Loop* loop = currentLoop(context);
                if (!loop) {
                    diagnostics_.error(operation.location, "Loop has not started");
                    continue;
                }
                for (int current_break : loop->current_breaks) {
                    loop->end_locations[current_break] = LoopLocation{
                        list_index,
                        static_cast<int>(current_output_ ? current_output_->size() : 0),
                        context.timeline_position
                    };
                }

                int loop_count = 0;
                if (operation.arguments.empty()) {
                    loop_count = 2;
                } else if (operation.arguments.size() == 1) {
                    auto loop_count_value = evaluateArgument(operation.arguments[0].value, context, DataType::number, operation.location);
                    loop_count = static_cast<int>(resolvedNumberValue(
                        loop_count_value, operation.location, tree_.base_count, diagnostics_));
                } else {
                    diagnostics_.error(operation.location, "Arguments at loop end exceeded");
                    continue;
                }

                auto completed_loop = std::move(context.loops.back());
                context.loops.pop_back();
                auto* outside = currentLoop(context);
                current_output_ = outside ? &outside->events : &const_cast<ResolvedTrack&>(track).events;

                for (const auto& [count_index, break_location] : completed_loop.breaks) {
                    if (count_index > loop_count) {
                        diagnostics_.error(
                            operations[static_cast<std::size_t>(break_location.source)].location,
                            "Loop break specified beyond the loop count");
                        completed_loop.breaks.clear();
                        break;
                    }
                }

                context.values = completed_loop.saved_values;

                const auto begin_at = *completed_loop.begin_at;
                if (!completed_loop.first_break_at) {
                    context.timeline_position = begin_at.tick;
                    for (int iteration = 0; iteration < loop_count; ++iteration) {
                        processOperations(track,
                                          context,
                                          operations,
                                          begin_at.source + 1,
                                          list_index - begin_at.source - 1,
                                          tail_args);
                    }
                } else {
                    const auto first_break_at = *completed_loop.first_break_at;
                    context.timeline_position = begin_at.tick;

                    for (int iteration = 0; iteration < loop_count; ++iteration) {
                        processOperations(track,
                                          context,
                                          operations,
                                          begin_at.source + 1,
                                          first_break_at.source - begin_at.source - 1,
                                          tail_args);
                        LoopLocation* break_location = nullptr;
                        auto explicit_break_it = completed_loop.breaks.find(iteration);
                        if (explicit_break_it != completed_loop.breaks.end()) {
                            break_location = &explicit_break_it->second;
                        } else {
                            if (iteration + 1 == loop_count)
                                break;
                            auto default_break_it = completed_loop.breaks.find(-1);
                            if (default_break_it == completed_loop.breaks.end()) {
                                diagnostics_.error(
                                    operations[static_cast<std::size_t>(begin_at.source)].location,
                                    "No corresponding loop break specification for iteration at " +
                                        std::to_string(iteration + 1) + " from the innermost loop");
                                completed_loop.breaks.clear();
                                break;
                            }
                            break_location = &default_break_it->second;
                        }
                        if (!break_location)
                            break;
                        LoopLocation end_location{};
                        auto end_it = completed_loop.end_locations.find(iteration);
                        if (end_it != completed_loop.end_locations.end()) {
                            end_location = end_it->second;
                        } else {
                            end_location = completed_loop.end_locations.at(-1);
                        }
                        processOperations(track,
                                          context,
                                          operations,
                                          break_location->source + 1,
                                          end_location.source - break_location->source - 1,
                                          tail_args);
                    }
                }
                continue;
            }

            if (operation.name.size() >= 2 && operation.name[0] == '_' && operation.name[1] == '_')
                continue;

            processMacroCall(track, context, operation, tail_args);
        }
    }

    void sortEvents(std::vector<ResolvedEvent>& events) {
        std::stable_sort(events.begin(), events.end(), [](const ResolvedEvent& left, const ResolvedEvent& right) {
            if (left.tick != right.tick)
                return left.tick < right.tick;
            const bool left_note_off = isNoteOffEvent(left);
            const bool right_note_off = isNoteOffEvent(right);
            const bool left_note_on = isNoteOnEvent(left);
            const bool right_note_on = isNoteOnEvent(right);
            if (left_note_off && right_note_on)
                return true;
            if (left_note_on && right_note_off)
                return false;
            return false;
        });
    }

    const SemanticTree& tree_;
    DiagnosticSink& diagnostics_;
    ResolveContext global_context_;
    ResolvedMusic result_{};
    bool is_midi2_{false};
    std::vector<ResolvedEvent>* current_output_{};
    std::vector<std::pair<std::vector<ResolvedEvent>*, std::size_t>> chord_{};
    bool record_next_as_chord_{false};
    std::unordered_map<int, StoredOperations> stored_operations_{};
    std::vector<StoredOperations> stored_operations_scratch_{};
    std::vector<const SemanticMacro*> expansion_stack_{};
};

bool validateMacroExpansion(const SemanticTree& tree, DiagnosticSink& diagnostics) {
    auto music = EventStreamGenerator(tree, diagnostics, false).generate();
    (void) music;
    return !diagnostics.hasErrors();
}

bool generateSmf2Clips(const SemanticTree& tree,
                       DiagnosticSink& diagnostics,
                       std::vector<TrackCompilationResult>& tracks) {
    const auto music = EventStreamGenerator(tree, diagnostics, true).generate();
    if (diagnostics.hasErrors())
        return false;

    uint32_t track_id = 1;
    for (const auto& track : music.tracks) {
        LocatedClip clip;
        clip.position_dctpq = 0;
        clip.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
        clip.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::dctpq(static_cast<uint16_t>(music.base_count / 4))));
        clip.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
        clip.smf2clip.push_back(umppi::UmpFactory::startOfClip());

        int current_tick = 0;
        for (const auto& event : track.events) {
            if (event.tick != current_tick)
                clip.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(
                    static_cast<uint32_t>(event.tick - current_tick))));

            if (event.operation == "FLEX_TEXT" || event.operation == "FLEX_BINARY") {
                auto umps = umppi::Ump::fromBytes(event.arguments);
                clip.smf2clip.insert(clip.smf2clip.end(), umps.begin(), umps.end());
            } else if (event.operation == "MIDI_NG") {
                if (event.arguments.size() >= 8) {
                    const uint32_t rest32 =
                        (static_cast<uint32_t>(event.arguments[4]) << 24) |
                        (static_cast<uint32_t>(event.arguments[5]) << 16) |
                        (static_cast<uint32_t>(event.arguments[6]) << 8) |
                        static_cast<uint32_t>(event.arguments[7]);
                    clip.smf2clip.emplace_back(umppi::UmpFactory::midi2ChannelMessage8_8_32(
                        static_cast<uint8_t>(event.arguments[1] / 0x10),
                        static_cast<uint8_t>(event.arguments[0]),
                        static_cast<uint8_t>(event.arguments[1] % 0x10),
                        event.arguments[2],
                        event.arguments[3],
                        rest32));
                }
            } else if (!event.arguments.empty() && event.arguments[0] == 0xFF) {
                std::vector<uint8_t> sysex8{0, 0, 0, 0, 0xFF, 0xFF, 0xFF};
                sysex8.insert(sysex8.end(), event.arguments.begin() + 1, event.arguments.end());
                auto umps = umppi::UmpFactory::sysex8(0, sysex8);
                clip.smf2clip.insert(clip.smf2clip.end(), umps.begin(), umps.end());
            } else if (!event.arguments.empty() && event.arguments[0] == 0xF0) {
                std::vector<uint8_t> sysex7(event.arguments.begin() + 1, event.arguments.end());
                auto umps = umppi::UmpFactory::sysex7(0, sysex7);
                clip.smf2clip.insert(clip.smf2clip.end(), umps.begin(), umps.end());
            } else if (!event.arguments.empty() && (event.arguments[0] & 0xF0) == 0xF0) {
                clip.smf2clip.emplace_back(umppi::UmpFactory::systemMessage(
                    0,
                    event.arguments[0],
                    event.arguments.size() > 1 ? event.arguments[1] : 0,
                    event.arguments.size() > 2 ? event.arguments[2] : 0));
            } else if (!event.arguments.empty()) {
                clip.smf2clip.emplace_back(umppi::UmpFactory::midi1Message(
                    0,
                    static_cast<uint8_t>(event.arguments[0] & 0xF0),
                    static_cast<uint8_t>(event.arguments[0] % 0x10),
                    event.arguments.size() > 1 ? event.arguments[1] : 0,
                    event.arguments.size() > 2 ? event.arguments[2] : 0));
            }

            current_tick = event.tick;
        }

        clip.smf2clip.push_back(umppi::UmpFactory::endOfClip());
        tracks.push_back(TrackCompilationResult{
            .track_id = track_id++,
            .clips = {std::move(clip)},
        });
    }
    return true;
}

bool generateSmf(const SemanticTree& tree,
                 DiagnosticSink& diagnostics,
                 std::vector<uint8_t>& smf) {
    const auto music = EventStreamGenerator(tree, diagnostics, false).generate();
    if (diagnostics.hasErrors())
        return false;

    umppi::Midi1Music midi1_music;
    midi1_music.deltaTimeSpec = music.base_count / 4;
    midi1_music.format = 1;

    for (const auto& track : music.tracks) {
        umppi::Midi1Track midi1_track;
        int current_tick = 0;
        for (const auto& event : track.events) {
            const int delta = event.tick - current_tick;
            std::shared_ptr<umppi::Midi1Message> message;
            if (!event.arguments.empty() && event.arguments[0] == 0xFF) {
                std::vector<uint8_t> extra;
                if (event.arguments.size() > 2) {
                    extra.assign(event.arguments.begin() + 2, event.arguments.end());
                }
                message = std::make_shared<umppi::Midi1CompoundMessage>(
                    event.arguments[0],
                    event.arguments.size() > 1 ? event.arguments[1] : 0,
                    0,
                    extra);
            } else if (event.arguments.size() <= 3) {
                message = std::make_shared<umppi::Midi1SimpleMessage>(
                    event.arguments.empty() ? 0 : event.arguments[0],
                    event.arguments.size() > 1 ? event.arguments[1] : 0,
                    event.arguments.size() > 2 ? event.arguments[2] : 0);
            } else {
                std::vector<uint8_t> extra(event.arguments.begin() + 1, event.arguments.end());
                message = std::make_shared<umppi::Midi1CompoundMessage>(
                    event.arguments.empty() ? 0 : event.arguments[0],
                    0,
                    0,
                    extra);
            }
            midi1_track.events.emplace_back(delta, std::move(message));
            current_tick = event.tick;
        }
        midi1_track.events.emplace_back(
            0,
            std::make_shared<umppi::Midi1CompoundMessage>(0xFF, 0x2F, 0, std::vector<uint8_t>{}));
        midi1_music.tracks.push_back(std::move(midi1_track));
    }

    std::ostringstream stream(std::ios::binary);
    umppi::Midi1Writer writer(stream);
    writer.write(midi1_music);
    const auto bytes = stream.str();
    smf.assign(bytes.begin(), bytes.end());
    return true;
}

} // namespace mugene2::detail
