#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/extensions/clusters/aggregate/v3/cluster.pb.h"
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

  const bool deferred_cluster_creation_;
  envoy::config::cluster::v3::Cluster cluster1_;
  envoy::config::cluster::v3::Cluster cluster2_;

  // !! TEMPORARY
  void printStatsForMaxRetries(const std::string& prefix) {
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

// Cluster maximum active retries: 
// The maximum number of retries that can be outstanding to all hosts in a cluster at any given time. 
// In general we recommend using retry budgets; however, if static circuit breaking is preferred it should aggressively circuit break retries. 
// This is so that retries for sporadic failures are allowed, but the overall retry volume cannot explode and cause large scale cascading failure. 
// If this circuit breaker overflows the upstream_rq_retry_overflow counter for the cluster will increment.

// max_retries (UInt32Value) - The maximum number of parallel retries that Envoy will allow to the upstream cluster. If not specified, the default is 3.
// rq_retry_open (Gauge) - Whether the retry circuit breaker is under its concurrency limit (0) or is at capacity and no longer admitting (1)
// remaining_retries (Gauge) - Number of remaining retries until the circuit breaker reaches its concurrency limit
// upstream_rq_retry_overflow (Counter) - Total requests not retried due to circuit breaking or exceeding the retry budget

TEST_P(AggregateIntegrationTest, CircuitBreakerMaxRetriesTest) {
  setDownstreamProtocol(Http::CodecType::HTTP2);
  
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* aggregate_cluster = static_resources->mutable_clusters(1);

    // remove cluster_2 from the aggregate cluster's clusters list
    auto* aggregate_cluster_type = aggregate_cluster->mutable_cluster_type();
    auto* aggregate_cluster_typed_config = aggregate_cluster_type->mutable_typed_config();
    envoy::extensions::clusters::aggregate::v3::ClusterConfig temp_aggregate_cluster_typed_config;
    aggregate_cluster_typed_config->UnpackTo(&temp_aggregate_cluster_typed_config);
    temp_aggregate_cluster_typed_config.clear_clusters();
    temp_aggregate_cluster_typed_config.add_clusters("cluster_1");
    aggregate_cluster_typed_config->PackFrom(temp_aggregate_cluster_typed_config);

    // set the aggregate_cluster circuit breakers
    auto* aggregate_cluster_circuit_breakers = aggregate_cluster->mutable_circuit_breakers();
    auto* aggregate_cluster_circuit_breakers_threshold_default = aggregate_cluster_circuit_breakers->add_thresholds();
    aggregate_cluster_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1); // set to 1
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
    aggregate_cluster_circuit_breakers_threshold_default->set_track_remaining(true);

    auto* listener = static_resources->mutable_listeners(0);
    auto* filter_chain = listener->mutable_filter_chains(0);
    auto* filter = filter_chain->mutable_filters(0);
    envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager http_connection_manager;
    filter->mutable_typed_config()->UnpackTo(&http_connection_manager);
    auto* virtual_host = http_connection_manager.mutable_route_config()->mutable_virtual_hosts(0);
    auto* aggregate_cluster_route = virtual_host->mutable_routes(2);
    aggregate_cluster_route->mutable_route()->mutable_retry_policy()->clear_retry_priority();
    // adjust the retry policy on the /aggregatecluster route
    aggregate_cluster_route->mutable_route()->mutable_retry_policy()->mutable_retry_on()->assign("5xx"); // retry on 5xx
    auto* cluster1_route = virtual_host->mutable_routes(0);
    // adjust the retry policy on the /cluster1 route
    cluster1_route->mutable_route()->mutable_retry_policy()->mutable_retry_on()->assign("5xx"); // retry on 5xx
    filter->mutable_typed_config()->PackFrom(http_connection_manager);
  });

  initialize();

  // set the cluster1 breakers
  auto* cluster1_circuit_breakers = cluster1_.mutable_circuit_breakers();
  auto* cluster1_circuit_breakers_threshold_default = cluster1_circuit_breakers->add_thresholds();
  cluster1_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  cluster1_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1); // set to 1
  cluster1_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->set_track_remaining(true);

  // update cluster1 via xDS
  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster, {cluster1_}, {cluster1_}, {}, "56");
  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  // confirm we are in the default state for both circuit breakers
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 0);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 1);
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 1);
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0);

  printStatsForMaxRetries("BEFORE");

  codec_client_ = makeHttpConnection(lookupPort("http"));

  // send the first request to /aggregatecluster
  auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  // wait for the first request (/aggregatecluster) to reach cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  printStatsForMaxRetries("REQUEST-1");

  // deliberately respond to the first request (/aggregatecluster) with 503 to trigger the first retry
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

  // wait for the first request retry (/aggregatecluster) to reach cluster_1 [this comes from an automatic retry]
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // save a reference to this specific request, so we can access it later, because upstream_request_ will get overwritten
  auto& first_upstream_request_retry = *upstream_request_;

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 1); // !! aggregate cluster circuit breaker is triggered
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 0); // no more retries allowed
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0); // unaffected
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 1); // unaffected
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0); // unaffected

  printStatsForMaxRetries("REQUEST-1-RETRY");

  // send the second request to /aggregatecluster
  auto aggregate_cluster_response2 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  // wait for the second request (/aggregatecluster) to arrive at cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  printStatsForMaxRetries("REQUEST-2");

  // deliberately respond to the second request (/aggregatecluster) with a 503 to trigger a retry
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 1); // persisting
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 0); // persisting
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 1); // 1 overflow on the aggregate cluster circuit breaker
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0); // unaffected
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 1); // unaffected
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0); // unaffected

  printStatsForMaxRetries("REQUEST-2-RETRY");

  // the second retry should be automatically rejected

  // send the third request directly to /cluster1
  auto cluster1_response = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/cluster1"},{":scheme", "http"},{":authority", "host"}}
  );

  // wait for the third request (/cluster1) to arrive at cluster_1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  printStatsForMaxRetries("REQUEST-3");

  // deliberately respond to the third request (/cluster1) with a 503 to trigger a retry
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

  // wait for the third request retry (/cluster1) to reach cluster_1 [this comes from an automatic retry]
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  printStatsForMaxRetries("REQUEST-3-RETRY");

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 1); // persisting
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 0); // persisting
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 1); // persisting
 
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 1); // cluster1 circuit breaker now triggered (but is separate from the aggregate cluster circuit breaker)
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 0); // see comment above
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0); // unaffected

  // respond to the third request retry
  // THE FACT WE CAN DO THIS PROVES THE AGGREGATE CLUSTER CIRCUIT BREAKER IS SEPARATE FROM THE CLUSTER1 CIRCUIT BREAKER
  // OTHERWISE THIS REQUEST WOULD BE AUTOREJECTED IF THE /aggregatecluster route max_retries circuit breaker affected the underlying cluster1
  upstream_request_->encodeHeaders(default_response_headers_, true);

  // complete request/response3 (/cluster1)
  ASSERT_TRUE(cluster1_response->waitForEndStream());
  EXPECT_EQ("200", cluster1_response->headers().getStatusValue());

  // (finally) respond to the first request retry (/aggregatecluster)
  first_upstream_request_retry.encodeHeaders(default_response_headers_, true);

  // we don't need to respond to the second request retry, because it will be automatically rejected by the circuit breaker

  // complete request/response1 (/aggregatecluster)
  ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());
  EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

  // complete request/response2 (/aggregatecluster)
  ASSERT_TRUE(aggregate_cluster_response2->waitForEndStream());
  EXPECT_EQ("503", aggregate_cluster_response2->headers().getStatusValue()); // this was from the auto-rejection

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open", 0); // returned to its initial state
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 1); // returned to its initial state
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_rq_retry_overflow", 1); // overflowed 1 time
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0); // returned to its initial state
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 1);// returned to its initial state
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_retry_overflow", 0); // unaffected
  
  printStatsForMaxRetries("AFTER");

  cleanupUpstreamAndDownstream();
}

} // namespace
} // namespace Envoy

