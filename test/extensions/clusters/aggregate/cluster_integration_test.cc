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

  // !! REMOVE THIS AFTER
  void printStatsForMaxConnections(const std::string& prefix) {
    std::cout << prefix << " aggregate_cluster cx_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.cx_open")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster remaining_cx: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_overflow: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_overflow")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster rq_pending_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster remaining_pending: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_pending_overflow: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_pending_overflow")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_total")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_cx_active")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_http1_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_http1_total")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_http2_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_http2_total")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_http3_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_http3_total")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_connect_fail: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_connect_fail")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_connect_timeout: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_connect_timeout")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_connect_with_0_rtt: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_connect_with_0_rtt")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_idle_timeout: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_idle_timeout")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_max_duration_reached: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_max_duration_reached")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_connect_attempts_exceeded: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_connect_attempts_exceeded")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_destroy: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_destroy")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_destroy_local: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_destroy_local")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_destroy_remote: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_destroy_remote")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_destroy_with_active_rq: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_destroy_with_active_rq")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_destroy_local_with_active_rq: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_destroy_local_with_active_rq")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_destroy_remote_with_active_rq: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_destroy_remote_with_active_rq")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_close_notify: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_close_notify")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_rx_bytes_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_rx_bytes_total")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_rx_bytes_buffered: " << test_server_->gauge("cluster.aggregate_cluster.upstream_cx_rx_bytes_buffered")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_tx_bytes_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_tx_bytes_total")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_tx_bytes_buffered: " << test_server_->gauge("cluster.aggregate_cluster.upstream_cx_tx_bytes_buffered")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_pool_overflow: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_pool_overflow")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_protocol_error: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_protocol_error")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_max_requests: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_max_requests")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_cx_none_healthy: " << test_server_->counter("cluster.aggregate_cluster.upstream_cx_none_healthy")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_total")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_active")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_pending_total: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_pending_total")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_pending_overflow: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_pending_overflow")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_pending_failure_eject: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_pending_failure_eject")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_pending_active: " << test_server_->gauge("cluster.aggregate_cluster.upstream_rq_pending_active")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_cancelled: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_cancelled")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_maintenance_mode: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_maintenance_mode")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_timeout: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_timeout")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_max_duration_reached: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_max_duration_reached")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_per_try_timeout: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_per_try_timeout")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_rx_reset: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_rx_reset")->value() << std::endl;
    std::cout << prefix << " aggregate_cluster upstream_rq_tx_reset: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_tx_reset")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_retry: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_retry_backoff_exponential: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_backoff_exponential")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_retry_backoff_ratelimited: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_backoff_ratelimited")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_retry_limit_exceeded: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_limit_exceeded")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_retry_success: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_success")->value() << std::endl;
    // std::cout << prefix << " aggregate_cluster upstream_rq_retry_overflow: " << test_server_->counter("cluster.aggregate_cluster.upstream_rq_retry_overflow")->value() << std::endl;
    std::cout << prefix << " cluster_1 cx_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.cx_open")->value() << std::endl;
    std::cout << prefix << " cluster_1 remaining_cx: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_cx")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_overflow: " << test_server_->counter("cluster.cluster_1.upstream_cx_overflow")->value() << std::endl;
    std::cout << prefix << " cluster_1 rq_pending_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value() << std::endl;
    std::cout << prefix << " cluster_1 remaining_pending: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_pending")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_pending_overflow: " << test_server_->counter("cluster.cluster_1.upstream_rq_pending_overflow")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_active: " << test_server_->gauge("cluster.cluster_1.upstream_cx_active")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_http1_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_http1_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_http2_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_http2_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_http3_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_http3_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_connect_fail: " << test_server_->counter("cluster.cluster_1.upstream_cx_connect_fail")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_connect_timeout: " << test_server_->counter("cluster.cluster_1.upstream_cx_connect_timeout")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_connect_with_0_rtt: " << test_server_->counter("cluster.cluster_1.upstream_cx_connect_with_0_rtt")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_idle_timeout: " << test_server_->counter("cluster.cluster_1.upstream_cx_idle_timeout")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_max_duration_reached: " << test_server_->counter("cluster.cluster_1.upstream_cx_max_duration_reached")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_connect_attempts_exceeded: " << test_server_->counter("cluster.cluster_1.upstream_cx_connect_attempts_exceeded")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_destroy: " << test_server_->counter("cluster.cluster_1.upstream_cx_destroy")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_destroy_local: " << test_server_->counter("cluster.cluster_1.upstream_cx_destroy_local")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_destroy_remote: " << test_server_->counter("cluster.cluster_1.upstream_cx_destroy_remote")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_destroy_with_active_rq: " << test_server_->counter("cluster.cluster_1.upstream_cx_destroy_with_active_rq")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_destroy_local_with_active_rq: " << test_server_->counter("cluster.cluster_1.upstream_cx_destroy_local_with_active_rq")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_destroy_remote_with_active_rq: " << test_server_->counter("cluster.cluster_1.upstream_cx_destroy_remote_with_active_rq")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_close_notify: " << test_server_->counter("cluster.cluster_1.upstream_cx_close_notify")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_rx_bytes_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_rx_bytes_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_rx_bytes_buffered: " << test_server_->gauge("cluster.cluster_1.upstream_cx_rx_bytes_buffered")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_tx_bytes_total: " << test_server_->counter("cluster.cluster_1.upstream_cx_tx_bytes_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_tx_bytes_buffered: " << test_server_->gauge("cluster.cluster_1.upstream_cx_tx_bytes_buffered")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_pool_overflow: " << test_server_->counter("cluster.cluster_1.upstream_cx_pool_overflow")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_protocol_error: " << test_server_->counter("cluster.cluster_1.upstream_cx_protocol_error")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_max_requests: " << test_server_->counter("cluster.cluster_1.upstream_cx_max_requests")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_cx_none_healthy: " << test_server_->counter("cluster.cluster_1.upstream_cx_none_healthy")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_active")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_pending_total: " << test_server_->counter("cluster.cluster_1.upstream_rq_pending_total")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_pending_overflow: " << test_server_->counter("cluster.cluster_1.upstream_rq_pending_overflow")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_pending_failure_eject: " << test_server_->counter("cluster.cluster_1.upstream_rq_pending_failure_eject")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_pending_active: " << test_server_->gauge("cluster.cluster_1.upstream_rq_pending_active")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_cancelled: " << test_server_->counter("cluster.cluster_1.upstream_rq_cancelled")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_maintenance_mode: " << test_server_->counter("cluster.cluster_1.upstream_rq_maintenance_mode")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_timeout: " << test_server_->counter("cluster.cluster_1.upstream_rq_timeout")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_max_duration_reached: " << test_server_->counter("cluster.cluster_1.upstream_rq_max_duration_reached")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_per_try_timeout: " << test_server_->counter("cluster.cluster_1.upstream_rq_per_try_timeout")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_rx_reset: " << test_server_->counter("cluster.cluster_1.upstream_rq_rx_reset")->value() << std::endl;
    std::cout << prefix << " cluster_1 upstream_rq_tx_reset: " << test_server_->counter("cluster.cluster_1.upstream_rq_tx_reset")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_retry: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_retry_backoff_exponential: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_backoff_exponential")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_retry_backoff_ratelimited: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_backoff_ratelimited")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_retry_limit_exceeded: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_limit_exceeded")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_retry_success: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_success")->value() << std::endl;
    // std::cout << prefix << " cluster_1 upstream_rq_retry_overflow: " << test_server_->counter("cluster.cluster_1.upstream_rq_retry_overflow")->value() << std::endl;
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

