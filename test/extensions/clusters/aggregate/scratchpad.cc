// ----------- NOTES FOR CONTEXT :

// the specific constructor used by this class:

// [0] (in here at the top)
AggregateIntegrationTest() : HttpIntegrationTest(Http::CodecType::HTTP1, std::get<0>(GetParam()), config()),

// [1] test/integration/http_integration.h (132-133)
HttpIntegrationTest(Http::CodecType downstream_protocol, Network::Address::IpVersion version, const std::string& config);

// [2] test/integration/http_integration.cc (325-334)
HttpIntegrationTest::HttpIntegrationTest(Http::CodecType downstream_protocol,
                                         Network::Address::IpVersion version,
                                         const std::string& config)
    : HttpIntegrationTest::HttpIntegrationTest(
          downstream_protocol,
          [version](int) {
            return Network::Utility::parseInternetAddressNoThrow(
                Network::Test::getLoopbackAddressString(version), 0);
          },
          version, config) {}

// [3] test/integration/http_integration.h (142-144)
HttpIntegrationTest(Http::CodecType downstream_protocol,
  const InstanceConstSharedPtrFn& upstream_address_fn,
  Network::Address::IpVersion version, const std::string& config);

// [4] test/integration/http_integration.cc (336-372)
HttpIntegrationTest::HttpIntegrationTest(Http::CodecType downstream_protocol,
                                         const InstanceConstSharedPtrFn& upstream_address_fn,
                                         Network::Address::IpVersion version,
                                         const std::string& config)
    : BaseIntegrationTest(upstream_address_fn, version, config),
      downstream_protocol_(downstream_protocol), quic_stat_names_(stats_store_.symbolTable()) {
  // Legacy integration tests expect the default listener to be named "http" for
  // lookupPort calls.
  config_helper_.renameListener("http");
  config_helper_.setClientCodec(typeToCodecType(downstream_protocol_));
  // Allow extension lookup by name in the integration tests.
  config_helper_.addRuntimeOverride("envoy.reloadable_features.no_extension_lookup_by_name",
                                    "false");

  config_helper_.addConfigModifier(
      [](envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager&
             hcm) {
        auto* range = hcm.mutable_internal_address_config()->add_cidr_ranges();
        // Set loopback to be trusted so tests can set x-envoy headers.
        range->set_address_prefix("127.0.0.1");
        range->mutable_prefix_len()->set_value(32);
        // Legacy tests also set XFF: 10.0.0.1
        range->set_address_prefix("10.0.0.0");
        range->mutable_prefix_len()->set_value(8);
        range = hcm.mutable_internal_address_config()->add_cidr_ranges();
        range->set_address_prefix("::1");
        range->mutable_prefix_len()->set_value(128);
      });

#ifdef ENVOY_ENABLE_QUIC
  if (downstream_protocol_ == Http::CodecType::HTTP3) {
    // Needed to config QUIC transport socket factory, and needs to be added before base class calls
    // initialize().
    config_helper_.addQuicDownstreamTransportSocketConfig();
  }
#endif
}

// [5] (from above this) test/integration/http_integration.cc
BaseIntegrationTest(upstream_address_fn, version, config)

// [6] test/integration/base_integration_test.h (73-75)
BaseIntegrationTest(const InstanceConstSharedPtrFn& upstream_address_fn,
  Network::Address::IpVersion version,
  const std::string& config = ConfigHelper::httpProxyConfig());

// [7] test/integration/base_integration_test.cc (86-89)
BaseIntegrationTest::BaseIntegrationTest(const InstanceConstSharedPtrFn& upstream_address_fn,
                                         Network::Address::IpVersion version,
                                         const std::string& config)
    : BaseIntegrationTest(upstream_address_fn, version, configToBootstrap(config)) {}

// [8] from above - delegate constructor
BaseIntegrationTest(upstream_address_fn, version, configToBootstrap(config))

