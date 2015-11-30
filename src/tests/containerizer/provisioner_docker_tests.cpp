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

#include <utility>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <stout/duration.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>

#include <process/address.hpp>
#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>
#include <process/subprocess.hpp>

#include <process/ssl/gtest.hpp>

#include "slave/containerizer/mesos/provisioner/docker/metadata_manager.hpp"
#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"
#include "slave/containerizer/mesos/provisioner/docker/puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/registry_client.hpp"
#include "slave/containerizer/mesos/provisioner/docker/registry_puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/spec.hpp"
#include "slave/containerizer/mesos/provisioner/docker/store.hpp"
#include "slave/containerizer/mesos/provisioner/docker/token_manager.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using std::list;
using std::map;
using std::pair;
using std::string;
using std::vector;

using process::Clock;
using process::Future;
using process::Owned;

using process::network::Socket;

using namespace process;
using namespace mesos::internal::slave;
using namespace mesos::internal::slave::docker;
using namespace mesos::internal::slave::docker::paths;
using namespace mesos::internal::slave::docker::registry;

namespace mesos {
namespace internal {
namespace tests {


TEST(DockerUtilsTest, ParseImageName)
{
  slave::docker::Image::Name name;

  name = parseImageName("library/busybox");
  EXPECT_FALSE(name.has_registry());
  EXPECT_EQ("library/busybox", name.repository());
  EXPECT_EQ("latest", name.tag());

  name = parseImageName("busybox");
  EXPECT_FALSE(name.has_registry());
  EXPECT_EQ("busybox", name.repository());
  EXPECT_EQ("latest", name.tag());

  name = parseImageName("library/busybox:tag");
  EXPECT_FALSE(name.has_registry());
  EXPECT_EQ("library/busybox", name.repository());
  EXPECT_EQ("tag", name.tag());

  // Note that the digest is stored as a tag.
  name = parseImageName(
      "library/busybox"
      "@sha256:bc8813ea7b3603864987522f02a7"
      "6101c17ad122e1c46d790efc0fca78ca7bfb");
  EXPECT_FALSE(name.has_registry());
  EXPECT_EQ("library/busybox", name.repository());
  EXPECT_EQ("sha256:bc8813ea7b3603864987522f02a7"
            "6101c17ad122e1c46d790efc0fca78ca7bfb",
            name.tag());

  name = parseImageName("registry.io/library/busybox");
  EXPECT_EQ("registry.io", name.registry());
  EXPECT_EQ("library/busybox", name.repository());
  EXPECT_EQ("latest", name.tag());

  name = parseImageName("registry.io/library/busybox:tag");
  EXPECT_EQ("registry.io", name.registry());
  EXPECT_EQ("library/busybox", name.repository());
  EXPECT_EQ("tag", name.tag());

  name = parseImageName("registry.io:80/library/busybox:tag");
  EXPECT_EQ("registry.io:80", name.registry());
  EXPECT_EQ("library/busybox", name.repository());
  EXPECT_EQ("tag", name.tag());

  // Note that the digest is stored as a tag.
  name = parseImageName(
      "registry.io:80/library/busybox"
      "@sha256:bc8813ea7b3603864987522f02a7"
      "6101c17ad122e1c46d790efc0fca78ca7bfb");
  EXPECT_EQ("registry.io:80", name.registry());
  EXPECT_EQ("library/busybox", name.repository());
  EXPECT_EQ("sha256:bc8813ea7b3603864987522f02a7"
            "6101c17ad122e1c46d790efc0fca78ca7bfb",
            name.tag());
}


/**
 * Provides token operations and defaults.
 */
class TokenHelper {
protected:
  const string hdrBase64 = base64::encode(
    "{ \
      \"alg\":\"ES256\", \
      \"typ\":\"JWT\", \
      \"x5c\":[\"test\"] \
    }");

  string getClaimsBase64() const
  {
    return base64::encode(claimsJsonString);
  }

  string getTokenString() const
  {
    return  hdrBase64 + "." + getClaimsBase64() + "." + signBase64;
  }

  string getDefaultTokenString()
  {
    // Construct response and send(server side).
    const double expirySecs = Clock::now().secs() + Days(365).secs();

    claimsJsonString =
      "{\"access\" \
        :[ \
        { \
          \"type\":\"repository\", \
            \"name\":\"library/busybox\", \
            \"actions\":[\"pull\"]}], \
            \"aud\":\"registry.docker.io\", \
            \"exp\":" + stringify(expirySecs) + ", \
            \"iat\":1438887168, \
            \"iss\":\"auth.docker.io\", \
            \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
            \"nbf\":1438887166, \
            \"sub\":\"\" \
        }";

    return getTokenString();
  }