// TEST_P(AggregateIntegrationTest, ORIGINALCircuitBreakerMaxConnectionsTest) {
//   setDownstreamProtocol(Http::CodecType::HTTP2);
  
//   config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
//     auto* static_resources = bootstrap.mutable_static_resources();
//     auto* aggregate_cluster = static_resources->mutable_clusters(1);
    
//     auto* aggregate_cluster_type = aggregate_cluster->mutable_cluster_type();
//     auto* aggregate_cluster_typed_config = aggregate_cluster_type->mutable_typed_config();
//     envoy::extensions::clusters::aggregate::v3::ClusterConfig temp_aggregate_cluster_typed_config;
//     aggregate_cluster_typed_config->UnpackTo(&temp_aggregate_cluster_typed_config);
//     temp_aggregate_cluster_typed_config.clear_clusters();
//     temp_aggregate_cluster_typed_config.add_clusters("cluster_1");
//     aggregate_cluster_typed_config->PackFrom(temp_aggregate_cluster_typed_config);

//     auto* aggregate_cluster_circuit_breakers = aggregate_cluster->mutable_circuit_breakers();
//     auto* aggregate_cluster_circuit_breakers_threshold_default = aggregate_cluster_circuit_breakers->add_thresholds();
//     aggregate_cluster_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
//     aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1); // set to 1
//     aggregate_cluster_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
//     aggregate_cluster_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
//     aggregate_cluster_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000); // set this high
//     aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
//     aggregate_cluster_circuit_breakers_threshold_default->set_track_remaining(true);

