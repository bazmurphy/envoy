#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/extensions/clusters/aggregate/v3/cluster.pb.h"
#include "envoy/extensions/upstreams/http/v3/http_protocol_options.pb.h"
#include "envoy/grpc/status.h"
#include "envoy/stats/scope.h"

#include "source/common/config/protobuf_link_hacks.h"
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
struct CircuitBreakerLimits {
  uint32_t max_connections = 1024;
  uint32_t max_requests = 1024;
  uint32_t max_pending_requests = 1024;
  uint32_t max_retries = 3;
  uint32_t max_connection_pools = std::numeric_limits<uint32_t>::max();

  CircuitBreakerLimits withMaxConnections(uint32_t max_connections) const {
    CircuitBreakerLimits limits = *this;
    limits.max_connections = max_connections;
    return limits;
  }

  CircuitBreakerLimits withMaxRequests(uint32_t max_requests) const {
    CircuitBreakerLimits limits = *this;
    limits.max_requests = max_requests;
    return limits;
  }

  CircuitBreakerLimits withMaxPendingRequests(uint32_t max_pending_requests) const {
    CircuitBreakerLimits limits = *this;
    limits.max_pending_requests = max_pending_requests;
    return limits;
  }

  CircuitBreakerLimits withMaxRetries(uint32_t max_retries) const {
    CircuitBreakerLimits limits = *this;
    limits.max_retries = max_retries;
    return limits;
  }
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
                               const CircuitBreakerLimits& limits) {
    auto* cluster_circuit_breakers = cluster.mutable_circuit_breakers();

    auto* cluster_circuit_breakers_threshold_default = cluster_circuit_breakers->add_thresholds();
    cluster_circuit_breakers_threshold_default->set_priority(
        envoy::config::core::v3::RoutingPriority::DEFAULT);

    cluster_circuit_breakers_threshold_default->mutable_max_connections()->set_value(
        limits.max_connections);
    cluster_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(
        limits.max_pending_requests);
    cluster_circuit_breakers_threshold_default->mutable_max_requests()->set_value(
        limits.max_requests);
    cluster_circuit_breakers_threshold_default->mutable_max_retries()->set_value(
        limits.max_retries);
    cluster_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(
        limits.max_connection_pools);
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

  void
  reduceAggregateClustersListToOneCluster(envoy::config::cluster::v3::Cluster& aggregate_cluster) {
    auto* aggregate_cluster_type = aggregate_cluster.mutable_cluster_type();
    auto* aggregate_cluster_typed_config = aggregate_cluster_type->mutable_typed_config();
    envoy::extensions::clusters::aggregate::v3::ClusterConfig new_aggregate_cluster_typed_config;
    aggregate_cluster_typed_config->UnpackTo(&new_aggregate_cluster_typed_config);
    new_aggregate_cluster_typed_config.clear_clusters();
    new_aggregate_cluster_typed_config.add_clusters("cluster_1");
    aggregate_cluster_typed_config->PackFrom(new_aggregate_cluster_typed_config);
  }

  // !! TEMPORARY - REMOVE LATER
  void printStatsForMaxRetries(const std::string& prefix) {
    std::cout << "--------------------" << std::endl;
    std::cout << prefix << " aggregate_cluster rq_retry_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster remaining_retries: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_retry: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_retry_success: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_success")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_retry_overflow: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_overflow")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_retry_limit_exceeded: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_limit_exceeded")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_retry_backoff_exponential: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_backoff_exponential")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_retry_backoff_ratelimited: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_backoff_ratelimited")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_total")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_active")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_cancelled: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_cancelled")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_timeout: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_timeout")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_completed: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_completed")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_max_duration_reached: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_max_duration_reached")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_per_try_timeout: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_per_try_timeout")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_rx_reset: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_rx_reset")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_tx_reset: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_tx_reset")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_503: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_503")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_504: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_504")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_cx_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_cx_active")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_cx_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 rq_retry_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_retry_open")->value() << std::endl;
    std::cout << prefix << " cluster_1 remaining_retries: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_retries")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_retry: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_retry_success: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_success")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_retry_overflow: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_overflow")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_retry_limit_exceeded: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_limit_exceeded")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_retry_backoff_exponential: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_backoff_exponential")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_retry_backoff_ratelimited: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_backoff_ratelimited")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_total")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_active")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_cancelled: " << test_server_->counter("cluster.cluster_1.upstream_rq_cancelled")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_timeout: " << test_server_->counter("cluster.cluster_1.upstream_rq_timeout")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_completed: " << test_server_->counter("cluster.cluster_1.upstream_rq_completed")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_max_duration_reached: " << test_server_->counter("cluster.cluster_1.upstream_rq_max_duration_reached")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_per_try_timeout: " << test_server_->counter("cluster.cluster_1.upstream_rq_per_try_timeout")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_rx_reset: " << test_server_->counter("cluster.cluster_1.upstream_rq_rx_reset")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_tx_reset: " << test_server_->counter("cluster.cluster_1.upstream_rq_tx_reset")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_503: " << test_server_->counter("cluster.cluster_1.upstream_rq_503")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_504: " << test_server_->counter("cluster.cluster_1.upstream_rq_504")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_cx_active: " << test_server_->gauge("cluster.cluster_1.upstream_cx_active")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_cx_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_total")->value() << std::endl;
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

