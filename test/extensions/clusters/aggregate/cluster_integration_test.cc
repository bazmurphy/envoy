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

  // !! TEMPORARY
  void printStatsForMaxRetries(const std::string& prefix) {
    std::cout << "--------------------" << std::endl;
    std::cout << prefix << " aggregate_cluster rq_retry_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster remaining_retries: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_retry: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_retry_success: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_success")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_retry_overflow: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_overflow")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_retry_limit_exceeded: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_limit_exceeded")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_retry_backoff_exponential: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_backoff_exponential")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_retry_backoff_ratelimited: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_backoff_ratelimited")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_total")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_active")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_cancelled: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_cancelled")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_timeout: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_timeout")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_completed: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_completed")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_max_duration_reached: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_max_duration_reached")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_per_try_timeout: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_per_try_timeout")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_rx_reset: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_rx_reset")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_tx_reset: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_tx_reset")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_time: " << test_server_->histogram("cluster.aggregate_cluster.upstream_rq_time")->quantileSummary() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_503: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_503")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_504: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_504")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_cx_active")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_total")->value() << std::endl;
    std::cout << "--------------------" << std::endl;
    std::cout << prefix << " cluster_1 rq_retry_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_retry_open")->value() << std::endl;
    std::cout << prefix << " cluster_1 remaining_retries: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_retries")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_retry: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_retry_success: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_success")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_retry_overflow: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_overflow")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_retry_limit_exceeded: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_limit_exceeded")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_retry_backoff_exponential: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_backoff_exponential")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_retry_backoff_ratelimited: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_backoff_ratelimited")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_active")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_cancelled: " << test_server_->counter("cluster.cluster_1.upstream_rq_cancelled")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_timeout: " << test_server_->counter("cluster.cluster_1.upstream_rq_timeout")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_completed: " << test_server_->counter("cluster.cluster_1.upstream_rq_completed")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_max_duration_reached: " << test_server_->counter("cluster.cluster_1.upstream_rq_max_duration_reached")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_per_try_timeout: " << test_server_->counter("cluster.cluster_1.upstream_rq_per_try_timeout")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_rx_reset: " << test_server_->counter("cluster.cluster_1.upstream_rq_rx_reset")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_tx_reset: " << test_server_->counter("cluster.cluster_1.upstream_rq_tx_reset")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_time: " << test_server_->histogram("cluster.cluster_1.upstream_rq_time")->quantileSummary() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_503: " << test_server_->counter("cluster.cluster_1.upstream_rq_503")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_504: " << test_server_->counter("cluster.cluster_1.upstream_rq_504")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_active: " << test_server_->gauge("cluster.cluster_1.upstream_cx_active")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_total")->value() << std::endl;
  }

  // !! TEMPORARY
  void printStatsForMaxConnectionPools(const std::string& prefix) {
    std::cout << prefix << " aggregate_cluster cx_pool_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.cx_pool_open")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster remaining_cx_pools: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx_pools")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_pool_overflow: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_pool_overflow")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_cx_active")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_total")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_http1_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_http1_total")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_http2_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_http2_total")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_active")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_total")->value() << std::endl;

    std::cout << prefix << " aggregate_cluster upstream_rq_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_active")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 cx_pool_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.cx_pool_open")->value() << std::endl;
    std::cout << prefix << " cluster_1 remaining_cx_pools: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_cx_pools")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_pool_overflow: " << test_server_->counter("cluster.cluster_1.upstream_cx_pool_overflow")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_active: " << test_server_->gauge("cluster.cluster_1.upstream_cx_active")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_http1_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_http1_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_http2_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_http2_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_active")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_total")->value() << std::endl;
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

// --------------------

// https://www.envoyproxy.io/docs/envoy/latest/configuration/upstream/cluster_manager/cluster_stats.html

// STATS OF INTEREST 

// CIRCUIT BREAKER

// cx_open (Gauge) - Whether the connection circuit breaker is under its concurrency limit (0) or is at capacity and no longer admitting (1)
// cx_pool_open (Gauge) - Whether the connection pool circuit breaker is under its concurrency limit (0) or is at capacity and no longer admitting (1)
// rq_pending_open (Gauge) - Whether the pending requests circuit breaker is under its concurrency limit (0) or is at capacity and no longer admitting (1)
// rq_open (Gauge) - Whether the requests circuit breaker is under its concurrency limit (0) or is at capacity and no longer admitting (1)
// rq_retry_open (Gauge) - Whether the retry circuit breaker is under its concurrency limit (0) or is at capacity and no longer admitting (1)

// cluster.aggregate_cluster.circuit_breakers.default.cx_open
// cluster.aggregate_cluster.circuit_breakers.default.cx_pool_open
// cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open
// cluster.aggregate_cluster.circuit_breakers.default.rq_open
// cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open

// REMAINING

// remaining_cx (Gauge) - Number of remaining connections until the circuit breaker reaches its concurrency limit
// remaining_pending (Gauge) - Number of remaining pending requests until the circuit breaker reaches its concurrency limit
// remaining_rq (Gauge) - Number of remaining requests until the circuit breaker reaches its concurrency limit
// remaining_retries (Gauge) - Number of remaining retries until the circuit breaker reaches its concurrency limit

// cluster.aggregate_cluster.circuit_breakers.default.remaining_cx
// cluster.aggregate_cluster.circuit_breakers.default.remaining_pending
// cluster.aggregate_cluster.circuit_breakers.default.remaining_rq
// cluster.aggregate_cluster.circuit_breakers.default.remaining_retries

// OVERFLOW

// upstream_cx_overflow (Counter) - Total times that the cluster’s connection circuit breaker overflowed
// upstream_cx_pool_overflow (Counter) - Total times that the cluster’s connection pool circuit breaker overflowed
// upstream_rq_pending_overflow (Counter) - Total requests that overflowed connection pool or requests (mainly for HTTP/2 and above) circuit breaking and were failed
// upstream_rq_retry_overflow (Counter) - Total requests not retried due to circuit breaking or exceeding the retry budget

// cluster.aggregate_cluster.upstream_cx_overflow
// cluster.aggregate_cluster.upstream_cx_pool_overflow
// cluster.aggregate_cluster.upstream_rq_pending_overflow
// cluster.aggregate_cluster.upstream_rq_retry_overflow

// OTHERS

// upstream_rq_active (Gauge) - Total active requests
// upstream_rq_total (Counter) - Total requests
// upstream_rq_pending_active (Gauge) - Total active requests pending a connection pool connection
// upstream_rq_pending_total (Counter) Total requests pending a connection pool connection
// upstream_rq_cancelled (Counter) - Total requests cancelled before obtaining a connection pool connection
// upstream_rq_timeout (Counter) - Total requests that timed out waiting for a response
// upstream_rq_completed (Counter) - Total upstream requests completed
// upstream_rq_<*xx> (Counter) - Aggregate HTTP response codes (e.g., 2xx, 3xx, etc.)
// upstream_rq_<*> (Counter) - Specific HTTP response codes (e.g., 201, 302, etc.)
// upstream_rq_time (Histogram) - Request time milliseconds
// upstream_rq_max_duration_reached (Counter) - Total requests closed due to max duration reached
// upstream_rq_per_try_timeout (Counter) - Total requests that hit the per try timeout (except when request hedging is enabled)
// upstream_rq_rx_reset (Counter) - Total requests that were reset remotely
// upstream_rq_tx_reset (Counter) - Total requests that were reset locally
// upstream_rq_retry (Counter) - Total request retries
// upstream_rq_retry_backoff_exponential (Counter) - Total retries using the exponential backoff strategy
// upstream_rq_retry_backoff_ratelimited (Counter) - Total retries using the ratelimited backoff strategy
// upstream_rq_retry_limit_exceeded (Counter) - Total requests not retried due to exceeding the configured number of maximum retries
// upstream_rq_retry_success (Counter) - Total request retry successes
// upstream_rq_retry_overflow (Counter) - Total requests not retried due to circuit breaking or exceeding the retry budget

// cluster.aggregate_cluster.upstream_rq_active
// cluster.aggregate_cluster.upstream_rq_total
// cluster.aggregate_cluster.upstream_rq_pending_active
// cluster.aggregate_cluster.upstream_rq_pending_total

// upstream_cx_active (Gauge) - Total active connections
// upstream cx_total (Counter) - Total connections

// cluster.aggregate_cluster.upstream_cx_active
// cluster.aggregate_cluster.cx_total

// --------------------

