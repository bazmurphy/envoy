// ----------- NOTES FOR CONTEXT :

// the specific constructor used by this class:

// [0] test/extensions/clusters/aggregate/cluster_integration_test.cc
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

// [8] from above - "delegate constructor"
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

// [10]

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
// test/integration/base_integration_test.h

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
// test/integration/base_integration_test.h

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


// ---------

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
// but this has all the methods for working with the circuit breaker fields

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
// Sets fake_upstream_connection_ to the connection, and upstream_request_ to stream.
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
  AssertionResult result = fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_);
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


// ---------

IntegrationStreamDecoderPtr IntegrationCodecClient::makeRequestWithBody(const Http::RequestHeaderMap& headers, const std::string& body, bool end_stream) {
	auto response = std::make_unique<IntegrationStreamDecoder>(dispatcher_);
	Http::RequestEncoder& encoder = newStream(*response); // THIS IS CALLED (SEE BELOW)
	encoder.getStream().addCallbacks(*response);
	encoder.encodeHeaders(headers, false).IgnoreError();
	Buffer::OwnedImpl data(body);
	encoder.encodeData(data, end_stream);
	flushWrite();
	return response;
  }

// CodecClient

// source/common/http/codec_client.h

/**
 * Create a new stream. Note: The CodecClient will NOT buffer multiple requests for HTTP1
 * connections. Thus, calling newStream() before the previous request has been fully encoded
 * is an error. Pipelining is supported however.
 * @param response_decoder supplies the decoder to use for response callbacks.
 * @return StreamEncoder& the encoder to use for encoding the request.
 */
RequestEncoder& newStream(ResponseDecoder& response_decoder);

// source/common/http/codec_client.cc

RequestEncoder& CodecClient::newStream(ResponseDecoder& response_decoder) {
	ActiveRequestPtr request(new ActiveRequest(*this, response_decoder));
	request->setEncoder(codec_->newStream(*request));
	LinkedList::moveIntoList(std::move(request), active_requests_);
  
	auto upstream_info = connection_->streamInfo().upstreamInfo();
	upstream_info->setUpstreamNumStreams(upstream_info->upstreamNumStreams() + 1);
  
	disableIdleTimer();
	return *active_requests_.front();
  }

// "codec_" is a protected: member variable in the Envoy::Http::CodecClient class
// ClientConnectionPtr codec_;

// envoy/http/codec.h

// using ClientConnectionPtr = std::unique_ptr<ClientConnection>;

// ClientConnection 
/**
 * A client side HTTP connection.
 */
class ClientConnection : public virtual Connection {
	public:
	  /**
	   * Create a new outgoing request stream.
	   * @param response_decoder supplies the decoder callbacks to fire response events into.
	   * @return RequestEncoder& supplies the encoder to write the request into.
	   */
	  virtual RequestEncoder& newStream(ResponseDecoder& response_decoder) PURE;
	};

// but this is just an interface, we can see in the stack trace is it related to the http1 implementation

// find the header:

// source/common/http/http1/codec_impl.h

/**
 * Implementation of Http::ClientConnection for HTTP/1.1.
 */
class ClientConnectionImpl : public ClientConnection, public ConnectionImpl {...}

// which has this "public:" method

// Http::ClientConnection
RequestEncoder& newStream(ResponseDecoder& response_decoder) override;

// find the implementation: 

// source/common/http/http1/codec_impl.cc

RequestEncoder& ClientConnectionImpl::newStream(ResponseDecoder& response_decoder) {
	// If reads were disabled due to flow control, we expect reads to always be enabled again before
	// reusing this connection. This is done when the response is received.
	ASSERT(connection_.readEnabled());
  
	ASSERT(!pending_response_.has_value());
	ASSERT(pending_response_done_);
	pending_response_.emplace(*this, std::move(bytes_meter_before_stream_), &response_decoder);
	pending_response_done_ = false;
	return pending_response_.value().encoder_;
  }

// readEnabled() bool whether reading is enabled on the connection.

// ASSERT(!pending_response_.has_value());

// in the Envoy::Http::Http1::ClientConnectionImpl class
// there is a private member variable "pending_response_"

// absl::optional<PendingResponse> pending_response_;

// in the Envoy::Http::Http1::ClientConnectionImpl class
// there is a "private:" struct called PendingResponse

// What we can learn:
// The ClientConnectionImpl has a connection
// (this is probably found in the inherited classes: class ClientConnectionImpl : public ClientConnection, public ConnectionImpl )