//     envoy::extensions::upstreams::http::v3::HttpProtocolOptions http_protocol_options;
//     // set http2_protocol_options max_concurrent_streams to 1
//     http_protocol_options.mutable_explicit_http_config()->mutable_http2_protocol_options()->mutable_max_concurrent_streams()->set_value(1);
//     (*aggregate_cluster->mutable_typed_extension_protocol_options())
//       ["envoy.extensions.upstreams.http.v3.HttpProtocolOptions"]
//         .PackFrom(http_protocol_options);
//   });

//   initialize();

//   auto* cluster1_circuit_breakers = cluster1_.mutable_circuit_breakers();
//   auto* cluster1_circuit_breakers_threshold_default = cluster1_circuit_breakers->add_thresholds();
//   cluster1_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
//   cluster1_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1); // set to 1
//   cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
//   cluster1_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
//   cluster1_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000); // set this high
//   cluster1_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
//   cluster1_circuit_breakers_threshold_default->set_track_remaining(true);
  
//   envoy::extensions::upstreams::http::v3::HttpProtocolOptions http_protocol_options;
//   // set http2_protocol_options max_concurrent_streams to 1
//   http_protocol_options.mutable_explicit_http_config()->mutable_http2_protocol_options()->mutable_max_concurrent_streams()->set_value(1);
//     (*cluster1_.mutable_typed_extension_protocol_options())
//     ["envoy.extensions.upstreams.http.v3.HttpProtocolOptions"]
//       .PackFrom(http_protocol_options);

//   EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));
//   sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster, {cluster1_}, {cluster1_}, {}, "56");
//   test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

//   test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);
//   test_server_->waitForGaugeGe("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx", 0);
//   test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 0);
//   test_server_->waitForGaugeGe("cluster.cluster_1.circuit_breakers.default.remaining_cx", 0);


//   codec_client_ = makeHttpConnection(lookupPort("http"));

//   auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
//     Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
//   );

//   waitForNextUpstreamRequest(FirstUpstreamIndex);

//   test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);
//   test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx", 1);
//   test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 1); // the circuit breaker is triggered
//   test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 0); //  no more connections are allowed

//   upstream_request_->encodeHeaders(default_response_headers_, true);
//   ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

//   ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());
//   EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

//   // close the upstream connection (because it will be kept around for a while if we don't)
//   // so we can check the circuit breaker returns to its initial state
//   ASSERT_TRUE(fake_upstream_connection_->close());

//   test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);
//   test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx", 1);
//   test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 0);
//   test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 1);