// https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/circuit_breaking

// Cluster maximum active retries: The maximum number of retries that can be outstanding to all hosts in a cluster at any given time. 
// In general we recommend using retry budgets; however, if static circuit breaking is preferred it should aggressively circuit break retries. 
// This is so that retries for sporadic failures are allowed, but the overall retry volume cannot explode and cause large scale cascading failure. 
// If this circuit breaker overflows the upstream_rq_retry_overflow counter for the cluster will increment.

// max_retries (UInt32Value) - The maximum number of parallel retries that Envoy will allow to the upstream cluster. If not specified, the default is 3.
// rq_retry_open (Gauge) - Whether the retry circuit breaker is under its concurrency limit (0) or is at capacity and no longer admitting (1)
// remaining_retries (Gauge) - Number of remaining retries until the circuit breaker reaches its concurrency limit
// upstream_rq_retry_overflow (Counter) - Total requests not retried due to circuit breaking or exceeding the retry budget

// TEST_P(AggregateIntegrationTest, CircuitBreakerMaxRetriesONE) {
//   setDownstreamProtocol(Http::CodecType::HTTP2);

//   config_helper_.addConfigModifier([this](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
//     auto* static_resources = bootstrap.mutable_static_resources();
//     auto* listener = static_resources->mutable_listeners(0);
//     auto* filter_chain = listener->mutable_filter_chains(0);
//     auto* filter = filter_chain->mutable_filters(0);
//     envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager
//         http_connection_manager;
//     filter->mutable_typed_config()->UnpackTo(&http_connection_manager);
//     auto* virtual_host = http_connection_manager.mutable_route_config()->mutable_virtual_hosts(0);
//     auto* aggregate_cluster_route = virtual_host->mutable_routes(2);
//     auto* cluster1_route = virtual_host->mutable_routes(0);
//     aggregate_cluster_route->mutable_route()->mutable_retry_policy()->clear_retry_priority();
//     // adjust the retry policy on both the /aggregatecluster and /cluster1 routes
//     aggregate_cluster_route->mutable_route()->mutable_retry_policy()->mutable_retry_on()->assign(
//         "5xx");
//     cluster1_route->mutable_route()->mutable_retry_policy()->mutable_retry_on()->assign("5xx");
//     // !!! allow the cluster1 route to retry 3 times
//     cluster1_route->mutable_route()->mutable_retry_policy()->mutable_num_retries()->set_value(3);
//     filter->mutable_typed_config()->PackFrom(http_connection_manager);

//     auto* aggregate_cluster = static_resources->mutable_clusters(1);
//     reduceAggregateClustersListToOneCluster(*aggregate_cluster);
//     // !!! 1 max_retries
//     setCircuitBreakerLimits(*aggregate_cluster, CircuitBreakerLimits{}.withMaxRetries(1));
//   });

//   initialize();

//   // !!! 3 max_retries
//   setCircuitBreakerLimits(cluster1_, CircuitBreakerLimits{}.withMaxRetries(3));