// It also can only hold one connection at a time
// We know that because it uses an optional (???) <-----
// We can tell because the optional gets set when a connection gets created
// And a second attempt to set a connection fails
// This is us reading the newStream function, and the stack trace, and knowing the test code...

// 1. we make request with body, that immediately calls newStream, optional with no value, the emplace, constructs that value, the value no longer nil, which has_value() true
// 2. we make the second query with body, that assert all the way down that stack newStream... has_value... no, i already have a value

// Class Naming
// a class name is usually named after the thing it handles for us

// if we want to control an ssh connection, we call the class SshConnection
// if we want to control an http connection, we call the class HttpConnection

// ClientConnectionImpl has the pending_response_
// which is absl::optional<PendingResponse>

// We ask if the class would handle more than one request, how would that work?
// And then we end up in the assertion failing for the second request
// So we know that the class cannot handle a second request
// Therefore we know the class can only handle one request

// --------
// SIDEBAR HTTP2 VERSION OF THE ABOVE

// "HTTP/2 client connection codec."

// Envoy::Http::Http2::ClientConnectionImpl class

// source/common/http/http2/codec_impl.h

// Http::ClientConnection
RequestEncoder& newStream(ResponseDecoder& response_decoder) override;

// NOTE: the comment above it, is telling us WHICH base class it is "override"-ing

// source/common/http/http2/codec_impl.cc

RequestEncoder& ClientConnectionImpl::newStream(ResponseDecoder& decoder) {
	// If the connection has been idle long enough to trigger a ping, send one
	// ahead of creating the stream.
	if (idle_session_requires_ping_interval_.count() != 0 &&
		(connection_.dispatcher().timeSource().monotonicTime() - lastReceivedDataTime() >
		 idle_session_requires_ping_interval_)) {
	  sendKeepalive();
	}
  
	ClientStreamImplPtr stream(new ClientStreamImpl(*this, per_stream_buffer_limit_, decoder));
	// If the connection is currently above the high watermark, make sure to inform the new stream.
	// The connection can not pass this on automatically as it has no awareness that a new stream is
	// created.
	if (connection_.aboveHighWatermark()) {
	  stream->runHighWatermarkCallbacks();
	}
	ClientStreamImpl& stream_ref = *stream;
	LinkedList::moveIntoList(std::move(stream), active_streams_);
	protocol_constraints_.incrementOpenedStreamCount();
	return stream_ref;
  }

// in HTTP2 there is a TCP connection
// and each TCP connection can hold multiple streams (request/response cycle)
// and each stream is a single HTTP request
// this is a concept called MULTIPLEX
// Multiplexing: "a system or signal involving simultaneous transmission of several messages along a single channel of communication."

// whereas HTTP1 there is a TCP connection
// and that can only hold one HTTP request/response cycle

// END OF SIDEBAR
// ----------

struct PendingResponse {
    PendingResponse(ConnectionImpl& connection, StreamInfo::BytesMeterSharedPtr&& bytes_meter,
                    ResponseDecoder* decoder)
        : encoder_(connection, std::move(bytes_meter)), decoder_(decoder) {}
    RequestEncoderImpl encoder_;
    ResponseDecoder* decoder_;
  };

// its constructor takes in the connection, bytes_meter, and decoder
// it initialises its own "encoder_" and "decoder_" member variables

// has_value() method comes along because it is an "optional" type (specifically absl::optional)
// absl::lts_20240722::optional<TYPE-HERE>::has_value()

// optional::has_value()
// "Determines whether the optional contains a value. Returns false if and only if *this is empty."

// ----------

// What is an ActiveRequest ?

// using ActiveRequestPtr = std::unique_ptr<ActiveRequest>;

