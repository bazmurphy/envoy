#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/extensions/clusters/aggregate/v3/cluster.pb.h"
#include "envoy/extensions/upstreams/http/v3/http_protocol_options.pb.h"
#include "envoy/grpc/status.h"
#include "envoy/stats/scope.h"

#include "source/common/config/protobuf_link_hacks.h"
#include "source/common/network/socket_option_factory.h"
#include "source/common/protobuf/protobuf.h"
#include "source/common/protobuf/utility.h"

#include "test/common/grpc/grpc_client_integration.h"
#include "test/config/v2_link_hacks.h"
#include "test/integration/http_integration.h"
#include "test/integration/utility.h"
#include "test/test_common/network_utility.h"
#include "test/test_common/resources.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "absl/synchronization/notification.h"
#include "gtest/gtest.h"

using testing::AssertionResult;

namespace Envoy {
namespace {

const char FirstClusterName[] = "cluster_1";
const char SecondClusterName[] = "cluster_2";
// Index in fake_upstreams_
const int FirstUpstreamIndex = 2;
const int SecondUpstreamIndex = 3;

struct Thresholds {
  uint32_t max_connections = 1024;
  uint32_t max_requests = 1024;
  uint32_t max_pending_requests = 1024;
  uint32_t max_retries = 3;
  uint32_t max_connection_pools = std::numeric_limits<uint32_t>::max();
};

const std::string& config() {
  CONSTRUCT_ON_FIRST_USE(std::string, fmt::format(R"EOF(
admin:
  access_log:
  - name: envoy.access_loggers.file
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.access_loggers.file.v3.FileAccessLog
      path: "{}"
  address:
    socket_address:
      address: 127.0.0.1
      port_value: 0
dynamic_resources:
  cds_config:
    api_config_source:
      api_type: GRPC
      grpc_services:
        envoy_grpc:
          cluster_name: my_cds_cluster
      set_node_on_first_message_only: true
static_resources:
  clusters:
  - name: my_cds_cluster
    typed_extension_protocol_options:
      envoy.extensions.upstreams.http.v3.HttpProtocolOptions:
        "@type": type.googleapis.com/envoy.extensions.upstreams.http.v3.HttpProtocolOptions
        explicit_http_config:
          http2_protocol_options: {{}}
    load_assignment:
      cluster_name: my_cds_cluster
      endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: 127.0.0.1
                port_value: 0
  - name: aggregate_cluster
    connect_timeout: 0.25s
    lb_policy: CLUSTER_PROVIDED
    cluster_type:
      name: envoy.clusters.aggregate
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.clusters.aggregate.v3.ClusterConfig
        clusters:
        - cluster_1
        - cluster_2
  listeners:
  - name: http
    address:
      socket_address:
        address: 127.0.0.1
        port_value: 0
    filter_chains:
      filters:
        name: http
        typed_config:
          "@type": type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.HttpConnectionManager
          stat_prefix: config_test
          http_filters:
            name: envoy.filters.http.router
          codec_type: HTTP1
          route_config:
            name: route_config_0
            validate_clusters: false
            virtual_hosts:
              name: integration
              routes:
              - route:
                  cluster: cluster_1
                match:
                  prefix: "/cluster1"
              - route:
                  cluster: cluster_2
                match:
                  prefix: "/cluster2"
              - route:
                  cluster: aggregate_cluster
                  retry_policy:
                    retry_priority:
                      name: envoy.retry_priorities.previous_priorities
                      typed_config:
                        "@type": type.googleapis.com/envoy.extensions.retry.priority.previous_priorities.v3.PreviousPrioritiesConfig
                        update_frequency: 1
                match:
                  prefix: "/aggregatecluster"
              domains: "*"
)EOF",
                                                  Platform::null_device_path));
}

class AggregateIntegrationTest
    : public testing::TestWithParam<std::tuple<Network::Address::IpVersion, bool>>,
      public HttpIntegrationTest {
public:
  AggregateIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP1, std::get<0>(GetParam()), config()),
        deferred_cluster_creation_(std::get<1>(GetParam())) {
    use_lds_ = false;
  }