//   std::cout << "AFTER aggregate_cluster cx_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.cx_open")->value() << std::endl;
//   std::cout << "AFTER aggregate_cluster remaining_cx: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx")->value() << std::endl;
//   std::cout << "AFTER cluster_1 cx_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.cx_open")->value() << std::endl;
//   std::cout << "AFTER cluster_1 remaining_cx: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_cx")->value() << std::endl;

//   cleanupUpstreamAndDownstream();

//   codec_client_->close();

//   // THE PROBLEM WITH THIS ORIGINAL TEST IS WE HAVEN'T TRIED TO CREATE MORE CONNECTIONS (via both /aggregatecluster and /cluster1)
//   // SO I HAVE NOT PROVED: 
//   // - THAT NEW CONNECTIONS ARE PREVENTED FROM BEING CREATED
//   // - THE OVERFLOWS CHANGE
//   // THIS IS WHAT I AM ATTEMTPING TO PROVE IN THE NEW ONE BELOW
// }

// cx_open (Gauge) - Whether the connection circuit breaker is under its concurrency limit (0) or is at capacity and no longer admitting (1)
// remaining_cx (Gauge) - Number of remaining connections until the circuit breaker reaches its concurrency limit
// upstream_cx_overflow (Counter) - Total times that the cluster’s connection circuit breaker overflowed
TEST_P(AggregateIntegrationTest, NEWCircuitBreakerMaxConnectionsTest) {
  std::cout << "---------- 00 TEST START" << std::endl;

  // set the downstream client to use http2
  setDownstreamProtocol(Http::CodecType::HTTP2);
  
  // modify the initial config
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

    envoy::extensions::upstreams::http::v3::HttpProtocolOptions http_protocol_options;
    // set max_requests_per_connection to 1 so that each request makes a new connection
    http_protocol_options.mutable_common_http_protocol_options()->mutable_max_requests_per_connection()->set_value(1);
    // set max_concurrent_streams to 1 so that a single stream will saturate the connection
    http_protocol_options.mutable_explicit_http_config()->mutable_http2_protocol_options()->mutable_max_concurrent_streams()->set_value(1);
    (*aggregate_cluster->mutable_typed_extension_protocol_options())
      ["envoy.extensions.upstreams.http.v3.HttpProtocolOptions"]
        .PackFrom(http_protocol_options);
  });

  initialize();

  auto* cluster1_circuit_breakers = cluster1_.mutable_circuit_breakers();
  auto* cluster1_circuit_breakers_threshold_default = cluster1_circuit_breakers->add_thresholds();
  cluster1_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  cluster1_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1); // set to 1
  cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_connection_pools()->set_value(1000000000); // set this high
  cluster1_circuit_breakers_threshold_default->set_track_remaining(true);

  envoy::extensions::upstreams::http::v3::HttpProtocolOptions http_protocol_options;
  // set max_requests_per_connection to 1 so that each request makes a new connection
  http_protocol_options.mutable_common_http_protocol_options()->mutable_max_requests_per_connection()->set_value(1);
  // set max_concurrent_streams to 1 so that a single stream will saturate the connection
  http_protocol_options.mutable_explicit_http_config()->mutable_http2_protocol_options()->mutable_max_concurrent_streams()->set_value(1);
  (*cluster1_.mutable_typed_extension_protocol_options())
    ["envoy.extensions.upstreams.http.v3.HttpProtocolOptions"]
      .PackFrom(http_protocol_options);

  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster, {cluster1_}, {cluster1_}, {}, "56");
  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  // initial circuit breaker states
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx", 1);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 1);

  std::cout << "---------- ^CHECKPOINT 01 : SETUP COMPLETE -------------" << std::endl;

  codec_client_ = makeHttpConnection(lookupPort("http"));

  printStatsForMaxConnections("01->02");

  std::cout << "---------- ^CHECKPOINT 02 : MADE HTTP CONNECTION -------------" << std::endl;

  // send the first request to /aggregatecluster which should go to cluster1
  auto aggregate_cluster_response1 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  printStatsForMaxConnections("02->03");

  std::cout << "---------- ^CHECKPOINT 03 : SENT FIRST REQUEST TO /aggregatecluster -------------" << std::endl;

  // wait for the first request to arrive at cluster1
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  printStatsForMaxConnections("03->04");

  std::cout << "---------- ^CHECKPOINT 04 : FIRST REQUEST ARRIVED AT UPSTREAM -------------" << std::endl;

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0); // unaffected
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx", 1); // unaffected

  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 1); // the cluster1 circuit breaker is NOW triggered
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 0); // no more connections are allowed

  test_server_->waitForGaugeEq("cluster.cluster_1.upstream_rq_active", 1); // the first request is in an active state

  printStatsForMaxConnections("04->05");
  
  std::cout << "---------- ^CHECKPOINT 05 : MAX CONNECTIONS CIRCUIT BREAKER TRIPPED -------------" << std::endl;

  // (HACKY)
  // UPDATE cluster1 via XDS to have max_pending_requests = 0   
  // cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(0);
  // EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "56", {}, {}, {}));
  // sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster, {cluster1_}, {cluster1_}, {}, "57");
  // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_pending", 0);
  // printStatsForMaxConnections("ADJUST CLUSTER1 max pending requests CIRCUIT BREAKER VIA XDS");

  // send a second request to /aggregatecluster
  // this should create a new upstream connection because 
  // we set max_requests_per_connection to 1 so each request should make a new connection
  auto aggregate_cluster_response2 = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}
  );

  printStatsForMaxConnections("05->06");

  std::cout << "---------- ^CHECKPOINT 06 : SENT SECOND REQUEST TO /aggregatecluster (USING SAME CLIENT) -------------" << std::endl;

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0); // unaffected
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx", 1); // unaffected
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_cx_overflow", 0); // unaffected

  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 1); // the cluster1 circuit breaker is STILL triggered
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 0); // connections are STILL NOT allowed
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_cx_overflow", 1); // cluster1 overflow was incremented
  
  test_server_->waitForGaugeEq("cluster.cluster_1.upstream_rq_pending_active", 1); // the second request is in a pending state

  printStatsForMaxConnections("06->07");

  std::cout << "---------- ^CHECKPOINT 07 : OVERFLOW INCREMENTED -------------" << std::endl;

  // send a third request directly to /cluster1
  auto cluster1_response = codec_client_->makeHeaderOnlyRequest(
    Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/cluster1"},{":scheme", "http"},{":authority", "host"}}
  );

  printStatsForMaxConnections("07->08");

  std::cout << "---------- ^CHECKPOINT 08 : SENT THIRD REQUEST to /cluster1 (USING SAME CLIENT) -------------" << std::endl;

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0); // unaffected
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx", 1); // unaffected
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_cx_overflow", 0); // unaffected

  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 1); // the cluster1 circuit breaker is STILL triggered
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 0); // connections are STILL NOT allowed
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_cx_overflow", 2); // cluster1 overflow was incremented AGAIN

  test_server_->waitForGaugeEq("cluster.cluster_1.upstream_rq_pending_active", 2); // the third request is also in a pending state

  printStatsForMaxConnections("08->09");
  
  std::cout << "---------- ^CHECKPOINT 09 : OVERFLOW INCREMENTED AGAIN -------------" << std::endl;

  // IVE EDITED THIS, ORIGINALLY MADE IT SO WE PROCESSED THE FIRST REQUEST, ETC BUT THAT LEADS TO OTHER ISSUES..
  // DONT EVEN BOTHER RESPONDING TO FIRST REQUEST BECAUSE IT JUST CAUSES THE PENDING REQUESTS TO START BEING PROCESSED...

  // cluster1 responds to the first request
  // upstream_request_->encodeHeaders(default_response_headers_, true);
  // ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  // printStatsForMaxConnections("09->10");
  
  // std::cout << "---------- ^CHECKPOINT 10 : UPSTREAM RESPONDED TO FIRST REQUEST -------------" << std::endl;

  // the first request response arrives downstream
  // ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());
  // EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

  // printStatsForMaxConnections("10->11");
  
  // std::cout << "---------- ^CHECKPOINT 11 : DOWNSTREAM RECEIVED RESPONSE  -------------" << std::endl;
  
  // assert failure: result. Details: Timed out waiting for new stream.
  // waitForNextUpstreamRequest(FirstUpstreamIndex);

  // its also related to pending requests that get queued if there is no connection available yet

  // close the upstream connection
  // so we can check the circuit breaker returns to its initial state
  // ASSERT_TRUE(fake_upstream_connection_->close());
  // ASSERT_TRUE(fake_upstream_connection_->waitForDisconnect());

  // BUT WE STILL HAVE 3 REQUESTS JUST SITTING THERE PENDING IN ENVOY

  // --------------------
  // IDEA
  // this is just brute force closing the upstream connection
  // ASSERT_TRUE(fake_upstream_connection_->close());
  // printStatsForMaxConnections("AFTER CLOSING UPSTREAM CONNECTION");
  // std::cout << "---------- ^CHECKPOINT 12 : AFTER CLOSING UPSTREAM CONNECTION  -------------" << std::endl;
  // --------------------

  // --------------------
  // IDEA
  // this is just brute force closing the client connection which means the upstream connections also get closed
  codec_client_->close();
  printStatsForMaxConnections("AFTER CLOSING CLIENT CONNECTION");
  std::cout << "---------- ^CHECKPOINT 12 : AFTER CLOSING CLIENT CONNECTION  -------------" << std::endl;
  // --------------------

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0); // unaffected
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx", 1); // unaffected
  test_server_->waitForCounterEq("cluster.aggregate_cluster.upstream_cx_overflow", 0); // unaffected

  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 0); // the cluster1 circuit breaker has returned to its initial state
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 1); // connections are allowed again
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_cx_overflow", 2); // the total circuit breaker max connections overflow was 2
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_cx_total", 1); // only 1 total connection was established
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_cx_http2_total", 1); // only 1 http2 connection was established
  test_server_->waitForCounterEq("cluster.cluster_1.upstream_rq_total", 1); // only the first request reached the upstream

  std::cout << "---------- ^CHECKPOINT 13 : CLUSTER1 CIRCUIT BREAKER RESET  -------------" << std::endl;

  printStatsForMaxConnections("BEFORE CLEANUP");

  cleanupUpstreamAndDownstream();

  printStatsForMaxConnections("AFTER CLEANUP");

  std::cout << "---------- 99 TEST END" << std::endl;
}

} // namespace
} // namespace Envoy