// [8-sidequest] test/integration/base_integration_test.cc (32-41)
// this is the configToBootstrap function (see above) that presumably converts the raw yaml string into a "Bootstrap" typed object(?)
// ""unmarshal"" the config into the bootstrap (load the yaml from the string, convert that into the bootstrap object)
envoy::config::bootstrap::v3::Bootstrap configToBootstrap(const std::string& config) {
  #ifdef ENVOY_ENABLE_YAML
    envoy::config::bootstrap::v3::Bootstrap bootstrap;
    TestUtility::loadFromYaml(config, bootstrap);
    return bootstrap;
  #else
    UNREFERENCED_PARAMETER(config);
    PANIC("YAML support compiled out: can't parse YAML");
  #endif
  }

// [9] test/integration/base_integration_test.cc (52-84)
BaseIntegrationTest::BaseIntegrationTest(const InstanceConstSharedPtrFn& upstream_address_fn,
                                         Network::Address::IpVersion version,
                                         const envoy::config::bootstrap::v3::Bootstrap& bootstrap)
    : api_(Api::createApiForTest(stats_store_, time_system_)),
      mock_buffer_factory_(new NiceMock<MockBufferFactory>),
      dispatcher_(api_->allocateDispatcher("test_thread",
                                           Buffer::WatermarkFactoryPtr{mock_buffer_factory_})),
      version_(version), upstream_address_fn_(upstream_address_fn),
      config_helper_(version, bootstrap),
      default_log_level_(TestEnvironment::getOptions().logLevel()) {
  Envoy::Server::validateProtoDescriptors();
  // This is a hack, but there are situations where we disconnect fake upstream connections and
  // then we expect the server connection pool to get the disconnect before the next test starts.
  // This does not always happen. This pause should allow the server to pick up the disconnect
  // notification and clear the pool connection if necessary. A real fix would require adding fairly
  // complex test hooks to the server and/or spin waiting on stats, neither of which I think are
  // necessary right now.
  timeSystem().realSleepDoNotUseWithoutScrutiny(std::chrono::milliseconds(10));
  ON_CALL(*mock_buffer_factory_, createBuffer_(_, _, _))
      .WillByDefault(Invoke([](std::function<void()> below_low, std::function<void()> above_high,
                               std::function<void()> above_overflow) -> Buffer::Instance* {
        return new Buffer::WatermarkBuffer(below_low, above_high, above_overflow);
      }));
  ON_CALL(factory_context_.server_context_, api()).WillByDefault(ReturnRef(*api_));
  ON_CALL(factory_context_, statsScope()).WillByDefault(ReturnRef(*stats_store_.rootScope()));
  ON_CALL(factory_context_, sslContextManager()).WillByDefault(ReturnRef(context_manager_));
  ON_CALL(factory_context_.server_context_, threadLocal()).WillByDefault(ReturnRef(thread_local_));

#ifndef ENVOY_ADMIN_FUNCTIONALITY
  config_helper_.addConfigModifier(
      [&](envoy::config::bootstrap::v3::Bootstrap& bootstrap) -> void { bootstrap.clear_admin(); });
#endif
}

// [11]

// TODO


// ----------
// Member VARIABLES in HttpIntegrationTest that maybe of use
// test/integration/http_integration.h (359+)

// The client making requests to Envoy.
IntegrationCodecClientPtr codec_client_;
// A placeholder for the first upstream connection.
FakeHttpConnectionPtr fake_upstream_connection_;
// A placeholder for the first request received at upstream.
FakeStreamPtr upstream_request_;

// The response headers sent by sendRequestAndWaitForResponse() by default.
Http::TestResponseHeaderMapImpl default_response_headers_{{":status", "200"}};
Http::TestRequestHeaderMapImpl default_request_headers_{{":method", "GET"},
                                                        {":path", "/test/long/url"},
                                                        {":scheme", "http"},
                                                        {":authority", "sni.lyft.com"}};

// The codec type for the client-to-Envoy connection [this is overriden to HTTP2 in our tests]
Http::CodecType downstream_protocol_{Http::CodecType::HTTP1};

// ----------
// Member METHODS in HttpIntegrationTest that maybe of use

// TODO


// ----------
// Member VARIABLES in BaseIntegrationTest that maybe of use
// test/integration/base_integration_test.h (??)

// work out what this is: (line 151)
Api::ApiPtr api_;