TEST_P(AggregateIntegrationTest, CircuitBreakerTestMaxRequests) {
  std::cout << "---------- 00 TEST START" << std::endl;

  // let's specifically use http2 on the downstream client
  // so that we can use the single code_client_ to send many requests (and don't have to create multiple codec_clients_ if we were using http1.1)
  setDownstreamProtocol(Http::CodecType::HTTP2);

  // this is how we can modify the config (from the top of this file) before calling "initialize()"
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    std::cout << "---------- 01 CONFIG MODIFY START" << std::endl;

    // --------------------

    // we want to access the "static_resources" to modify the "aggregate_cluster"

    // "aggregate_cluster" is in "static_resources" > "clusters"
    auto* static_resources = bootstrap.mutable_static_resources();

    // "aggregate_cluster" is at index 1 of the "static_resources" > "clusters"
    auto* aggregate_cluster = static_resources->mutable_clusters(1);
    std::cout << "aggregate_cluster name(): " << aggregate_cluster->name() << std::endl;

    // --------------------

    // we want to reduce the "aggregate_cluster" "clusters" list down to just "cluster_1" (and therefore remove "cluster_2") so we can control our tests
    // because "aggregate_cluster" is in "static_resources" not in "dynamic_resources" this is a bit more fiddly    

    // get the "typed_config" of the "aggregate_cluster"
    auto* aggregate_cluster_type = aggregate_cluster->mutable_cluster_type();
    auto* aggregate_cluster_typed_config = aggregate_cluster_type->mutable_typed_config();

    // make a new ClusterConfig to parse the "typed_config" into
    envoy::extensions::clusters::aggregate::v3::ClusterConfig temp_aggregate_cluster_typed_config;

    // unpack the typed_config into cluster_config
    aggregate_cluster_typed_config->UnpackTo(&temp_aggregate_cluster_typed_config);
    
    std::cout << "BEFORE temp_aggregate_cluster_typed_config.clusters_size(): " << temp_aggregate_cluster_typed_config.clusters_size() << std::endl;
    for (int i = 0; i < temp_aggregate_cluster_typed_config.clusters_size(); i++) {
      std::cout << "BEFORE temp_aggregate_cluster_typed_config clusters[" << i << "]: " << temp_aggregate_cluster_typed_config.clusters(i) << std::endl;
    }

    // clear the existing clusters list
    temp_aggregate_cluster_typed_config.clear_clusters();
    
    // add only "cluster_1" back to the clusters list
    temp_aggregate_cluster_typed_config.add_clusters("cluster_1");

    std::cout << "AFTER temp_aggregate_cluster_typed_config.clusters_size(): " << temp_aggregate_cluster_typed_config.clusters_size() << std::endl;
    for (int i = 0; i < temp_aggregate_cluster_typed_config.clusters_size(); i++) {
      std::cout << "AFTER temp_aggregate_cluster_typed_config clusters[" << i << "]: " << temp_aggregate_cluster_typed_config.clusters(i) << std::endl;
    }
    
    // re-pack the adjusted config back
    aggregate_cluster_typed_config->PackFrom(temp_aggregate_cluster_typed_config);

    // --------------------
  
    // we want to set the circuit breaker configuration on the aggregate cluster

    std::cout << "BEFORE aggregate_cluster has_circuit_breakers(): " << aggregate_cluster->has_circuit_breakers() << std::endl;

    auto* aggregate_cluster_circuit_breakers = aggregate_cluster->mutable_circuit_breakers();

    std::cout << "BEFORE aggregate_cluster thresholds_size(): " << aggregate_cluster_circuit_breakers->thresholds_size() << std::endl;

    // set the aggregate_cluster circuit breakers
    auto* aggregate_cluster_circuit_breakers_threshold_default = aggregate_cluster_circuit_breakers->add_thresholds();
    aggregate_cluster_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1); // set this to 1
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->set_track_remaining(true);

    std::cout << "AFTER aggregate_cluster thresholds_size(): " << aggregate_cluster_circuit_breakers->thresholds_size() << std::endl;
    std::cout << "AFTER aggregate_cluster has_circuit_breakers(): " << aggregate_cluster->has_circuit_breakers() << std::endl;

    std::cout << "aggregate_cluster threshold default max_connections().value(): " << aggregate_cluster_circuit_breakers_threshold_default->max_connections().value() << std::endl;
    std::cout << "aggregate_cluster threshold default max_pending_requests().value(): " << aggregate_cluster_circuit_breakers_threshold_default->max_pending_requests().value() << std::endl;
    std::cout << "aggregate_cluster threshold default max_requests().value(): " << aggregate_cluster_circuit_breakers_threshold_default->max_requests().value() << std::endl;
    std::cout << "aggregate_cluster threshold default max_retries().value(): " << aggregate_cluster_circuit_breakers_threshold_default->max_retries().value() << std::endl;
    std::cout << "aggregate_cluster threshold default track_remaining(): " << aggregate_cluster_circuit_breakers_threshold_default->track_remaining() << std::endl;

    // --------------------
    std::cout << "---------- 02 CONFIG MODIFY FINISH" << std::endl;
  });

  std::cout << "---------- 03 INITIALIZE START" << std::endl;

  // --------------------

  // now call initialize (and that will add cluster_1 to the "dynamic_resources" > "clusters")

  initialize();

  // --------------------

  std::cout << "---------- 04 INITIALIZE FINISH" << std::endl;

  std::cout << "cluster1_ name(): " << cluster1_.name() << std::endl;

  std::cout << "BEFORE cluster1_ has_circuit_breakers(): " << cluster1_.has_circuit_breakers() << std::endl;

  auto* cluster1_circuit_breakers = cluster1_.mutable_circuit_breakers();

  std::cout << "BEFORE cluster1_circuit_breakers thresholds_size(): " << cluster1_circuit_breakers->thresholds_size() << std::endl;

  auto* cluster1_circuit_breakers_threshold_default = cluster1_circuit_breakers->add_thresholds();
  cluster1_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  cluster1_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1); // set this to 1
  cluster1_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000);// set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->set_track_remaining(true);

  std::cout << "AFTER cluster1_ has_circuit_breakers(): " << cluster1_.has_circuit_breakers() << std::endl;
  std::cout << "AFTER cluster1_circuit_breakers thresholds_size(): " << cluster1_circuit_breakers->thresholds_size() << std::endl;

  std::cout << "cluster1_ threshold default max_connections().value(): " << cluster1_circuit_breakers_threshold_default->max_connections().value() << std::endl;
  std::cout << "cluster1_ threshold default max_pending_requests().value(): " << cluster1_circuit_breakers_threshold_default->max_pending_requests().value() << std::endl;
  std::cout << "cluster1_ threshold default max_requests().value(): " << cluster1_circuit_breakers_threshold_default->max_requests().value() << std::endl;
  std::cout << "cluster1_ threshold default max_retries().value(): " << cluster1_circuit_breakers_threshold_default->max_retries().value() << std::endl;
  std::cout << "cluster1_ threshold default track_remaining(): " << cluster1_circuit_breakers_threshold_default->track_remaining() << std::endl;

  // --------------------

  std::cout << "---------- 05 UPDATING XDS CONFIG FOR cluster1_" << std::endl;

  // !!! we need to send the updated cluster1_ to envoy via xds so we the "remaining" stats become available
  // (because they are not initialized by default during cluster creation)

  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));

  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster, {cluster1_}, {cluster1_}, {}, "56");

  // make sure we still only have 3 clusters: "my_cds_cluster" [0], "aggregate_cluster" [1], "cluster_1" [2]
  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  // wait to make sure the cluster_1 "remaining" gauges are ready
  test_server_->waitForGaugeGe("cluster.cluster_1.circuit_breakers.default.remaining_rq", 0);

  // --------------------

  // check the initial circuit breaker stats

  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_open", 0);

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_open", 0);

  // aggregate_cluster max_requests
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq")->value(), 1);

  // cluster_1 max_requests
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_rq")->value(), 1);
 
  std::cout << "---------- 06 CIRCUIT BREAKER STATS [BEFORE]" << std::endl;

  // aggregate_cluster max_requests
  std::cout << "BEFORE request/response1 aggregate_cluster rq_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_open")->value() << std::endl;
  std::cout << "BEFORE request/response1 aggregate_cluster remaining_rq: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq")->value() << std::endl;

  // cluster_1 max_requests
  std::cout << "BEFORE request/response1 cluster_1 rq_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_open")->value() << std::endl;
  std::cout << "BEFORE request/response1 cluster_1 remaining_rq: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_rq")->value() << std::endl;

  // --------------------

  std::cout << "---------- 07 MAKING HTTP CONNECTION" << std::endl;

  // now we want to make the requests to check the circuit breakers behaviour...

  codec_client_ = makeHttpConnection(lookupPort("http"));

  std::cout << "---------- 08 SENDING REQUEST TO /aggregatecluster" << std::endl;

  // send the first request (this should go via "aggregate_cluster" through to "cluster_1")
  auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  std::cout << "---------- 09 WAIT FOR REQUEST TO ARRIVE AT cluster_1" << std::endl;
  
  // tell the upstream cluster (index 2) (which is cluster_1) to wait for the request to arrive
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  std::cout << "---------- 10 WAIT FOR THE REQUEST TO TRIGGER THE CIRCUIT BREAKER(S)" << std::endl;

  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_open", 0);
  // test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open", 0);

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_open", 1);
  // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 0);
  
  // now check the circuit breakers again:

  // aggregate_cluster max_requests
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq")->value(), 1);
  
  // cluster_1 max_requests
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_open")->value(), 1); // !! the circuit breaker is triggered
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_rq")->value(), 0); // !! there are no more requests allowed
  
  std::cout << "---------- 11 CIRCUIT BREAKER STATS [DURING]" << std::endl;

  // aggregate_cluster
  std::cout << "DURING request/response1 aggregate_cluster rq_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_open")->value() << std::endl;
  std::cout << "DURING request/response1 aggregate_cluster remaining_rq: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq")->value() << std::endl;

  // cluster_1
  std::cout << "DURING request/response1 cluster_1 rq_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_open")->value() << std::endl;
  std::cout << "DURING request/response1 cluster_1 remaining_rq: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_rq")->value() << std::endl;

  // now complete the request
  std::cout << "---------- 12 ENCODING HEADERS AND ENDING STREAM, RETURNING RESPONSE" << std::endl;
  // respond with headers
  upstream_request_->encodeHeaders(default_response_headers_, true); // default_response_headers_ is just {{":status", "200"}}, the bool is to denote end of stream or not
  
  // wait for the end of stream which should come from above (because of the true bool)
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  // now check the circuit breaker stats again

  // aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_open", 0);

  // cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_open", 0);
  
  // aggregate_cluster max_requests
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq")->value(), 1);
  
  // cluster_1 max_requests
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_open")->value(), 0); // the circuit breaker is back to its initial state
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_rq")->value(), 1); // and this is also back to its initial state

  std::cout << "---------- 13 CIRCUIT BREAKER STATS [AFTER]" << std::endl;

  // aggregate_cluster
  std::cout << "AFTER request/response1 aggregate_cluster rq_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_open")->value() << std::endl;
  std::cout << "AFTER request/response1 aggregate_cluster remaining_rq: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq")->value() << std::endl;

  // cluster_1
  std::cout << "AFTER request/response1 cluster_1 rq_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_open")->value() << std::endl;
  std::cout << "AFTER request/response1 cluster_1 remaining_rq: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_rq")->value() << std::endl;

  // --------------------
  
  std::cout << "---------- 14 WAIT FOR RESPONSE END OF STREAM AND CHECK FOR 200" << std::endl;

  ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());

  EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

  // --------------------

  // send the first request to /aggregatecluster
  auto aggregate_cluster_response2 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  // tell the upstream cluster (index 2) (which is cluster_1) to wait for the request2 to arrive
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // send the second request to /aggregatecluster
  auto aggregate_cluster_response3 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  // wait for the response to complete and return to the client
  ASSERT_TRUE(aggregate_cluster_response3->waitForEndStream());

  // check the status of the response is 503 (because the circuit breaker is triggered and so the request to /aggregatecluster was rejected)
  EXPECT_EQ("503", aggregate_cluster_response3->headers().getStatusValue());
  std::cout << "aggregate_cluster_response3 headers().getStatusValue(): " << aggregate_cluster_response3->headers().getStatusValue() << std::endl;

  // check the upstream_rq_pending_overflow counters:
  EXPECT_EQ(test_server_->counter("cluster.aggregate_cluster.upstream_rq_pending_overflow")->value(), 0);
  EXPECT_EQ(test_server_->counter("cluster.cluster_1.upstream_rq_pending_overflow")->value(), 1); // the overflow is 1 because the circuit breaker rejected the request

  // send the first request to /cluster1
  auto cluster1_response1 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/cluster1"},{":scheme", "http"},{":authority", "host"}}
  );

  // wait for cluster1 response to complete and return to the client
  ASSERT_TRUE(cluster1_response1->waitForEndStream());

  // check the status of the response is 503 (because the circuit breaker is triggered and so the request to /cluster1 was rejected)
  EXPECT_EQ("503", cluster1_response1->headers().getStatusValue());
  std::cout << "cluster1_response1 headers().getStatusValue(): " << cluster1_response1->headers().getStatusValue() << std::endl;

  // check the upstream_rq_pending_overflow counters:
  EXPECT_EQ(test_server_->counter("cluster.aggregate_cluster.upstream_rq_pending_overflow")->value(), 0);
  EXPECT_EQ(test_server_->counter("cluster.cluster_1.upstream_rq_pending_overflow")->value(), 2); // the overflow is now 2

  // allow the first request to have its header encoded
  upstream_request_->encodeHeaders(default_response_headers_, true);

  // wait for the response to complete and return to the client
  ASSERT_TRUE(aggregate_cluster_response2->waitForEndStream());

  // check the status of the response is 200
  EXPECT_EQ("200", aggregate_cluster_response2->headers().getStatusValue());
  std::cout << "aggregate_cluster_response2 headers().getStatusValue(): " << aggregate_cluster_response2->headers().getStatusValue() << std::endl;

  std::cout << "aggregate_cluster upstream_rq_pending_overflow: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_pending_overflow")->value() << std::endl;
  std::cout << "cluster_1 upstream_rq_pending_overflow: " << test_server_->counter("cluster.cluster_1.upstream_rq_pending_overflow")->value() << std::endl;
  
  // we can reset stats for subsequent tests if we need to
  // test_server_->counter("cluster.cluster_1.upstream_rq_pending_overflow")->reset();
  // std::cout << "RESET COUNTER TEST cluster_1 upstream_rq_pending_overflow: " << test_server_->counter("cluster.cluster_1.upstream_rq_pending_overflow")->value() << std::endl;

  // --------------------

  // "test requires explicit cleanupUpstreamAndDownstream"
  cleanupUpstreamAndDownstream();

  // part of the purpose of cleanUpstreamAndDownstream() is to close the codec_client_ connection
  // but this doesn't seem to work properly? why??
  // for now let's do it manually...
  codec_client_->close();

  std::cout << "---------- 99 TEST END" << std::endl;
}

