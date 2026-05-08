#include <string>

#include <antlr4-runtime.h>
#include "MugeneLexer.h"
#include "MugeneParser.h"

namespace augene2::detail {

bool antlrSmokeParsesExpression(const std::string& text) {
    antlr4::ANTLRInputStream input(text);
    MugeneLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    MugeneParser parser(&tokens);
    parser.expressionOrOperationUses();
    return parser.getNumberOfSyntaxErrors() == 0;
}

} // namespace augene2::detail