// AFTER aggregate_cluster rq_retry_open: 0
// AFTER aggregate_cluster remaining_retries: 1
// AFTER aggregate_cluster upstream_rq_retry: 1
// AFTER aggregate_cluster upstream_rq_retry_success: 1
// AFTER aggregate_cluster upstream_rq_retry_overflow: 1
// AFTER aggregate_cluster upstream_rq_retry_limit_exceeded: 0
// AFTER aggregate_cluster upstream_rq_retry_backoff_exponential: 1
// AFTER aggregate_cluster upstream_rq_retry_backoff_ratelimited: 0
// AFTER aggregate_cluster upstream_rq_total: 0
// AFTER aggregate_cluster upstream_rq_active: 0
// AFTER aggregate_cluster upstream_rq_cancelled: 0
// AFTER aggregate_cluster upstream_rq_timeout: 0
// AFTER aggregate_cluster upstream_rq_completed: 2
// AFTER aggregate_cluster upstream_rq_max_duration_reached: 0
// AFTER aggregate_cluster upstream_rq_per_try_timeout: 0
// AFTER aggregate_cluster upstream_rq_rx_reset: 0
// AFTER aggregate_cluster upstream_rq_tx_reset: 0
// AFTER aggregate_cluster upstream_cx_active: 0
// AFTER aggregate_cluster upstream_cx_total: 0
// AFTER cluster_1 rq_retry_open: 0
// AFTER cluster_1 remaining_retries: 1
// AFTER cluster_1 upstream_rq_retry: 1
// AFTER cluster_1 upstream_rq_retry_success: 1
// AFTER cluster_1 upstream_rq_retry_overflow: 0
// AFTER cluster_1 upstream_rq_retry_limit_exceeded: 0
// AFTER cluster_1 upstream_rq_retry_backoff_exponential: 1
// AFTER cluster_1 upstream_rq_retry_backoff_ratelimited: 0
// AFTER cluster_1 upstream_rq_total: 5
// AFTER cluster_1 upstream_rq_active: 0
// AFTER cluster_1 upstream_rq_cancelled: 0
// AFTER cluster_1 upstream_rq_timeout: 0
// AFTER cluster_1 upstream_rq_completed: 1
// AFTER cluster_1 upstream_rq_max_duration_reached: 0
// AFTER cluster_1 upstream_rq_per_try_timeout: 0
// AFTER cluster_1 upstream_rq_rx_reset: 0
// AFTER cluster_1 upstream_rq_tx_reset: 0
// AFTER cluster_1 upstream_cx_active: 1
// AFTER cluster_1 upstream_cx_total: 1