//   EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));
//   sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
//                                                              {cluster1_}, {cluster1_}, {}, "56");
//   test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

//   // initial circuit breaker states:
//   // the aggregate cluster circuit breaker is closed
//   test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 0);
//   test_server_->waitForGaugeEq(
//       "cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 1);
//   test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 0);
//   // the cluster1 circuit breaker is closed
//   test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);
//   test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 3);
//   test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0);

//   codec_client_ = makeHttpConnection(lookupPort("http"));

//   // send a first request to the aggregate cluster
//   auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
//       Http::TestRequestHeaderMapImpl{{":method", "GET"},
//                                      {":path", "/aggregatecluster"},
//                                      {":scheme", "http"},
//                                      {":authority", "host"}});

//   // wait for the first request to arrive at cluster1
//   waitForNextUpstreamRequest(FirstUpstreamIndex);

//   // respond to the first request with a 503 to trigger a retry
//   upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

//   // wait for the first request retry to arrive at cluster1
//   waitForNextUpstreamRequest(FirstUpstreamIndex);
//   auto first_request_retry = std::move(upstream_request_);

//   // the aggregate cluster circuit breaker opens
//   test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 1);
//   test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 0);
//   test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 0);
//   // the cluster1 circuit breaker remains closed
//   test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);
//   test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 3);
//   test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0);

//   // send a second request to the aggregate cluster
//   auto aggregate_cluster_response2 = codec_client_->makeHeaderOnlyRequest(
//       Http::TestRequestHeaderMapImpl{{":method", "GET"},
//                                      {":path", "/aggregatecluster"},
//                                      {":scheme", "http"},
//                                      {":authority", "host"}});

//   // wait for the second request to arrive at cluster1
//   waitForNextUpstreamRequest(FirstUpstreamIndex);

//   // respond to the second request with a 503 to trigger a retry
//   upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

//   // the aggregate cluster circuit breaker remains open and overflows
//   test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 1);
//   test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 0);
//   test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 1);
//   // the cluster1 circuit breaker remains closed
//   test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);
//   test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 3);
//   test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0);

//   std::cout << "FIRST PART OF THE TEST ABOVE IS OK" << std::endl;

//   std::cout << "NOW CHECK DIRECT REQUESTS TO /cluster1 WHILST THE AGGREGATE CIRCUIT BREAKER IS OPEN" << std::endl;
  
//   printStatsForMaxRetries("BEFORE STARTING DIRECT REQUESTS TO /cluster1");

//   // send a request directly to cluster1 to verify its circuit breaker operates independently
//   auto cluster1_response1 = codec_client_->makeHeaderOnlyRequest(Http::TestRequestHeaderMapImpl{
//       {":method", "GET"}, {":path", "/cluster1"}, {":scheme", "http"}, {":authority", "host"}});

//   // wait for the direct request to arrive at cluster1
//   waitForNextUpstreamRequest(FirstUpstreamIndex);

//   printStatsForMaxRetries("AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM");

//   // respond with a 503 to trigger a retry
//   upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

//   printStatsForMaxRetries("AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1");

//   // wait for the first retry of the direct request
//   waitForNextUpstreamRequest(FirstUpstreamIndex);

//   printStatsForMaxRetries("AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM");

//   // respond with another 503 to trigger a second retry
//   upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

//   printStatsForMaxRetries("AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1");

//   // wait for the second retry of the direct request
//   waitForNextUpstreamRequest(FirstUpstreamIndex);

//   printStatsForMaxRetries("AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM");

//   // respond with another 503 to trigger a third retry
//   upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

//   printStatsForMaxRetries("AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2");

//   // wait for the third retry of the direct request
//   waitForNextUpstreamRequest(FirstUpstreamIndex);

//   printStatsForMaxRetries("AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM");

//   // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);
//   // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries",  1); 
//   // test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0);

//   // auto direct_request_third_retry = std::move(upstream_request_);

//   // // the aggregate cluster circuit breaker remains open
//   // test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 1);
//   // test_server_->waitForGaugeEq( "cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 0);
//   // test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 1);
//   // // the cluster1 circuit breaker opens after reaching its own retry limit
//   // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 1);
//   // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 0); 
//   // test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0);

