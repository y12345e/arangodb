////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Aql/ExecutionNode/ExecutionNode.h"
#include "Aql/ExpressionContext.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/RegisterPlan.h"
#include "Basics/Exceptions.h"
#include "Containers/FlatHashMap.h"

#include <velocypack/Slice.h>

namespace arangodb {

namespace aql {
class AqlFunctionsInternalCache;
class AqlItemBlock;
struct AstNode;
class ExecutionEngine;
class QueryContext;
struct Variable;
}  // namespace aql

namespace iresearch {

class IResearchViewNode;

///////////////////////////////////////////////////////////////////////////////
/// @struct ViewExpressionContextBase
/// @brief FIXME remove this struct once IResearchView will be able to evaluate
///        epxressions with loop variable in SEARCH expressions.
///        simon: currently also used in tests
///////////////////////////////////////////////////////////////////////////////
struct ViewExpressionContextBase : public arangodb::aql::ExpressionContext {
  explicit ViewExpressionContextBase(arangodb::transaction::Methods* trx,
                                     aql::QueryContext* query,
                                     aql::AqlFunctionsInternalCache* cache)
      : ExpressionContext(),
        _trx(trx),
        _query(query),
        _aqlFunctionsInternalCache(cache) {}

  void registerWarning(ErrorCode errorCode,
                       std::string_view msg) override final;
  void registerError(ErrorCode errorCode, std::string_view msg) override final;

  icu_64_64::RegexMatcher* buildRegexMatcher(
      std::string_view expr, bool caseInsensitive) override final;
  icu_64_64::RegexMatcher* buildLikeMatcher(
      std::string_view expr, bool caseInsensitive) override final;
  icu_64_64::RegexMatcher* buildSplitMatcher(
      aql::AqlValue splitExpression, velocypack::Options const* opts,
      bool& isEmptyExpression) override final;

  arangodb::ValidatorBase* buildValidator(
      arangodb::velocypack::Slice) override final;

  TRI_vocbase_t& vocbase() const override final;
  /// may be inaccessible on some platforms
  transaction::Methods& trx() const override final;
  bool killed() const override final;

  aql::AstNode const* _expr{};  // for troubleshooting

 protected:
  arangodb::transaction::Methods* _trx;
  arangodb::aql::QueryContext* _query;
  arangodb::aql::AqlFunctionsInternalCache* _aqlFunctionsInternalCache;
};  // ViewExpressionContextBase

///////////////////////////////////////////////////////////////////////////////
/// @struct ViewExpressionContext
///////////////////////////////////////////////////////////////////////////////
struct ViewExpressionContext final : public ViewExpressionContextBase {
  ViewExpressionContext(arangodb::transaction::Methods& trx,
                        aql::QueryContext& query,
                        aql::AqlFunctionsInternalCache& cache,
                        aql::Variable const& outVar,
                        aql::VarInfoMap const& varInfoMap, int nodeDepth)
      : ViewExpressionContextBase(&trx, &query, &cache),
        _outVar(outVar),
        _varInfoMap(varInfoMap),
        _nodeDepth(nodeDepth) {}

  // register a temporary variable in the ExpressionContext. the
  // slice used here is not owned by the QueryExpressionContext!
  // the caller has to make sure the data behind the slice remains
  // valid until clearVariable() is called or the context is discarded.
  void setVariable(arangodb::aql::Variable const* variable,
                   arangodb::velocypack::Slice value) override;

  // unregister a temporary variable from the ExpressionContext.
  void clearVariable(arangodb::aql::Variable const* variable) noexcept override;

  aql::AqlValue getVariableValue(aql::Variable const* variable, bool doCopy,
                                 bool& mustDestroy) const override;

  inline auto const& outVariable() const noexcept { return _outVar; }
  inline auto const& varInfoMap() const noexcept { return _varInfoMap; }
  inline int nodeDepth() const noexcept { return _nodeDepth; }

  aql::InputAqlItemRow _inputRow{aql::CreateInvalidInputRowHint{}};
  aql::Variable const& _outVar;
  aql::VarInfoMap const& _varInfoMap;
  int const _nodeDepth;

  // variables only temporarily valid during execution
  // variables only temporarily valid during execution. Slices stored
  // here are not owned by the QueryExpressionContext!
  containers::FlatHashMap<arangodb::aql::Variable const*,
                          arangodb::velocypack::Slice>
      _variables;
};  // ViewExpressionContext

}  // namespace iresearch
}  // namespace arangodb
