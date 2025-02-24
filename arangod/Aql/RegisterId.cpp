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
/// @author Manuel Pöter
////////////////////////////////////////////////////////////////////////////////

#include "Aql/types.h"
#include "Basics/Exceptions.h"

#include <absl/strings/str_cat.h>

namespace arangodb::aql {
RegisterId RegisterId::fromUInt32(uint32_t value) {
  auto v = static_cast<value_t>(value);
  auto type = static_cast<Type>(value >> (sizeof(value_t) * 8));
  RegisterId result(v, type);
  TRI_ASSERT(result.isValid());
  if (!result.isValid()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        absl::StrCat("Cannot parse RegisterId from value ", value));
  }
  return result;
}

uint32_t RegisterId::toUInt32() const noexcept {
  uint32_t result = _value;
  result |= static_cast<uint32_t>(_type) << (sizeof(value_t) * 8);
  return result;
}
}  // namespace arangodb::aql