  const string signBase64 = base64::encode("{\"\"}");
  string claimsJsonString;
};


/**
 * Fixture for testing TokenManager component.
 */
class RegistryTokenTest : public TokenHelper, public ::testing::Test
{};


// Tests JSON Web Token parsing for a valid token string.
TEST_F(RegistryTokenTest, ValidToken)
{
  const double expirySecs = Clock::now().secs() + Days(365).secs();

  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"exp\":" + stringify(expirySecs) + ", \
          \"iat\":1438887168, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":1438887166, \
          \"sub\":\"\" \
         }";

  Try<Token> token = Token::create(getTokenString());

  ASSERT_SOME(token);
}


// Tests JSON Web Token parsing for a token string with expiration date in the
// past.
TEST_F(RegistryTokenTest, ExpiredToken)
{
  const double expirySecs = Clock::now().secs() - Days(365).secs();

  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"exp\":" + stringify(expirySecs) + ", \
          \"iat\":1438887166, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":1438887166, \
          \"sub\":\"\" \
         }";

  Try<Token> token = Token::create(getTokenString());

  EXPECT_ERROR(token);
}


// Tests JSON Web Token parsing for a token string with no expiration date.
TEST_F(RegistryTokenTest, NoExpiration)
{
  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"iat\":1438887166, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":1438887166, \
          \"sub\":\"\" \
      }";

  const Try<Token> token = Token::create(getTokenString());

  ASSERT_SOME(token);
}


// Tests JSON Web Token parsing for a token string with not-before date in the
// future.
TEST_F(RegistryTokenTest, NotBeforeInFuture)
{
  const double expirySecs = Clock::now().secs() + Days(365).secs();
  const double nbfSecs = Clock::now().secs() + Days(7).secs();

  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"exp\":" + stringify(expirySecs) + ", \
          \"iat\":1438887166, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":" + stringify(nbfSecs) + ", \
          \"sub\":\"\" \
         }";

  const Try<Token> token = Token::create(getTokenString());

  ASSERT_SOME(token);
  ASSERT_EQ(token.get().isValid(), false);
}


class DockerSpecTest : public ::testing::Test {};

TEST_F(DockerSpecTest, SerializeDockerManifest)
{
  JSON::Value manifest = JSON::parse(
    "{"
    "   \"name\": \"dmcgowan/test-image\","
    "   \"tag\": \"latest\","
    "   \"architecture\": \"amd64\","
    "   \"fsLayers\": ["
    "      {"
    "         \"blobSum\": "
  "\"sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\""
    "      },"
    "      {"
    "         \"blobSum\": "
  "\"sha256:cea0d2071b01b0a79aa4a05ea56ab6fdf3fafa03369d9f4eea8d46ea33c43e5f\""
    "      },"
    "      {"
    "         \"blobSum\": "
  "\"sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\""
    "      },"
    "      {"
    "         \"blobSum\": "
  "\"sha256:2a7812e636235448785062100bb9103096aa6655a8f6bb9ac9b13fe8290f66df\""
    "      }"
    "   ],"
    "   \"history\": ["
    "      {"
    "         \"v1Compatibility\": "
    "           {"
    "             \"id\": "
    "\"2ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea\","
    "             \"parent\": "
    "\"cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff\""
    "           }"
    "      },"
    "      {"
    "         \"v1Compatibility\": "
    "           {"
    "             \"id\": "
    "\"2ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea\","
    "             \"parent\": "
    "\"cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff\""
    "           }"
    "      },"
    "      {"
    "         \"v1Compatibility\": "
    "           {"
    "             \"id\": "
    "\"2ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea\","
    "             \"parent\": "
    "\"cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff\""
    "           }"
    "      },"
    "      {"
    "         \"v1Compatibility\": "
    "           {"
    "             \"id\": "
    "\"2ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea\","
    "             \"parent\": "
    "\"cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff\""
    "           }"
    "      }"
    "   ],"
    "   \"schemaVersion\": 1,"
    "   \"signatures\": ["
    "      {"
    "         \"header\": {"
    "            \"jwk\": {"
    "               \"crv\": \"P-256\","
    "               \"kid\": "
    "\"LYRA:YAG2:QQKS:376F:QQXY:3UNK:SXH7:K6ES:Y5AU:XUN5:ZLVY:KBYL\","
    "               \"kty\": \"EC\","
    "               \"x\": \"Cu_UyxwLgHzE9rvlYSmvVdqYCXY42E9eNhBb0xNv0SQ\","
    "               \"y\": \"zUsjWJkeKQ5tv7S-hl1Tg71cd-CqnrtiiLxSi6N_yc8\""
    "            },"
    "            \"alg\": \"ES256\""
    "         },"
    "         \"signature\": \"m3bgdBXZYRQ4ssAbrgj8Kjl7GNgrKQvmCSY-00yzQosKi-8"
    "UBrIRrn3Iu5alj82B6u_jNrkGCjEx3TxrfT1rig\","
    "         \"protected\": \"eyJmb3JtYXRMZW5ndGgiOjYwNjMsImZvcm1hdFRhaWwiOiJ"
    "DbjAiLCJ0aW1lIjoiMjAxNC0wOS0xMVQxNzoxNDozMFoifQ\""
    "      }"
    "   ]"
    "}").get();

  Try<JSON::Object> json = JSON::parse<JSON::Object>(stringify(manifest));
  ASSERT_SOME(json);

  Try<slave::docker::DockerImageManifest> dockerImageManifest =
    spec::parse(json.get());

  ASSERT_SOME(dockerImageManifest);

  EXPECT_EQ(dockerImageManifest.get().name(), "dmcgowan/test-image");
  EXPECT_EQ(dockerImageManifest.get().tag(), "latest");
  EXPECT_EQ(dockerImageManifest.get().architecture(), "amd64");

  EXPECT_EQ(dockerImageManifest.get().fslayers(0).blobsum(),
    "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(dockerImageManifest.get().fslayers(1).blobsum(),
    "sha256:cea0d2071b01b0a79aa4a05ea56ab6fdf3fafa03369d9f4eea8d46ea33c43e5f");
  EXPECT_EQ(dockerImageManifest.get().fslayers(2).blobsum(),
    "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(dockerImageManifest.get().fslayers(3).blobsum(),
    "sha256:2a7812e636235448785062100bb9103096aa6655a8f6bb9ac9b13fe8290f66df");

  EXPECT_EQ(dockerImageManifest.get().history(1).v1compatibility().id(),
    "2ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea");
  EXPECT_EQ(dockerImageManifest.get().history(2).v1compatibility().parent(),
    "cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff");

  EXPECT_EQ(dockerImageManifest.get().schemaversion(), 1u);

  EXPECT_EQ(dockerImageManifest.get().signatures(0).header().jwk().kid(),
    "LYRA:YAG2:QQKS:376F:QQXY:3UNK:SXH7:K6ES:Y5AU:XUN5:ZLVY:KBYL");
  EXPECT_EQ(dockerImageManifest.get().signatures(0).signature(),
    "m3bgdBXZYRQ4ssAbrgj8Kjl7GNgrKQvmCSY-00yzQosKi-8"
    "UBrIRrn3Iu5alj82B6u_jNrkGCjEx3TxrfT1rig");
}

// Test invalid JSON object, expecting an error.
TEST_F(DockerSpecTest, SerializeDockerInvalidManifest)
{
  // This is an invalid manifest. The repeated fields 'history' and 'fsLayers'
  // must be >= 1. The 'signatures' and 'schemaVersion' are not set.
  JSON::Value manifest = JSON::parse(
    "{"
    "   \"name\": \"dmcgowan/test-image\","
    "   \"tag\": \"latest\","
    "   \"architecture\": \"amd64\""
    "}").get();

  Try<JSON::Object> json = JSON::parse<JSON::Object>(stringify(manifest));
  ASSERT_SOME(json);

  Try<slave::docker::DockerImageManifest> dockerImageManifest =
    spec::parse(json.get());

  EXPECT_ERROR(dockerImageManifest);
}

// Test Manifest Validation with empty repeated 'fsLayers' field.
TEST_F(DockerSpecTest, ValidationDockerManifestFsLayersNonEmpty)
{
  JSON::Value manifest = JSON::parse(
    "{"
    "   \"name\": \"dmcgowan/test-image\","
    "   \"tag\": \"latest\","
    "   \"architecture\": \"amd64\","
    "   \"schemaVersion\": 1,"
    "   \"signatures\": ["
    "      {"
    "         \"header\": {"
    "            \"jwk\": {"
    "               \"crv\": \"P-256\","
    "               \"kid\": "
    "\"LYRA:YAG2:QQKS:376F:QQXY:3UNK:SXH7:K6ES:Y5AU:XUN5:ZLVY:KBYL\","
    "               \"kty\": \"EC\","
    "               \"x\": \"Cu_UyxwLgHzE9rvlYSmvVdqYCXY42E9eNhBb0xNv0SQ\","
    "               \"y\": \"zUsjWJkeKQ5tv7S-hl1Tg71cd-CqnrtiiLxSi6N_yc8\""
    "            },"
    "            \"alg\": \"ES256\""
    "         },"
    "         \"signature\": \"m3bgdBXZYRQ4ssAbrgj8Kjl7GNgrKQvmCSY-00yzQosKi-8"
    "UBrIRrn3Iu5alj82B6u_jNrkGCjEx3TxrfT1rig\","
    "         \"protected\": \"eyJmb3JtYXRMZW5ndGgiOjYwNjMsImZvcm1hdFRhaWwiOiJ"
    "DbjAiLCJ0aW1lIjoiMjAxNC0wOS0xMVQxNzoxNDozMFoifQ\""
    "      }"
    "   ]"
    "}").get();

  Try<JSON::Object> json = JSON::parse<JSON::Object>(stringify(manifest));
  ASSERT_SOME(json);

  Try<slave::docker::DockerImageManifest> dockerImageManifest =
    spec::parse(json.get());

  EXPECT_ERROR(dockerImageManifest);
}

// Test Manifest Validation with empty repeated 'signatures' field.
TEST_F(DockerSpecTest, ValidationDockerManifestSignaturesNonEmpty)
{
  JSON::Value manifest = JSON::parse(
    "{"
    "   \"name\": \"dmcgowan/test-image\","
    "   \"tag\": \"latest\","
    "   \"architecture\": \"amd64\","
    "   \"fsLayers\": ["
    "      {"
    "         \"blobSum\": "
  "\"sha256:2a7812e636235448785062100bb9103096aa6655a8f6bb9ac9b13fe8290f66df\""
    "      }"
    "   ],"
    "   \"schemaVersion\": 1"
    "}").get();

  Try<JSON::Object> json = JSON::parse<JSON::Object>(stringify(manifest));
  ASSERT_SOME(json);

  Try<slave::docker::DockerImageManifest> dockerImageManifest =
    spec::parse(json.get());

  EXPECT_ERROR(dockerImageManifest);
}


#ifdef USE_SSL_SOCKET

// Test suite for docker registry tests.
class RegistryClientTest : public virtual SSLTest, public TokenHelper
{
protected:
  RegistryClientTest() {}