// its a struct inside of the "private:" of the Envoy::Http::CodecClient class

  /**
   * Wrapper for an outstanding request. Designed for handling stream multiplexing.
   */
  struct ActiveRequest : LinkedObject<ActiveRequest>,
                         public Event::DeferredDeletable,
                         public StreamCallbacks,
                         public ResponseDecoderWrapper,
                         public RequestEncoderWrapper {
    ActiveRequest(CodecClient& parent, ResponseDecoder& inner)
        : ResponseDecoderWrapper(inner), RequestEncoderWrapper(nullptr), parent_(parent),
          header_validator_(
              parent.host_->cluster().makeHeaderValidator(parent.codec_->protocol())) {
      switch (parent.protocol()) {
      case Protocol::Http10:
      case Protocol::Http11:
        // HTTP/1.1 codec does not support half-close on the response completion.
        wait_encode_complete_ = false;
        break;
      case Protocol::Http2:
      case Protocol::Http3:
        wait_encode_complete_ = true;
        break;
      }
    }

    void decodeHeaders(ResponseHeaderMapPtr&& headers, bool end_stream) override;

    // StreamCallbacks
    void onResetStream(StreamResetReason reason, absl::string_view) override {
      parent_.onReset(*this, reason);
    }
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    // StreamDecoderWrapper
    void onPreDecodeComplete() override { parent_.responsePreDecodeComplete(*this); }
    void onDecodeComplete() override {}

    // RequestEncoderWrapper
    void onEncodeComplete() override { parent_.requestEncodeComplete(*this); }

    // RequestEncoder
    Status encodeHeaders(const RequestHeaderMap& headers, bool end_stream) override;

    void setEncoder(RequestEncoder& encoder) {
      inner_encoder_ = &encoder;
      inner_encoder_->getStream().addCallbacks(*this);
    }

    void removeEncoderCallbacks() { inner_encoder_->getStream().removeCallbacks(*this); }

    CodecClient& parent_;
    Http::ClientHeaderValidatorPtr header_validator_;
    bool wait_encode_complete_{true};
    bool encode_complete_{false};
    bool decode_complete_{false};
  };


// ----------

// INITIALIZE()

// we need to dig in to the initialize() function

// test/extensions/clusters/aggregate/cluster_integration_test.cc

void initialize() override {
  use_lds_ = false;
  setUpstreamCount(2);                         // the CDS cluster
  setUpstreamProtocol(Http::CodecType::HTTP2); // CDS uses gRPC uses HTTP2.

  defer_listener_finalization_ = true;
  config_helper_.addConfigModifier([this](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    bootstrap.mutable_cluster_manager()->set_enable_deferred_cluster_creation(
        deferred_cluster_creation_);
  });
  HttpIntegrationTest::initialize();

  addFakeUpstream(Http::CodecType::HTTP2);
  addFakeUpstream(Http::CodecType::HTTP2);
  cluster1_ = ConfigHelper::buildStaticCluster(
      FirstClusterName, fake_upstreams_[FirstUpstreamIndex]->localAddress()->ip()->port(),
      Network::Test::getLoopbackAddressString(version_));
  cluster2_ = ConfigHelper::buildStaticCluster(
      SecondClusterName, fake_upstreams_[SecondUpstreamIndex]->localAddress()->ip()->port(),
      Network::Test::getLoopbackAddressString(version_));

  // Let Envoy establish its connection to the CDS server.
  acceptXdsConnection();

  // Do the initial compareDiscoveryRequest / sendDiscoveryResponse for cluster_1.
  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "", {}, {}, {}, true));
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "55");

  test_server_->waitForGaugeGe("cluster_manager.active_clusters", 3);

  // Wait for our statically specified listener to become ready, and register its port in the
  // test framework's downstream listener port map.
  test_server_->waitUntilListenersReady();
  registerTestServerPorts({"http"});
}


// the above changes some config settings: use_lds_, setUpstreamCount and setUpstreamProtocol

// test/integration/base_integration_test.h

// this is initialized as true (but we change it to false)
bool use_lds_{true}; // Use the integration framework's LDS set up.

// Sets fake_upstreams_count_
void setUpstreamCount(uint32_t count) { fake_upstreams_count_ = count; }

// Sets upstream_protocol_ and alters the upstream protocol in the config_helper_
void setUpstreamProtocol(Http::CodecType protocol);

// test/integration/base_integration_test.cc