  void TearDown() override { cleanUpXdsConnection(); }

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

  void acceptXdsConnection() {
    AssertionResult result = // xds_connection_ is filled with the new FakeHttpConnection.
        fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, xds_connection_);
    RELEASE_ASSERT(result, result.message());
    result = xds_connection_->waitForNewStream(*dispatcher_, xds_stream_);
    RELEASE_ASSERT(result, result.message());
    xds_stream_->startGrpcStream();
  }

  void setCircuitBreakerLimits(envoy::config::cluster::v3::Cluster& cluster,
                               const Thresholds& thresholds) {
    auto* cluster_circuit_breakers = cluster.mutable_circuit_breakers();

    auto* cluster_circuit_breakers_threshold_default = cluster_circuit_breakers->add_thresholds();
    cluster_circuit_breakers_threshold_default->set_priority(
        envoy::config::core::v3::RoutingPriority::DEFAULT);

    cluster_circuit_breakers_threshold_default->mutable_max_connections()->set_value(
        thresholds.max_connections);
    cluster_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(
        thresholds.max_pending_requests);
    cluster_circuit_breakers_threshold_default->mutable_max_requests()->set_value(
        thresholds.max_requests);
    cluster_circuit_breakers_threshold_default->mutable_max_retries()->set_value(
        thresholds.max_retries);
    cluster_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(
        thresholds.max_connection_pools);
    cluster_circuit_breakers_threshold_default->set_track_remaining(true);
  }

  void setMaxConcurrentStreams(envoy::config::cluster::v3::Cluster& cluster,
                               uint32_t max_concurrent_streams) {
    envoy::extensions::upstreams::http::v3::HttpProtocolOptions http_protocol_options;
    http_protocol_options.mutable_explicit_http_config()
        ->mutable_http2_protocol_options()
        ->mutable_max_concurrent_streams()
        ->set_value(max_concurrent_streams);
    (*cluster.mutable_typed_extension_protocol_options())
        ["envoy.extensions.upstreams.http.v3.HttpProtocolOptions"]
            .PackFrom(http_protocol_options);
  }

  void makeAggregateClustersListHaveOnlyOneCluster(
      envoy::config::cluster::v3::Cluster& aggregate_cluster) {
    auto* aggregate_cluster_type = aggregate_cluster.mutable_cluster_type();
    auto* aggregate_cluster_typed_config = aggregate_cluster_type->mutable_typed_config();
    envoy::extensions::clusters::aggregate::v3::ClusterConfig new_aggregate_cluster_typed_config;
    aggregate_cluster_typed_config->UnpackTo(&new_aggregate_cluster_typed_config);
    new_aggregate_cluster_typed_config.clear_clusters();
    new_aggregate_cluster_typed_config.add_clusters("cluster_1");
    aggregate_cluster_typed_config->PackFrom(new_aggregate_cluster_typed_config);
  }

  const bool deferred_cluster_creation_;
  envoy::config::cluster::v3::Cluster cluster1_;
  envoy::config::cluster::v3::Cluster cluster2_;
};

INSTANTIATE_TEST_SUITE_P(
    IpVersions, AggregateIntegrationTest,
    testing::Combine(testing::ValuesIn(TestEnvironment::getIpVersionsForTest()), testing::Bool()));