  Try<Socket> getServer() {
    return setup_server({
        {"SSL_ENABLED", "true"},
        {"SSL_KEY_FILE", key_path().value},
        {"SSL_CERT_FILE", certificate_path().value}});
  }

  static void SetUpTestCase()
  {
    if (os::mkdir(RegistryClientTest::OUTPUT_DIR).isError()) {
      ABORT("Could not create temporary directory: " +
          RegistryClientTest::OUTPUT_DIR);
    }
  }

  static void TearDownTestCase()
  {
    SSLTest::TearDownTestCase();

    os::rmdir(RegistryClientTest::OUTPUT_DIR);
  }

  static const string OUTPUT_DIR;
};

const string RegistryClientTest::OUTPUT_DIR = "output_dir";

// Tests TokenManager for a simple token request.
TEST_F(RegistryClientTest, SimpleGetToken)
{
  Try<Socket> server = getServer();

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  // Create URL from server hostname and port.
  const process::http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  Try<Owned<TokenManager>> tokenMgr = TokenManager::create(url);
  ASSERT_SOME(tokenMgr);

  Future<Token> token =
    tokenMgr.get()->getToken(
        "registry.docker.io",
        "repository:library/busybox:pull",
        None());

  AWAIT_ASSERT_READY(socket);

  // Construct response and send(server side).
  const double expirySecs = Clock::now().secs() + Days(365).secs();

  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"exp\":" + stringify(expirySecs) + ", \
          \"iat\":1438887168, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":1438887166, \
          \"sub\":\"\" \
         }";

  const string tokenString(getTokenString());
  const string tokenResponse = "{\"token\":\"" + tokenString + "\"}";

  const string buffer =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(tokenResponse.length()) + "\r\n" +
    "\r\n" +
    tokenResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(buffer));