void BaseIntegrationTest::setUpstreamProtocol(Http::CodecType protocol) {
  upstream_config_.upstream_protocol_ = protocol;
  if (upstream_config_.upstream_protocol_ == Http::CodecType::HTTP2) {
    config_helper_.addConfigModifier(
        [&](envoy::config::bootstrap::v3::Bootstrap& bootstrap) -> void {
          RELEASE_ASSERT(bootstrap.mutable_static_resources()->clusters_size() >= 1, "");
          ConfigHelper::HttpProtocolOptions protocol_options;
          protocol_options.mutable_explicit_http_config()->mutable_http2_protocol_options();
          ConfigHelper::setProtocolOptions(
              *bootstrap.mutable_static_resources()->mutable_clusters(0), protocol_options);
        });
  } else if (upstream_config_.upstream_protocol_ == Http::CodecType::HTTP1) {
    config_helper_.addConfigModifier(
        [&](envoy::config::bootstrap::v3::Bootstrap& bootstrap) -> void {
          RELEASE_ASSERT(bootstrap.mutable_static_resources()->clusters_size() >= 1, "");
          ConfigHelper::HttpProtocolOptions protocol_options;
          protocol_options.mutable_explicit_http_config()->mutable_http_protocol_options();
          ConfigHelper::setProtocolOptions(
              *bootstrap.mutable_static_resources()->mutable_clusters(0), protocol_options);
        });
  } else {
    RELEASE_ASSERT(protocol == Http::CodecType::HTTP3, "");
    setUdpFakeUpstream(FakeUpstreamConfig::UdpConfig());
    upstream_tls_ = true;
    config_helper_.configureUpstreamTls(false, true);
    config_helper_.addConfigModifier(
        [&](envoy::config::bootstrap::v3::Bootstrap& bootstrap) -> void {
          // Docker doesn't allow writing to the v6 address returned by
          // Network::Utility::getLocalAddress.
          if (version_ == Network::Address::IpVersion::v6) {
            auto* bind_config_address = bootstrap.mutable_static_resources()
                                            ->mutable_clusters(0)
                                            ->mutable_upstream_bind_config()
                                            ->mutable_source_address();
            bind_config_address->set_address("::1");
            bind_config_address->set_port_value(0);
          }

          RELEASE_ASSERT(bootstrap.mutable_static_resources()->clusters_size() >= 1, "");
          ConfigHelper::HttpProtocolOptions protocol_options;
          protocol_options.mutable_explicit_http_config()->mutable_http3_protocol_options();
          ConfigHelper::setProtocolOptions(
              *bootstrap.mutable_static_resources()->mutable_clusters(0), protocol_options);
        });
  }
}

// the above initialize() then calls HttpIntegrationTest::initialize();

// test/integration/http_integration.cc
void HttpIntegrationTest::initialize() {
  if (downstream_protocol_ != Http::CodecType::HTTP3) {
    return BaseIntegrationTest::initialize();
  }
#ifdef ENVOY_ENABLE_QUIC
  // Needs to be instantiated before base class calls initialize() which starts a QUIC listener
  // according to the config.
  quic_transport_socket_factory_ = IntegrationUtil::createQuicUpstreamTransportSocketFactory(
      *api_, stats_store_, context_manager_, thread_local_, san_to_match_);

  BaseIntegrationTest::initialize();
  registerTestServerPorts({"http"}, test_server_);

  // Needs to outlive all QUIC connections.
  auto cluster = std::make_shared<NiceMock<Upstream::MockClusterInfo>>();
  auto quic_connection_persistent_info =
      Quic::createPersistentQuicInfoForCluster(*dispatcher_, *cluster);
  // Config IETF QUIC flow control window.
  quic_connection_persistent_info->quic_config_
      .SetInitialMaxStreamDataBytesIncomingBidirectionalToSend(
          Http3::Utility::OptionsLimits::DEFAULT_INITIAL_STREAM_WINDOW_SIZE);
  // Config Google QUIC flow control window.
  quic_connection_persistent_info->quic_config_.SetInitialStreamFlowControlWindowToSend(
      Http3::Utility::OptionsLimits::DEFAULT_INITIAL_STREAM_WINDOW_SIZE);
  // Adjust timeouts.
  quic::QuicTime::Delta connect_timeout = quic::QuicTime::Delta::FromSeconds(5 * TIMEOUT_FACTOR);
  quic_connection_persistent_info->quic_config_.set_max_time_before_crypto_handshake(
      connect_timeout);
  quic_connection_persistent_info->quic_config_.set_max_idle_time_before_crypto_handshake(
      connect_timeout);

  quic_connection_persistent_info_ = std::move(quic_connection_persistent_info);
#else
  ASSERT(false, "running a QUIC integration test without compiling QUIC");
#endif
}

// which itself calls BaseIntegrationTest::initialize(); (because we aren't using HTTP3)

