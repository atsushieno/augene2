#pragma once

#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <antlr4-runtime.h>

#include <mugene2/mugene2.hpp>

namespace mugene2::detail {

struct LineInfo {
    std::string source_name{};
    int line{0};
    int column{0};
};

enum class TokenType {
    none,
    identifier,
    string_literal,
    number_literal,
    period,
    comma,
    percent,
    open_paren,
    close_paren,
    open_curly,
    close_curly,
    question,
    plus,
    minus,
    asterisk,
    slash,
    dollar,
    colon,
    caret,
    backslash_lesser,
    backslash_lesser_equal,
    backslash_greater,
    backslash_greater_equal,
    keyword_number,
    keyword_length,
    keyword_string,
    keyword_buffer,
};

using TokenValue = std::variant<std::monostate, double, std::string>;

struct Token {
    TokenType type{TokenType::none};
    TokenValue value{};
    LineInfo location{};
};

struct TrackSource {
    uint32_t track_id{};
    std::string block_name{};
    std::vector<double> track_numbers{};
    std::vector<Token> tokens{};
};

struct PragmaSource {
    std::string name{};
    std::vector<std::string> lines{};
    LineInfo first_location{};
};

struct VariableSource {
    std::vector<std::string> lines{};
    LineInfo first_location{};
    std::vector<std::string> parsed_names{};
};

struct MacroSource {
    std::vector<std::string> lines{};
    LineInfo first_location{};
    std::optional<std::string> parsed_name{};
};

enum class DataType {
    any,
    number,
    length,
    string,
    buffer,
};

struct Length {
    int number{};
    int dots{};
    bool is_value_by_step{};
};

enum class ComparisonType {
    lesser,
    lesser_equal,
    greater,
    greater_equal,
};

struct ValueExpr {
    explicit ValueExpr(LineInfo location) : location(std::move(location)) {}
    virtual ~ValueExpr() = default;

    LineInfo location{};
};

using ValueExprPtr = std::shared_ptr<ValueExpr>;

struct ConstantExpr final : ValueExpr {
    using Value = std::variant<std::monostate, double, std::string, Length>;

    ConstantExpr(LineInfo location, DataType type, Value value)
        : ValueExpr(std::move(location)), type(type), value(std::move(value)) {}

    DataType type{DataType::any};
    Value value{};
};

struct VariableReferenceExpr final : ValueExpr {
    VariableReferenceExpr(LineInfo location, std::string name, int scope = 1)
        : ValueExpr(std::move(location)), scope(scope), name(std::move(name)) {}

    int scope{1};
    std::string name{};
};

struct ParenthesizedExpr final : ValueExpr {
    ParenthesizedExpr(LineInfo location, ValueExprPtr content)
        : ValueExpr(std::move(location)), content(std::move(content)) {}

    ValueExprPtr content{};
};

struct ArithmeticExpr : ValueExpr {
    ArithmeticExpr(LineInfo location, ValueExprPtr left, ValueExprPtr right)
        : ValueExpr(std::move(location)), left(std::move(left)), right(std::move(right)) {}

    ValueExprPtr left{};
    ValueExprPtr right{};
};

struct AddExpr final : ArithmeticExpr { using ArithmeticExpr::ArithmeticExpr; };
struct SubtractExpr final : ArithmeticExpr { using ArithmeticExpr::ArithmeticExpr; };
struct MultiplyExpr final : ArithmeticExpr { using ArithmeticExpr::ArithmeticExpr; };
struct DivideExpr final : ArithmeticExpr { using ArithmeticExpr::ArithmeticExpr; };
struct ModuloExpr final : ArithmeticExpr { using ArithmeticExpr::ArithmeticExpr; };

struct ConditionalExpr final : ValueExpr {
    ConditionalExpr(LineInfo location, ValueExprPtr condition, ValueExprPtr true_expr, ValueExprPtr false_expr)
        : ValueExpr(std::move(location)),
          condition(std::move(condition)),
          true_expr(std::move(true_expr)),
          false_expr(std::move(false_expr)) {}

    ValueExprPtr condition{};
    ValueExprPtr true_expr{};
    ValueExprPtr false_expr{};
};

struct ComparisonExpr final : ValueExpr {
    ComparisonExpr(LineInfo location, ValueExprPtr left, ValueExprPtr right, ComparisonType type)
        : ValueExpr(std::move(location)),
          left(std::move(left)),
          right(std::move(right)),
          comparison_type(type) {}

    ValueExprPtr left{};
    ValueExprPtr right{};
    ComparisonType comparison_type{ComparisonType::lesser};
};

struct OperationUse {
    struct Argument {
        bool skipped{};
        ValueExprPtr value{};
    };