// LOG:
// AFTER CLEANUP aggregate_cluster cx_open: 0
// AFTER CLEANUP aggregate_cluster remaining_cx: 1
// AFTER CLEANUP aggregate_cluster upstream_cx_overflow: 0
// AFTER CLEANUP aggregate_cluster rq_pending_open: 0
// AFTER CLEANUP aggregate_cluster remaining_pending: 1000000000
// AFTER CLEANUP aggregate_cluster upstream_rq_pending_overflow: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_total: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_active: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_http1_total: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_http2_total: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_http3_total: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_connect_fail: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_connect_timeout: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_connect_with_0_rtt: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_idle_timeout: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_max_duration_reached: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_connect_attempts_exceeded: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_destroy: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_destroy_local: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_destroy_remote: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_destroy_with_active_rq: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_destroy_local_with_active_rq: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_destroy_remote_with_active_rq: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_close_notify: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_rx_bytes_total: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_rx_bytes_buffered: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_tx_bytes_total: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_tx_bytes_buffered: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_pool_overflow: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_protocol_error: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_max_requests: 0
// AFTER CLEANUP aggregate_cluster upstream_cx_none_healthy: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_total: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_active: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_pending_total: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_pending_overflow: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_pending_failure_eject: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_pending_active: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_cancelled: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_maintenance_mode: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_timeout: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_max_duration_reached: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_per_try_timeout: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_rx_reset: 0
// AFTER CLEANUP aggregate_cluster upstream_rq_tx_reset: 0
// AFTER CLEANUP cluster_1 cx_open: 0
// AFTER CLEANUP cluster_1 remaining_cx: 1
// AFTER CLEANUP cluster_1 upstream_cx_overflow: 2 <-- 2 connections overflowed
// AFTER CLEANUP cluster_1 rq_pending_open: 0
// AFTER CLEANUP cluster_1 remaining_pending: 1000000000
// AFTER CLEANUP cluster_1 upstream_rq_pending_overflow: 0
// AFTER CLEANUP cluster_1 upstream_cx_total: 1
// AFTER CLEANUP cluster_1 upstream_cx_active: 0
// AFTER CLEANUP cluster_1 upstream_cx_http1_total: 0
// AFTER CLEANUP cluster_1 upstream_cx_http2_total: 1
// AFTER CLEANUP cluster_1 upstream_cx_http3_total: 0
// AFTER CLEANUP cluster_1 upstream_cx_connect_fail: 0
// AFTER CLEANUP cluster_1 upstream_cx_connect_timeout: 0
// AFTER CLEANUP cluster_1 upstream_cx_connect_with_0_rtt: 0
// AFTER CLEANUP cluster_1 upstream_cx_idle_timeout: 0
// AFTER CLEANUP cluster_1 upstream_cx_max_duration_reached: 0
// AFTER CLEANUP cluster_1 upstream_cx_connect_attempts_exceeded: 0
// AFTER CLEANUP cluster_1 upstream_cx_destroy: 1
// AFTER CLEANUP cluster_1 upstream_cx_destroy_local: 1
// AFTER CLEANUP cluster_1 upstream_cx_destroy_remote: 0
// AFTER CLEANUP cluster_1 upstream_cx_destroy_with_active_rq: 0
// AFTER CLEANUP cluster_1 upstream_cx_destroy_local_with_active_rq: 0
// AFTER CLEANUP cluster_1 upstream_cx_destroy_remote_with_active_rq: 0
// AFTER CLEANUP cluster_1 upstream_cx_close_notify: 0
// AFTER CLEANUP cluster_1 upstream_cx_rx_bytes_total: 55
// AFTER CLEANUP cluster_1 upstream_cx_rx_bytes_buffered: 0
// AFTER CLEANUP cluster_1 upstream_cx_tx_bytes_total: 201
// AFTER CLEANUP cluster_1 upstream_cx_tx_bytes_buffered: 0
// AFTER CLEANUP cluster_1 upstream_cx_pool_overflow: 0
// AFTER CLEANUP cluster_1 upstream_cx_protocol_error: 0
// AFTER CLEANUP cluster_1 upstream_cx_max_requests: 1
// AFTER CLEANUP cluster_1 upstream_cx_none_healthy: 0
// AFTER CLEANUP cluster_1 upstream_rq_total: 1 <-- 1 request reached the upstream in total
// AFTER CLEANUP cluster_1 upstream_rq_active: 0
// AFTER CLEANUP cluster_1 upstream_rq_pending_total: 3
// AFTER CLEANUP cluster_1 upstream_rq_pending_overflow: 0
// AFTER CLEANUP cluster_1 upstream_rq_pending_failure_eject: 0
// AFTER CLEANUP cluster_1 upstream_rq_pending_active: 0
// AFTER CLEANUP cluster_1 upstream_rq_cancelled: 2
// AFTER CLEANUP cluster_1 upstream_rq_maintenance_mode: 0
// AFTER CLEANUP cluster_1 upstream_rq_timeout: 0
// AFTER CLEANUP cluster_1 upstream_rq_max_duration_reached: 0
// AFTER CLEANUP cluster_1 upstream_rq_per_try_timeout: 0
// AFTER CLEANUP cluster_1 upstream_rq_rx_reset: 0
// AFTER CLEANUP cluster_1 upstream_rq_tx_reset: 1