// --------------------

// cx_open (Gauge) - Whether the connection circuit breaker is under its concurrency limit (0) or is at capacity and no longer admitting (1)

// the connections means the number of connections made between envoy and the upstream services
// clusters always talk about upstream

//         downstream subsystem       upstream subsystem
// downstream client -->     [] envoy []        --> upstream cluster
//                    ^      ^         ^          ^ connection between envoy and an upstream cluster
//          codec client     listener  clustermgr   that connection can be http1.1/2/3 
//                           hcm                   (depending on how envoy is configured, regarding establishing that upstream connection)
//                           routerfilter 

// if its http1.1 then a connection can only handle a single request at a time, so if we want 2 requests, then 2 connections are required
// if its http2 then we can use multiplexing to send both requests at the same time using a single connection

// the max_connections circuit breaker limits how many of those connections are allowed to exist
// theres an option in envoy to set an object that describes how to do HTTP2
// and we can tell the upstream that it can handle at most one stream per connection
// by using max_concurrent_streams setting on the cluster with a value of 1

// we need to add the http2 protocol options with max_concurrent_streams set to 1 on BOTH clusters to their configs:

// ABOUT "max_concurrent_streams"

// https://www.envoyproxy.io/docs/envoy/latest/api-v3/config/core/v3/protocol.proto#envoy-v3-api-msg-config-core-v3-http2protocoloptions

// config.core.v3.Http2ProtocolOptions

// {
//   "hpack_table_size": {...},
//   "max_concurrent_streams": {...},
//   "initial_stream_window_size": {...},
//   "initial_connection_window_size": {...},
//   "allow_connect": ...,
//   "max_outbound_frames": {...},
//   "max_outbound_control_frames": {...},
//   "max_consecutive_inbound_frames_with_empty_payload": {...},
//   "max_inbound_priority_frames_per_stream": {...},
//   "max_inbound_window_update_frames_per_data_frame_sent": {...},
//   "stream_error_on_invalid_http_messaging": ...,
//   "override_stream_error_on_invalid_http_message": {...},
//   "connection_keepalive": {...},
//   "max_metadata_size": {...}
// }

// max_concurrent_streams

// (UInt32Value) Maximum concurrent streams allowed for peer on one HTTP/2 connection. 
// Valid values range from 1 to 2147483647 (2^31 - 1) and defaults to 2147483647.
// For upstream connections, this also limits how many streams Envoy will initiate concurrently on a single connection. 
// If the limit is reached, Envoy may queue requests or establish additional connections (as allowed per circuit breaker limits).
// This acts as an upper bound: Envoy will lower the max concurrent streams allowed on a given connection based on upstream settings. 
// Config dumps will reflect the configured upper bound, not the per-connection negotiated limits.

// example yaml config:

// clusters:
//  - name: <cluster_name>
//   typed_extension_protocol_options:
//     envoy.extensions.upstreams.http.v3.HttpProtocolOptions:
//       "@type": type.googleapis.com/envoy.extensions.upstreams.http.v3.HttpProtocolOptions
//       explicit_http_config:
//         http2_protocol_options:
//           max_concurrent_streams: 1

// --------------------

TEST_P(AggregateIntegrationTest, CircuitBreakerTestMaxConnections) {
  std::cout << "---------- 00 TEST START" << std::endl;

  // make the downstream client use http2
  setDownstreamProtocol(Http::CodecType::HTTP2);
  
  // modify the config
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* aggregate_cluster = static_resources->mutable_clusters(1);
    
    auto* aggregate_cluster_type = aggregate_cluster->mutable_cluster_type();
    auto* aggregate_cluster_typed_config = aggregate_cluster_type->mutable_typed_config();
    envoy::extensions::clusters::aggregate::v3::ClusterConfig temp_aggregate_cluster_typed_config;
    aggregate_cluster_typed_config->UnpackTo(&temp_aggregate_cluster_typed_config);
    temp_aggregate_cluster_typed_config.clear_clusters();
    temp_aggregate_cluster_typed_config.add_clusters("cluster_1");
    aggregate_cluster_typed_config->PackFrom(temp_aggregate_cluster_typed_config);

    auto* aggregate_cluster_circuit_breakers = aggregate_cluster->mutable_circuit_breakers();
    auto* aggregate_cluster_circuit_breakers_threshold_default = aggregate_cluster_circuit_breakers->add_thresholds();
    aggregate_cluster_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1); // set to 1
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->set_track_remaining(true);

    // create a new HttpProtocolOptions
    envoy::extensions::upstreams::http::v3::HttpProtocolOptions http_protocol_options;
    // set http2_protocol_options max_concurrent_streams to 1
    http_protocol_options.mutable_explicit_http_config()->mutable_http2_protocol_options()->mutable_max_concurrent_streams()->set_value(1);
    // add the http_protocol_options to aggregate_cluster
    // (found this example here: test/integration/shadow_policy_integration_test.cc - we have to do this packing stuff because the protobuf type is Any -_-)
    (*aggregate_cluster->mutable_typed_extension_protocol_options())
      ["envoy.extensions.upstreams.http.v3.HttpProtocolOptions"]
        .PackFrom(http_protocol_options);

    std::cout << "aggregate_cluster max_concurrent_streams: " << http_protocol_options.explicit_http_config().http2_protocol_options().max_concurrent_streams().value() << std::endl;
  });

  initialize();

  // create a new HttpProtocolOptions
  envoy::extensions::upstreams::http::v3::HttpProtocolOptions http_protocol_options;
  // set http2_protocol_options max_concurrent_streams to 1
  http_protocol_options.mutable_explicit_http_config()->mutable_http2_protocol_options()->mutable_max_concurrent_streams()->set_value(1);
  // add the http_protocol_options to cluster_1
  (*cluster1_.mutable_typed_extension_protocol_options())
    ["envoy.extensions.upstreams.http.v3.HttpProtocolOptions"]
      .PackFrom(http_protocol_options);

  std::cout << "cluster1_ max_concurrent_streams: " << http_protocol_options.explicit_http_config().http2_protocol_options().max_concurrent_streams().value() << std::endl;

  auto* cluster1_circuit_breakers = cluster1_.mutable_circuit_breakers();
  auto* cluster1_circuit_breakers_threshold_default = cluster1_circuit_breakers->add_thresholds();
  cluster1_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  cluster1_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1); // set to 1
  cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->set_track_remaining(true);

  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));

  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster, {cluster1_}, {cluster1_}, {}, "56");

  // make sure we still have 3 active clusters
  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  // wait for the "remaining" stats to be available
  test_server_->waitForGaugeGe("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx", 0);
  test_server_->waitForGaugeGe("cluster.cluster_1.circuit_breakers.default.remaining_cx", 0);

  // initial check
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 0);

  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.cx_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx")->value(), 1);

  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.cx_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_cx")->value(), 1);

  // aggregate_cluster max_connections
  std::cout << "BEFORE aggregate_cluster cx_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.cx_open")->value() << std::endl;
  std::cout << "BEFORE aggregate_cluster remaining_cx: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx")->value() << std::endl;

  // cluster_1 max_connections
  std::cout << "BEFORE cluster_1 cx_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.cx_open")->value() << std::endl;
  std::cout << "BEFORE cluster_1 remaining_cx: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_cx")->value() << std::endl;

  codec_client_ = makeHttpConnection(lookupPort("http"));

  // send the first request (this should go via "aggregate_cluster" through to "cluster_1")
  auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  // !!! ^this counts as a single stream
  // and we specified in the http2 protocol options on both clusters to allow max_concurrent_streams: 1
  // so now one request (stream) should use up 1 entire connection

  // wait for the request to arrive at the upstream cluster
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 1);

  // aggregate_cluster max_connections
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.cx_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx")->value(), 1);
  
  // cluster_1 max_connections
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.cx_open")->value(), 1); // !! the circuit breaker is triggered
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_cx")->value(), 0); // !! there are no more connections allowed

  // aggregate_cluster
  std::cout << "DURING aggregate_cluster cx_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.cx_open")->value() << std::endl;
  std::cout << "DURING aggregate_cluster remaining_cx: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx")->value() << std::endl;

  // cluster_1
  std::cout << "DURING cluster_1 cx_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.cx_open")->value() << std::endl;
  std::cout << "DURING cluster_1 remaining_cx: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_cx")->value() << std::endl;

  // respond with headers
  upstream_request_->encodeHeaders(default_response_headers_, true);

  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());

  EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

  // close the upstream connection (because it will be kept around for a while if we don't)
  // so we can check the circuit breaker returns to its initial state
  ASSERT_TRUE(fake_upstream_connection_->close());

  // note: there are also these methods:
  // ASSERT_TRUE(fake_upstream_connection_->waitForDisconnect());
  // fake_upstream_connection_.reset();

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 0);

  // aggregate_cluster max_connections
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.cx_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx")->value(), 1);
  
  // cluster_1 max_connections
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.cx_open")->value(), 0); // the circuit breaker is back to its initial state
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_cx")->value(), 1); // and this is also back to its initial state

  // aggregate_cluster
  std::cout << "AFTER aggregate_cluster cx_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.cx_open")->value() << std::endl;
  std::cout << "AFTER aggregate_cluster remaining_cx: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx")->value() << std::endl;

  // cluster_1
  std::cout << "AFTER cluster_1 cx_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.cx_open")->value() << std::endl;
  std::cout << "AFTER cluster_1 remaining_cx: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_cx")->value() << std::endl;

  cleanupUpstreamAndDownstream();

  // TODO: try to get rid of this
  codec_client_->close();

  std::cout << "---------- 99 TEST END" << std::endl;
}