// Make sure the test server will be torn down after any fake client.
// The test server owns the runtime, which is often accessed by client and
// fake upstream codecs and must outlast them.
IntegrationTestServerPtr test_server_;

// ^^ because from above in this file we can see:
test_server_->waitForGaugeGe("cluster_manager.active_clusters", 4);
// and thats how we check the circuit breaker triggered or not...

// IP Address to use when binding sockets on upstreams.
InstanceConstSharedPtrFn upstream_address_fn_;

// [! LOOK INTO WHAT THIS IS/HAS]
// The config for envoy start-up.
ConfigHelper config_helper_;

// The fake upstreams_ are created using the context_manager, so make sure
// they are destroyed before it is.
std::vector<std::unique_ptr<FakeUpstream>> fake_upstreams_;

// Target number of upstreams.
uint32_t fake_upstreams_count_{1};

// The number of worker threads that the test server uses.
uint32_t concurrency_{1};

// Configuration for the fake upstream.
FakeUpstreamConfig upstream_config_{time_system_};

// ----------
// Member METHODS in BaseIntegrationTest that maybe of use
// test/integration/base_integration_test.h (??)

// Initialize the basic proto configuration, create fake upstreams, and start Envoy.
virtual void initialize();
// Set up the fake upstream connections. This is called by initialize() and
// is virtual to allow subclass overrides.
virtual void createUpstreams();
// Create a single upstream, based on the supplied config.
void createUpstream(Network::Address::InstanceConstSharedPtr endpoint, FakeUpstreamConfig& config);
// Sets upstream_protocol_ and alters the upstream protocol in the config_helper_
void setUpstreamProtocol(Http::CodecType protocol);


// this is why we pass in "http" and get back the port:
// (from above in this file: codec_client_ = makeHttpConnection(lookupPort("http"));)
// Test-wide port map.
void registerPort(const std::string& key, uint32_t port);
uint32_t lookupPort(const std::string& key);


Network::ClientConnectionPtr makeClientConnection(uint32_t port);

// Functions for testing reloadable config (xDS)
virtual void createXdsUpstream();
void createXdsConnection();
void cleanUpXdsConnection(); // this is used in TearDown (in this file)

// ---------


// IntegrationTestServer
// test/integration/server.h


// VARIABLES


// METHODS
std::vector<Stats::GaugeSharedPtr> gauges() override { return statStore().gauges(); }



// Protobufs

// bazel-bin/external/envoy_api/envoy/config/cluster/v3/cluster.pb.h (6233-6246)

public:
// .envoy.config.cluster.v3.CircuitBreakers circuit_breakers = 10;
bool has_circuit_breakers() const;
void clear_circuit_breakers() ;
const ::envoy::config::cluster::v3::CircuitBreakers& circuit_breakers() const;
PROTOBUF_NODISCARD ::envoy::config::cluster::v3::CircuitBreakers* release_circuit_breakers();
::envoy::config::cluster::v3::CircuitBreakers* mutable_circuit_breakers();
void set_allocated_circuit_breakers(::envoy::config::cluster::v3::CircuitBreakers* value);
void unsafe_arena_set_allocated_circuit_breakers(::envoy::config::cluster::v3::CircuitBreakers* value);
::envoy::config::cluster::v3::CircuitBreakers* unsafe_arena_release_circuit_breakers();

private:
const ::envoy::config::cluster::v3::CircuitBreakers& _internal_circuit_breakers() const;
::envoy::config::cluster::v3::CircuitBreakers* _internal_mutable_circuit_breakers();


// bazel-bin/external/envoy_api/envoy/config/cluster/v3/circuit_breaker.pb.h 

// TOO MUCH TO PASTE HERE...


----------

// LIST OF MORE THINGS/EXAMPLES I FOUND THAT I THINK MAY BE USEFUL :

codec_client_ = makeHttpConnection(lookupPort("http"));

auto response = codec_client_->makeRequestWithBody(
                                    Http::TestRequestHeaderMapImpl{
                                      {":method", "GET"},
                                      {":path", "/aggregatecluster"},
                                      {":scheme", "http"},
                                      {":authority", "host"},
                                      {"x-forwarded-for", "10.0.0.1"},
                                      {"x-envoy-retry-on", "5xx"}
                                    }
                              );