//   // // send another direct request to cluster1 to verify its circuit breaker is now open
//   // auto cluster1_response2 = codec_client_->makeHeaderOnlyRequest(Http::TestRequestHeaderMapImpl{
//   //     {":method", "GET"}, {":path", "/cluster1"}, {":scheme", "http"}, {":authority", "host"}});

//   // // wait for the request to arrive at cluster1
//   // waitForNextUpstreamRequest(FirstUpstreamIndex);
//   // // respond with a 503 to trigger a retry attempt
//   // upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

//   // std::cout << "AFTER RESPONDING TO REQUEST2 WITH 503 TO TRIGGER RETRY" << std::endl;

//   // // the cluster1 circuit breaker should now overflow
//   // test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 1);

//   // // respond to all the pending requests
//   // // respond to the first request to the aggregate cluster
//   // first_request_retry->encodeHeaders(default_response_headers_, true);
//   // ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());
//   // EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

//   // // the second aggregate cluster request should have failed with 503 due to retry overflow
//   // ASSERT_TRUE(aggregate_cluster_response2->waitForEndStream());
//   // EXPECT_EQ("503", aggregate_cluster_response2->headers().getStatusValue());

//   // // respond to the direct request to cluster1
//   // direct_request_third_retry->encodeHeaders(default_response_headers_, true);
//   // ASSERT_TRUE(cluster1_response1->waitForEndStream());
//   // EXPECT_EQ("200", cluster1_response1->headers().getStatusValue());

//   // // the second direct request should have failed with 503 due to retry overflow
//   // ASSERT_TRUE(cluster1_response2->waitForEndStream());
//   // EXPECT_EQ("503", cluster1_response2->headers().getStatusValue());

//   // // after all the requests complete both the circuit breakers should close
//   // test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open",
//   //                              0);
//   // test_server_->waitForGaugeEq(
//   //     "cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 1);
//   // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);
//   // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries",
//   // 3);

//   // std::cout << "DID WE REACH HERE ?? THE END" << std::endl;

//   cleanupUpstreamAndDownstream();
// }