// --------------------

// rq_pending_open (Gauge) - Whether the pending requests circuit breaker is under its concurrency limit (0) or is at capacity and no longer admitting (1)

// https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/circuit_breaking

// Cluster maximum pending requests: 
// The maximum number of requests that will be queued while waiting for a ready connection pool connection. 
// Requests are added to the list of pending requests whenever there aren’t enough upstream connections available to immediately dispatch the request. 
// For HTTP/2 connections, if max concurrent streams and max requests per connection are not configured, all requests will be multiplexed over the same connection 
// so this circuit breaker will only be hit when no connection is already established. 
// If this circuit breaker overflows the upstream_rq_pending_overflow counter for the cluster will increment. 
// For HTTP/3 the equivalent to HTTP/2’s max concurrent streams is max concurrent streams

// use 1 connection and then to hold that up, we can use the max_concurrent_streams (AND POSSIBLY max_requests_per_connection)
// together they will "put a bound on the number of in-flight requests to the upstream"
// so 1 request should hold up that 1 connection, and no more connections are available, 
// so requests will queue and we should be able to trigger the max_pending_requests circuit breaker

// NOTE: the option in Cluster is DEPRECATED: https://www.envoyproxy.io/docs/envoy/latest/api-v3/config/cluster/v3/cluster.proto.html`

// This replaces the prior pattern of explicit protocol configuration directly in the cluster. 
// So a configuration like this, explicitly configuring the use of HTTP/2 upstream:

// clusters:
//   - name: some_service
//     connect_timeout: 5s
//     upstream_http_protocol_options:
//       auto_sni: true
//     common_http_protocol_options:
//       idle_timeout: 1s
//     http2_protocol_options:
//       max_concurrent_streams: 100
//     .... [further cluster config]

// Would now look like this:

// clusters:
//   - name: some_service
//     connect_timeout: 5s
//     typed_extension_protocol_options:
//       envoy.extensions.upstreams.http.v3.HttpProtocolOptions:
//         "@type": type.googleapis.com/envoy.extensions.upstreams.http.v3.HttpProtocolOptions
//         upstream_http_protocol_options:
//           auto_sni: true
//         common_http_protocol_options:
//           idle_timeout: 1s
//         explicit_http_config:
//           http2_protocol_options:
//             max_concurrent_streams: 100
//     .... [further cluster config]


// config.core.v3.HttpProtocolOptions

// https://www.envoyproxy.io/docs/envoy/latest/api-v3/extensions/upstreams/http/v3/http_protocol_options.proto.html

// {
//   "common_http_protocol_options": {...},
//   "upstream_http_protocol_options": {...},
//   "explicit_http_config": {...},
//   "use_downstream_protocol_config": {...},
//   "auto_config": {...},
//   "http_filters": []
// }

// https://www.envoyproxy.io/docs/envoy/latest/api-v3/config/core/v3/protocol.proto#envoy-v3-api-msg-config-core-v3-httpprotocoloptions

// common_http_protocol_options

// {
//   "idle_timeout": {...},
//   "max_connection_duration": {...},
//   "max_headers_count": {...},
//   "max_response_headers_kb": {...},
//   "max_stream_duration": {...},
//   "headers_with_underscores_action": ...,
//   "max_requests_per_connection": {...}
// }

// https://www.envoyproxy.io/docs/envoy/latest/api-v3/config/core/v3/protocol.proto#envoy-v3-api-field-config-core-v3-httpprotocoloptions-max-requests-per-connection

// max_requests_per_connection

// (UInt32Value) Optional maximum requests for both upstream and downstream connections. 
// If not specified, there is no limit. 
// Setting this parameter to 1 will effectively disable keep alive. 
// For HTTP/2 and HTTP/3, due to concurrent stream processing, the limit is approximate.

// NOTE: this is like a cap where when we reach the cap that connection gets torn down

// --------------------