// test/integration/base_integration_test.cc
void BaseIntegrationTest::initialize() {
  RELEASE_ASSERT(!initialized_, "");
  RELEASE_ASSERT(Event::Libevent::Global::initialized(), "");
  initialized_ = true;

  createUpstreams();
  createXdsUpstream();
  createEnvoy();

#ifdef ENVOY_ADMIN_FUNCTIONALITY
  if (!skip_tag_extraction_rule_check_) {
    checkForMissingTagExtractionRules();
  }
#endif
}

// the above calls the three methods: createUpstreams, createXdsUpstream, createEnvoy

// test/integration/base_integration_test.cc
void BaseIntegrationTest::createUpstreams() {
  for (uint32_t i = 0; i < fake_upstreams_count_; ++i) {
    auto endpoint = upstream_address_fn_(i);
    createUpstream(endpoint, upstreamConfig());
  }
}

// upstreamConfig() returns the "upstream_copnfig_" member variable

// test/integration/base_integration_test.h
FakeUpstreamConfig& upstreamConfig() { return upstream_config_; }

// createUpstreams() calls createUpstream (...)

// test/integration/base_integration_test.cc
void BaseIntegrationTest::createUpstream(Network::Address::InstanceConstSharedPtr endpoint,
                                         FakeUpstreamConfig& config) {
  Network::DownstreamTransportSocketFactoryPtr factory =
      upstream_tls_ ? createUpstreamTlsContext(config)
                    : Network::Test::createRawBufferDownstreamSocketFactory();
  if (autonomous_upstream_) {
    fake_upstreams_.emplace_back(std::make_unique<AutonomousUpstream>(
        std::move(factory), endpoint, config, autonomous_allow_incomplete_streams_));
  } else {
    fake_upstreams_.emplace_back(
        std::make_unique<FakeUpstream>(std::move(factory), endpoint, config));
  }
}

// test/integration/base_integration_test.cc
void BaseIntegrationTest::createXdsUpstream() {
  if (create_xds_upstream_ == false) {
    return;
  }
  if (tls_xds_upstream_ == false) {
    addFakeUpstream(Http::CodecType::HTTP2);
  } else {
    envoy::extensions::transport_sockets::tls::v3::DownstreamTlsContext tls_context;
    auto* common_tls_context = tls_context.mutable_common_tls_context();
    common_tls_context->add_alpn_protocols(Http::Utility::AlpnNames::get().Http2);
    auto* tls_cert = common_tls_context->add_tls_certificates();
    tls_cert->mutable_certificate_chain()->set_filename(
        TestEnvironment::runfilesPath("test/config/integration/certs/upstreamcert.pem"));
    tls_cert->mutable_private_key()->set_filename(
        TestEnvironment::runfilesPath("test/config/integration/certs/upstreamkey.pem"));
    auto cfg = *Extensions::TransportSockets::Tls::ServerContextConfigImpl::create(
        tls_context, factory_context_, false);

    upstream_stats_store_ = std::make_unique<Stats::TestIsolatedStoreImpl>();
    auto context = *Extensions::TransportSockets::Tls::ServerSslSocketFactory::create(
        std::move(cfg), context_manager_, *upstream_stats_store_->rootScope(),
        std::vector<std::string>{});
    addFakeUpstream(std::move(context), Http::CodecType::HTTP2, /*autonomous_upstream=*/false);
  }
  xds_upstream_ = fake_upstreams_.back().get();
}

// "tls_xds_upstream_" is a member variable of the BaseIntegrationTest class

// so we probably end up in the second if block, because the "tls_xds_upstream_" value is initialized as false:

// test/integration/base_integration_test.h
bool tls_xds_upstream_{false};

// therefore we probably call addFakeUpstream(...)

// test/integration/base_integration_test.h
FakeUpstream& addFakeUpstream(Http::CodecType type) {
  auto config = configWithType(type);
  fake_upstreams_.emplace_back(std::make_unique<FakeUpstream>(0, version_, config));
  return *fake_upstreams_.back();
}

// "version_" is a member variable of the BaseIntegrationTest class
// that gets initialised by the argument "version" that gets given to the BaseIntegrationTest constructor

// test/integration/base_integration_test.h
Network::Address::IpVersion version_; // The IpVersion (IPv4, IPv6) to use.

// test/integration/base_integration_test.h
FakeUpstreamConfig configWithType(Http::CodecType type) const {
  FakeUpstreamConfig config = upstream_config_;
  config.upstream_protocol_ = type;
  if (type != Http::CodecType::HTTP3) {
    config.udp_fake_upstream_ = absl::nullopt;
  }
  return config;
}