// [ RUN      ] IpVersions/AggregateIntegrationTest.CircuitBreakerMaxRetriesAggregateClusterOnly/3
// FIRST PART OF THE TEST ABOVE IS OK
// NOW CHECK DIRECT REQUESTS TO /cluster1 WHILST THE AGGREGATE CIRCUIT BREAKER IS OPEN
// --------------------
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 aggregate_cluster rq_retry_open: 1
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 aggregate_cluster remaining_retries: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 aggregate_cluster upstream_rq_retry_overflow: 1
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 rq_retry_open: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 remaining_retries: 3
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_retry: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_retry_success: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_retry_overflow: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_retry_limit_exceeded: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_retry_backoff_exponential: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_retry_backoff_ratelimited: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_total: 3
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_active: 1
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_cancelled: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_timeout: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_completed: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_max_duration_reached: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_per_try_timeout: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_rx_reset: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_rq_tx_reset: 0
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_cx_active: 1
// BEFORE STARTING DIRECT REQUESTS TO /cluster1 cluster_1 upstream_cx_total: 1
// --------------------
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM aggregate_cluster rq_retry_open: 1
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM aggregate_cluster remaining_retries: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM aggregate_cluster upstream_rq_retry_overflow: 1
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 rq_retry_open: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 remaining_retries: 3
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_retry: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_retry_success: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_retry_overflow: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_retry_limit_exceeded: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_retry_backoff_exponential: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_retry_backoff_ratelimited: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_total: 4
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_active: 2
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_cancelled: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_timeout: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_completed: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_max_duration_reached: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_per_try_timeout: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_rx_reset: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_rq_tx_reset: 0
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_cx_active: 1
// AFTER WAIT FOR REQUEST1 TO REACH UPSTREAM cluster_1 upstream_cx_total: 1
// --------------------
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 aggregate_cluster rq_retry_open: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 aggregate_cluster remaining_retries: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 aggregate_cluster upstream_rq_retry_overflow: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 rq_retry_open: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 remaining_retries: 2
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_retry: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_retry_success: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_retry_overflow: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_retry_limit_exceeded: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_retry_backoff_exponential: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_retry_backoff_ratelimited: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_total: 4
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_active: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_cancelled: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_timeout: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_completed: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_max_duration_reached: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_per_try_timeout: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_rx_reset: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_rq_tx_reset: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_cx_active: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1 cluster_1 upstream_cx_total: 1
// --------------------
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM aggregate_cluster rq_retry_open: 1
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM aggregate_cluster remaining_retries: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM aggregate_cluster upstream_rq_retry_overflow: 1
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 rq_retry_open: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 remaining_retries: 2
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_retry: 1
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_retry_success: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_retry_overflow: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_retry_limit_exceeded: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_retry_backoff_exponential: 1
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_retry_backoff_ratelimited: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_total: 5
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_active: 2
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_cancelled: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_timeout: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_completed: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_max_duration_reached: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_per_try_timeout: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_rx_reset: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_rq_tx_reset: 0
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_cx_active: 1
// AFTER WAIT FOR REQUEST1-RETRY1 TO REACH UPSTREAM cluster_1 upstream_cx_total: 1
// --------------------
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 aggregate_cluster rq_retry_open: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 aggregate_cluster remaining_retries: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 aggregate_cluster upstream_rq_retry_overflow: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 rq_retry_open: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 remaining_retries: 2
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_retry: 2
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_retry_success: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_retry_overflow: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_retry_limit_exceeded: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_retry_backoff_exponential: 2
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_retry_backoff_ratelimited: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_total: 5
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_active: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_cancelled: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_timeout: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_completed: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_max_duration_reached: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_per_try_timeout: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_rx_reset: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_rq_tx_reset: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_cx_active: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY1 cluster_1 upstream_cx_total: 1
// --------------------
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM aggregate_cluster rq_retry_open: 1
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM aggregate_cluster remaining_retries: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM aggregate_cluster upstream_rq_retry_overflow: 1
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 rq_retry_open: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 remaining_retries: 2
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_retry: 2
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_retry_success: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_retry_overflow: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_retry_limit_exceeded: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_retry_backoff_exponential: 2
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_retry_backoff_ratelimited: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_total: 6
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_active: 2
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_cancelled: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_timeout: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_completed: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_max_duration_reached: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_per_try_timeout: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_rx_reset: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_rq_tx_reset: 0
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_cx_active: 1
// AFTER WAIT FOR REQUEST1-RETRY2 TO REACH UPSTREAM cluster_1 upstream_cx_total: 1
// --------------------
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 aggregate_cluster rq_retry_open: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 aggregate_cluster remaining_retries: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 aggregate_cluster upstream_rq_retry_overflow: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 rq_retry_open: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 remaining_retries: 2
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_retry: 3
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_retry_success: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_retry_overflow: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_retry_limit_exceeded: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_retry_backoff_exponential: 3
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_retry_backoff_ratelimited: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_total: 6
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_active: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_cancelled: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_timeout: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_completed: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_max_duration_reached: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_per_try_timeout: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_rx_reset: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_rq_tx_reset: 0
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_cx_active: 1
// AFTER UPSTREAM ENCODE HEADERS 503 ON REQUEST1-RETRY2 cluster_1 upstream_cx_total: 1
// --------------------
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM aggregate_cluster rq_retry_open: 1
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM aggregate_cluster remaining_retries: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM aggregate_cluster upstream_rq_retry_overflow: 1
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 rq_retry_open: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 remaining_retries: 2
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_retry: 3
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_retry_success: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_retry_overflow: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_retry_limit_exceeded: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_retry_backoff_exponential: 3
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_retry_backoff_ratelimited: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_total: 7
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_active: 2
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_cancelled: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_timeout: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_completed: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_max_duration_reached: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_per_try_timeout: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_rx_reset: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_rq_tx_reset: 0
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_cx_active: 1
// AFTER WAIT FOR REQUEST1-RETRY3 TO REACH UPSTREAM cluster_1 upstream_cx_total: 1