auto response = codec_client_->makeRequestWithBody(default_request_headers_, 1024);

ASSERT_TRUE(response->waitForEndStream());

// ----------

config_helper_.addRuntimeOverride("circuit_breakers.cluster_0.default.max_requests", "0");
config_helper_.addRuntimeOverride("circuit_breakers.cluster_0.default.max_retries", "1024");

// ----------

test_server_->waitForGaugeEq("cluster.cluster_0.upstream_rq_active", 0);
test_server_->waitForGaugeEq("cluster.cluster_0.upstream_rq_pending_active", 0);

EXPECT_EQ(test_server_->counter("cluster.cluster_0.upstream_rq_pending_overflow")->value(), 1);

// ----------

config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
  // BOOTSTRAP METHODS THAT MIGHT BE USEFUL
  bootstrap.mutable_static_resources()->add_clusters();
});

// ----------

// CODEC CLIENT "codec_client_"
// "codec_client_" is a HTTP codec client used during integration testing.

// "The codec client is part of Envoy’s HTTP handling architecture, specifically responsible for managing outbound HTTP connections. 
// It abstracts the differences between various HTTP versions (HTTP/1.1, HTTP/2, HTTP/3) and provides a unified interface for sending requests and receiving responses."
// - Encodes HTTP requests and sends them to upstream servers.
// - Decodes HTTP responses from upstream servers.
// - Manages the lifecycle of an HTTP connection (e.g., connection establishment, keep-alive, connection pooling).
// - Provides an interface for Envoy’s upstream HTTP filters and network components.

// test/integration/http_integration.h
// IntegrationCodecClient

IntegrationCodecClientPtr makeHttpConnection(uint32_t port);
IntegrationCodecClientPtr makeHttpConnection(Network::ClientConnectionPtr&& conn);

IntegrationStreamDecoderPtr makeHeaderOnlyRequest(const Http::RequestHeaderMap& headers);

IntegrationStreamDecoderPtr makeRequestWithBody(const Http::RequestHeaderMap& headers, uint64_t body_size, bool end_stream = true);
IntegrationStreamDecoderPtr makeRequestWithBody(const Http::RequestHeaderMap& headers, const std::string& body, bool end_stream = true);

// test/integration/http_integration.cc
// IntegrationCodecClient

// makeHttpConnections (variant 1)
// codec_client_->makeHttpConnection(...)
IntegrationCodecClientPtr HttpIntegrationTest::makeHttpConnection(uint32_t port) {
  return makeHttpConnection(makeClientConnection(port));
}

// makeHttpConnections (variant 2)
// codec_client_->makeHttpConnection(...)
IntegrationCodecClientPtr HttpIntegrationTest::makeHttpConnection(Network::ClientConnectionPtr&& conn) {
  auto codec = makeRawHttpConnection(std::move(conn), absl::nullopt);
  EXPECT_TRUE(codec->connected()) << codec->connection()->transportFailureReason();
  return codec;
}

// makeHeaderOnlyRequest
// codec_client_->makeHeaderOnlyRequest(...)
IntegrationStreamDecoderPtr IntegrationCodecClient::makeHeaderOnlyRequest(const Http::RequestHeaderMap& headers) {
  auto response = std::make_unique<IntegrationStreamDecoder>(dispatcher_);
  Http::RequestEncoder& encoder = newStream(*response);
  encoder.getStream().addCallbacks(*response);
  encoder.encodeHeaders(headers, true).IgnoreError();
  flushWrite();
  return response;
}

// makeRequestWithBody (variant 1)
// codec_client_->makeRequestWithBody(...)
IntegrationStreamDecoderPtr IntegrationCodecClient::makeRequestWithBody(const Http::RequestHeaderMap& headers, uint64_t body_size, bool end_stream) {
  return makeRequestWithBody(headers, std::string(body_size, 'a'), end_stream);
}