TEST_P(AggregateIntegrationTest, ClusterUpDownUp) {
  // Calls our initialize(), which includes establishing a listener, route, and cluster.
  testRouterHeaderOnlyRequestAndResponse(nullptr, FirstUpstreamIndex, "/aggregatecluster");

  // Tell Envoy that cluster_1 is gone.
  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster, {}, {},
                                                             {FirstClusterName}, "42");
  // We can continue the test once we're sure that Envoy's ClusterManager has made use of
  // the DiscoveryResponse that says cluster_1 is gone.
  test_server_->waitForCounterGe("cluster_manager.cluster_removed", 1);

  // Now that cluster_1 is gone, the listener (with its routing to cluster_1) should 503.
  BufferingStreamDecoderPtr response =
      IntegrationUtil::makeSingleRequest(lookupPort("http"), "GET", "/aggregatecluster", "",
                                         downstream_protocol_, version_, "foo.com");
  ASSERT_TRUE(response->complete());
  EXPECT_EQ("503", response->headers().getStatusValue());

  cleanupUpstreamAndDownstream();
  ASSERT_TRUE(codec_client_->waitForDisconnect());

  // Tell Envoy that cluster_1 is back.
  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "42", {}, {}, {}));
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "413");

  test_server_->waitForGaugeGe("cluster_manager.active_clusters", 3);
  testRouterHeaderOnlyRequestAndResponse(nullptr, FirstUpstreamIndex, "/aggregatecluster");

  cleanupUpstreamAndDownstream();
}

// Tests adding a cluster, adding another, then removing the first.
TEST_P(AggregateIntegrationTest, TwoClusters) {
  // Calls our initialize(), which includes establishing a listener, route, and cluster.
  testRouterHeaderOnlyRequestAndResponse(nullptr, FirstUpstreamIndex, "/aggregatecluster");

  cleanupUpstreamAndDownstream();
  ASSERT_TRUE(codec_client_->waitForDisconnect());

  // Tell Envoy that cluster_2 is here.
  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(
      Config::TypeUrl::get().Cluster, {cluster1_, cluster2_}, {cluster2_}, {}, "42");
  // The '4' includes the fake CDS server and aggregate cluster.
  test_server_->waitForGaugeGe("cluster_manager.active_clusters", 4);

  // A request for aggregate cluster should be fine.
  testRouterHeaderOnlyRequestAndResponse(nullptr, FirstUpstreamIndex, "/aggregatecluster");
  cleanupUpstreamAndDownstream();
  ASSERT_TRUE(codec_client_->waitForDisconnect());

  // Tell Envoy that cluster_1 is gone.
  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "42", {}, {}, {}));
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(
      Config::TypeUrl::get().Cluster, {cluster2_}, {}, {FirstClusterName}, "43");
  // We can continue the test once we're sure that Envoy's ClusterManager has made use of
  // the DiscoveryResponse that says cluster_1 is gone.
  test_server_->waitForCounterGe("cluster_manager.cluster_removed", 1);

  testRouterHeaderOnlyRequestAndResponse(nullptr, SecondUpstreamIndex, "/aggregatecluster");
  cleanupUpstreamAndDownstream();
  ASSERT_TRUE(codec_client_->waitForDisconnect());

  // Tell Envoy that cluster_1 is back.
  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "43", {}, {}, {}));
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(
      Config::TypeUrl::get().Cluster, {cluster1_, cluster2_}, {cluster1_}, {}, "413");

  test_server_->waitForGaugeGe("cluster_manager.active_clusters", 4);
  testRouterHeaderOnlyRequestAndResponse(nullptr, FirstUpstreamIndex, "/aggregatecluster");

  cleanupUpstreamAndDownstream();
}

// Test that the PreviousPriorities retry predicate works as expected. It is configured
// in this test to exclude a priority after a single failure, so the first failure
// on cluster_1 results in the retry going to cluster_2.
TEST_P(AggregateIntegrationTest, PreviousPrioritiesRetryPredicate) {
  initialize();

  // Tell Envoy that cluster_2 is here.
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(
      Config::TypeUrl::get().Cluster, {cluster1_, cluster2_}, {cluster2_}, {}, "42");
  // The '4' includes the fake CDS server and aggregate cluster.
  test_server_->waitForGaugeGe("cluster_manager.active_clusters", 4);

  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"x-forwarded-for", "10.0.0.1"},
                                     {"x-envoy-retry-on", "5xx"}},
      1024);
  waitForNextUpstreamRequest(FirstUpstreamIndex);
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, false);

  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
  ASSERT_TRUE(fake_upstream_connection_->close());
  ASSERT_TRUE(fake_upstream_connection_->waitForDisconnect());
  fake_upstream_connection_.reset();

  waitForNextUpstreamRequest(SecondUpstreamIndex);
  upstream_request_->encodeHeaders(default_response_headers_, true);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_TRUE(upstream_request_->complete());

  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
  cleanupUpstreamAndDownstream();
}

