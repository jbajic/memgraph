#include <iostream>
#include <sstream>
#include <string>

#include "query/frontend/ast/ast.hpp"
#include "query/frontend/ast/cypher_main_visitor.hpp"
#include "query/frontend/ast/pretty_print.hpp"
#include "query/frontend/opencypher/parser.hpp"

std::string AssembleQueryString(const std::string &expression_string) {
  return "return " + expression_string + " as expr";
}

query::Query *ParseQuery(const std::string &query_string, query::AstStorage *ast_storage) {
  query::frontend::ParsingContext context;
  query::frontend::opencypher::Parser parser(query_string);
  query::frontend::CypherMainVisitor visitor(context, ast_storage);

  visitor.visit(parser.tree());
  return visitor.query();
}

query::Expression *GetExpression(query::Query *query) {
  auto cypher_query = dynamic_cast<query::CypherQuery *>(query);
  auto ret = dynamic_cast<query::Return *>(cypher_query->single_query_->clauses_[0]);
  auto expr = ret->body_.named_expressions[0]->expression_;

  return expr;
}

int main() {
  query::AstStorage ast_storage;
  std::ostringstream ss;
  ss << std::cin.rdbuf();

  auto query_string = AssembleQueryString(ss.str());
  auto query = ParseQuery(query_string, &ast_storage);
  auto expr = GetExpression(query);

  query::PrintExpression(expr, &std::cout);
  std::cout << std::endl;

  return 0;
}