// Tests that the max_retries circuit breaker on the aggregate cluster works independently of
// the underlying clusters. When retries exceed the configured limit on the aggregate cluster,
// its circuit breaker opens and prevents additional retries through the aggregate cluster.
// However, the underlying cluster's circuit breaker remains unaffected, allowing direct requests
// to the underlying cluster to still perform retries.
TEST_P(AggregateIntegrationTest, CircuitBreakerMaxRetriesTWO) {
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
    auto* cluster1_route = virtual_host->mutable_routes(0);
    aggregate_cluster_route->mutable_route()->mutable_retry_policy()->clear_retry_priority();
    
    aggregate_cluster_route->mutable_route()->mutable_retry_policy()->mutable_retry_on()->assign("5xx");
    cluster1_route->mutable_route()->mutable_retry_policy()->mutable_retry_on()->assign("5xx");
    
    // set cluster1 route to retry 3 times
    cluster1_route->mutable_route()->mutable_retry_policy()->mutable_num_retries()->set_value(3);
    
    // Set a short retry timeout to make retries happen quickly
    cluster1_route->mutable_route()->mutable_retry_policy()->mutable_per_try_timeout()->set_seconds(1);
    aggregate_cluster_route->mutable_route()->mutable_retry_policy()->mutable_per_try_timeout()->set_seconds(1);
    
    filter->mutable_typed_config()->PackFrom(http_connection_manager);

    auto* aggregate_cluster = static_resources->mutable_clusters(1);
    reduceAggregateClustersListToOneCluster(*aggregate_cluster);
    // max_retries 1
    setCircuitBreakerLimits(*aggregate_cluster, CircuitBreakerLimits{}.withMaxRetries(1));
  });

  initialize();

  // max_retries 3
  setCircuitBreakerLimits(cluster1_, CircuitBreakerLimits{}.withMaxRetries(3));
  
  // !!! MORE concurrent streams (probably remove this later)
  setMaxConcurrentStreams(cluster1_, 100);

  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "56");
  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  // initial circuit breaker states
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 0);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 1); // 1 retry
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 0);  
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 3); // 3 retries
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0);

  // this will now do multiple concurrent streams
  codec_client_ = makeHttpConnection(lookupPort("http"));

  // send a request to the aggregate cluster
  auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  // wait for the request to arrive at cluster1
  waitForNextUpstreamRequest(FirstUpstreamIndex);
  
  // respond with 503 to trigger a retry
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

  // wait for the retry to arrive
  waitForNextUpstreamRequest(FirstUpstreamIndex);
   
  // aggregate cluster circuit breaker opens
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 1);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 0);
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 0);
  // cluster1 circuit breaker unaffected
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 3);
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0);

  // send a second request to the aggregate cluster
  auto aggregate_cluster_response2 = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  // wait for the second request to arrive
  waitForNextUpstreamRequest(FirstUpstreamIndex);
  
  // respond with 503 to trigger a retry#']
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

  // aggregate cluster circuit breaker stays open and overflows
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 1);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 0);
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 1);
  // cluster1 circuit breaker unaffected
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 3);
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0);

  // ----------------------
  
  // now directly test the cluster1 circuit breaker WHILST the aggregate cluster circuit breaker is OPEN
  
  // !! we need to create multiple CONCURRENT requests to cluster1

  std::vector<IntegrationStreamDecoderPtr> cluster1_responses;
  
  // send four requests so we overflow the max_retries 3
  for (int i = 0; i < 4; i++) {
    cluster1_responses.push_back(codec_client_->makeHeaderOnlyRequest(
        Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                      {":path", "/cluster1"},
                                      {":scheme", "http"},
                                      {":authority", "host"}}));
    printStatsForMaxRetries("SEND REQUEST " + std::to_string(i));
  }
  
  // wait for all four requests to reach cluster1, each time responding with 503
  for (int i = 0; i < 4; i++) {
    waitForNextUpstreamRequest(FirstUpstreamIndex);
    upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);
    printStatsForMaxRetries("REQUEST " + std::to_string(i) + " REACH UPSTREAM + RESPOND");
  }
  
  // wait for 3 retries to reach cluster1
  for (int i = 0; i < 3; i++) {
    waitForNextUpstreamRequest(FirstUpstreamIndex);
    upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);
    printStatsForMaxRetries("RETRY " + std::to_string(i) + " REACH UPSTREAM + RESPOND");
  }
  
  // this SHOULD overflow...
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 1);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 0);
  test_server_->waitForCounterGe("cluster.cluster_1.upstream_rq_retry_overflow", 1);
   
  // // complete the first aggregate cluster request
  // waitForNextUpstreamRequest(FirstUpstreamIndex);
  // upstream_request_->encodeHeaders(default_response_headers_, true);
  
  // ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());
  // EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());
  
  // ASSERT_TRUE(aggregate_cluster_response2->waitForEndStream());
  // EXPECT_EQ("503", aggregate_cluster_response2->headers().getStatusValue());
  
  // // complete all the pending requests
  // for (size_t i = 0; i < 5; i++) {
  //   if (i < cluster1_responses.size()) {
  //     waitForNextUpstreamRequest(FirstUpstreamIndex);
  //     upstream_request_->encodeHeaders(default_response_headers_, true);
  //   }
  // }
  
  // // wait for all responses to complete
  // for (auto& response : cluster1_responses) {
  //   ASSERT_TRUE(response->waitForEndStream());
  // }
  
  // ASSERT_TRUE(additional_response->waitForEndStream());
  
  // // after all the requests complete, both circuit breakers should close
  // test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 0);
  // test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 1);
  // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);
  // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 3);

  cleanupUpstreamAndDownstream();
}