TEST_P(AggregateIntegrationTest, CircuitBreakerTestMaxConnections) {
  // make the downstream client use http2
  setDownstreamProtocol(Http::CodecType::HTTP2);

  config_helper_.addConfigModifier([this](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* aggregate_cluster = static_resources->mutable_clusters(1);

    makeAggregateClustersListHaveOnlyOneCluster(*aggregate_cluster);
    setCircuitBreakerLimits(*aggregate_cluster, Thresholds{.max_connections = 1});
    setMaxConcurrentStreams(*aggregate_cluster, 1U);
  });

  initialize();

  setCircuitBreakerLimits(cluster1_, Thresholds{.max_connections = 1});
  setMaxConcurrentStreams(cluster1_, 1U);

  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));

  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "56");

  // make sure we still have 3 active clusters
  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  // initial check
  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx",
                               1);
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_cx_overflow", 0);

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 1);
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_cx_overflow", 0);

  codec_client_ = makeHttpConnection(lookupPort("http"));

  // send the first request (this should go via "aggregate_cluster" through to "cluster_1")
  auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  // !!! ^this counts as a single stream
  // and we specified in the http2 protocol options on both clusters to allow
  // max_concurrent_streams: 1 so now one request (stream) should use up 1 entire connection

  // wait for the request to arrive at the upstream cluster
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // after creating a connection and sending a request to cluster_1 via aggregate cluster
  // aggregate_cluster: we expect the circuit breakers to not have changed, because they're not
  // used
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx",
                               1);
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_cx_overflow", 0);

  // cluster_1: we expect the circuit breakers to respond to the first connection
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 1);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 0);
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_cx_overflow", 0);

  // send a 2nd request that will be rejected by the circuit_breaker
  auto aggregate_cluster_response2 = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  // check the upstream_cx_overflow counters
  // we can't handle the request as the connection can not be created because of the circuit
  // breaker limit , but since the upstream_cx_overflow counter is incremented for cluster_1
  // meaning that the connection is not created and the request is rejected
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_cx_overflow", 0);
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_cx_overflow", 1);

  // send a 3rd request directly to cluster_1 to show that the circuit breakers on cluster_1 are
  // used by both the cluster_1 and aggregate_cluster.
  auto cluster1_response1 = codec_client_->makeHeaderOnlyRequest(Http::TestRequestHeaderMapImpl{
      {":method", "GET"}, {":path", "/cluster1"}, {":scheme", "http"}, {":authority", "host"}});

  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_cx_overflow", 0);
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_cx_overflow", 2);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_pending",
                               1022); // 2 pending requests

  // Send response for first request
  upstream_request_->encodeHeaders(default_response_headers_, true);
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
  ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());
  EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

  ASSERT_TRUE(fake_upstream_connection_->close());
  ASSERT_TRUE(fake_upstream_connection_->waitForDisconnect());

  // after sending the response
  // aggregate_cluster : we expect the circuit breakers to not have changed, because they're not
  // used
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx",
                               1);
  test_server_->waitForCounterGe("cluster.aggregate_cluster.upstream_cx_overflow", 0);

  // cluster_1 : we expect the circuit breakers to go back to initial state
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 1);
  test_server_->waitForCounterGe("cluster.cluster_1.upstream_cx_overflow", 3);

  cleanupUpstreamAndDownstream();
}