// makeRequestWithBody (variant 2)
// codec_client_->makeRequestWithBody(...)
IntegrationStreamDecoderPtr IntegrationCodecClient::makeRequestWithBody(const Http::RequestHeaderMap& headers, const std::string& body, bool end_stream) {
  auto response = std::make_unique<IntegrationStreamDecoder>(dispatcher_);
  Http::RequestEncoder& encoder = newStream(*response);
  encoder.getStream().addCallbacks(*response);
  encoder.encodeHeaders(headers, false).IgnoreError();
  Buffer::OwnedImpl data(body);
  encoder.encodeData(data, end_stream);
  flushWrite();
  return response;
}

// ----------

// "A stream decoder is responsible for processing incoming data in a streaming fashion. 
// Specifically, it is part of the HTTP processing pipeline and is used 
// to decode (interpret and process) HTTP requests or responses as they are received."

// "A stream decoder in Envoy is responsible for taking raw bytes from a network connection 
// and transforming them into structured protocol-specific messages or events that the rest of the proxy can understand and process."

// test/integration/integration_stream_decoder.h
// IntegrationStreamDecoder

// Wait for the end of stream on the next upstream stream on any of the provided fake upstreams.
// Sets fake_upstream_connection_ to the connection and upstream_request_ to stream.
// In cases where the upstream that will receive the request is not deterministic, a second
// upstream index may be provided, in which case both upstreams will be checked for requests.
absl::optional<uint64_t> waitForNextUpstreamRequest(const std::vector<uint64_t>& upstream_indices,std::chrono::milliseconds connection_wait_timeout = TestUtility::DefaultTimeout);
void waitForNextUpstreamRequest(uint64_t upstream_index = 0, std::chrono::milliseconds connection_wait_timeout = TestUtility::DefaultTimeout);

waitForEndStream(std::chrono::milliseconds timeout = TestUtility::DefaultTimeout);

// test/integration/integration_stream_decoder.cc
// IntegrationStreamDecoder


absl::optional<uint64_t> HttpIntegrationTest::waitForNextUpstreamRequest(const std::vector<uint64_t>& upstream_indices, std::chrono::milliseconds connection_wait_timeout) {
  absl::optional<uint64_t> upstream_with_request;
  // If there is no upstream connection, wait for it to be established.
  if (!fake_upstream_connection_) {
    upstream_with_request = waitForNextUpstreamConnection(upstream_indices, connection_wait_timeout,
                                                          fake_upstream_connection_);
  }
  // Wait for the next stream on the upstream connection.
  AssertionResult result =
      fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_);
  RELEASE_ASSERT(result, result.message());
  // Wait for the stream to be completely received.
  result = upstream_request_->waitForEndStream(*dispatcher_);
  RELEASE_ASSERT(result, result.message());

  return upstream_with_request;
}

void HttpIntegrationTest::waitForNextUpstreamRequest(uint64_t upstream_index, std::chrono::milliseconds connection_wait_timeout) {
  waitForNextUpstreamRequest(std::vector<uint64_t>({upstream_index}), connection_wait_timeout);
}


// response1->waitForEndStream(...)
AssertionResult IntegrationStreamDecoder::waitForEndStream(std::chrono::milliseconds timeout) {
  bool timer_fired = false;
  while (!saw_end_stream_) {
    Event::TimerPtr timer(dispatcher_.createTimer([this, &timer_fired]() -> void {
      timer_fired = true;
      dispatcher_.exit();
    }));
    timer->enableTimer(timeout);
    waiting_for_end_stream_ = true;
    dispatcher_.run(Event::Dispatcher::RunType::Block);
    if (!saw_end_stream_) {
      ENVOY_LOG_MISC(warn, "non-end stream event.");
    }
    if (timer_fired) {
      return AssertionFailure() << "Timed out waiting for end stream\n";
    }
  }
  return AssertionSuccess();
}

// ----------

// test/integration/server.h
// IntegrationTestServer

Stats::GaugeSharedPtr gauge(const std::string& name) override {
  // When using the thread local store, only gauges() is thread safe. This also allows us
  // to test if a counter exists at all versus just defaulting to zero.
  return TestUtility::findGauge(statStore(), name);
}
