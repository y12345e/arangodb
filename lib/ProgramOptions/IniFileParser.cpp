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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include <cstddef>
#include <map>
#include <sstream>

#include "IniFileParser.h"

#include "Basics/Exceptions.h"
#include "Basics/FileUtils.h"
#include "Basics/StringUtils.h"
#include "Basics/application-exit.h"
#include "Basics/exitcodes.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "ProgramOptions/Parameters.h"
#include "ProgramOptions/ProgramOptions.h"

namespace arangodb::options {

IniFileParser::IniFileParser(ProgramOptions* options) : _options(options) {
  // a line with just comments, e.g. #... or ;...
  _matchers.comment = std::regex("^[ \t]*([#;].*)?$",
                                 std::regex::nosubs | std::regex::ECMAScript);
  // a line that starts a section, e.g. [server]
  _matchers.section = std::regex("^[ \t]*\\[([-_A-Za-z0-9]*)\\][ \t]*$",
                                 std::regex::ECMAScript);
  // a line that starts a community section, e.g. [server:community]
  _matchers.communitySection = std::regex(
      "^[ \t]*\\[([-_A-Za-z0-9]*):community\\][ \t]*$", std::regex::ECMAScript);
  // a line that starts an enterprise section, e.g. [server:enterprise]
  _matchers.enterpriseSection =
      std::regex("^[ \t]*\\[([-_A-Za-z0-9]*):enterprise\\][ \t]*$",
                 std::regex::ECMAScript);
  // a line that assigns a value to a named variable
  _matchers.assignment = std::regex(
      "^[ \t]*(([-_A-Za-z0-9]*\\.)?[-_A-Za-z0-9]*)[ \t]*=[ \t]*(.*?)?[ \t]*$",
      std::regex::ECMAScript);
  // an include line
  _matchers.include =
      std::regex("^[ \t]*@include[ \t]*([-_A-Za-z0-9/\\.]*)[ \t]*$",
                 std::regex::ECMAScript);
}

// parse a config file. returns true if all is well, false otherwise
// errors that occur during parse are reported to _options
bool IniFileParser::parse(std::string const& filename, bool endPassAfterwards) {
  if (filename.empty()) {
    _options->fail(
        TRI_EXIT_CONFIG_NOT_FOUND,
        "unable to open configuration file: no configuration file specified");
    return false;
  }

  std::string buf;
  try {
    buf = arangodb::basics::FileUtils::slurp(filename);
  } catch (arangodb::basics::Exception const& ex) {
    _options->fail(TRI_EXIT_CONFIG_NOT_FOUND,
                   std::string("Couldn't open configuration file: '") +
                       filename + "' - " + ex.what());
    return false;
  }

  return parseContent(filename, buf, endPassAfterwards);
}

// parse a config file, with the contents already read into <buf>.
// returns true if all is well, false otherwise
// errors that occur during parse are reported to _options
bool IniFileParser::parseContent(std::string const& filename,
                                 std::string const& buf,
                                 bool endPassAfterwards) {
  bool isCommunity = false;
  bool isEnterprise = false;
  std::string currentSection;
  size_t lineNumber = 0;

  std::istringstream iss(buf);
  for (std::string line; std::getline(iss, line);) {
    basics::StringUtils::trimInPlace(line);
    ++lineNumber;

    if (std::regex_match(line, _matchers.comment)) {
      // skip over comments
      continue;
    }

    // set context for parsing (used in error messages)
    _options->setContext("config file '" + filename + "', line #" +
                         std::to_string(lineNumber));

    std::smatch match;
    if (std::regex_match(line, match, _matchers.section)) {
      // found section
      currentSection = match[1].str();
      isCommunity = false;
      isEnterprise = false;
    } else if (std::regex_match(line, match, _matchers.communitySection)) {
      // found section
      currentSection = match[1].str();
      isCommunity = true;
      isEnterprise = false;
    } else if (std::regex_match(line, match, _matchers.enterpriseSection)) {
      // found section
      currentSection = match[1].str();
      isCommunity = false;
      isEnterprise = true;
    } else if (std::regex_match(line, match, _matchers.include)) {
      // found include
      std::string include(match[1].str());

      if (!include.ends_with(".conf")) {
        include += ".conf";
      }
      if (_seen.find(include) != _seen.end()) {
        LOG_TOPIC("cc815", FATAL, Logger::CONFIG)
            << "recursive include of file '" << include << "'";
        FATAL_ERROR_EXIT_CODE(TRI_EXIT_CONFIG_NOT_FOUND);
      }

      _seen.insert(include);

      if (!basics::FileUtils::isRegularFile(include)) {
        auto dn = basics::FileUtils::dirname(filename);
        include = basics::FileUtils::buildFilename(dn, include);
      }

      LOG_TOPIC("36d6b", DEBUG, Logger::CONFIG)
          << "reading include file '" << include << "'";

      if (!parse(include, false)) {
        return false;
      }
    } else if (std::regex_match(line, match, _matchers.assignment)) {
      // found assignment
      std::string option;
      std::string value(match[3].str());

      if (currentSection.empty() || !match[2].str().empty()) {
        // use option as specified
        option = match[1].str();
      } else {
        // use option prefixed with current section
        option = currentSection + "." + match[1].str();
      }

#ifdef USE_ENTERPRISE
      if (isCommunity) {
        continue;
      }
#else
      if (isEnterprise) {
        continue;
      }
#endif

      if (!_options->setValue(option, value)) {
        return false;
      }
    } else {
      // unknown type of line. cannot handle it
      _options->fail(TRI_EXIT_CONFIG_NOT_FOUND,
                     "unknown line type in file '" + filename + "', line " +
                         std::to_string(lineNumber) + ": '" + line + "'");
      return false;
    }
  }

  // make sure the compiler does not complain about these variables
  // being unused
  (void)isCommunity;
  (void)isEnterprise;

  // all is well
  if (endPassAfterwards) {
    _options->endPass();
  }
  return true;
}

}  // namespace arangodb::options
