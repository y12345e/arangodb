////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
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
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <absl/container/flat_hash_map.h>

namespace arangodb::containers {

template<class K, class V,
         class Hash = iresearch_absl::container_internal::hash_default_hash<K>,
         class Eq = iresearch_absl::container_internal::hash_default_eq<K>,
         class Allocator = std::allocator<std::pair<const K, V>>>
using FlatHashMap = iresearch_absl::flat_hash_map<K, V, Hash, Eq, Allocator>;

}  // namespace arangodb::containers