TEST_P(AggregateIntegrationTest, CircuitBreakerTestMaxPendingRequests) {
  std::cout << "---------- 00 TEST START" << std::endl;

  setDownstreamProtocol(Http::CodecType::HTTP2);
  
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* aggregate_cluster = static_resources->mutable_clusters(1);
    
    auto* aggregate_cluster_type = aggregate_cluster->mutable_cluster_type();
    auto* aggregate_cluster_typed_config = aggregate_cluster_type->mutable_typed_config();
    envoy::extensions::clusters::aggregate::v3::ClusterConfig temp_aggregate_cluster_typed_config;
    aggregate_cluster_typed_config->UnpackTo(&temp_aggregate_cluster_typed_config);
    temp_aggregate_cluster_typed_config.clear_clusters();
    temp_aggregate_cluster_typed_config.add_clusters("cluster_1");
    aggregate_cluster_typed_config->PackFrom(temp_aggregate_cluster_typed_config);

    auto* aggregate_cluster_circuit_breakers = aggregate_cluster->mutable_circuit_breakers();
    auto* aggregate_cluster_circuit_breakers_threshold_default = aggregate_cluster_circuit_breakers->add_thresholds();
    aggregate_cluster_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1); // set to 1, limit the conections to 1 so we can queue up pending requests on that single connection
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1); // set to 1
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->set_track_remaining(true);

    // note: when i tried to use max_concurrent_streams with value 0
    // i got the error: "must be inside range [1, 2147483647]"
    // so we must to set max_concurrent_streams as 1

    envoy::extensions::upstreams::http::v3::HttpProtocolOptions http_protocol_options;
    // set the max_concurrent_streams to 1
    http_protocol_options.mutable_explicit_http_config()->mutable_http2_protocol_options()->mutable_max_concurrent_streams()->set_value(1);
    (*aggregate_cluster->mutable_typed_extension_protocol_options())
      ["envoy.extensions.upstreams.http.v3.HttpProtocolOptions"]
        .PackFrom(http_protocol_options);
    
    std::cout << "aggregate_cluster max_concurrent_streams: " << http_protocol_options.explicit_http_config().http2_protocol_options().max_concurrent_streams().value() << std::endl;
  });

  initialize();

  auto* cluster1_circuit_breakers = cluster1_.mutable_circuit_breakers();
  auto* cluster1_circuit_breakers_threshold_default = cluster1_circuit_breakers->add_thresholds();
  cluster1_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  cluster1_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1); // set to 1, limit the conections to 1 so we can queue up pending requests on that single connection
  cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1); // set to 1
  cluster1_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->set_track_remaining(true);

  envoy::extensions::upstreams::http::v3::HttpProtocolOptions http_protocol_options;
  // set the max_concurrent_streams to 1
  http_protocol_options.mutable_explicit_http_config()->mutable_http2_protocol_options()->mutable_max_concurrent_streams()->set_value(1);
  (*cluster1_.mutable_typed_extension_protocol_options())
    ["envoy.extensions.upstreams.http.v3.HttpProtocolOptions"]
      .PackFrom(http_protocol_options);

  std::cout << "cluster1_ max_concurrent_streams: " << http_protocol_options.explicit_http_config().http2_protocol_options().max_concurrent_streams().value() << std::endl;

  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));

  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster, {cluster1_}, {cluster1_}, {}, "56");

  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  // wait for the "remaining" stats to be available
  test_server_->waitForGaugeGe("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending", 0);
  test_server_->waitForGaugeGe("cluster.cluster_1.circuit_breakers.default.remaining_pending", 0);

  // make sure we are in the default state for both circuit breakers
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 0);

  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending")->value(), 1);

  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_pending")->value(), 1);

  std::cout << "--------------------" << std::endl;

  std::cout << "BEFORE aggregate_cluster rq_pending_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  std::cout << "BEFORE aggregate_cluster remaining_pending: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending")->value() << std::endl;

  std::cout << "BEFORE aggregate_cluster upstream_rq_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_active")->value() << std::endl;
  std::cout << "BEFORE aggregate_cluster upstream_rq_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_total")->value() << std::endl;
  std::cout << "BEFORE aggregate_cluster upstream_rq_pending_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_pending_active")->value() << std::endl;
  std::cout << "BEFORE aggregate_cluster upstream_rq_pending_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_pending_total")->value() << std::endl;
  std::cout << "BEFORE aggregate_cluster upstream_cx_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_cx_active")->value() << std::endl;
  std::cout << "BEFORE aggregate_cluster upstream_cx_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_total")->value() << std::endl;

  std::cout << "BEFORE cluster_1 rq_pending_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  std::cout << "BEFORE cluster_1 remaining_pending: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_pending")->value() << std::endl;

  std::cout << "BEFORE cluster_1 upstream_rq_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_active")->value() << std::endl;
  std::cout << "BEFORE cluster_1 upstream_rq_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_total")->value() << std::endl;
  std::cout << "BEFORE cluster_1 upstream_rq_pending_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_pending_active")->value() << std::endl;
  std::cout << "BEFORE cluster_1 upstream_rq_pending_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_pending_total")->value() << std::endl;
  std::cout << "BEFORE cluster_1 upstream_cx_active: " << test_server_->gauge("cluster.cluster_1.upstream_cx_active")->value() << std::endl;
  std::cout << "BEFORE cluster_1 upstream_cx_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_total")->value() << std::endl;

  codec_client_ = makeHttpConnection(lookupPort("http"));

  // make the first request
  // the connection should now be "saturated" since it will only allow 1 concurrent stream
  // now subsequent requests should go into a "pending" state
  auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  // wait for the first request to arrive at cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  std::cout << "--------------------" << std::endl;

  std::cout << "DURING [request1] aggregate_cluster rq_pending_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  std::cout << "DURING [request1] aggregate_cluster remaining_pending: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending")->value() << std::endl;

  std::cout << "DURING [request1] aggregate_cluster upstream_rq_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_active")->value() << std::endl;
  std::cout << "DURING [request1] aggregate_cluster upstream_rq_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_total")->value() << std::endl;
  std::cout << "DURING [request1] aggregate_cluster upstream_rq_pending_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_pending_active")->value() << std::endl;
  std::cout << "DURING [request1] aggregate_cluster upstream_rq_pending_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_pending_total")->value() << std::endl;
  std::cout << "DURING [request1] aggregate_cluster upstream_cx_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_cx_active")->value() << std::endl;
  std::cout << "DURING [request1] aggregate_cluster upstream_cx_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_total")->value() << std::endl;

  std::cout << "DURING [request1] cluster_1 rq_pending_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  std::cout << "DURING [request1] cluster_1 remaining_pending: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_pending")->value() << std::endl;

  std::cout << "DURING [request1] cluster_1 upstream_rq_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_active")->value() << std::endl;
  std::cout << "DURING [request1] cluster_1 upstream_rq_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_total")->value() << std::endl;
  std::cout << "DURING [request1] cluster_1 upstream_rq_pending_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_pending_active")->value() << std::endl;
  std::cout << "DURING [request1] cluster_1 upstream_rq_pending_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_pending_total")->value() << std::endl;
  std::cout << "DURING [request1] cluster_1 upstream_cx_active: " << test_server_->gauge("cluster.cluster_1.upstream_cx_active")->value() << std::endl;
  std::cout << "DURING [request1] cluster_1 upstream_cx_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_total")->value() << std::endl;

  // make the second request, this will be the first "pending" request
  auto aggregate_cluster_response2 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 1); // !! the circuit breaker should now be triggered
  
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending")->value(), 1);
  
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value(), 1); // !! the circuit breaker should now be triggered
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_pending")->value(), 0);

  std::cout << "--------------------" << std::endl;
  
  std::cout << "DURING [request2] aggregate_cluster rq_pending_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  std::cout << "DURING [request2] aggregate_cluster remaining_pending: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending")->value() << std::endl;

  std::cout << "DURING [request2] aggregate_cluster upstream_rq_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_active")->value() << std::endl;
  std::cout << "DURING [request2] aggregate_cluster upstream_rq_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_total")->value() << std::endl;
  std::cout << "DURING [request2] aggregate_cluster upstream_rq_pending_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_pending_active")->value() << std::endl;
  std::cout << "DURING [request2] aggregate_cluster upstream_rq_pending_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_pending_total")->value() << std::endl;
  std::cout << "DURING [request2] aggregate_cluster upstream_cx_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_cx_active")->value() << std::endl;
  std::cout << "DURING [request2] aggregate_cluster upstream_cx_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_total")->value() << std::endl;

  std::cout << "DURING [request2] cluster_1 rq_pending_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  std::cout << "DURING [request2] cluster_1 remaining_pending: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_pending")->value() << std::endl;

  std::cout << "DURING [request2] cluster_1 upstream_rq_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_active")->value() << std::endl;
  std::cout << "DURING [request2] cluster_1 upstream_rq_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_total")->value() << std::endl;
  std::cout << "DURING [request2] cluster_1 upstream_rq_pending_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_pending_active")->value() << std::endl;
  std::cout << "DURING [request2] cluster_1 upstream_rq_pending_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_pending_total")->value() << std::endl;
  std::cout << "DURING [request2] cluster_1 upstream_cx_active: " << test_server_->gauge("cluster.cluster_1.upstream_cx_active")->value() << std::endl;
  std::cout << "DURING [request2] cluster_1 upstream_cx_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_total")->value() << std::endl;

  // make the third request, this will be the second "pending" request
  auto aggregate_cluster_response3 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  // the third request should fail immediately with 503
  // because the max_pending_requests circuit breaker is triggered
  ASSERT_TRUE(aggregate_cluster_response3->waitForEndStream());
  EXPECT_EQ("503", aggregate_cluster_response3->headers().getStatusValue());

  // there should have been a single pending request overflow
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_pending_overflow", 1);
  EXPECT_EQ(test_server_->counter("cluster.cluster_1.upstream_rq_pending_overflow")->value(), 1);
  
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

  std::cout << "--------------------" << std::endl;

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 0); // the circuit breaker should have returned to its initial state

  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending")->value(), 1);
  
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value(), 0); // the circuit breaker should have returned to its initial state
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_pending")->value(), 1);

  std::cout << "AFTER [all responses] aggregate_cluster rq_pending_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  std::cout << "AFTER [all responses] aggregate_cluster remaining_pending: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending")->value() << std::endl;

  std::cout << "AFTER [all responses] aggregate_cluster upstream_rq_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_active")->value() << std::endl;
  std::cout << "AFTER [all responses] aggregate_cluster upstream_rq_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_total")->value() << std::endl;
  std::cout << "AFTER [all responses] aggregate_cluster upstream_rq_pending_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_pending_active")->value() << std::endl;
  std::cout << "AFTER [all responses] aggregate_cluster upstream_rq_pending_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_pending_total")->value() << std::endl;
  std::cout << "AFTER [all responses] aggregate_cluster upstream_cx_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_cx_active")->value() << std::endl;
  std::cout << "AFTER [all responses] aggregate_cluster upstream_cx_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_total")->value() << std::endl;

  std::cout << "AFTER [all responses] cluster_1 rq_pending_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  std::cout << "AFTER [all responses] cluster_1 remaining_pending: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_pending")->value() << std::endl;

  std::cout << "AFTER [all responses] cluster_1 upstream_rq_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_active")->value() << std::endl;
  std::cout << "AFTER [all responses] cluster_1 upstream_rq_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_total")->value() << std::endl;
  std::cout << "AFTER [all responses] cluster_1 upstream_rq_pending_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_pending_active")->value() << std::endl;
  std::cout << "AFTER [all responses] cluster_1 upstream_rq_pending_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_pending_total")->value() << std::endl;
  std::cout << "AFTER [all responses] cluster_1 upstream_cx_active: " << test_server_->gauge("cluster.cluster_1.upstream_cx_active")->value() << std::endl;
  std::cout << "AFTER [all responses] cluster_1 upstream_cx_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_total")->value() << std::endl;

  cleanupUpstreamAndDownstream();

  // TODO: try to get rid of this
  codec_client_->close();

  std::cout << "---------- 99 TEST END" << std::endl;
}

// --------------------

// https://www.envoyproxy.io/docs/envoy/latest/api-v3/config/cluster/v3/circuit_breaker.proto

// max_retries
// (UInt32Value) The maximum number of parallel retries that Envoy will allow to the upstream cluster. 
// If not specified, the default is 3.

// https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/circuit_breaking

// Cluster maximum active retries: 

// The maximum number of retries that can be outstanding to all hosts in a cluster at any given time. 
// In general we recommend using retry budgets; however, if static circuit breaking is preferred it should aggressively circuit break retries. 
// This is so that retries for sporadic failures are allowed, but the overall retry volume cannot explode and cause large scale cascading failure. 
// If this circuit breaker overflows the upstream_rq_retry_overflow counter for the cluster will increment.

// https://www.envoyproxy.io/docs/envoy/latest/configuration/upstream/cluster_manager/cluster_stats

// rq_retry_open (Gauge) - Whether the retry circuit breaker is under its concurrency limit (0) or is at capacity and no longer admitting (1)

// cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open
// cluster.cluster_1.circuit_breakers.default.rq_retry_open

// remaining_retries (Gauge) - Number of remaining retries until the circuit breaker reaches its concurrency limit

// cluster.aggregate_cluster.circuit_breakers.default.remaining_retries
// cluster.cluster_1.circuit_breakers.default.remaining_retries

// upstream_rq_retry (Counter) - Total request retries
// upstream_rq_retry_backoff_exponential (Counter) - Total retries using the exponential backoff strategy
// upstream_rq_retry_backoff_ratelimited (Counter) - Total retries using the ratelimited backoff strategy
// upstream_rq_retry_limit_exceeded (Counter) - Total requests not retried due to exceeding the configured number of maximum retries
// upstream_rq_retry_success (Counter) - Total request retry successes
// upstream_rq_retry_overflow (Counter) - Total requests not retried due to circuit breaking or exceeding the retry budget

// cluster.aggregate_cluster.upstream_rq_retry
// cluster.aggregate_cluster.upstream_rq_retry_limit_exceeded
// cluster.aggregate_cluster.upstream_rq_retry_success
// cluster.aggregate_cluster.upstream_rq_retry_overflow

// cluster.cluster_1.upstream_rq_retry
// cluster.cluster_1.upstream_rq_retry_limit_exceeded
// cluster.cluster_1.upstream_rq_retry_success
// cluster.cluster_1.upstream_rq_retry_overflow