  AWAIT_ASSERT_READY(token);
  ASSERT_EQ(token.get().raw, tokenString);
}


// Tests TokenManager for bad token response from server.
TEST_F(RegistryClientTest, BadTokenResponse)
{
  Try<Socket> server = getServer();

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  // Create URL from server hostname and port.
  const process::http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  Try<Owned<TokenManager>> tokenMgr = TokenManager::create(url);
  ASSERT_SOME(tokenMgr);

  Future<Token> token =
    tokenMgr.get()->getToken(
        "registry.docker.io",
        "repository:library/busybox:pull",
        None());

  AWAIT_ASSERT_READY(socket);

  const string tokenString("bad token");
  const string tokenResponse = "{\"token\":\"" + tokenString + "\"}";

  const string buffer =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(tokenResponse.length()) + "\r\n" +
    "\r\n" +
    tokenResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(buffer));

  AWAIT_FAILED(token);
}


// Tests TokenManager for request to invalid server.
TEST_F(RegistryClientTest, BadTokenServerAddress)
{
  // Create an invalid URL with current time.
  const process::http::URL url("https", stringify(Clock::now().secs()), 0);

  Try<Owned<TokenManager>> tokenMgr = TokenManager::create(url);
  ASSERT_SOME(tokenMgr);

  Future<Token> token =
    tokenMgr.get()->getToken(
        "registry.docker.io",
        "repository:library/busybox:pull",
        None());

  AWAIT_FAILED(token);
}


// Tests docker registry's getManifest API.
TEST_F(RegistryClientTest, SimpleGetManifest)
{
  Try<Socket> server = getServer();

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  const process::http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  Try<Owned<RegistryClient>> registryClient =
    RegistryClient::create(url, url, None());

  ASSERT_SOME(registryClient);

  Future<DockerImageManifest> manifestResponse =
    registryClient.get()->getManifest(parseImageName("library/busybox"));

  const string unauthResponseHeaders = "Www-Authenticate: Bearer"
    " realm=\"https://auth.docker.io/token\","
    "service=" + stringify(server.get().address().get()) + ","
    "scope=\"repository:library/busybox:pull\"";

  const string unauthHttpResponse =
    string("HTTP/1.1 401 Unauthorized\r\n") +
    unauthResponseHeaders + "\r\n" +
    "\r\n";

  AWAIT_ASSERT_READY(socket);

  // Send 401 Unauthorized response for a manifest request.
  Future<string> manifestHttpRequest = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(manifestHttpRequest);
  AWAIT_ASSERT_READY(Socket(socket.get()).send(unauthHttpResponse));

  // Token response.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  Future<string> tokenRequest = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(tokenRequest);

  const string tokenResponse =
    "{\"token\":\"" + getDefaultTokenString() + "\"}";

  const string tokenHttpResponse =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(tokenResponse.length()) + "\r\n" +
    "\r\n" +
    tokenResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(tokenHttpResponse));

  // Manifest response.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  manifestHttpRequest = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(manifestHttpRequest);

