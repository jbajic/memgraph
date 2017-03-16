#pragma once

#include <memory>
#include <stdexcept>

#include "query/frontend/ast/ast.hpp"
#include "query/frontend/logical/operator.hpp"

namespace query {

std::shared_ptr<LogicalOperator> GenCreate(
    Create& match, std::shared_ptr<LogicalOperator> current_op)
{
  if (current_op) {
    throw std::runtime_error("Not implemented");
  }
  if (match.patterns_.size() != 1) {
    throw std::runtime_error("Not implemented");
  }
  auto& pattern = match.patterns_[0];
  if (pattern->atoms_.size() != 1) {
    throw std::runtime_error("Not implemented");
  }
  auto node_atom = std::dynamic_pointer_cast<NodeAtom>(pattern->atoms_[0]);
  return std::make_shared<CreateOp>(node_atom);
}

std::shared_ptr<LogicalOperator> GenMatch(
    Match& match, std::shared_ptr<LogicalOperator> current_op)
{
  if (current_op) {
    throw std::runtime_error("Not implemented");
  }
  if (match.patterns_.size() != 1) {
    throw std::runtime_error("Not implemented");
  }
  auto& pattern = match.patterns_[0];
  if (pattern->atoms_.size() != 1) {
    throw std::runtime_error("Not implemented");
  }
  auto node_atom = std::dynamic_pointer_cast<NodeAtom>(pattern->atoms_[0]);
  return std::make_shared<ScanAll>(node_atom);
}

std::shared_ptr<LogicalOperator> GenReturn(
    Return& ret, std::shared_ptr<LogicalOperator> current_op)
{
  if (!current_op) {
    throw std::runtime_error("Not implemented");
  }
  return std::make_shared<Produce>(current_op, ret.named_expressions_);
}

std::shared_ptr<LogicalOperator> MakeLogicalPlan(Query& query)
{
  std::shared_ptr<LogicalOperator> current_op;
  for (auto& clause : query.clauses_) {
    auto* clause_ptr = clause.get();
    auto* create = dynamic_cast<Create*>(clause_ptr);
    auto* match = dynamic_cast<Match*>(clause_ptr);
    auto* ret = dynamic_cast<Return*>(clause_ptr);
    if (create) {
      current_op = GenCreate(*create, current_op);
    } else if (match) {
      current_op = GenMatch(*match, current_op);
    } else if (ret) {
      return GenReturn(*ret, current_op);
    } else {
      throw std::runtime_error("Not implemented");
    }
  }
  return current_op;
}
}