TEST_P(AggregateIntegrationTest, CircuitBreakerTestMaxRetries) {
  std::cout << "---------- 00 TEST START" << std::endl;

  setDownstreamProtocol(Http::CodecType::HTTP2);
  
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* aggregate_cluster = static_resources->mutable_clusters(1);
    
    auto* aggregate_cluster_type = aggregate_cluster->mutable_cluster_type();
    auto* aggregate_cluster_typed_config = aggregate_cluster_type->mutable_typed_config();
    envoy::extensions::clusters::aggregate::v3::ClusterConfig temp_aggregate_cluster_typed_config;
    aggregate_cluster_typed_config->UnpackTo(&temp_aggregate_cluster_typed_config);
    temp_aggregate_cluster_typed_config.clear_clusters();
    temp_aggregate_cluster_typed_config.add_clusters("cluster_1");
    aggregate_cluster_typed_config->PackFrom(temp_aggregate_cluster_typed_config);

    auto* aggregate_cluster_circuit_breakers = aggregate_cluster->mutable_circuit_breakers();
    auto* aggregate_cluster_circuit_breakers_threshold_default = aggregate_cluster_circuit_breakers->add_thresholds();
    aggregate_cluster_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1); // set to 1
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->set_track_remaining(true);

    // we need to be careful about the retry_policy in aggregate_cluster route config:
    // because this is making the retry go to the second cluster (cluster_2) in the aggregate_cluster clusters list
    // but that second cluster (cluster_2) doesn't exist because we specifically removed it in the config modifier above to keep this test simpler

    // so we can use a different and simpler retry_policy

    // https://www.envoyproxy.io/docs/envoy/latest/api-v3/config/route/v3/route_components.proto#envoy-v3-api-msg-config-route-v3-retrypolicy

    // {
    //   "retry_on": ...,
    //   "num_retries": {...},
    //   "per_try_timeout": {...},
    //   "per_try_idle_timeout": {...},
    //   "retry_priority": {...},
    //   "retry_host_predicate": [],
    //   "retry_options_predicates": [],
    //   "host_selection_retry_max_attempts": ...,
    //   "retriable_status_codes": [],
    //   "retry_back_off": {...},
    //   "rate_limited_retry_back_off": {...},
    //   "retriable_headers": [],
    //   "retriable_request_headers": []
    // }

    // retry_on (string) 
    // Specifies the conditions under which retry takes place. 
    // These are the same conditions documented for x-envoy-retry-on and x-envoy-retry-grpc-on.

    // num_retries (UInt32Value)
    // Specifies the allowed number of retries. This parameter is optional and defaults to 1. 
    // These are the same conditions documented for x-envoy-max-retries.

    // retry_priority (config.route.v3.RetryPolicy.RetryPriority) 
    // Specifies an implementation of a RetryPriority which is used to determine the distribution of load across priorities used for retries. 
    // Refer to retry plugin configuration for more details.

    // retriable_status_codes (repeated uint32) 
    // HTTP status codes that should trigger a retry in addition to those specified by retry_on.

    auto* listener = static_resources->mutable_listeners(0);
    auto* filter_chain = listener->mutable_filter_chains(0);
    auto* filter = filter_chain->mutable_filters(0);

    envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager http_connection_manager;

    // unpack it into the above object^
    filter->mutable_typed_config()->UnpackTo(&http_connection_manager);

    auto* virtual_host = http_connection_manager.mutable_route_config()->mutable_virtual_hosts(0);

    // the aggregate_cluster route is the third route in the config at the top, so we need index 2
    auto* route = virtual_host->mutable_routes(2);

    // make sure this is actually changing the aggregate cluster o_o
    std::cout << "route we are applying the retry changes to: " << route->match().prefix() << std::endl;

    // clear the "retry_priority:"
    route->mutable_route()->mutable_retry_policy()->clear_retry_priority();
    
    // and then change it to:
    // - route:
    //   cluster: aggregate_cluster
    //   retry_policy:
    //     retry_on: 5xx
    //     num_retries: 3

    route->mutable_route()->mutable_retry_policy()->mutable_retry_on()->assign("5xx");

    // !!! THINK ABOUT THIS CAREFULLY... THIS MAY AFFECT THE TESTS...
    // route->mutable_route()->mutable_retry_policy()->mutable_num_retries()->set_value(3);
    // DISABLING IT FOR NOW TO USE THE DEFAULT OF 1 RETRY
    // DISABLING THIS FIXED THE TIMEOUT PROBLEM FROM BEFORE(?)

    // pack it back
    filter->mutable_typed_config()->PackFrom(http_connection_manager);

    // !! alternatively we can, adjust the current one's update_frequency to 3?
    // !! so that both the first and second retries are allowed to go to cluster_1
    // this will matter when we have two underlying clusters...
  });

  initialize();

  auto* cluster1_circuit_breakers = cluster1_.mutable_circuit_breakers();
  auto* cluster1_circuit_breakers_threshold_default = cluster1_circuit_breakers->add_thresholds();
  cluster1_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  cluster1_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1); // set to 1
  cluster1_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->set_track_remaining(true);

  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));

  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster, {cluster1_}, {cluster1_}, {}, "56");

  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  // wait for the "remaining" stats to be available
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 1);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 1);

  // make sure we are in the default state for both circuit breakers
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);

  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries")->value(), 1);

  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_retry_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_retries")->value(), 1);

  printStatsForMaxRetries("BEFORE");

  std::cout << "--------------------" << std::endl;
  std::cout << "DOWNSTREAM CLIENT: MAKING HTTP CONNECTION" << std::endl;

  codec_client_ = makeHttpConnection(lookupPort("http"));

  std::cout << "--------------------" << std::endl;
  std::cout << "DOWNSTREAM CLIENT: SENDING REQUEST-1" << std::endl;

  // send the first request
  auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  std::cout << "--------------------" << std::endl;
  std::cout << "UPSTREAM CLUSTER: WAITING FOR REQUEST-1" << std::endl;

  // wait for the first request to reach cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  printStatsForMaxRetries("REQUEST-1");

  std::cout << "--------------------" << std::endl;
  std::cout << "UPSTREAM CLUSTER: RESPOND TO REQUEST-1 WITH 503" << std::endl;

  // respond with 503 to trigger the first retry
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true); // remember true is the upstream saying it is done processing the request

  std::cout << "--------------------" << std::endl;
  std::cout << "UPSTREAM CLUSTER: WAITING FOR REQUEST-1-RETRY" << std::endl;

  // wait for the first request retry to reach cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // save a reference to this specific request
  // so we can access it later, because upstream_request_ will get overwritten
  auto& first_upstream_request_retry = *upstream_request_;

  std::cout << "--------------------" << std::endl;
  std::cout << "CHECKING STATS (both clusters) rq_retry_open AND remaining_retries" << std::endl;

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 1); // !!! the aggregate_cluster circuit breaker triggers (FINALLY SOME MOVEMENT)
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0); // !!! but the cluster_1 circuit breaker is unchanged
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 1);

  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open")->value(), 1);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_retry_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_retries")->value(), 1);

  // // the first request retry
  // test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry", 1);
  // EXPECT_EQ(test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry")->value(), 1);

  printStatsForMaxRetries("REQUEST-1-RETRY");

  std::cout << "--------------------" << std::endl;
  std::cout << "DOWNSTREAM CLIENT: SENDING REQUEST-2" << std::endl;

  // send the second request
  auto aggregate_cluster_response2 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  std::cout << "--------------------" << std::endl;
  std::cout << "UPSTREAM CLUSTER: WAITING FOR REQUEST-2" << std::endl;

  // wait for the second request to arrive at cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  printStatsForMaxRetries("REQUEST-2");

  std::cout << "--------------------" << std::endl;
  std::cout << "UPSTREAM CLUSTER: RESPOND TO REQUEST-2 WITH 503" << std::endl;

  // respond to the second request with a 503, so we trigger a retry
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

  // // the second request retry
  // test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry", 2);
  // EXPECT_EQ(test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry")->value(), 2);

  std::cout << "--------------------" << std::endl;
  std::cout << "CHECKING STATS upstream_rq_retry_overflow" << std::endl;

  // !! upstream_rq_retry_overflow on the aggregate_cluster
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 1);
  EXPECT_EQ(test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_overflow")->value(), 1);

  printStatsForMaxRetries("REQUEST-2-RETRY");

  std::cout << "--------------------" << std::endl;
  std::cout << "UPSTREAM CLUSTER: RESPOND TO REQUEST-1-RETRY WITH 200" << std::endl;
  
  // respond to the first request retry
  first_upstream_request_retry.encodeHeaders(default_response_headers_, true);

  // and we shouldn't need to do this with the second request retry
  // because it SHOULD be auto rejected by the circuit breaker

  std::cout << "--------------------" << std::endl;
  std::cout << "DOWNSTREAM CLIENT: RESPONSE-1 WAIT FOR END STREAM" << std::endl;
  ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());
  
  std::cout << "--------------------" << std::endl;
  std::cout << "DOWNSTREAM CLIENT: RESPONSE-1 CHECKING HEADER STATUS IS 200" << std::endl;
  EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

  std::cout << "--------------------" << std::endl;
  std::cout << "DOWNSTREAM CLIENT: RESPONSE-2 WAIT FOR END STREAM" << std::endl;
  ASSERT_TRUE(aggregate_cluster_response2->waitForEndStream());
  
  std::cout << "--------------------" << std::endl;
  std::cout << "DOWNSTREAM CLIENT: RESPONSE-2 CHECKING HEADER STATUS IS 503" << std::endl;
  EXPECT_EQ("503", aggregate_cluster_response2->headers().getStatusValue());

  std::cout << "--------------------" << std::endl;
  std::cout << "CHECKING STATS (both clusters) rq_retry_open AND remaining_retries" << std::endl;

  // the circuit breakers return to their initial state
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 0);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 1);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 1);

  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries")->value(), 1);
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_retry_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_retries")->value(), 1);

  printStatsForMaxRetries("AFTER");

  cleanupUpstreamAndDownstream();
  // TODO: try to get rid of this
  codec_client_->close();
 
  std::cout << "---------- 99 TEST END" << std::endl;
}

// --------------------

// https://www.envoyproxy.io/docs/envoy/latest/api-v3/config/cluster/v3/circuit_breaker.proto

// max_connection_pools (UInt32Value) 
// The maximum number of connection pools per cluster that Envoy will concurrently support at once. 
// If not specified, the default is unlimited. 
// Set this for clusters which create a large number of connection pools. 
// See Circuit Breaking for more details.

// https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/circuit_breaking