  const string manifestJSON =
    "{"
    "   \"schemaVersion\": 1,"
    "   \"name\": \"library/busybox\","
    "   \"tag\": \"latest\","
    "   \"architecture\": \"amd64\","
    "   \"fsLayers\": ["
    "      {"
    "         \"blobSum\": "
  "\"sha256:a3ed95caeb02ffe68cdd9fd84406680ae93d633cb16422d00e8a7c22955b46d4\""
    "      },"
    "      {"
    "         \"blobSum\": "
  "\"sha256:1db09adb5ddd7f1a07b6d585a7db747a51c7bd17418d47e91f901bdf420abd66\""
    "      },"
    "      {"
    "         \"blobSum\": "
  "\"sha256:a3ed95caeb02ffe68cdd9fd84406680ae93d633cb16422d00e8a7c22955b46d4\""
    "      }"
    "   ],"
    "   \"history\": ["
    "      {"
    "         \"v1Compatibility\": "
    "           {"
    "             \"id\": "
    "\"1ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea\","
    "             \"parent\": "
    "\"cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff\""
    "           }"
    "      },"
    "      {"
    "         \"v1Compatibility\": "
    "           {"
    "             \"id\": "
    "\"2ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea\","
    "             \"parent\": "
    "\"cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff\""
    "           }"
    "      },"
    "      {"
    "         \"v1Compatibility\": "
    "           {"
    "             \"id\": "
    "\"3ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea\","
    "             \"parent\": "
    "\"cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff\""
    "           }"
    "      }"
    "   ],"
    "   \"signatures\": ["
    "      {"
    "         \"header\": {"
    "            \"jwk\": {"
    "               \"crv\": \"P-256\","
    "               \"kid\": "
    "\"OOI5:SI3T:LC7D:O7DX:FY6S:IAYW:WDRN:VQEM:BCFL:OIST:Q3LO:GTQQ\","
    "               \"kty\": \"EC\","
    "               \"x\": \"J2N5ePGhlblMI2cdsR6NrAG_xbNC_X7s1HRtk5GXvzM\","
    "               \"y\": \"Idr-tEBjnNnfq6_71aeXBi3Z9ah_rrE209l4wiaohk0\""
    "            },"
    "            \"alg\": \"ES256\""
    "         },"
    "         \"signature\": \"65vq57TakC_yperuhfefF4uvTbKO2L45gYGDs5bIEgO"
    "EarAs7_4dbEV5u-W7uR8gF6EDKfowUCmTq3a5vEOJ3w\","
    "         \"protected\": \"eyJmb3JtYXRMZW5ndGgiOjYwNjMsImZvcm1hdFRhaWwiOiJ"
    "DbjAiLCJ0aW1lIjoiMjAxNC0wOS0xMVQxNzoxNDozMFoifQ\""
    "      }"
    "   ]"
    "}";

  const string manifestHttpResponse =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(manifestJSON.length()) + "\r\n" +
    "Docker-Content-Digest: "
    "sha256:df9e13f36d2d5b30c16bfbf2a6110c45ebed0bfa1ea42d357651bc6c736d5322"
    + "\r\n" +
    "\r\n" +
    manifestJSON;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(manifestHttpResponse));

  AWAIT_ASSERT_READY(manifestResponse);

  EXPECT_EQ(
      manifestResponse.get().history(2).v1compatibility().id(),
      "1ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea");

  EXPECT_EQ(
      manifestResponse.get().history(1).v1compatibility().id(),
      "2ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea");

  EXPECT_EQ(
      manifestResponse.get().history(0).v1compatibility().id(),
      "3ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea");
}


// Tests docker registry's getBlob API.
TEST_F(RegistryClientTest, SimpleGetBlob)
{
  Try<Socket> server = getServer();

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  const process::http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  Try<Owned<RegistryClient>> registryClient =
    RegistryClient::create(url, url, None());

  ASSERT_SOME(registryClient);

  const Path blobPath(RegistryClientTest::OUTPUT_DIR + "/blob");

  Future<size_t> result =
    registryClient.get()->getBlob(
        parseImageName("blob"),
        "digest",
        blobPath);

  const string unauthResponseHeaders = "WWW-Authenticate: Bearer"
    " realm=\"https://auth.docker.io/token\","
    "service=" + stringify(server.get().address().get()) + ","
    "scope=\"repository:library/busybox:pull\"";

  const string unauthHttpResponse =
    string("HTTP/1.1 401 Unauthorized\r\n") +
    unauthResponseHeaders + "\r\n" +
    "\r\n";

  AWAIT_ASSERT_READY(socket);

  // Send 401 Unauthorized response.
  Future<string> blobHttpRequest = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(blobHttpRequest);
  AWAIT_ASSERT_READY(Socket(socket.get()).send(unauthHttpResponse));

  // Send token response.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  Future<string> tokenRequest = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(tokenRequest);

  const string tokenResponse =
    "{\"token\":\"" + getDefaultTokenString() + "\"}";

  const string tokenHttpResponse =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(tokenResponse.length()) + "\r\n" +
    "\r\n" +
    tokenResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(tokenHttpResponse));

  // Send redirect.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  blobHttpRequest = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(blobHttpRequest);

  const string redirectHttpResponse =
    string("HTTP/1.1 307 Temporary Redirect\r\n") +
    "Location: https://" +
    stringify(server.get().address().get()) + "\r\n" +
    "\r\n";

  AWAIT_ASSERT_READY(Socket(socket.get()).send(redirectHttpResponse));

  // Finally send blob response.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  blobHttpRequest = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(blobHttpRequest);

  const string blobResponse = stringify(Clock::now());

  const string blobHttpResponse =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(blobResponse.length()) + "\r\n" +
    "\r\n" +
    blobResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(blobHttpResponse));

  AWAIT_ASSERT_READY(result);

  Try<string> blob = os::read(blobPath);
  ASSERT_SOME(blob);
  ASSERT_EQ(blob.get(), blobResponse);
}


TEST_F(RegistryClientTest, BadRequest)
{
  Try<Socket> server = getServer();

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  const process::http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  Try<Owned<RegistryClient>> registryClient =
    RegistryClient::create(url, url, None());

  ASSERT_SOME(registryClient);

  const Path blobPath(RegistryClientTest::OUTPUT_DIR + "/blob");

  Future<size_t> result =
    registryClient.get()->getBlob(
        parseImageName("blob"),
        "digest",
        blobPath);

  const string badRequestResponse =
    "{\"errors\": [{\"message\": \"Error1\" }, {\"message\": \"Error2\"}]}";

  const string badRequestHttpResponse =
    string("HTTP/1.1 400 Bad Request\r\n") +
    "Content-Length : " + stringify(badRequestResponse.length()) + "\r\n" +
    "\r\n" +
    badRequestResponse;

  AWAIT_ASSERT_READY(socket);

  // Send 400 Bad Request.
  Future<string> blobHttpRequest = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(blobHttpRequest);
  AWAIT_ASSERT_READY(Socket(socket.get()).send(badRequestHttpResponse));

  AWAIT_FAILED(result);

  ASSERT_TRUE(strings::contains(result.failure(), "Error1"));
  ASSERT_TRUE(strings::contains(result.failure(), "Error2"));
}


