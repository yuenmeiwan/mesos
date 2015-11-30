// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <sys/wait.h>

#include <string>
#include <tuple>
#include <vector>

#include <process/check.hpp>
#include <process/collect.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/mkdir.hpp>

#include "uri/fetchers/curl.hpp"

using namespace process;

using std::string;
using std::vector;

namespace mesos {
namespace uri {

// Forward declarations.
static Future<Nothing> _fetch(const Future<std::tuple<
    Future<Option<int>>,
    Future<string>,
    Future<string>>>& future);


Try<Owned<Fetcher::Plugin>> CurlFetcherPlugin::create()
{
  // TODO(jieyu): Make sure curl is available.

  return Owned<Fetcher::Plugin>(new CurlFetcherPlugin());
}


Future<Nothing> CurlFetcherPlugin::fetch(
    const URI& uri,
    const string& directory)
{
  // TODO(jieyu): Validate the given URI.

  if (!uri.has_path()) {
    return Failure("URI path is not specified");
  }

  if (!os::exists(directory)) {
    Try<Nothing> mkdir = os::mkdir(directory);
    if (mkdir.isError()) {
      return Failure(
          "Failed to create directory '" +
          directory + "': " + mkdir.error());
    }
  }

  // TODO(jieyu): Allow user to specify the name of the output file.
  const string output = path::join(directory, Path(uri.path()).basename());

  const vector<string> argv = {
    "curl",
    "-s",                 // Don’t show progress meter or error messages.
    "-S",                 // Makes curl show an error message if it fails.
    "-L",                 // Follow HTTP 3xx redirects.
    "-w", "%{http_code}", // Display HTTP response code on stdout.
    "-o", output,         // Write output to the file.
    strings::trim(stringify(uri))
  };

  Try<Subprocess> s = subprocess(
      "curl",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to exec the curl subprocess: " + s.error());
  }

  return await(
      s.get().status(),
      io::read(s.get().out().get()),
      io::read(s.get().err().get()))
    .then(_fetch);
}


static Future<Nothing> _fetch(const Future<std::tuple<
    Future<Option<int>>,
    Future<string>,
    Future<string>>>& future)
{
  CHECK_READY(future);

  Future<Option<int>> status = std::get<0>(future.get());
  if (!status.isReady()) {
    return Failure(
        "Failed to get the exit status of the curl subprocess: " +
        (status.isFailed() ? status.failure() : "discarded"));
  }

  if (status->isNone()) {
    return Failure("Failed to reap the curl subprocess");
  }

  if (status->get() != 0) {
    Future<string> error = std::get<2>(future.get());
    if (!error.isReady()) {
      return Failure(
          "Failed to perform 'curl'. Reading stderr failed: " +
          (error.isFailed() ? error.failure() : "discarded"));
    }

    return Failure("Failed to perform 'curl': " + error.get());
  }

  Future<string> output = std::get<1>(future.get());
  if (!output.isReady()) {
    return Failure(
        "Failed to read stdout from 'curl': " +
        (output.isFailed() ? output.failure() : "discarded"));
  }

  // Parse the output and get the HTTP response code.
  Try<int> code = numify<int>(output.get());
  if (code.isError()) {
    return Failure("Unexpected output from 'curl': " + output.get());
  }

  if (code.get() != http::Status::OK) {
    return Failure(
        "Unexpected HTTP response code: " +
        http::Status::string(code.get()));
  }

  return Nothing();
}

} // namespace uri {
} // namespace mesos {
