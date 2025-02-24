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

#include <array>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>

#include "Basics/ReadWriteLock.h"
#include "Logger/LogLevel.h"
#include "Logger/Topics.h"

namespace arangodb {
class LogTopic;
struct LogMessage;

class LogAppender {
 public:
  LogAppender();
  virtual ~LogAppender() = default;

  LogAppender(LogAppender const&) = delete;
  LogAppender& operator=(LogAppender const&) = delete;

  void setCurrentLevelsAsDefault();
  void resetLevelsToDefault();
  auto getLogLevel(LogTopic const& topic) -> LogLevel;
  void setLogLevel(LogTopic const& topic, LogLevel level);
  auto getLogLevels() -> std::unordered_map<LogTopic*, LogLevel>;

  void logMessageGuarded(LogMessage const&);

  virtual std::string details() const = 0;

 protected:
  virtual void logMessage(LogMessage const& message) = 0;

 private:
  basics::ReadWriteLock _logOutputMutex;
  std::atomic<std::thread::id> _logOutputMutexOwner;
  std::array<std::atomic<LogLevel>, logger::kNumTopics> _topicLevels;
  std::array<LogLevel, logger::kNumTopics> _defaultLevels;
};
}  // namespace arangodb