// Tests docker RegistryPuller component. It simulates pulling an image layer
// from remote registry and then verifies the content saved on disk.
TEST_F(RegistryClientTest, SimpleRegistryPuller)
{
  Try<Socket> server = getServer();

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  Flags flags;
  flags.docker_registry = server.get().address().get().hostname().get();
  flags.docker_registry_port = stringify(server.get().address().get().port);
  flags.docker_auth_server = server.get().address().get().hostname().get();
  flags.docker_auth_server_port = stringify(server.get().address().get().port);

  Try<Owned<Puller>> registryPuller = RegistryPuller::create(flags);

  ASSERT_SOME(registryPuller);

  const Path registryPullerPath(RegistryClientTest::OUTPUT_DIR);

  Try<slave::docker::Image::Name> imageName =
    parseImageName("busybox");

  ASSERT_SOME(imageName);

  Future<list<pair<string, string>>> registryPullerFuture =
    registryPuller.get()->pull(imageName.get(), registryPullerPath);

  const string unauthResponseHeaders = "WWW-Authenticate: Bearer"
    " realm=\"https://auth.docker.io/token\","
    "service=" + stringify(server.get().address().get()) + ","
    "scope=\"repository:library/busybox:pull\"";

  const string unauthHttpResponse =
    string("HTTP/1.1 401 Unauthorized\r\n") +
    unauthResponseHeaders + "\r\n" +
    "\r\n";

  AWAIT_ASSERT_READY(socket);

  // Send 401 Unauthorized response for a manifest request.
  Future<string> registryPullerHttpRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(registryPullerHttpRequestFuture);
  AWAIT_ASSERT_READY(Socket(socket.get()).send(unauthHttpResponse));

  // Token response.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  Future<string> tokenRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(tokenRequestFuture);

  const string tokenResponse =
    "{\"token\":\"" + getDefaultTokenString() + "\"}";

  const string tokenHttpResponse =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(tokenResponse.length()) + "\r\n" +
    "\r\n" +
    tokenResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(tokenHttpResponse));

  // Manifest response.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  registryPullerHttpRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(registryPullerHttpRequestFuture);

  const string manifestResponse =
    "{"
    "   \"schemaVersion\": 1,"
    "   \"name\": \"library/busybox\","
    "   \"tag\": \"latest\","
    "   \"architecture\": \"amd64\","
    "   \"fsLayers\": ["
    "      {"
    "         \"blobSum\": "
  "\"sha256:a3ed95caeb02ffe68cdd9fd84406680ae93d633cb16422d00e8a7c22955b46d4\""
    "      }"
    "   ],"
    "   \"history\": ["
    "      {"
    "         \"v1Compatibility\": "
    "           {"
    "             \"id\": "
    "\"1ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea\","
    "             \"parent\": "
    "\"cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff\""
    "           }"
    "      }"
    "   ],"
    "   \"signatures\": ["
    "      {"
    "         \"header\": {"
    "            \"jwk\": {"
    "               \"crv\": \"P-256\","
    "               \"kid\": "
    "\"OOI5:SI3T:LC7D:O7DX:FY6S:IAYW:WDRN:VQEM:BCFL:OIST:Q3LO:GTQQ\","
    "               \"kty\": \"EC\","
    "               \"x\": \"J2N5ePGhlblMI2cdsR6NrAG_xbNC_X7s1HRtk5GXvzM\","
    "               \"y\": \"Idr-tEBjnNnfq6_71aeXBi3Z9ah_rrE209l4wiaohk0\""
    "            },"
    "            \"alg\": \"ES256\""
    "         },"
    "         \"signature\": \"65vq57TakC_yperuhfefF4uvTbKO2L45gYGDs5bIEgO"
    "EarAs7_4dbEV5u-W7uR8gF6EDKfowUCmTq3a5vEOJ3w\","
    "         \"protected\": \"eyJmb3JtYXRMZW5ndGgiOjYwNjMsImZvcm1hdFRhaWwiOiJ"
    "DbjAiLCJ0aW1lIjoiMjAxNC0wOS0xMVQxNzoxNDozMFoifQ\""
    "      }"
    "   ]"
    "}";

  const string manifestHttpResponse =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(manifestResponse.length()) + "\r\n" +
    "Docker-Content-Digest: "
    "sha256:df9e13f36d2d5b30c16bfbf2a6110c45ebed0bfa1ea42d357651bc6c736d5322"
    + "\r\n" +
    "\r\n" +
    manifestResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(manifestHttpResponse));

  // Redirect response.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  registryPullerHttpRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(registryPullerHttpRequestFuture);

  const string redirectHttpResponse =
    string("HTTP/1.1 307 Temporary Redirect\r\n") +
    "Location: https://" +
    stringify(server.get().address().get()) + "\r\n" +
    "\r\n";

  AWAIT_ASSERT_READY(Socket(socket.get()).send(redirectHttpResponse));

  // Prepare the blob response from the server. The blob response buffer is a
  // tarball. So we create a tarball of our test response and send that.
  const string blobFile = "blob";
  const string blobResponse = "hello docker";

  Path blobPath(path::join(registryPullerPath, blobFile));
  ASSERT_SOME(os::write(blobPath, blobResponse));

  Path blobTarPath(path::join(registryPullerPath, blobFile + ".tar"));

  vector<string> argv = {
    "tar",
    "-C",
    registryPullerPath,
    "-c",
    "-f",
    blobTarPath,
    blobFile
  };

  Try<Subprocess> s = subprocess(
      "tar",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"));
  ASSERT_SOME(s);
  AWAIT_ASSERT_READY(s.get().status());

  Try<Bytes> tarSize = os::stat::size(blobTarPath);
  ASSERT_SOME(tarSize);

  ASSERT_SOME(os::rm(blobPath));

  std::unique_ptr<char[]> tarBuffer(new char[tarSize.get().bytes()]);
  ASSERT_NE(tarBuffer.get(), nullptr);

  Try<int> fd = os::open(
      blobTarPath,
      O_RDONLY,
      S_IRUSR | S_IRGRP | S_IROTH);
  ASSERT_SOME(fd);

  ASSERT_SOME(os::nonblock(fd.get()));

  AWAIT_ASSERT_READY(
      io::read(fd.get(), tarBuffer.get(), tarSize.get().bytes()));

  const string blobHttpResponse =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-type : application/octet-stream\r\n" +
    "Content-Length : " +
    stringify(tarSize.get().bytes()) + "\r\n" +
    "\r\n";

  const size_t blobResponseSize =
    blobHttpResponse.length() + tarSize.get().bytes();

  std::unique_ptr<char[]> responseBuffer(new char[blobResponseSize]);
  ASSERT_NE(responseBuffer.get(), nullptr);

  memcpy(
      responseBuffer.get(),
      blobHttpResponse.c_str(),
      blobHttpResponse.length());

  memcpy(
      responseBuffer.get() + blobHttpResponse.length(),
      tarBuffer.get(),
      tarSize.get().bytes());

  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  registryPullerHttpRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(registryPullerHttpRequestFuture);

  AWAIT_ASSERT_READY(Socket(socket.get()).send(
      responseBuffer.get(),
      blobResponseSize));

  AWAIT_ASSERT_READY(registryPullerFuture);
  list<pair<string, string>> layers = registryPullerFuture.get();
  ASSERT_EQ(1u, layers.size());
  ASSERT_EQ(layers.front().first,
            "1ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea");

  Try<string> blob = os::read(
      path::join(layers.front().second, blobFile));
  ASSERT_SOME(blob);
  ASSERT_EQ(blob.get(), blobResponse);
}