// test/integration/base_integration_test.cc
void BaseIntegrationTest::createEnvoy() {
  std::vector<uint32_t> ports;
  for (auto& upstream : fake_upstreams_) {
    if (upstream->localAddress()->ip()) {
      ports.push_back(upstream->localAddress()->ip()->port());
    }
  }

  const std::string bootstrap_path = finalizeConfigWithPorts(config_helper_, ports, use_lds_);

  std::vector<std::string> named_ports;
  const auto& static_resources = config_helper_.bootstrap().static_resources();
  named_ports.reserve(static_resources.listeners_size());
  for (int i = 0; i < static_resources.listeners_size(); ++i) {
    named_ports.push_back(static_resources.listeners(i).name());
  }
  createGeneratedApiTestServer(bootstrap_path, named_ports, {false, true, false}, false);
}

// ----------

// Look into the "config_helper"

// test/integration/base_integration_test.h

// config_helper_ is a "protected" member variable on the BaseIntegrationTest class

// The config for envoy start-up.
ConfigHelper config_helper_;

// test/config/utility.h
class ConfigHelper

// it has a "private:" member variable "config_modifiers_"
// which is a vector of config modifier functions

// test/config/utility.h
// The config modifiers added via addConfigModifier() which will be applied in finalize()
std::vector<ConfigModifierFunction> config_modifiers_;

// a ConfigModifierFunction is a function that takes in a bootstrap config returns nothing (and presumably modifies the bootstrap config):

// test/config/utility.h
using ConfigModifierFunction = std::function<void(envoy::config::bootstrap::v3::Bootstrap&)>;

// the addConfigModifier method has 3 variants, the one we are interested in is:

// test/config/utility.h
// Allows callers to do their own modification to |bootstrap_| which will be
// applied just before ports are modified in finalize().
void addConfigModifier(ConfigModifierFunction function);

// test/config/utility.cc
void ConfigHelper::addConfigModifier(ConfigModifierFunction function) {
  RELEASE_ASSERT(!finalized_, "");
  config_modifiers_.push_back(std::move(function));
}

// finalze() is what executes the ConfigModifierFunction(s)

// test/config/utility.h

// Run the final config modifiers, and then set the upstream ports based on upstream connections.
// This is the last operation run on |bootstrap_| before it is handed to Envoy.
// Ports are assigned by looping through clusters, hosts, and addresses in the
// order they are stored in |bootstrap_|
void finalize(const std::vector<uint32_t>& ports);

// test/config/utility.cc
void ConfigHelper::finalize(const std::vector<uint32_t>& ports) {
  RELEASE_ASSERT(!finalized_, "");

  applyConfigModifiers();

  setPorts(ports);

  if (!connect_timeout_set_) {
#ifdef __APPLE__
    // Set a high default connect timeout. Under heavy load (and in particular in CI), macOS
    // connections can take inordinately long to complete.
    setConnectTimeout(std::chrono::seconds(30));
#else
    // Set a default connect timeout.
    setConnectTimeout(std::chrono::seconds(5));
#endif
  }

  // Make sure we don't setAsyncLb() when we intend to use a non-default LB algorithm.
  for (int i = 0; i < bootstrap_.mutable_static_resources()->clusters_size(); ++i) {
    auto* cluster = bootstrap_.mutable_static_resources()->mutable_clusters(i);
    if (cluster->has_load_balancing_policy() &&
        cluster->load_balancing_policy().policies(0).typed_extension_config().name() ==
            "envoy.load_balancing_policies.async_round_robin") {
      ASSERT_EQ(::envoy::config::cluster::v3::Cluster::ROUND_ROBIN, cluster->lb_policy());
    }
  }

  finalized_ = true;
}

// test/config/utility.h
// Allow a finalized configuration to be edited for generating xDS responses
void applyConfigModifiers();

// test/config/utility.cc
void ConfigHelper::applyConfigModifiers() {
  for (const auto& config_modifier : config_modifiers_) {
    config_modifier(bootstrap_);
  }
  config_modifiers_.clear();
}

// ----------

// LIST OF MORE THINGS/EXAMPLES FOUND THAT I THINK MAY BE USEFUL/WORTH REMEMBERING :

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
  // BOOTSTRAP HAS METHODS THAT MIGHT BE USEFUL (look at the protobuf fields and methods)
  bootstrap.mutable_static_resources()->add_clusters();
});

// ----------
