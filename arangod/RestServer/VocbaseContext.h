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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Rest/GeneralRequest.h"
#include "Utils/ExecContext.h"

#include <atomic>

struct TRI_vocbase_t;

namespace arangodb {

/// @brief just also stores the context
class VocbaseContext final : public arangodb::ExecContext {
 public:
  ~VocbaseContext();

  static std::shared_ptr<VocbaseContext> create(GeneralRequest& req,
                                                TRI_vocbase_t& vocbase);
  TEST_VIRTUAL TRI_vocbase_t& vocbase() const { return _vocbase; }

  /// @brief upgrade to internal superuser
  void forceSuperuser();

  /// @brief upgrade to internal read-only user
  void forceReadOnly();

#ifdef USE_ENTERPRISE
  std::string clientAddress() const override {
    return _request.connectionInfo().fullClient();
  }
  std::string requestUrl() const override { return _request.fullUrl(); }
  std::string authMethod() const override;
#endif

  /// @brief tells you if this execution was canceled
  bool isCanceled() const override {
    return _canceled.load(std::memory_order_relaxed);
  }

  /// cancel execution
  void cancel() { _canceled.store(true, std::memory_order_relaxed); }

 private:
#ifdef USE_ENTERPRISE
  GeneralRequest const& _request;
#endif
  TRI_vocbase_t& _vocbase;

  /// should be used to indicate a canceled request / thread
  std::atomic<bool> _canceled;

  class ConstructorToken {};

 public:
  VocbaseContext(ConstructorToken, GeneralRequest& req, TRI_vocbase_t& vocbase,
                 ExecContext::Type type, auth::Level systemLevel,
                 auth::Level dbLevel, bool isAdminUser);
  VocbaseContext(VocbaseContext const&) = delete;
  VocbaseContext& operator=(VocbaseContext const&) = delete;
};

}  // namespace arangodb