TEST_P(AggregateIntegrationTest, CircuitBreakerTestMaxRequests) {
  // let's specifically use http2 on the downstream client
  // so that we can use the single code_client_ to send many requests (and don't have to create
  // multiple codec_clients_ if we were using http1.1)
  setDownstreamProtocol(Http::CodecType::HTTP2);

  config_helper_.addConfigModifier([this](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* aggregate_cluster =
        static_resources->mutable_clusters(1); // use name of the aggregate cluster

    makeAggregateClustersListHaveOnlyOneCluster(*aggregate_cluster);
    setCircuitBreakerLimits(*aggregate_cluster, Thresholds{.max_requests = 1});
  });

  initialize();

  setCircuitBreakerLimits(cluster1_, Thresholds{.max_requests = 1});

  // !!! we need to send the updated cluster1_ to envoy via xds so we the "remaining" stats
  // become available (because they are not initialized by default during cluster creation) do
  // we need this expectation ?
  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));

  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "56");

  // make sure we still only have 3 clusters: "my_cds_cluster" [0], "aggregate_cluster" [1],
  // "cluster_1" [2]
  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  // check the initial circuit breaker stats
  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_open", 0);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq",
                               1);

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_rq", 1);

  // now we want to make the requests to check the circuit breakers behaviour...
  // creates http connection from downstream to envoy
  codec_client_ = makeHttpConnection(lookupPort("http"));

  // send the first request (this should go via "aggregate_cluster" through to "cluster_1")
  auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  // tell the upstream cluster (index 2) (which is cluster_1) to wait for the request to arrive
  // creates connection from envoy to upstream and send request to cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // check the circuit breaker stats after sending request
  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_open", 0);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq",
                               1);

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_open", 1);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_rq", 0);

  // sending a second request to /aggregatecluster before the first request is completed
  auto aggregate_cluster_response2 = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  ASSERT_TRUE(aggregate_cluster_response2->waitForEndStream());

  // check the status of the response2 is 503 (because the circuit breaker is triggered and so
  // the request to /aggregatecluster was rejected)
  EXPECT_EQ("503", aggregate_cluster_response2->headers().getStatusValue());

  // check the upstream_rq_pending_overflow counters
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_pending_overflow", 0);
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_pending_overflow", 1);

  // sending a request  directly to /cluster1 while circuit breaker is tripped
  auto cluster1_response1 = codec_client_->makeHeaderOnlyRequest(Http::TestRequestHeaderMapImpl{
      {":method", "GET"}, {":path", "/cluster1"}, {":scheme", "http"}, {":authority", "host"}});

  // wait for cluster1 response to complete and return to the client
  ASSERT_TRUE(cluster1_response1->waitForEndStream());

  // check the status of the response is 503 (because the circuit breaker is triggered and so
  // the request to /cluster1 was rejected)
  EXPECT_EQ("503", cluster1_response1->headers().getStatusValue());

  // check the upstream_rq_pending_overflow counters
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_pending_overflow", 0);
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_pending_overflow", 2);

  // send response back to complete the first request
  upstream_request_->encodeHeaders(default_response_headers_, true);

  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());

  EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

  // now check the circuit breaker stats again after receiving the response back
  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_open", 0);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq",
                               1);
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_pending_overflow", 0);

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_rq", 1);
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_pending_overflow",
                                 2); // it's a counter it doesn't go back to 0 unless we reset it

  cleanupUpstreamAndDownstream();
}