#endif // USE_SSL_SOCKET


class ProvisionerDockerLocalStoreTest : public TemporaryDirectoryTest
{
public:
  void verifyLocalDockerImage(
      const slave::Flags& flags,
      const vector<string>& layers)
  {
    const string layersPath = path::join(flags.docker_store_dir, "layers");

    // Verify contents of the image in store directory.
    const string layerPath1 =
      getImageLayerRootfsPath(flags.docker_store_dir, "123");

    const string layerPath2 =
      getImageLayerRootfsPath(flags.docker_store_dir, "456");

    EXPECT_TRUE(os::exists(layerPath1));
    EXPECT_TRUE(os::exists(layerPath2));
    EXPECT_SOME_EQ(
        "foo 123",
        os::read(path::join(layerPath1 , "temp")));
    EXPECT_SOME_EQ(
        "bar 456",
        os::read(path::join(layerPath2, "temp")));

    // Verify the Docker Image provided.
    vector<string> expectedLayers;
    expectedLayers.push_back(layerPath1);
    expectedLayers.push_back(layerPath2);
    EXPECT_EQ(expectedLayers, layers);
  }

protected:
  virtual void SetUp()
  {
    TemporaryDirectoryTest::SetUp();

    const string imageDir = path::join(os::getcwd(), "images");
    const string image = path::join(imageDir, "abc:latest");
    ASSERT_SOME(os::mkdir(imageDir));
    ASSERT_SOME(os::mkdir(image));

    JSON::Value repositories = JSON::parse(
        "{"
        "  \"abc\": {"
        "    \"latest\": \"456\""
        "  }"
        "}").get();
    ASSERT_SOME(
        os::write(path::join(image, "repositories"), stringify(repositories)));

    ASSERT_SOME(os::mkdir(path::join(image, "123")));
    JSON::Value manifest123 = JSON::parse(
        "{"
        "  \"parent\": \"\""
        "}").get();
    ASSERT_SOME(os::write(
        path::join(image, "123", "json"), stringify(manifest123)));
    ASSERT_SOME(os::mkdir(path::join(image, "123", "layer")));
    ASSERT_SOME(
        os::write(path::join(image, "123", "layer", "temp"), "foo 123"));

    // Must change directory to avoid carrying over /path/to/archive during tar.
    const string cwd = os::getcwd();
    ASSERT_SOME(os::chdir(path::join(image, "123", "layer")));
    ASSERT_SOME(os::tar(".", "../layer.tar"));
    ASSERT_SOME(os::chdir(cwd));
    ASSERT_SOME(os::rmdir(path::join(image, "123", "layer")));

    ASSERT_SOME(os::mkdir(path::join(image, "456")));
    JSON::Value manifest456 = JSON::parse(
        "{"
        "  \"parent\": \"123\""
        "}").get();
    ASSERT_SOME(
        os::write(path::join(image, "456", "json"), stringify(manifest456)));
    ASSERT_SOME(os::mkdir(path::join(image, "456", "layer")));
    ASSERT_SOME(
        os::write(path::join(image, "456", "layer", "temp"), "bar 456"));

    ASSERT_SOME(os::chdir(path::join(image, "456", "layer")));
    ASSERT_SOME(os::tar(".", "../layer.tar"));
    ASSERT_SOME(os::chdir(cwd));
    ASSERT_SOME(os::rmdir(path::join(image, "456", "layer")));

    ASSERT_SOME(os::chdir(image));
    ASSERT_SOME(os::tar(".", "../abc:latest.tar"));
    ASSERT_SOME(os::chdir(cwd));
    ASSERT_SOME(os::rmdir(image));
  }
};