// Cluster maximum concurrent connection pools

// The maximum number of connection pools that can be concurrently instantiated. 
// Some features, such as the Original Src Listener Filter, can create an unbounded number of connection pools. 
// When a cluster has exhausted its concurrent connection pools, it will attempt to reclaim an idle one. 
// If it cannot, then the circuit breaker will overflow. 
// This differs from Cluster maximum connections in that connection pools never time out, whereas connections typically will. 
// Connections automatically clean up; connection pools do not. 
// Note that in order for a connection pool to function it needs at least one upstream connection, so this value should likely be no greater than Cluster maximum connections. 
// If this circuit breaker overflows the upstream_cx_pool_overflow counter for the cluster will increment.

// https://www.envoyproxy.io/docs/envoy/latest/configuration/upstream/cluster_manager/cluster_stats

// cx_pool_open (Gauge) - Whether the connection pool circuit breaker is under its concurrency limit (0) or is at capacity and no longer admitting (1)

// cluster.aggregate_cluster.circuit_breakers.default.cx_pool_open
// cluster.cluster_1.circuit_breakers.default.cx_pool_open

// upstream_cx_pool_overflow (Counter) - Total times that the cluster’s connection pool circuit breaker overflowed

// cluster.aggregate_cluster.upstream_cx_pool_overflow

// cluster.cluster_1.upstream_cx_pool_overflow

// --------------------------

// this test is much harder to achieve,
// because we have to force envoy into making new connection pools,
// this only happens under certain circumstances:

// https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/connection_pooling#number-of-connection-pools

// Number of connection pools

// Each host in each cluster will have one or more connection pools. 
// If the cluster has a single explicit protocol configured, then the host may have only a single connection pool. 
// However, if the cluster supports multiple upstream protocols, then unless it is using ALPN, one connection pool per protocol may be allocated. 
// Separate connection pools are also allocated for each of the following features:

// - Routing priority
// - Socket options
// - Transport socket (e.g. TLS) options
// - Downstream filter state objects that are hashable and marked as shared with the upstream connection.

// Each worker thread maintains its own connection pools for each cluster, 
// so if an Envoy has two threads and a cluster with both HTTP/1 and HTTP/2 support, there will be at least 4 connection pools.

TEST_P(AggregateIntegrationTest, CircuitBreakerTestMaxConnectionPools) {
  std::cout << "---------- 00 TEST START" << std::endl;

  setDownstreamProtocol(Http::CodecType::HTTP2);
  
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* aggregate_cluster = static_resources->mutable_clusters(1); 
    auto* aggregate_cluster_type = aggregate_cluster->mutable_cluster_type();
    auto* aggregate_cluster_typed_config = aggregate_cluster_type->mutable_typed_config();
    envoy::extensions::clusters::aggregate::v3::ClusterConfig temp_aggregate_cluster_typed_config;
    aggregate_cluster_typed_config->UnpackTo(&temp_aggregate_cluster_typed_config);
    temp_aggregate_cluster_typed_config.clear_clusters();
    temp_aggregate_cluster_typed_config.add_clusters("cluster_1");
    aggregate_cluster_typed_config->PackFrom(temp_aggregate_cluster_typed_config);

    auto* aggregate_cluster_circuit_breakers = aggregate_cluster->mutable_circuit_breakers();
    auto* aggregate_cluster_circuit_breakers_threshold_default = aggregate_cluster_circuit_breakers->add_thresholds();
    aggregate_cluster_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1); // set to 1
    aggregate_cluster_circuit_breakers_threshold_default->set_track_remaining(true);
 
    // https://www.envoyproxy.io/docs/envoy/latest/api-v3/config/core/v3/protocol.proto#envoy-v3-api-field-config-core-v3-httpprotocoloptions-max-requests-per-connection
    
    // aiming for this config...

    // clusters:
    //   - name: aggregate_cluster
    //     connect_timeout: 0.25s
    //     lb_policy: CLUSTER_PROVIDED
    //     cluster_type:
    //       name: envoy.clusters.aggregate
    //       typed_config:
    //         "@type": type.googleapis.com/envoy.extensions.clusters.aggregate.v3.ClusterConfig
    //         clusters:
    //         - cluster_1
    //       typed_extension_protocol_options:
    //         envoy.extensions.upstreams.http.v3.HttpProtocolOptions:
    //           "@type": type.googleapis.com/envoy.extensions.upstreams.http.v3.HttpProtocolOptions
    //           common_http_protocol_options:
    //             max_requests_per_connection: 1
    //           use_downstream_protocol_config:
    //             http_protocol_options: {}
    //             http2_protocol_options: {}

    // try to configure aggregate_cluster to support http1.1 and http2 (and carry that through from downstream)
    envoy::extensions::upstreams::http::v3::HttpProtocolOptions http_protocol_options;
    // common http protocol options
    http_protocol_options.mutable_common_http_protocol_options()->mutable_max_requests_per_connection()->set_value(1);
    // http1.1 options
    http_protocol_options.mutable_use_downstream_protocol_config()->mutable_http_protocol_options();
    // http2 options
    http_protocol_options.mutable_use_downstream_protocol_config()->mutable_http2_protocol_options();
    http_protocol_options.mutable_use_downstream_protocol_config()->mutable_http2_protocol_options()->mutable_max_concurrent_streams()->set_value(1);

    (*aggregate_cluster->mutable_typed_extension_protocol_options())
      ["envoy.extensions.upstreams.http.v3.HttpProtocolOptions"]
        .PackFrom(http_protocol_options);
  });

  initialize();

  auto* cluster1_circuit_breakers = cluster1_.mutable_circuit_breakers();
  auto* cluster1_circuit_breakers_threshold_default = cluster1_circuit_breakers->add_thresholds();
  cluster1_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  cluster1_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1);// set to 1
  cluster1_circuit_breakers_threshold_default->set_track_remaining(true);

  // try to configure cluster_1 to support http1.1 and http2 (and carry that through from downstream)
  envoy::extensions::upstreams::http::v3::HttpProtocolOptions http_protocol_options;
  // common http protocol options
  http_protocol_options.mutable_common_http_protocol_options()->mutable_max_requests_per_connection()->set_value(1);
  // http1.1 options
  http_protocol_options.mutable_use_downstream_protocol_config()->mutable_http_protocol_options();
  // http2 options
  http_protocol_options.mutable_use_downstream_protocol_config()->mutable_http2_protocol_options();
  http_protocol_options.mutable_use_downstream_protocol_config()->mutable_http2_protocol_options()->mutable_max_concurrent_streams()->set_value(1);

  (*cluster1_.mutable_typed_extension_protocol_options())
    ["envoy.extensions.upstreams.http.v3.HttpProtocolOptions"]
      .PackFrom(http_protocol_options);

  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster, {cluster1_}, {cluster1_}, {}, "56");
  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_pool_open", 0);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx_pools", 1);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_pool_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx_pools", 1);

  printStatsForMaxConnectionPools("BEFORE");

  std::cout << "---------- MAKE HTTP CONNECTION" << std::endl;
  codec_client_ = makeHttpConnection(lookupPort("http"));
  
  std::cout << "---------- SEND FIRST REQUEST" << std::endl;
  auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"}, {":path", "/aggregatecluster"},{":scheme", "http"}, {":authority", "host"}}
  );

  std::cout << "---------- WAIT FOR FIRST REQUEST TO REACH UPSTREAM" << std::endl;
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  std::cout << "---------- WAIT FOR cluster.cluster_1.circuit_breakers.default.cx_pool_open = 1" << std::endl;
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_pool_open", 1);

  printStatsForMaxConnectionPools("DURING FIRST REQUEST");

  // std::cout << "---------- RESPOND TO THE FIRST REQUEST WITH 200" << std::endl;
  // upstream_request_->encodeHeaders(default_response_headers_, true);

  // std::cout << "---------- HANDLE RESPONSE 1 ON DOWNSTREAM CLIENT" << std::endl;
  // ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());
  // EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

  // printStatsForMaxConnectionPools("AFTER FIRST REQUEST");
  
  // but the cx_pool_open circuit breaker reverts when we complete the request???
  // AFTER FIRST REQUEST cluster_1 cx_pool_open: 0

  std::cout << "---------- MAKE HTTP1.1 CONNECTION AND SEND REQUEST" << std::endl;
  // try this -_-
  auto response = IntegrationUtil::makeSingleRequest(
      lookupPort("http"),           // port
      "GET",                        // method
      "/aggregatecluster",          // path
      "",                           // body
      Http::CodecType::HTTP1,       // upstream protocol
      version_);                    // IP version

  printStatsForMaxConnectionPools("DURING SECOND REQUEST");

  std::cout << "---------- WAIT FOR cluster.cluster_1.upstream_cx_pool_overflow = 1" << std::endl;
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_cx_pool_overflow", 1);

  cleanupUpstreamAndDownstream();

  std::cout << "---------- 99 TEST END" << std::endl;
}

} // namespace
} // namespace Envoy


// CURRENT LOG STATE:

// ---------- 00 TEST START
// BEFORE aggregate_cluster cx_pool_open: 0
// BEFORE aggregate_cluster remaining_cx_pools: 1
// BEFORE aggregate_cluster upstream_cx_pool_overflow: 0
// BEFORE aggregate_cluster upstream_cx_active: 0
// BEFORE aggregate_cluster upstream_cx_total: 0
// BEFORE aggregate_cluster upstream_cx_http1_total: 0
// BEFORE aggregate_cluster upstream_cx_http2_total: 0
// BEFORE aggregate_cluster upstream_rq_active: 0
// BEFORE aggregate_cluster upstream_rq_total: 0
// BEFORE aggregate_cluster upstream_rq_active: 0
// BEFORE aggregate_cluster upstream_rq_total: 0
// BEFORE cluster_1 cx_pool_open: 0
// BEFORE cluster_1 remaining_cx_pools: 1
// BEFORE cluster_1 upstream_cx_pool_overflow: 0
// BEFORE cluster_1 upstream_cx_active: 0
// BEFORE cluster_1 upstream_cx_total: 0
// BEFORE cluster_1 upstream_cx_http1_total: 0
// BEFORE cluster_1 upstream_cx_http2_total: 0
// BEFORE cluster_1 upstream_rq_active: 0
// BEFORE cluster_1 upstream_rq_total: 0
// ---------- MAKE HTTP CONNECTION
// ---------- SEND FIRST REQUEST
// ---------- WAIT FOR FIRST REQUEST TO REACH UPSTREAM
// ---------- WAIT FOR cluster.cluster_1.circuit_breakers.default.cx_pool_open = 1
// DURING FIRST REQUEST aggregate_cluster cx_pool_open: 0
// DURING FIRST REQUEST aggregate_cluster remaining_cx_pools: 1
// DURING FIRST REQUEST aggregate_cluster upstream_cx_pool_overflow: 0
// DURING FIRST REQUEST aggregate_cluster upstream_cx_active: 0
// DURING FIRST REQUEST aggregate_cluster upstream_cx_total: 0
// DURING FIRST REQUEST aggregate_cluster upstream_cx_http1_total: 0
// DURING FIRST REQUEST aggregate_cluster upstream_cx_http2_total: 0
// DURING FIRST REQUEST aggregate_cluster upstream_rq_active: 0
// DURING FIRST REQUEST aggregate_cluster upstream_rq_total: 0
// DURING FIRST REQUEST aggregate_cluster upstream_rq_active: 0
// DURING FIRST REQUEST aggregate_cluster upstream_rq_total: 0
// DURING FIRST REQUEST cluster_1 cx_pool_open: 1
// DURING FIRST REQUEST cluster_1 remaining_cx_pools: 0
// DURING FIRST REQUEST cluster_1 upstream_cx_pool_overflow: 0
// DURING FIRST REQUEST cluster_1 upstream_cx_active: 1
// DURING FIRST REQUEST cluster_1 upstream_cx_total: 1
// DURING FIRST REQUEST cluster_1 upstream_cx_http1_total: 0
// DURING FIRST REQUEST cluster_1 upstream_cx_http2_total: 1
// DURING FIRST REQUEST cluster_1 upstream_rq_active: 1
// DURING FIRST REQUEST cluster_1 upstream_rq_total: 1
// ---------- MAKE HTTP1.1 CONNECTION AND SEND REQUEST
// test/integration/utility.cc:102: Failure
// Failed
// Stack trace:
//   0x3573003: Envoy::BufferingStreamDecoder::onResetStream()
//   0x5164021: Envoy::Http::StreamCallbackHelper::runResetCallbacks()
//   0x56b404c: Envoy::Http::Http1::ClientConnectionImpl::onResetStream()
//   0x5693180: Envoy::Http::Http1::ConnectionImpl::onResetStreamBase()
//   0x5692def: Envoy::Http::Http1::StreamEncoderImpl::resetStream()
//   0x5687681: Envoy::Http::CodecClient::onEvent()
//   0x609e31a: Envoy::Network::ConnectionImplBase::raiseConnectionEvent()
//   0x607eaa9: Envoy::Network::ConnectionImpl::raiseEvent()
//   0x607d92d: Envoy::Network::ConnectionImpl::closeSocket()
//   0x608d8b6: Envoy::Network::ConnectionImpl::onReadReady()
//   0x60882e3: Envoy::Network::ConnectionImpl::onFileEvent()
//   0x609760d: Envoy::Network::ConnectionImpl::ConnectionImpl()::$_7::operator()()
//   0x60975c4: std::__invoke_impl<>()
//   0x6097545: std::__invoke_r<>()
//   0x6097425: std::_Function_handler<>::_M_invoke()
//   0x604ee17: std::function<>::operator()()
//   0x60465e2: Envoy::Event::DispatcherImpl::createFileEvent()::$_6::operator()()
//   0x6046584: std::__invoke_impl<>()
//   0x6046515: std::__invoke_r<>()
//   0x6046385: std::_Function_handler<>::_M_invoke()
//   0x604ee17: std::function<>::operator()()
//   0x6055f65: Envoy::Event::FileEventImpl::mergeInjectedEventsAndRunCb()
//   0x60566a4: Envoy::Event::FileEventImpl::assignEvents()::$_11::operator()()
//   0x60562f9: Envoy::Event::FileEventImpl::assignEvents()::$_11::__invoke()
//   0x6cc982b: event_persist_closure
//   0x6cc8ea2: event_process_active_single_queue
//   0x6cc37c8: event_process_active
//   0x6cc26cc: event_base_loop
//   0x6491e64: Envoy::Event::LibeventScheduler::run()
//   0x6040829: Envoy::Event::DispatcherImpl::run()
//   0x3574088: Envoy::sendRequestAndWaitForResponse()
//   0x3574a61: Envoy::IntegrationUtil::makeSingleRequest()
//   0x3575988: Envoy::IntegrationUtil::makeSingleRequest()
//   0x2b8257c: Envoy::(anonymous namespace)::AggregateIntegrationTest_CircuitBreakerTestMaxConnectionPools_Test::TestBody()
//   0x74501cb: testing::internal::HandleSehExceptionsInMethodIfSupported<>()
//   0x743fd7d: testing::internal::HandleExceptionsInMethodIfSupported<>()
//   0x74285f3: testing::Test::Run()
//   0x74291ba: testing::TestInfo::Run()
// ... Google Test internal frames ...

// DURING SECOND REQUEST aggregate_cluster cx_pool_open: 0
// DURING SECOND REQUEST aggregate_cluster remaining_cx_pools: 1
// DURING SECOND REQUEST aggregate_cluster upstream_cx_pool_overflow: 0
// DURING SECOND REQUEST aggregate_cluster upstream_cx_active: 0
// DURING SECOND REQUEST aggregate_cluster upstream_cx_total: 0
// DURING SECOND REQUEST aggregate_cluster upstream_cx_http1_total: 0
// DURING SECOND REQUEST aggregate_cluster upstream_cx_http2_total: 0
// DURING SECOND REQUEST aggregate_cluster upstream_rq_active: 0
// DURING SECOND REQUEST aggregate_cluster upstream_rq_total: 0
// DURING SECOND REQUEST aggregate_cluster upstream_rq_active: 0
// DURING SECOND REQUEST aggregate_cluster upstream_rq_total: 0
// DURING SECOND REQUEST cluster_1 cx_pool_open: 1
// DURING SECOND REQUEST cluster_1 remaining_cx_pools: 0
// DURING SECOND REQUEST cluster_1 upstream_cx_pool_overflow: 0
// DURING SECOND REQUEST cluster_1 upstream_cx_active: 1
// DURING SECOND REQUEST cluster_1 upstream_cx_total: 1
// DURING SECOND REQUEST cluster_1 upstream_cx_http1_total: 0
// DURING SECOND REQUEST cluster_1 upstream_cx_http2_total: 1
// DURING SECOND REQUEST cluster_1 upstream_rq_active: 1
// DURING SECOND REQUEST cluster_1 upstream_rq_total: 1
// ---------- WAIT FOR cluster.cluster_1.upstream_cx_pool_overflow = 1
// ./test/integration/server.h:452: Failure
// Value of: TestUtility::waitForCounterEq(statStore(), name, value, time_system_, timeout, dispatcher)
//   Actual: false (timed out waiting for cluster.cluster_1.upstream_cx_pool_overflow to be 1, current value 0)
// Expected: true
// Stack trace:
//   0x355b8b1: Envoy::IntegrationTestServer::waitForCounterEq()
//   0x2b8273d: Envoy::(anonymous namespace)::AggregateIntegrationTest_CircuitBreakerTestMaxConnectionPools_Test::TestBody()
//   0x74501cb: testing::internal::HandleSehExceptionsInMethodIfSupported<>()
//   0x743fd7d: testing::internal::HandleExceptionsInMethodIfSupported<>()
//   0x74285f3: testing::Test::Run()
//   0x74291ba: testing::TestInfo::Run()
// ... Google Test internal frames ...

// ---------- 99 TEST END


// TEMPORARY SNIPPETS:

// std::cout << "---------- SEND SECOND REQUEST" << std::endl;
// auto aggregate_cluster_response2 = ->makeHeaderOnlyRequest(
//   Http::TestRequestHeaderMapImpl{{":method", "GET"}, {":path", "/aggregatecluster"},{":scheme", "http"}, {":authority", "host"}}
// );

// std::cout << "---------- WAIT FOR SECOND REQUEST TO REACH UPSTREAM" << std::endl;
// waitForNextUpstreamRequest(FirstUpstreamIndex);

// std::cout << "---------- HANDLE RESPONSE 2 ON DOWNSTREAM CLIENT" << std::endl;
// ASSERT_TRUE(response2->waitForEndStream());
// EXPECT_EQ("200", response2->headers().getStatusValue());

// printStatsForMaxConnectionPools("AFTER SECOND REQUEST COMPLETES");

// std::cout << "---------- RESPOND TO THE SECOND REQUEST WITH 200" << std::endl;
// upstream_request_->encodeHeaders(default_response_headers_, true);

// std::cout << "---------- HANDLE RESPONSE 2 ON DOWNSTREAM CLIENT" << std::endl;
// ASSERT_TRUE(response2->waitForEndStream());
// EXPECT_EQ("200", response2->headers().getStatusValue());

// std::cout << "---------- WAIT FOR UPSTREAM CONNECTION TO BE CLOSED DUE TO max_requests_per_connection = 1" << std::endl;
// ASSERT_TRUE(fake_upstream_connection_->waitForDisconnect());
// fake_upstream_connection_.reset();

// std::cout << "---------- WAIT FOR cluster.cluster_1.circuit_breakers.default.cx_pool_open = 0" << std::endl;

// // verify that the connection pool is cleaned up after closing all connections
// test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_pool_open", 0);

// std::cout << "---------- STATS AFTER CONNECTION CLOSE" << std::endl;
// printStatsForMaxConnectionPools("AFTER");

// std::cout << "---------- RESPOND TO THE FIRST REQUEST WITH 200" << std::endl;
// upstream_request_->encodeHeaders(default_response_headers_, true);

// std::cout << "---------- HANDLE RESPONSE 1 ON DOWNSTREAM CLIENT" << std::endl;
// ASSERT_TRUE(response1->waitForEndStream());
// EXPECT_EQ("200", response1->headers().getStatusValue());

// printStatsForMaxConnectionPools("AFTER FIRST REQUEST COMPLETES");

// std::cout << "---------- WAIT FOR UPSTREAM CONNECTION TO BE CLOSED DUE TO max_requests_per_connection = 1" << std::endl;
// ASSERT_TRUE(fake_upstream_connection_->waitForDisconnect());
// fake_upstream_connection_.reset();