TEST_P(AggregateIntegrationTest, CircuitBreakerTestMaxPendingRequests) {
  setDownstreamProtocol(Http::CodecType::HTTP2);
  config_helper_.addConfigModifier([this](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* aggregate_cluster =
        static_resources->mutable_clusters(1); // use name of the aggregate cluster

    makeAggregateClustersListHaveOnlyOneCluster(*aggregate_cluster);
    setCircuitBreakerLimits(*aggregate_cluster,
                            Thresholds{.max_connections = 1, .max_pending_requests = 1});
    setMaxConcurrentStreams(*aggregate_cluster, 1U);
  });

  initialize();

  setCircuitBreakerLimits(cluster1_, Thresholds{.max_connections = 1, .max_pending_requests = 1});
  setMaxConcurrentStreams(cluster1_, 1U);

  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));

  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "56");

  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  // check the initial circuit breaker stats
  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open",
                               0);
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_pending", 1);

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_pending", 1);

  codec_client_ = makeHttpConnection(lookupPort("http"));

  // make the first request
  // the connection should now be "saturated" since it will only allow 1 concurrent stream
  // now subsequent requests should go into a "pending" state
  auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  // wait for the first request to arrive at cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // make the second request, this will be the first "pending" request
  auto aggregate_cluster_response2 = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  // check the circuit breaker stats after sending a pending request
  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open",
                               0);
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_pending", 1);
  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open",
                               1); // !! the circuit breaker should now be triggered
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_pending", 0);

  // make the third request, this will be the second "pending" request
  auto aggregate_cluster_response3 = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  // the third request should fail immediately with 503
  // because the max_pending_requests circuit breaker is triggered
  ASSERT_TRUE(aggregate_cluster_response3->waitForEndStream());
  EXPECT_EQ("503", aggregate_cluster_response3->headers().getStatusValue());

  // check the upstream_rq_pending_overflow counters
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_pending_overflow", 0);
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_pending_overflow", 1);

  // sending a request  directly to /cluster1 while circuit breaker is tripped
  auto cluster1_response1 = codec_client_->makeHeaderOnlyRequest(Http::TestRequestHeaderMapImpl{
      {":method", "GET"}, {":path", "/cluster1"}, {":scheme", "http"}, {":authority", "host"}});

  // the request should fail immediately with 503
  // because the max_pending_requests circuit breaker is triggered
  ASSERT_TRUE(cluster1_response1->waitForEndStream());
  EXPECT_EQ("503", cluster1_response1->headers().getStatusValue());

  // check the upstream_rq_pending_overflow counters
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_pending_overflow", 0);
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_pending_overflow", 2);

  // complete the first request/response
  upstream_request_->encodeHeaders(default_response_headers_, true);
  ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());
  EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

  // wait for the second request to reach cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // complete the second request/response
  upstream_request_->encodeHeaders(default_response_headers_, true);
  ASSERT_TRUE(aggregate_cluster_response2->waitForEndStream());
  EXPECT_EQ("200", aggregate_cluster_response2->headers().getStatusValue());

  // now check the circuit breaker stats again after receiving all the responses back
  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open",
                               0);
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_pending", 1);
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_pending_overflow", 0);

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_pending", 1);
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_pending_overflow", 2);

  cleanupUpstreamAndDownstream();
}