// This test verifies that a locally stored Docker image in the form of a
// tar achive created from a 'docker save' command can be unpacked and
// stored in the proper locations accessible to the Docker provisioner.
TEST_F(ProvisionerDockerLocalStoreTest, LocalStoreTestWithTar)
{
  const string imageDir = path::join(os::getcwd(), "images");
  const string image = path::join(imageDir, "abc:latest");
  ASSERT_SOME(os::mkdir(imageDir));
  ASSERT_SOME(os::mkdir(image));

  slave::Flags flags;
  flags.docker_puller = "local";
  flags.docker_store_dir = path::join(os::getcwd(), "store");
  flags.docker_local_archives_dir = imageDir;

  Try<Owned<slave::Store>> store = slave::docker::Store::create(flags);
  ASSERT_SOME(store);

  Image mesosImage;
  mesosImage.set_type(Image::DOCKER);
  mesosImage.mutable_docker()->set_name("abc");

  Future<vector<string>> layers = store.get()->get(mesosImage);
  AWAIT_READY(layers);

  verifyLocalDockerImage(flags, layers.get());
}


// This tests the ability of the metadata manger to recover the images it has
// already stored on disk when it is initialized.
TEST_F(ProvisionerDockerLocalStoreTest, MetadataManagerInitialization)
{
  slave::Flags flags;
  flags.docker_puller = "local";
  flags.docker_store_dir = path::join(os::getcwd(), "store");
  flags.docker_local_archives_dir = path::join(os::getcwd(), "images");

  Try<Owned<slave::Store>> store = slave::docker::Store::create(flags);
  ASSERT_SOME(store);

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("abc");

  Future<vector<string>> layers = store.get()->get(image);
  AWAIT_READY(layers);

  // Store is deleted and recreated. Metadata Manager is initialized upon
  // creation of the store.
  store.get().reset();
  store = slave::docker::Store::create(flags);
  ASSERT_SOME(store);
  Future<Nothing> recover = store.get()->recover();
  AWAIT_READY(recover);

  layers = store.get()->get(image);
  AWAIT_READY(layers);
  verifyLocalDockerImage(flags, layers.get());
}


class MockPuller : public Puller
{
public:
  MockPuller()
  {
    EXPECT_CALL(*this, pull(_, _))
      .WillRepeatedly(Invoke(this, &MockPuller::unmocked_pull));
  }

  virtual ~MockPuller() {}

  MOCK_METHOD2(
      pull,
      Future<list<pair<string, string>>>(
          const slave::docker::Image::Name&,
          const Path&));

  Future<list<pair<string, string>>> unmocked_pull(
      const slave::docker::Image::Name& name,
      const Path& directory)
  {
    // TODO(gilbert): Allow return list to be overridden.
    return list<pair<string, string>>();
  }
};


// This tests the store to pull the same image simutanuously.
// This test verifies that the store only calls the puller once
// when multiple requests for the same image is in flight.
TEST_F(ProvisionerDockerLocalStoreTest, PullingSameImageSimutanuously)
{
  const string imageDir = path::join(os::getcwd(), "images");
  const string image = path::join(imageDir, "abc:latest");
  ASSERT_SOME(os::mkdir(imageDir));
  ASSERT_SOME(os::mkdir(image));

  slave::Flags flags;
  flags.docker_puller = "local";
  flags.docker_store_dir = path::join(os::getcwd(), "store");
  flags.docker_local_archives_dir = imageDir;

  MockPuller* puller = new MockPuller();
  Future<Nothing> pull;
  Promise<list<pair<string, string>>> promise;

  EXPECT_CALL(*puller, pull(_, _))
    .WillOnce(testing::DoAll(FutureSatisfy(&pull),
                             Return(promise.future())));

  Try<Owned<slave::Store>> store =
      slave::docker::Store::create(flags, Owned<Puller>(puller));
  ASSERT_SOME(store);

  Image mesosImage;
  mesosImage.set_type(Image::DOCKER);
  mesosImage.mutable_docker()->set_name("abc");

  Future<vector<string>> layers1 = store.get()->get(mesosImage);
  AWAIT_READY(pull);

  const string rootfsPath1 = path::join(os::getcwd(), "rootfs1");
  const string rootfsPath2 = path::join(os::getcwd(), "rootfs2");

  Try<Nothing> mkdir1 = os::mkdir(rootfsPath1);
  ASSERT_SOME(mkdir1);
  Try<Nothing> mkdir2 = os::mkdir(rootfsPath2);
  ASSERT_SOME(mkdir2);

  ASSERT_TRUE(layers1.isPending());
  Future<vector<string>> layers2 = store.get()->get(mesosImage);

  const std::list<std::pair<std::string, std::string>> result =
      {{"123", rootfsPath1},
       {"456", rootfsPath2}};

  ASSERT_TRUE(layers2.isPending());
  promise.set(result);

  AWAIT_READY(layers1);
  AWAIT_READY(layers2);

  EXPECT_EQ(layers1.get(), layers2.get());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
