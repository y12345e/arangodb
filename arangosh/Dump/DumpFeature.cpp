////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
/// @author Dan Larkin-York
////////////////////////////////////////////////////////////////////////////////

#include "DumpFeature.h"

#include <chrono>
#include <thread>

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>
#include <boost/algorithm/clamp.hpp>

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Exceptions.h"
#include "Basics/FileUtils.h"
#include "Basics/MutexLocker.h"
#include "Basics/NumberOfCores.h"
#include "Basics/files.h"
#include "Basics/Result.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/application-exit.h"
#include "Basics/system-functions.h"
#include "FeaturePhases/BasicFeaturePhaseClient.h"
#include "Maskings/Maskings.h"
#include "ProgramOptions/ProgramOptions.h"
#include "Random/RandomGenerator.h"
#include "Shell/ClientFeature.h"
#include "SimpleHttpClient/SimpleHttpClient.h"
#include "SimpleHttpClient/SimpleHttpResult.h"
#include "Ssl/SslInterface.h"
#include "Utils/ManagedDirectory.h"

namespace {

/// @brief fake client id we will send to the server. the server keeps
/// track of all connected clients
static std::string clientId;

/// @brief name of the feature to report to application server
constexpr auto FeatureName = "Dump";

/// @brief minimum amount of data to fetch from server in a single batch
constexpr uint64_t MinChunkSize = 1024 * 128;

/// @brief maximum amount of data to fetch from server in a single batch
// NB: larger value may cause tcp issues (check exact limits)
constexpr uint64_t MaxChunkSize = 1024 * 1024 * 96;

/// @brief generic error for if server returns bad/unexpected json
const arangodb::Result ErrorMalformedJsonResponse = {
    TRI_ERROR_INTERNAL, "got malformed JSON response from server"};

/// @brief check whether HTTP response is valid, complete, and not an error
arangodb::Result checkHttpResponse(arangodb::httpclient::SimpleHttpClient& client,
                                   std::unique_ptr<arangodb::httpclient::SimpleHttpResult> const& response) {
  using arangodb::basics::StringUtils::itoa;
  if (response == nullptr || !response->isComplete()) {
    return {TRI_ERROR_INTERNAL,
            "got invalid response from server: " + client.getErrorMessage()};
  }
  if (response->wasHttpError()) {
    int errorNum = TRI_ERROR_INTERNAL;
    std::string errorMsg = response->getHttpReturnMessage();
    std::shared_ptr<arangodb::velocypack::Builder> bodyBuilder(response->getBodyVelocyPack());
    arangodb::velocypack::Slice error = bodyBuilder->slice();
    if (!error.isNone() && error.hasKey(arangodb::StaticStrings::ErrorMessage)) {
      errorNum = error.get(arangodb::StaticStrings::ErrorNum).getNumericValue<int>();
      errorMsg = error.get(arangodb::StaticStrings::ErrorMessage).copyString();
    }
    return {errorNum, "got invalid response from server: HTTP " +
                          itoa(response->getHttpReturnCode()) + ": " + errorMsg};
  }
  return {};
}

/// @brief checks that a file pointer is valid and file status is ok
bool fileOk(arangodb::ManagedDirectory::File* file) {
  return (file && file->status().ok());
}

/// @brief assuming file pointer is not ok, generate/extract proper error
arangodb::Result fileError(arangodb::ManagedDirectory::File* file, bool isWritable) {
  if (!file) {
    if (isWritable) {
      return {TRI_ERROR_CANNOT_WRITE_FILE};
    } else {
      return {TRI_ERROR_CANNOT_READ_FILE};
    }
  }
  return file->status();
}

/// @brief get a list of available databases to dump for the current user
std::pair<arangodb::Result, std::vector<std::string>> getDatabases(arangodb::httpclient::SimpleHttpClient& client) {
  std::string const url = "/_api/database/user";

  std::vector<std::string> databases;

  std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response(
      client.request(arangodb::rest::RequestType::GET, url, "", 0));
  auto check = ::checkHttpResponse(client, response);
  if (check.fail()) {
    LOG_TOPIC("47882", ERR, arangodb::Logger::DUMP)
        << "An error occurred while trying to determine list of databases: " << check.errorMessage();
    return {check, databases};
  }

  // extract vpack body from response
  std::shared_ptr<VPackBuilder> parsedBody;
  try {
    parsedBody = response->getBodyVelocyPack();
  } catch (...) {
    return {::ErrorMalformedJsonResponse, databases};
  }
  VPackSlice resBody = parsedBody->slice();
  if (resBody.isObject()) {
    resBody = resBody.get("result");
  }
  if (!resBody.isArray()) {
    return {{TRI_ERROR_FAILED, "expecting list of databases to be an array"}, databases};
  }

  for (auto const& it : arangodb::velocypack::ArrayIterator(resBody)) {
    if (it.isString()) {
      databases.push_back(it.copyString());
    }
  }

  // sort by name, with _system first
  std::sort(databases.begin(), databases.end(), [](std::string const& lhs, std::string const& rhs) {
    if (lhs == arangodb::StaticStrings::SystemDatabase && rhs != arangodb::StaticStrings::SystemDatabase) {
      return true;
    } else if (rhs == arangodb::StaticStrings::SystemDatabase && lhs != arangodb::StaticStrings::SystemDatabase) {
      return false;
    }
    return lhs < rhs;
  });

  return {{TRI_ERROR_NO_ERROR}, databases};
}

/// @brief start a batch via the replication API
std::pair<arangodb::Result, uint64_t> startBatch(arangodb::httpclient::SimpleHttpClient& client,
                                                 std::string const& DBserver) {
  using arangodb::basics::VelocyPackHelper;
  using arangodb::basics::StringUtils::uint64;

  std::string url = "/_api/replication/batch?serverId=" + clientId;
  std::string const body = "{\"ttl\":600}";
  if (!DBserver.empty()) {
    url += "&DBserver=" + DBserver;
  }

  std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response(
      client.request(arangodb::rest::RequestType::POST, url, body.c_str(), body.size()));
  auto check = ::checkHttpResponse(client, response);
  if (check.fail()) {
    LOG_TOPIC("34dbf", ERR, arangodb::Logger::DUMP)
        << "An error occurred while creating dump context: " << check.errorMessage();
    return {check, 0};
  }

  // extract vpack body from response
  std::shared_ptr<VPackBuilder> parsedBody;
  try {
    parsedBody = response->getBodyVelocyPack();
  } catch (...) {
    return {::ErrorMalformedJsonResponse, 0};
  }
  VPackSlice const resBody = parsedBody->slice();

  // look up "id" value
  std::string const id = VelocyPackHelper::getStringValue(resBody, "id", "");

  return {{TRI_ERROR_NO_ERROR}, uint64(id)};
}

/// @brief prolongs a batch to ensure we can complete our dump
void extendBatch(arangodb::httpclient::SimpleHttpClient& client,
                 std::string const& DBserver, uint64_t batchId) {
  using arangodb::basics::StringUtils::itoa;
  TRI_ASSERT(batchId > 0);

  std::string url = "/_api/replication/batch/" + itoa(batchId) +
                    "?serverId=" + clientId;
  std::string const body = "{\"ttl\":600}";
  if (!DBserver.empty()) {
    url += "&DBserver=" + DBserver;
  }

  std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response(
      client.request(arangodb::rest::RequestType::PUT, url, body.c_str(), body.size()));
  // ignore any return value
}

/// @brief mark our batch finished so resources can be freed on server
void endBatch(arangodb::httpclient::SimpleHttpClient& client,
              std::string DBserver, uint64_t& batchId) {
  using arangodb::basics::StringUtils::itoa;
  TRI_ASSERT(batchId > 0);

  std::string url = "/_api/replication/batch/" + itoa(batchId) +
                    "?serverId=" + clientId;
  if (!DBserver.empty()) {
    url += "&DBserver=" + DBserver;
  }

  std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response(
      client.request(arangodb::rest::RequestType::DELETE_REQ, url, nullptr, 0));
  // ignore any return value

  // overwrite the input id
  batchId = 0;
}

bool isIgnoredHiddenEnterpriseCollection(arangodb::DumpFeature::Options const& options,
                                         std::string const& name) {
#ifdef USE_ENTERPRISE
  if (!options.force && name[0] == '_') {
    if (strncmp(name.c_str(), "_local_", 7) == 0 ||
        strncmp(name.c_str(), "_from_", 6) == 0 || strncmp(name.c_str(), "_to_", 4) == 0) {
      LOG_TOPIC("d921a", INFO, arangodb::Logger::DUMP)
          << "Dump is ignoring collection '" << name
          << "'. Will be created via SmartGraphs of a full dump. If you want "
             "to "
             "dump this collection anyway use 'arangodump --force'. "
             "However this is not recommended and you should instead dump "
             "the EdgeCollection of the SmartGraph instead.";
      return true;
    }
  }
#endif
  return false;
}

arangodb::Result dumpJsonObjects(arangodb::DumpFeature::DumpJob& job,
                                 arangodb::ManagedDirectory::File& file,
                                 arangodb::basics::StringBuffer const& body) {
  size_t length;
  if (job.maskings != nullptr) {
    arangodb::basics::StringBuffer masked(256, false);
    job.maskings->mask(job.collectionName, body, masked);
    file.write(masked.data(), masked.length());
    length = masked.length();
  } else {
    file.write(body.data(), body.length());
    length = body.length();
  }

  if (file.status().fail()) {
    return {TRI_ERROR_CANNOT_WRITE_FILE,
            arangodb::basics::StringUtils::concatT("cannot write file '", file.path(),
                                                   "': ", file.status().errorMessage())};
  }

  job.stats.totalWritten += static_cast<uint64_t>(length);

  return {};
}

/// @brief dump the actual data from an individual collection
arangodb::Result dumpCollection(arangodb::httpclient::SimpleHttpClient& client,
                                arangodb::DumpFeature::DumpJob& job,
                                arangodb::ManagedDirectory::File& file,
                                std::string const& name, std::string const& server,
                                uint64_t batchId, uint64_t minTick, uint64_t maxTick) {
  using arangodb::basics::StringUtils::boolean;
  using arangodb::basics::StringUtils::itoa;
  using arangodb::basics::StringUtils::uint64;
  using arangodb::basics::StringUtils::urlEncode;

  uint64_t fromTick = minTick;
  uint64_t chunkSize = job.options.initialChunkSize;  // will grow adaptively up to max
  std::string baseUrl = "/_api/replication/dump?collection=" + urlEncode(name) +
                        "&batchId=" + itoa(batchId) + "&ticks=false" + 
                        "&useEnvelope=" + (job.options.useEnvelope ? "true" : "false");
  if (job.options.clusterMode) {
    // we are in cluster mode, must specify dbserver
    baseUrl += "&DBserver=" + server;
  } else {
    // we are in single-server mode, we already flushed the wal
    baseUrl += "&flush=false";
  }
  
  std::unordered_map<std::string, std::string> headers;
  headers.emplace(arangodb::StaticStrings::Accept, arangodb::StaticStrings::MimeTypeDump);

  while (true) {
    std::string url = baseUrl + "&from=" + itoa(fromTick) + "&chunkSize=" + itoa(chunkSize);
    if (maxTick > 0) {  // limit to a certain timeframe
      url += "&to=" + itoa(maxTick);
    }

    ++job.stats.totalBatches;  // count how many chunks we are fetching

    // make the actual request for data
    std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response(
        client.request(arangodb::rest::RequestType::GET, url, nullptr, 0, headers));
    auto check = ::checkHttpResponse(client, response);
    if (check.fail()) {
      LOG_TOPIC("ac972", ERR, arangodb::Logger::DUMP)
          << "An error occurred while dumping collection '" << name
          << "': " << check.errorMessage();
      return check;
    }

    // find out whether there are more results to fetch
    bool checkMore = false;

    bool headerExtracted;
    std::string header = response->getHeaderField(arangodb::StaticStrings::ReplicationHeaderCheckMore,
                                                  headerExtracted);
    if (headerExtracted) {
      // first check the basic flag
      checkMore = boolean(header);
      if (checkMore) {
        // now check if the actual tick has changed
        header = response->getHeaderField(arangodb::StaticStrings::ReplicationHeaderLastIncluded,
                                          headerExtracted);
        if (headerExtracted) {
          uint64_t tick = uint64(header);
          if (tick > fromTick) {
            fromTick = tick;
          } else {
            // we got the same tick again, this indicates we're at the end
            checkMore = false;
          }
        }
      }
    }
    if (!headerExtracted) {  // NOT else, fallthrough from outer or inner above
      return {TRI_ERROR_REPLICATION_INVALID_RESPONSE,
              std::string("got invalid response from server: required header "
                          "is missing while dumping collection '") +
                  name + "'"};
    }
    
    header = response->getHeaderField(arangodb::StaticStrings::ContentTypeHeader, headerExtracted);
    if (!headerExtracted || header.compare(0, 25, "application/x-arango-dump") != 0) {
      return {TRI_ERROR_REPLICATION_INVALID_RESPONSE,
        "got invalid response from server: content-type is invalid"};
    }

    // now actually write retrieved data to dump file
    arangodb::basics::StringBuffer const& body = response->getBody();
    arangodb::Result result = dumpJsonObjects(job, file, body);

    if (result.fail()) {
      return result;
    }

    if (!checkMore || fromTick == 0) {
      // all done, return successful
      return {TRI_ERROR_NO_ERROR};
    }

    // more data to retrieve, adaptively increase chunksize
    if (chunkSize < job.options.maxChunkSize) {
      chunkSize = static_cast<uint64_t>(chunkSize * 1.5);
      if (chunkSize > job.options.maxChunkSize) {
        chunkSize = job.options.maxChunkSize;
      }
    }
  }

  // should never get here, but need to make compiler play nice
  TRI_ASSERT(false);
  return {TRI_ERROR_INTERNAL};
}

/// @brief process a single job from the queue
void processJob(arangodb::httpclient::SimpleHttpClient& client, arangodb::DumpFeature::DumpJob& job) {
  arangodb::Result res;
  try {
    res = job.run(client);
  } catch (arangodb::basics::Exception const& ex) {
    res.reset(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    res.reset(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    res.reset(TRI_ERROR_INTERNAL, "unknown exception");
  }

  if (res.fail()) {
    job.feature.reportError(res);
  }
}

}  // namespace

namespace arangodb {
// job base class
DumpFeature::DumpJob::DumpJob(
    ManagedDirectory& directory, DumpFeature& feature,
    Options const& options, maskings::Maskings* maskings,
    Stats& stats, VPackSlice collectionInfo)
    : directory{directory}, 
      feature{feature}, 
      options{options}, 
      maskings{maskings}, 
      stats{stats},
      collectionInfo{collectionInfo} {
  
  // extract parameters about the individual collection
  TRI_ASSERT(collectionInfo.isObject());
  VPackSlice parameters = collectionInfo.get("parameters");
  TRI_ASSERT(parameters.isObject());

  // extract basic info about the collection
  int type = arangodb::basics::VelocyPackHelper::getNumericValue<int>(
        parameters, StaticStrings::DataSourceType.c_str(), 2);

  collectionName =
      arangodb::basics::VelocyPackHelper::getStringValue(parameters, StaticStrings::DataSourceName, "");
  TRI_ASSERT(!collectionName.empty());
  collectionType = (type == 2 ? "document" : "edge");
}

DumpFeature::DumpJob::~DumpJob() = default;

DumpFeature::DumpCollectionJob::DumpCollectionJob(
    ManagedDirectory& directory, DumpFeature& feature,
    Options const& options, maskings::Maskings* maskings,
    Stats& stats, VPackSlice collectionInfo,
    uint64_t batchId)
    : DumpJob(directory, feature, options, maskings, stats, collectionInfo),
      batchId(batchId) {}

DumpFeature::DumpCollectionJob::~DumpCollectionJob() = default;

Result DumpFeature::DumpCollectionJob::run(arangodb::httpclient::SimpleHttpClient& client) {
  Result res;
  
  if (options.progress) {
    LOG_TOPIC("a9ec1", INFO, arangodb::Logger::DUMP)
        << "# Dumping collection '" << collectionName << "'...";
  }

  bool dumpStructure = true;
  bool dumpData = options.dumpData;

  if (maskings != nullptr) {
    dumpStructure = maskings->shouldDumpStructure(collectionName);
  }
  if (dumpData && maskings != nullptr) {
    dumpData = maskings->shouldDumpData(collectionName);
  }

  if (!dumpStructure && !dumpData) {
    return res;
  }

  // prep hex string of collection name
  std::string const hexString(arangodb::rest::SslInterface::sslMD5(collectionName));
    
  ++stats.totalCollections;

  if (dumpStructure) {
    // save meta data
    auto file = directory.writableFile(
        collectionName + (options.clusterMode ? "" : ("_" + hexString)) + ".structure.json",
        true /*overwrite*/, 0, false /*gzipOk*/);
    if (!::fileOk(file.get())) {
      return ::fileError(file.get(), true);
    }

    VPackBuilder excludes;
    {  // { parameters: { shadowCollections: null } }
      VPackObjectBuilder object(&excludes);
      {
        VPackObjectBuilder subObject(&excludes, "parameters");
        subObject->add("shadowCollections", VPackSlice::nullSlice());
      }
    }

    VPackBuilder collectionWithExcludedParametersBuilder =
        VPackCollection::merge(collectionInfo, excludes.slice(), true, true);

    std::string const newCollectionInfo =
        collectionWithExcludedParametersBuilder.slice().toJson();

    file->write(newCollectionInfo.data(), newCollectionInfo.size());
    if (file->status().fail()) {
      // close file and bail out
      res = file->status();
    }
  }

  if (res.ok()) {
    // always create the file so that arangorestore does not complain
    auto file = directory.writableFile(collectionName + "_" + hexString + ".data.json",
                                       true /*overwrite*/, 0, true /*gzipOk*/);
    if (!::fileOk(file.get())) {
      return ::fileError(file.get(), true);
    }

    if (dumpData) {
      // save the actual data
      if (options.clusterMode) {
        // multiple shards may write to the same outfile, so turn the unique_ptr into a
        // shared_ptr here
        auto sharedFile = std::shared_ptr<arangodb::ManagedDirectory::File>(file.release());

        VPackSlice parameters = collectionInfo.get("parameters");
        VPackSlice shards = parameters.get("shards");

        // Iterate over the Map of shardId to server list
        for (auto const it : VPackObjectIterator(shards)) {
          // extract shard name
          TRI_ASSERT(it.key.isString());
          std::string shardName = it.key.copyString();

          // extract dbserver id
          if (!it.value.isArray() || it.value.length() == 0 || !it.value[0].isString()) {
            return {TRI_ERROR_BAD_PARAMETER, "unexpected value for 'shards' attribute"};
          }

          std::string server = it.value[0].copyString();

          // create one new job per shard
          auto dumpJob =
            std::make_unique<arangodb::DumpFeature::DumpShardJob>(directory, feature, options, maskings, stats,
                                                                  collectionInfo, shardName, server, sharedFile);
          feature.taskQueue().queueJob(std::move(dumpJob));
        }

        TRI_ASSERT(res.ok());
      } else {
        // keep the batch alive
        ::extendBatch(client, "", batchId);

        // do the hard work in another function...
        res = ::dumpCollection(client, *this, *file, collectionName, "", batchId, options.tickStart, options.tickEnd);
      }
    }
  }
  
  return res;
}

DumpFeature::DumpShardJob::DumpShardJob(
    ManagedDirectory& directory, DumpFeature& feature,
    Options const& options, maskings::Maskings* maskings,
    Stats& stats, VPackSlice collectionInfo,
    std::string const& shardName,
    std::string const& server,
    std::shared_ptr<ManagedDirectory::File> file)
    : DumpJob(directory, feature, options, maskings, stats, collectionInfo),
      shardName(shardName),
      server(server),
      file(file) {}

DumpFeature::DumpShardJob::~DumpShardJob() = default;

Result DumpFeature::DumpShardJob::run(arangodb::httpclient::SimpleHttpClient& client) {
  if (options.progress) {
    LOG_TOPIC("a27be", INFO, arangodb::Logger::DUMP)
        << "# Dumping shard '" << shardName << "' from DBserver '" << server
        << "' ...";
  }

  // make sure we have a batch on this dbserver
  auto [res, batchId] = ::startBatch(client, server);
  if (res.ok()) {
    // do the hard work elsewhere
    res = ::dumpCollection(client, *this, *file, shardName, server, batchId, 0, UINT64_MAX);
    ::endBatch(client, server, batchId);
  }

  return res;
}

DumpFeature::DumpFeature(application_features::ApplicationServer& server, int& exitCode)
    : ApplicationFeature(server, DumpFeature::featureName()),
      _clientManager{server, Logger::DUMP},
      _clientTaskQueue{server, ::processJob},
      _exitCode{exitCode} {
  requiresElevatedPrivileges(false);
  setOptional(false);
  startsAfter<application_features::BasicFeaturePhaseClient>();

  using arangodb::basics::FileUtils::buildFilename;
  using arangodb::basics::FileUtils::currentDirectory;
  _options.outputPath = buildFilename(currentDirectory().result(), "dump");
}

std::string DumpFeature::featureName() { return ::FeatureName; }

void DumpFeature::collectOptions(std::shared_ptr<options::ProgramOptions> options) {
  using arangodb::options::BooleanParameter;
  using arangodb::options::StringParameter;
  using arangodb::options::UInt32Parameter;
  using arangodb::options::UInt64Parameter;
  using arangodb::options::VectorParameter;

  options->addOption(
      "--collection",
      "restrict to collection name (can be specified multiple times)",
      new VectorParameter<StringParameter>(&_options.collections));

  options->addOption("--initial-batch-size",
                     "initial size for individual data batches (in bytes)",
                     new UInt64Parameter(&_options.initialChunkSize));

  options->addOption("--batch-size",
                     "maximum size for individual data batches (in bytes)",
                     new UInt64Parameter(&_options.maxChunkSize));

  options->addOption(
      "--threads",
      "maximum number of collections to process in parallel. From v3.4.0",
      new UInt32Parameter(&_options.threadCount));

  options->addOption("--dump-data", "dump collection data",
                     new BooleanParameter(&_options.dumpData));

  options->addOption(
      "--all-databases", "dump data of all databases",
      new BooleanParameter(&_options.allDatabases))
      .setIntroducedIn(30500);

  options->addOption(
      "--force", "continue dumping even in the face of some server-side errors",
      new BooleanParameter(&_options.force));

  options->addOption("--ignore-distribute-shards-like-errors",
                     "continue dump even if sharding prototype collection is "
                     "not backed up along",
                     new BooleanParameter(&_options.ignoreDistributeShardsLikeErrors));

  options->addOption("--include-system-collections",
                     "include system collections",
                     new BooleanParameter(&_options.includeSystemCollections));

  options->addOption("--output-directory", "output directory",
                     new StringParameter(&_options.outputPath));

  options->addOption("--overwrite", "overwrite data in output directory",
                     new BooleanParameter(&_options.overwrite));

  options->addOption("--progress", "show progress",
                     new BooleanParameter(&_options.progress));
  
  options->addOption("--envelope", "wrap each document into a {type, data} envelope "
                     "(this is required from compatibility with v3.7 and before)",
                     new BooleanParameter(&_options.useEnvelope))
                     .setIntroducedIn(30800);

  options->addOption("--tick-start", "only include data after this tick",
                     new UInt64Parameter(&_options.tickStart));

  options->addOption("--tick-end", "last tick to be included in data dump",
                     new UInt64Parameter(&_options.tickEnd));

  options
      ->addOption("--maskings", "file with maskings definition",
                  new StringParameter(&_options.maskingsFile))
      .setIntroducedIn(30322)
      .setIntroducedIn(30402);

  options->addOption("--compress-output",
                     "compress files containing collection contents using gzip format (not compatible with encryption)",
                     new BooleanParameter(&_options.useGzip))
                     .setIntroducedIn(30406)
                     .setIntroducedIn(30500);
}

void DumpFeature::validateOptions(std::shared_ptr<options::ProgramOptions> options) {
  auto const& positionals = options->processingResult()._positionals;
  size_t n = positionals.size();

  if (1 == n) {
    _options.outputPath = positionals[0];
  } else if (1 < n) {
    LOG_TOPIC("a62e0", FATAL, arangodb::Logger::DUMP)
        << "expecting at most one directory, got " +
               arangodb::basics::StringUtils::join(positionals, ", ");
    FATAL_ERROR_EXIT();
  }

  // clamp chunk values to allowed ranges
  _options.initialChunkSize =
      boost::algorithm::clamp(_options.initialChunkSize, ::MinChunkSize, ::MaxChunkSize);
  _options.maxChunkSize =
      boost::algorithm::clamp(_options.maxChunkSize, _options.initialChunkSize, ::MaxChunkSize);

  if (_options.tickEnd < _options.tickStart) {
    LOG_TOPIC("25a0a", FATAL, arangodb::Logger::DUMP)
        << "invalid values for --tick-start or --tick-end";
    FATAL_ERROR_EXIT();
  }

  if (options->processingResult().touched("server.database") &&
      _options.allDatabases) {
    LOG_TOPIC("17e2b", FATAL, arangodb::Logger::DUMP)
        << "cannot use --server.database and --all-databases at the same time";
    FATAL_ERROR_EXIT();
  }

  // trim trailing slash from path because it may cause problems on ...
  // Windows
  if (!_options.outputPath.empty() && _options.outputPath.back() == TRI_DIR_SEPARATOR_CHAR) {
    TRI_ASSERT(_options.outputPath.size() > 0);
    _options.outputPath.pop_back();
  }
  TRI_NormalizePath(_options.outputPath);

  uint32_t clamped =
      boost::algorithm::clamp(_options.threadCount, 1,
                              4 * static_cast<uint32_t>(NumberOfCores::getValue()));
  if (_options.threadCount != clamped) {
    LOG_TOPIC("0460e", WARN, Logger::DUMP) << "capping --threads value to " << clamped;
    _options.threadCount = clamped;
  }
}

// dump data from cluster via a coordinator
Result DumpFeature::runClusterDump(httpclient::SimpleHttpClient& client,
                                   std::string const& dbName) {
  // get the cluster inventory
  std::string const url = "/_api/replication/clusterInventory?includeSystem=" +
                          std::string(_options.includeSystemCollections ? "true" : "false");

  return runDump(client, url, dbName, 0);
}

// dump data from single server
Result DumpFeature::runSingleDump(httpclient::SimpleHttpClient& client,
                                  std::string const& dbName) {
  Result res;
  uint64_t batchId;
  std::tie(res, batchId) = ::startBatch(client, "");
  if (res.fail()) {
    return res;
  }
  TRI_DEFER(::endBatch(client, "", batchId));

  // get the cluster inventory
  std::string const url = "/_api/replication/inventory?includeSystem=" +
                          std::string(_options.includeSystemCollections ? "true" : "false") +
                          "&includeFoxxQueues=" + 
                          std::string(_options.includeSystemCollections ? "true" : "false") +
                          "&batchId=" + basics::StringUtils::itoa(batchId);

  return runDump(client, url, dbName, batchId);
}

Result DumpFeature::runDump(httpclient::SimpleHttpClient& client,
                            std::string const& baseUrl,
                            std::string const& dbName,
                            uint64_t batchId) {
  std::unique_ptr<httpclient::SimpleHttpResult> response(
      client.request(rest::RequestType::GET, baseUrl, nullptr, 0));
  auto check = ::checkHttpResponse(client, response);
  if (check.fail()) {
    LOG_TOPIC("eb7f4", ERR, arangodb::Logger::DUMP)
        << "An error occurred while fetching inventory: " << check.errorMessage();
    return check;
  }

  // parse the inventory vpack body
  std::shared_ptr<VPackBuilder> parsedBody;
  try {
    parsedBody = response->getBodyVelocyPack();
  } catch (...) {
    return ::ErrorMalformedJsonResponse;
  }
  VPackSlice const body = parsedBody->slice();
  if (!body.isObject()) {
    return ::ErrorMalformedJsonResponse;
  }
  
  // use tick provided by server if user did not specify one
  if (_options.tickEnd == 0 && !_options.clusterMode) {
    uint64_t tick = basics::VelocyPackHelper::stringUInt64(body, "tick");
    _options.tickEnd = tick;
  }

  // parse collections array
  VPackSlice const collections = body.get("collections");
  if (!collections.isArray()) {
    return ::ErrorMalformedJsonResponse;
  }

  // get the view list
  VPackSlice views = body.get("views");
  if (!views.isArray()) {
    views = VPackSlice::emptyArraySlice();
  }

  // Step 1. Store view definition files
  Result res = storeDumpJson(body, dbName);
  if (res.fail()) {
    return res;
  }

  // Step 2. Store view definition files
  res = storeViews(views);
  if (res.fail()) {
    return res;
  }
  
  // create a lookup table for collections
  std::map<std::string, arangodb::velocypack::Slice> restrictList;
  for (auto const& name : _options.collections) {
    if (!name.empty() && name[0] == '_') {
      // if the user explictly asked for dumping certain collections, toggle the system collection flag automatically
      _options.includeSystemCollections = true;
    }
    
    restrictList.emplace(name, arangodb::velocypack::Slice::noneSlice());
  }
  // restrictList now contains all collections the user has requested (can be empty)

  // Step 3. iterate over collections
  for (auto const& collection : VPackArrayIterator(collections)) {
    // extract parameters about the individual collection
    if (!collection.isObject()) {
      return ::ErrorMalformedJsonResponse;
    }
    VPackSlice const parameters = collection.get("parameters");

    if (!parameters.isObject()) {
      return ::ErrorMalformedJsonResponse;
    }

    // extract basic info about the collection
    uint64_t const cid = basics::VelocyPackHelper::extractIdValue(parameters);
    std::string const name =
        arangodb::basics::VelocyPackHelper::getStringValue(parameters, StaticStrings::DataSourceName, "");
    bool const deleted = arangodb::basics::VelocyPackHelper::getBooleanValue(
        parameters, StaticStrings::DataSourceDeleted.c_str(), false);
  
    // simple filtering
    if (cid == 0 || name.empty()) {
      return ::ErrorMalformedJsonResponse;
    }
    if (deleted) {
      continue;
    }
    if (name[0] == '_' && !_options.includeSystemCollections) {
      continue;
    }
    
    // filter by specified names
    if (!_options.collections.empty() && restrictList.find(name) == restrictList.end()) {
      // collection name not in list
      continue;
    }

    if (_options.clusterMode && isIgnoredHiddenEnterpriseCollection(_options, name)) {
      continue;
    }
    
    // verify distributeShardsLike info
    if (!_options.ignoreDistributeShardsLikeErrors) {
      std::string prototypeCollection =
          basics::VelocyPackHelper::getStringValue(parameters,
                                                   StaticStrings::DistributeShardsLike, "");

      if (!prototypeCollection.empty() && !_options.collections.empty()) {
        if (std::find(_options.collections.begin(), _options.collections.end(),
                      prototypeCollection) == _options.collections.end()) {
          return {
              TRI_ERROR_INTERNAL,
              std::string("Collection ") + name +
                  "'s shard distribution is based on that of collection " + prototypeCollection +
                  ", which is not dumped along. You may dump the collection "
                  "regardless of the missing prototype collection by using "
                  "the "
                  "--ignore-distribute-shards-like-errors parameter."};
        }
      }
    }

    restrictList[name] = collection;
  }
  
  // now check if at least one of the specified collections was found
  if (!_options.collections.empty() &&
      std::all_of(restrictList.begin(), restrictList.end(), [](auto const& it) {
        return it.second.isNone();
      })) {
    LOG_TOPIC("11523", FATAL, arangodb::Logger::DUMP)
        << "None of the requested collections were found in the database";
    FATAL_ERROR_EXIT();
  }


  for (auto const& [name, collectionInfo] : restrictList) {
    if (collectionInfo.isNone()) {
      LOG_TOPIC("e650c", WARN, arangodb::Logger::DUMP)
          << "Requested collection '" << name << "' not found in database";
      continue;
    }

    // queue job to actually dump collection
    auto dumpJob = std::make_unique<DumpCollectionJob>(*_directory, *this, _options,
                                                       _maskings.get(), _stats, collectionInfo,
                                                       batchId); 
    _clientTaskQueue.queueJob(std::move(dumpJob));
  }

  // wait for all jobs to finish, then check for errors
  _clientTaskQueue.waitForIdle();
  {
    MUTEX_LOCKER(lock, _workerErrorLock);
    if (!_workerErrors.empty()) {
      return _workerErrors.front();
    }
  }

  return {};
}

Result DumpFeature::storeDumpJson(VPackSlice body, std::string const& dbName) const {
  // read the server's max tick value
  std::string const tickString =
      basics::VelocyPackHelper::getStringValue(body, "tick", "");
  if (tickString == "") {
    return ::ErrorMalformedJsonResponse;
  }
  LOG_TOPIC("e4134", INFO, Logger::DUMP) << "Last tick provided by server is: " << tickString;

  try {
    VPackBuilder meta;
    meta.openObject();
    meta.add("database", VPackValue(dbName));
    meta.add("lastTickAtDumpStart", VPackValue(tickString));
    meta.add("useEnvelope", VPackValue(_options.useEnvelope));
    auto props = body.get("properties");
    if (props.isObject()) {
      meta.add("properties", props);
    }
    meta.close();

    // save last tick in file
    auto file = _directory->writableFile("dump.json", true, 0, false);
    if (!::fileOk(file.get())) {
      return ::fileError(file.get(), true);
    }

    std::string const metaString = meta.slice().toJson();
    file->write(metaString.data(), metaString.size());
    if (file->status().fail()) {
      return file->status();
    }
  } catch (basics::Exception const& ex) {
    return {ex.code(), ex.what()};
  } catch (std::exception const& ex) {
    return {TRI_ERROR_INTERNAL, ex.what()};
  } catch (...) {
    return {TRI_ERROR_OUT_OF_MEMORY, "out of memory"};
  }
  return {};
}

Result DumpFeature::storeViews(VPackSlice const& views) const {
  for (VPackSlice view : VPackArrayIterator(views)) {
    auto nameSlice = view.get(StaticStrings::DataSourceName);
    if (!nameSlice.isString() || nameSlice.getStringLength() == 0) {
      continue;  // ignore
    }

    try {
      std::string fname = nameSlice.copyString();
      fname.append(".view.json");
      // save last tick in file
      auto file = _directory->writableFile(fname, true, 0, false);
      if (!::fileOk(file.get())) {
        return ::fileError(file.get(), true);
      }

      std::string const viewString = view.toJson();
      file->write(viewString.c_str(), viewString.size());
      if (file->status().fail()) {
        return file->status();
      }
    } catch (basics::Exception const& ex) {
      return {ex.code(), ex.what()};
    } catch (std::exception const& ex) {
      return {TRI_ERROR_INTERNAL, ex.what()};
    } catch (...) {
      return {TRI_ERROR_OUT_OF_MEMORY, "out of memory"};
    }
  }
  return {};
}

void DumpFeature::reportError(Result const& error) {
  try {
    MUTEX_LOCKER(lock, _workerErrorLock);
    _workerErrors.emplace(error);
    _clientTaskQueue.clearQueue();
  } catch (...) {
  }
}
  
ClientTaskQueue<DumpFeature::DumpJob>& DumpFeature::taskQueue() {
  return _clientTaskQueue;
}

void DumpFeature::start() {
  if (!_options.maskingsFile.empty()) {
    maskings::MaskingsResult m = maskings::Maskings::fromFile(_options.maskingsFile);

    if (m.status != maskings::MaskingsResult::VALID) {
      LOG_TOPIC("cabd7", FATAL, Logger::CONFIG)
          << m.message << " in maskings file '" << _options.maskingsFile << "'";
      FATAL_ERROR_EXIT();
    }

    _maskings = std::move(m.maskings);
  }

  _exitCode = EXIT_SUCCESS;

  // generate a fake client id that we send to the server
  // TODO: convert this into a proper string "arangodump-<numeric id>"
  // in the future, if we are sure the server is an ArangoDB 3.5 or
  // higher
  ::clientId = std::to_string(RandomGenerator::interval(static_cast<uint64_t>(0x0000FFFFFFFFFFFFULL)));

  double const start = TRI_microtime();

  // set up the output directory, not much else
  _directory = std::make_unique<ManagedDirectory>(server(), _options.outputPath,
                                                  !_options.overwrite, true,
                                                  _options.useGzip);
  if (_directory->status().fail()) {
    switch (_directory->status().errorNumber()) {
      case TRI_ERROR_FILE_EXISTS:
        LOG_TOPIC("efed0", FATAL, Logger::DUMP) << "cannot write to output directory '"
                                       << _options.outputPath << "'";
        break;
      case TRI_ERROR_CANNOT_OVERWRITE_FILE:
        LOG_TOPIC("bd7fe", FATAL, Logger::DUMP)
            << "output directory '" << _options.outputPath
            << "' already exists. use \"--overwrite true\" to "
               "overwrite data in it";
        break;
      default:
        LOG_TOPIC("8f227", ERR, Logger::DUMP) << _directory->status().errorMessage();
        break;
    }
    FATAL_ERROR_EXIT();
  }

  // get database name to operate on
  auto& client = server().getFeature<HttpEndpointProvider, ClientFeature>();

  // get a client to use in main thread
  auto httpClient = _clientManager.getConnectedClient(_options.force, true, true);

  // check if we are in cluster or single-server mode
  Result result{TRI_ERROR_NO_ERROR};
  std::string role;
  std::tie(result, role) = _clientManager.getArangoIsCluster(*httpClient);
  _options.clusterMode = (role == "COORDINATOR");
  if (result.fail()) {
    LOG_TOPIC("8ba2f", FATAL, arangodb::Logger::DUMP)
        << "Error: could not detect ArangoDB instance type: " << result.errorMessage();
    FATAL_ERROR_EXIT();
  }

  if (role == "DBSERVER" || role == "PRIMARY") {
    LOG_TOPIC("eeabc", WARN, arangodb::Logger::DUMP) << "You connected to a DBServer node, but operations in a cluster should be carried out via a Coordinator. This is an unsupported operation!";
  }

  // special cluster-mode parameter checks
  if (_options.clusterMode) {
    if (_options.tickStart != 0 || _options.tickEnd != 0) {
      LOG_TOPIC("38f26", ERR, Logger::DUMP)
          << "Error: cannot use tick-start or tick-end on a cluster";
      FATAL_ERROR_EXIT();
    }
  }

  // set up threads and workers
  _clientTaskQueue.spawnWorkers(_clientManager, _options.threadCount);

  if (_options.progress) {
    LOG_TOPIC("f3a1f", INFO, Logger::DUMP)
        << "Connected to ArangoDB '" << client.endpoint() << "', database: '"
        << client.databaseName() << "', username: '" << client.username() << "'";

    LOG_TOPIC("5e989", INFO, Logger::DUMP)
        << "Writing dump to output directory '" << _directory->path()
        << "' with " << _options.threadCount << " thread(s)";
  }

  // final result
  Result res;

  std::vector<std::string> databases;
  if (_options.allDatabases) {
    // get list of available databases
    std::tie(res, databases) = ::getDatabases(*httpClient);
  } else {
    // use just the single database that was specified
    databases.push_back(client.databaseName());
  }

  if (res.ok()) {
    for (auto const& db : databases) {
      if (_options.allDatabases) {
        // inject current database
        LOG_TOPIC("4af42", INFO, Logger::DUMP) << "Dumping database '" << db << "'";
        client.setDatabaseName(db);
        httpClient = _clientManager.getConnectedClient(_options.force, false, true);

        _directory = std::make_unique<ManagedDirectory>(
            server(), arangodb::basics::FileUtils::buildFilename(_options.outputPath, db),
            true, true, _options.useGzip);

        if (_directory->status().fail()) {
          res = _directory->status();
          LOG_TOPIC("94201", ERR, Logger::DUMP) << _directory->status().errorMessage();
          break;
        }
      }

      try {
        if (_options.clusterMode) {
          res = runClusterDump(*httpClient, db);
        } else {
          res = runSingleDump(*httpClient, db);
        }
      } catch (basics::Exception const& ex) {
        LOG_TOPIC("771d0", ERR, Logger::DUMP) << "caught exception: " << ex.what();
        res = {ex.code(), ex.what()};
      } catch (std::exception const& ex) {
        LOG_TOPIC("ad866", ERR, Logger::DUMP) << "caught exception: " << ex.what();
        res = {TRI_ERROR_INTERNAL, ex.what()};
      } catch (...) {
        LOG_TOPIC("7d8c3", ERR, Logger::DUMP) << "caught unknown exception";
        res = {TRI_ERROR_INTERNAL};
      }

      if (res.fail() && !_options.force) {
        break;
      }
    }
  }

  if (res.fail()) {
    LOG_TOPIC("f7ff5", ERR, Logger::DUMP) << "An error occurred: " << res.errorMessage();
    _exitCode = EXIT_FAILURE;
  }

  if (_options.progress) {
    double totalTime = TRI_microtime() - start;

    if (_options.dumpData) {
      LOG_TOPIC("66c0e", INFO, Logger::DUMP)
          << "Processed " << _stats.totalCollections.load()
          << " collection(s) in " << Logger::FIXED(totalTime, 6) << " s,"
          << " wrote " << _stats.totalWritten.load() << " byte(s) into datafiles, sent "
          << _stats.totalBatches.load() << " batch(es)";
    } else {
      LOG_TOPIC("aaa17", INFO, Logger::DUMP)
          << "Processed " << _stats.totalCollections.load()
          << " collection(s) in " << Logger::FIXED(totalTime, 6) << " s";
    }
  }
}

}  // namespace arangodb