TEST_P(AggregateIntegrationTest, CircuitBreakerMaxRetriesTest) {

  setDownstreamProtocol(Http::CodecType::HTTP2);

  config_helper_.addConfigModifier([this](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* listener = static_resources->mutable_listeners(0);
    auto* filter_chain = listener->mutable_filter_chains(0);
    auto* filter = filter_chain->mutable_filters(0);
    envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager
        http_connection_manager;
    filter->mutable_typed_config()->UnpackTo(&http_connection_manager);
    auto* virtual_host = http_connection_manager.mutable_route_config()->mutable_virtual_hosts(0);
    auto* aggregate_cluster_route = virtual_host->mutable_routes(2);
    aggregate_cluster_route->mutable_route()->mutable_retry_policy()->clear_retry_priority();

    // adjust the retry policy on the /aggregatecluster route
    aggregate_cluster_route->mutable_route()->mutable_retry_policy()->mutable_retry_on()->assign(
        "5xx"); // retry on 5xx
    auto* cluster1_route = virtual_host->mutable_routes(0);

    // adjust the retry policy on the /cluster1 route
    cluster1_route->mutable_route()->mutable_retry_policy()->mutable_retry_on()->assign(
        "5xx"); // retry on 5xx
    filter->mutable_typed_config()->PackFrom(http_connection_manager);

    auto* aggregate_cluster =
        static_resources->mutable_clusters(1); // use name of the aggregate cluster

    makeAggregateClustersListHaveOnlyOneCluster(*aggregate_cluster);
    setCircuitBreakerLimits(*aggregate_cluster, Thresholds{.max_retries = 1});
  });

  initialize();

  setCircuitBreakerLimits(cluster1_, Thresholds{.max_retries = 1});

  // update cluster1 via xDS
  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "56");
  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  // confirm we are in the default state for both circuit breakers
  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open",
                               0);
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 1);
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 0);

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 1);
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0);

  codec_client_ = makeHttpConnection(lookupPort("http"));

  // send the first request to /aggregatecluster
  auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  // wait for the first request (/aggregatecluster) to reach cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // deliberately respond to the first request (/aggregatecluster) with 503 to trigger the first
  // retry
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

  // wait for the first request retry (/aggregatecluster) to reach cluster_1 [this comes from an
  // automatic retry]
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // save a reference to this specific request, so we can access it later, because
  // upstream_request_ will get overwritten
  auto& first_upstream_request_retry = *upstream_request_;

  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open",
                               1); // !! aggregate cluster circuit breaker is triggered
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_retries",
      0); // no more retries allowed
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 0);

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open",
                               0); // unaffected
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries",
                               1); // unaffected
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow",
                                 0); // unaffected

  // send the second request to /aggregatecluster
  auto aggregate_cluster_response2 = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  // wait for the second request (/aggregatecluster) to arrive at cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // deliberately respond to the second request (/aggregatecluster) with a 503 to trigger a
  // retry
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open",
                               1); // persisting
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 0); // persisting
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow",
                                 1); // 1 overflow on the aggregate cluster circuit breaker

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open",
                               0); // unaffected
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries",
                               1); // unaffected
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow",
                                 0); // unaffected

  // the second retry should be automatically rejected

  // send the third request directly to /cluster1
  auto cluster1_response = codec_client_->makeHeaderOnlyRequest(Http::TestRequestHeaderMapImpl{
      {":method", "GET"}, {":path", "/cluster1"}, {":scheme", "http"}, {":authority", "host"}});

  // wait for the third request (/cluster1) to arrive at cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // deliberately respond to the third request (/cluster1) with a 503 to trigger a retry
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

  // wait for the third request retry (/cluster1) to reach cluster_1 [this comes from an
  // automatic retry]
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open",
                               1); // persisting
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 0); // persisting
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow",
                                 1); // persisting

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open",
                               1); // cluster1 circuit breaker now triggered (but is separate
                                   // from the aggregate cluster circuit breaker)
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries",
                               0); // see comment above
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow",
                                 0); // unaffected

  // respond to the third request retry
  // THE FACT WE CAN DO THIS PROVES THE AGGREGATE CLUSTER CIRCUIT BREAKER IS SEPARATE FROM THE
  // CLUSTER1 CIRCUIT BREAKER OTHERWISE THIS REQUEST WOULD BE AUTOREJECTED IF THE
  // /aggregatecluster route max_retries circuit breaker affected the underlying cluster1
  upstream_request_->encodeHeaders(default_response_headers_, true);

  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow",
                                 0); // unaffected

  // complete request/response3 (/cluster1)
  ASSERT_TRUE(cluster1_response->waitForEndStream());
  EXPECT_EQ("200", cluster1_response->headers().getStatusValue());

  // (finally) respond to the first request retry (/aggregatecluster)
  first_upstream_request_retry.encodeHeaders(default_response_headers_, true);

  // we don't need to respond to the second request retry, because it will be automatically
  // rejected by the circuit breaker

  // complete request/response1 (/aggregatecluster)
  ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());
  EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

  // complete request/response2 (/aggregatecluster)
  ASSERT_TRUE(aggregate_cluster_response2->waitForEndStream());
  EXPECT_EQ("503",
            aggregate_cluster_response2->headers()
                .getStatusValue()); // this was from the auto - rejection

  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open",
                               0); // returned to its initial state
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_retries",
      1); // returned to its initial state
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow",
                                 1); // overflowed 1 time

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open",
                               0); // returned to its initial state
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries",
                               1); // returned to its initial state
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow",
                                 0); // unaffected

  cleanupUpstreamAndDownstream();
}

} // namespace
} // namespace Envoy