// [ RUN      ] IpVersions/AggregateIntegrationTest.CircuitBreakerMaxRetriesTWO/3
// --------------------
// SEND REQUEST 0 aggregate_cluster rq_retry_open: 1
// SEND REQUEST 0 aggregate_cluster remaining_retries: 0
// SEND REQUEST 0 aggregate_cluster upstream_rq_retry_overflow: 1
// SEND REQUEST 0 cluster_1 rq_retry_open: 0
// SEND REQUEST 0 cluster_1 remaining_retries: 3
// SEND REQUEST 0 cluster_1 upstream_rq_retry_overflow: 0
// --------------------
// SEND REQUEST 1 aggregate_cluster rq_retry_open: 1
// SEND REQUEST 1 aggregate_cluster remaining_retries: 0
// SEND REQUEST 1 aggregate_cluster upstream_rq_retry_overflow: 1
// SEND REQUEST 1 cluster_1 rq_retry_open: 0
// SEND REQUEST 1 cluster_1 remaining_retries: 3
// SEND REQUEST 1 cluster_1 upstream_rq_retry_overflow: 0
// --------------------
// SEND REQUEST 2 aggregate_cluster rq_retry_open: 1
// SEND REQUEST 2 aggregate_cluster remaining_retries: 0
// SEND REQUEST 2 aggregate_cluster upstream_rq_retry_overflow: 1
// SEND REQUEST 2 cluster_1 rq_retry_open: 0
// SEND REQUEST 2 cluster_1 remaining_retries: 3
// SEND REQUEST 2 cluster_1 upstream_rq_retry_overflow: 0
// --------------------
// SEND REQUEST 3 aggregate_cluster rq_retry_open: 1
// SEND REQUEST 3 aggregate_cluster remaining_retries: 0
// SEND REQUEST 3 aggregate_cluster upstream_rq_retry_overflow: 1
// SEND REQUEST 3 cluster_1 rq_retry_open: 0
// SEND REQUEST 3 cluster_1 remaining_retries: 3
// SEND REQUEST 3 cluster_1 upstream_rq_retry_overflow: 0
// --------------------
// REQUEST 0 REACH UPSTREAM + RESPOND aggregate_cluster rq_retry_open: 1
// REQUEST 0 REACH UPSTREAM + RESPOND aggregate_cluster remaining_retries: 0
// REQUEST 0 REACH UPSTREAM + RESPOND aggregate_cluster upstream_rq_retry_overflow: 1
// REQUEST 0 REACH UPSTREAM + RESPOND cluster_1 rq_retry_open: 0
// REQUEST 0 REACH UPSTREAM + RESPOND cluster_1 remaining_retries: 2
// REQUEST 0 REACH UPSTREAM + RESPOND cluster_1 upstream_rq_retry_overflow: 0
// --------------------
// REQUEST 1 REACH UPSTREAM + RESPOND aggregate_cluster rq_retry_open: 1
// REQUEST 1 REACH UPSTREAM + RESPOND aggregate_cluster remaining_retries: 0
// REQUEST 1 REACH UPSTREAM + RESPOND aggregate_cluster upstream_rq_retry_overflow: 1
// REQUEST 1 REACH UPSTREAM + RESPOND cluster_1 rq_retry_open: 0
// REQUEST 1 REACH UPSTREAM + RESPOND cluster_1 remaining_retries: 1
// REQUEST 1 REACH UPSTREAM + RESPOND cluster_1 upstream_rq_retry_overflow: 0
// --------------------
// REQUEST 2 REACH UPSTREAM + RESPOND aggregate_cluster rq_retry_open: 1
// REQUEST 2 REACH UPSTREAM + RESPOND aggregate_cluster remaining_retries: 0
// REQUEST 2 REACH UPSTREAM + RESPOND aggregate_cluster upstream_rq_retry_overflow: 1
// REQUEST 2 REACH UPSTREAM + RESPOND cluster_1 rq_retry_open: 1
// REQUEST 2 REACH UPSTREAM + RESPOND cluster_1 remaining_retries: 0
// REQUEST 2 REACH UPSTREAM + RESPOND cluster_1 upstream_rq_retry_overflow: 0
// --------------------
// REQUEST 3 REACH UPSTREAM + RESPOND aggregate_cluster rq_retry_open: 1
// REQUEST 3 REACH UPSTREAM + RESPOND aggregate_cluster remaining_retries: 0
// REQUEST 3 REACH UPSTREAM + RESPOND aggregate_cluster upstream_rq_retry_overflow: 1
// REQUEST 3 REACH UPSTREAM + RESPOND cluster_1 rq_retry_open: 1
// REQUEST 3 REACH UPSTREAM + RESPOND cluster_1 remaining_retries: 0
// REQUEST 3 REACH UPSTREAM + RESPOND cluster_1 upstream_rq_retry_overflow: 1
// --------------------
// RETRY 0 REACH UPSTREAM + RESPOND aggregate_cluster rq_retry_open: 1
// RETRY 0 REACH UPSTREAM + RESPOND aggregate_cluster remaining_retries: 0
// RETRY 0 REACH UPSTREAM + RESPOND aggregate_cluster upstream_rq_retry_overflow: 1
// RETRY 0 REACH UPSTREAM + RESPOND cluster_1 rq_retry_open: 1
// RETRY 0 REACH UPSTREAM + RESPOND cluster_1 remaining_retries: 0
// RETRY 0 REACH UPSTREAM + RESPOND cluster_1 upstream_rq_retry_overflow: 1
// --------------------
// RETRY 1 REACH UPSTREAM + RESPOND aggregate_cluster rq_retry_open: 1
// RETRY 1 REACH UPSTREAM + RESPOND aggregate_cluster remaining_retries: 0
// RETRY 1 REACH UPSTREAM + RESPOND aggregate_cluster upstream_rq_retry_overflow: 1
// RETRY 1 REACH UPSTREAM + RESPOND cluster_1 rq_retry_open: 1
// RETRY 1 REACH UPSTREAM + RESPOND cluster_1 remaining_retries: 0
// RETRY 1 REACH UPSTREAM + RESPOND cluster_1 upstream_rq_retry_overflow: 1
// --------------------
// RETRY 2 REACH UPSTREAM + RESPOND aggregate_cluster rq_retry_open: 1
// RETRY 2 REACH UPSTREAM + RESPOND aggregate_cluster remaining_retries: 0
// RETRY 2 REACH UPSTREAM + RESPOND aggregate_cluster upstream_rq_retry_overflow: 1
// RETRY 2 REACH UPSTREAM + RESPOND cluster_1 rq_retry_open: 1
// RETRY 2 REACH UPSTREAM + RESPOND cluster_1 remaining_retries: 0
// RETRY 2 REACH UPSTREAM + RESPOND cluster_1 upstream_rq_retry_overflow: 1

} // namespace
} // namespace Envoy