    std::string name{};
    LineInfo location{};
    std::vector<Argument> arguments{};
};

struct SemanticTrack {
    double number{};
    std::vector<OperationUse> data{};
};

struct SemanticVariable {
    LineInfo location{};
    std::string name{};
    DataType type{DataType::any};
    ValueExprPtr default_value{};
};

struct SemanticMacro {
    LineInfo location{};
    std::string name{};
    std::vector<double> target_tracks{};
    std::vector<SemanticVariable> arguments{};
    std::vector<OperationUse> data{};
};

struct SemanticTree {
    int base_count{192};
    std::vector<SemanticTrack> tracks{};
    std::vector<SemanticMacro> macros{};
    std::unordered_map<std::string, SemanticVariable> variables{};
    std::unordered_map<std::string, std::string> aliases{};
};

class DiagnosticSink {
public:
    explicit DiagnosticSink(std::vector<Diagnostic>& diagnostics) : diagnostics_(diagnostics) {}

    void error(const LineInfo& location, std::string message);
    void warning(const LineInfo& location, std::string message);
    void information(const LineInfo& location, std::string message);
    void merge(const std::vector<Diagnostic>& diagnostics);

    [[nodiscard]] bool hasErrors() const;

private:
    void add(DiagnosticSeverity severity, const LineInfo& location, std::string message);

    std::vector<Diagnostic>& diagnostics_;
};

class FrontEnd {
public:
    FrontEnd(DiagnosticSink& diagnostics,
             std::span<const SourceText> sources,
             IncludeResolver resolver);

    [[nodiscard]] bool process();
    [[nodiscard]] const std::vector<TrackSource>& tracks() const { return tracks_; }
    [[nodiscard]] const std::vector<PragmaSource>& pragmas() const { return pragmas_; }
    [[nodiscard]] const std::vector<VariableSource>& variables() const { return variables_; }
    [[nodiscard]] const std::vector<MacroSource>& macros() const { return macros_; }
    static std::optional<std::vector<double>> parseRange(std::string_view text,
                                                         std::size_t& pos,
                                                         DiagnosticSink& diagnostics,
                                                         const LineInfo& location);

private:
    struct RawTrackLine {
        std::string block_name{};
        std::vector<double> track_numbers{};
        std::vector<std::string> physical_lines{};
        LineInfo first_location{};
    };

    bool processSource(const SourceText& source);
    bool processPragma(const std::string& pragma, const LineInfo& location, std::vector<std::string>** continued_lines);
    bool processTrackLine(const std::string& text, const LineInfo& location, RawTrackLine** continued_track);
    bool tokenizeTrack(RawTrackLine& track,
                       const std::unordered_map<std::string, std::string>& aliases);

    static std::string trimComments(const std::string& text, std::size_t start = 0);
    static bool isWhitespace(char c);
    static bool isIdentifierChar(char c, bool start_char, bool escaped_continue = false);
    std::string readIdentifier(std::string_view text, std::size_t& pos) const;
    std::optional<std::string> longestMatchingIdentifier(std::string_view text, std::size_t pos) const;
    void registerPrimitiveIdentifiers();
    void registerIdentifier(std::string identifier);

    DiagnosticSink& diagnostics_;
    std::span<const SourceText> sources_;
    IncludeResolver resolver_;
    std::vector<std::string> include_stack_{};
    bool in_comment_mode_{false};
    std::string previous_block_name_{};
    std::optional<std::vector<double>> previous_track_numbers_{};
    std::vector<RawTrackLine> raw_tracks_{};
    std::vector<TrackSource> tracks_{};
    std::vector<PragmaSource> pragmas_{};
    std::vector<VariableSource> variables_{};
    std::vector<MacroSource> macros_{};
    std::unordered_set<std::string> known_identifiers_{};
    uint32_t next_track_id_{1};
};

class ParserErrorListener final : public antlr4::BaseErrorListener {
public:
    ParserErrorListener(DiagnosticSink& diagnostics, std::string source_name)
        : diagnostics_(diagnostics), source_name_(std::move(source_name)) {}

    void syntaxError(antlr4::Recognizer* recognizer,
                     antlr4::Token* offendingSymbol,
                     size_t line,
                     size_t charPositionInLine,
                     const std::string& msg,
                     std::exception_ptr e) override;

private:
    DiagnosticSink& diagnostics_;
    std::string source_name_;
};

bool parseTokenStream(const std::vector<Token>& tokens, DiagnosticSink& diagnostics);
std::vector<OperationUse> compileOperationUses(const std::vector<Token>& tokens, DiagnosticSink& diagnostics);
ValueExprPtr compileExpression(const std::vector<Token>& tokens, DiagnosticSink& diagnostics);
SemanticTree buildTrackSemanticTree(const std::vector<TrackSource>& tracks, DiagnosticSink& diagnostics);
SemanticTree buildSemanticTree(const FrontEnd& front_end, DiagnosticSink& diagnostics);
bool prepareSemanticTree(SemanticTree& tree, DiagnosticSink& diagnostics);
bool validateMacroExpansion(const SemanticTree& tree, DiagnosticSink& diagnostics);
bool generateSmf2Clips(const SemanticTree& tree,
                       DiagnosticSink& diagnostics,
                       std::vector<TrackCompilationResult>& tracks);
bool generateSmf(const SemanticTree& tree,
                 DiagnosticSink& diagnostics,
                 std::vector<uint8_t>& smf);

} // namespace mugene2::detail
