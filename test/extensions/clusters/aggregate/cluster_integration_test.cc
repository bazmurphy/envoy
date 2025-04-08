#include "envoy/config/cluster/v3/cluster.pb.h"
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
};

INSTANTIATE_TEST_SUITE_P(
    IpVersions, AggregateIntegrationTest,
    testing::Combine(testing::ValuesIn(TestEnvironment::getIpVersionsForTest()), testing::Bool()));

TEST_P(AggregateIntegrationTest, CircuitBreakingChildLimitLowerThanAggregateMaxConn) {
  // 1. Add circuit breaker config for aggregate cluster - done
  // 2. Add circuit breaker config for cluster1_ and/or cluster2_ clusters - done
  // 3. Check the circuit breaker stats for the aggregate cluster - done
  // 4. Check the circuit breaker stats for the cluster1_ and cluster2_ - done
  // 5. Send a request - done

  // Add circuit breaker config to aggregate cluster
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* cluster = static_resources->mutable_clusters(1);
    auto* threshold = cluster->mutable_circuit_breakers()->mutable_thresholds()->Add();
    threshold->set_track_remaining(true);
    threshold->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    threshold->mutable_max_connections()->set_value(10);
  });

  initialize();

  // Create an updated version of cluster1_ with circuit breakers
  auto* threshold_cluster1 = cluster1_.mutable_circuit_breakers()->mutable_thresholds()->Add();
  threshold_cluster1->set_track_remaining(true);
  threshold_cluster1->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  threshold_cluster1->mutable_max_connections()->set_value(1);

  // Send the updated cluster via CDS
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "413");

  // before creating a connection - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 1);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 0);

  // before creating a connection - aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx",
                               10);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);

  testRouterHeaderOnlyRequestAndResponse(nullptr, FirstUpstreamIndex, "/aggregatecluster");

  // after creating a connection - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 1);

  // after creating a connection - aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx",
                               10);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);

  cleanupUpstreamAndDownstream();
}

TEST_P(AggregateIntegrationTest, CircuitBreakingAggregateLimitLowerThanChildMaxConn) {
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* cluster = static_resources->mutable_clusters(1);
    auto* threshold = cluster->mutable_circuit_breakers()->mutable_thresholds()->Add();
    threshold->set_track_remaining(true);
    threshold->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    threshold->mutable_max_connections()->set_value(1);
  });

  initialize();

  // Create an updated version of cluster1_ with circuit breakers
  auto* threshold_cluster1 = cluster1_.mutable_circuit_breakers()->mutable_thresholds()->Add();
  threshold_cluster1->set_track_remaining(true);
  threshold_cluster1->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  threshold_cluster1->mutable_max_connections()->set_value(10);

  // Send the updated cluster via CDS
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "413");

  // before creating a connection - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 10);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 0);

  // before creating a connection - aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx",
                               1);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);

  testRouterHeaderOnlyRequestAndResponse(nullptr, FirstUpstreamIndex, "/aggregatecluster");

  // after creating a connection - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_cx", 9);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 0);

  // after creating a connection - aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_cx",
                               1);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);

  cleanupUpstreamAndDownstream();
}

TEST_P(AggregateIntegrationTest, CircuitBreakingChildLimitLowerThanAggregateMaxRequets) {

  // Add circuit breaker config to aggregate cluster
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* cluster = static_resources->mutable_clusters(1);
    auto* threshold = cluster->mutable_circuit_breakers()->mutable_thresholds()->Add();
    threshold->set_track_remaining(true);
    threshold->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    threshold->mutable_max_requests()->set_value(10);
  });

  initialize();

  // Create an updated version of cluster1_ with circuit breakers
  auto* threshold_cluster1 = cluster1_.mutable_circuit_breakers()->mutable_thresholds()->Add();
  threshold_cluster1->set_track_remaining(true);
  threshold_cluster1->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  threshold_cluster1->mutable_max_requests()->set_value(1);

  // Send the updated cluster via CDS
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "413");

  // before sending a request - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_rq", 1);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_open", 0);

  // before sending a request - aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq",
                               10);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_open", 0);

  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto response = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // after sending a request - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_rq", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_open", 1);

  // after sending a request - aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq",
                               10);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_open", 0);

  cleanupUpstreamAndDownstream();
}

TEST_P(AggregateIntegrationTest, CircuitBreakingAggregateLimitLowerThanChildMaxRequests) {

  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* cluster = static_resources->mutable_clusters(1);
    auto* threshold = cluster->mutable_circuit_breakers()->mutable_thresholds()->Add();
    threshold->set_track_remaining(true);
    threshold->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    threshold->mutable_max_requests()->set_value(1);
  });

  initialize();

  // Create an updated version of cluster1_ with circuit breakers
  auto* threshold_cluster1 = cluster1_.mutable_circuit_breakers()->mutable_thresholds()->Add();
  threshold_cluster1->set_track_remaining(true);
  threshold_cluster1->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  threshold_cluster1->mutable_max_requests()->set_value(10);

  // Send the updated cluster via CDS
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "413");

  // before sending a request - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_rq", 10);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_open", 0);

  // before sending a request - aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq",
                               1);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_open", 0);

  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto response = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // after sending a request - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_rq", 9);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_open", 0);

  // after sending a request - aggregate_cluster
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq",
                               1);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_open", 0);

  cleanupUpstreamAndDownstream();
}

TEST_P(AggregateIntegrationTest, CircuitBreakingChildLimitLowerThanAggregateMaxPendingRequets) {

  // Add circuit breaker config to aggregate cluster
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* cluster = static_resources->mutable_clusters(1);
    auto* threshold = cluster->mutable_circuit_breakers()->mutable_thresholds()->Add();
    threshold->set_track_remaining(true);
    threshold->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    threshold->mutable_max_pending_requests()->set_value(10);
  });

  initialize();

  // Create an updated version of cluster1_ with circuit breakers
  auto* threshold_cluster1 = cluster1_.mutable_circuit_breakers()->mutable_thresholds()->Add();
  threshold_cluster1->set_track_remaining(true);
  threshold_cluster1->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  threshold_cluster1->mutable_max_pending_requests()->set_value(1);

  // Use an unreachable upstream address (different from the one set for the fakeUpstreams[2]
  // "127.0.0.1" ) to make the request pending
  cluster1_.mutable_load_assignment()
      ->mutable_endpoints(0)
      ->mutable_lb_endpoints(0)
      ->mutable_endpoint()
      ->mutable_address()
      ->mutable_socket_address()
      ->set_address("1.1.1.1");

  // Send the updated cluster via CDS
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "413");

  // before sending a request - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_pending", 1);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 0);

  // before sending a request - aggregate_cluster
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_pending", 10);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open",
                               0);

  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto response = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  // after sending a request - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_pending", 0);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 1);

  // after sending a request - aggregate_cluster
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_pending", 10);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open",
                               0);

  cleanupUpstreamAndDownstream();
}

TEST_P(AggregateIntegrationTest, CircuitBreakingAggregateLimitLowerThanChildMaxPendingRequests) {

  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* cluster = static_resources->mutable_clusters(1);
    auto* threshold = cluster->mutable_circuit_breakers()->mutable_thresholds()->Add();
    threshold->set_track_remaining(true);
    threshold->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    threshold->mutable_max_pending_requests()->set_value(1);
  });

  initialize();

  // Create an updated version of cluster1_ with circuit breakers
  auto* threshold_cluster1 = cluster1_.mutable_circuit_breakers()->mutable_thresholds()->Add();
  threshold_cluster1->set_track_remaining(true);
  threshold_cluster1->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  threshold_cluster1->mutable_max_pending_requests()->set_value(10);

  // Use an unreachable upstream address (different from the one set for the fakeUpstreams[2]
  // "127.0.0.1" ) to make the request pending
  cluster1_.mutable_load_assignment()
      ->mutable_endpoints(0)
      ->mutable_lb_endpoints(0)
      ->mutable_endpoint()
      ->mutable_address()
      ->mutable_socket_address()
      ->set_address("1.1.1.1");

  // Send the updated cluster via CDS
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "413");

  // before sending a request - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_pending", 10);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 0);

  // before sending a request - aggregate_cluster
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_pending", 1);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open",
                               0);

  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto response = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                     {":path", "/aggregatecluster"},
                                     {":scheme", "http"},
                                     {":authority", "host"}});

  // after sending a request - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_pending", 9);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 0);

  // after sending a request - aggregate_cluster
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_pending", 1);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open",
                               0);

  cleanupUpstreamAndDownstream();
}

TEST_P(AggregateIntegrationTest, CircuitBreakingChildLimitLowerThanAggregateMaxRetries) {
  config_helper_.addConfigModifier(
      [](envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager&
             hcm) {
        auto* virtual_host = hcm.mutable_route_config()->mutable_virtual_hosts(0);

        // Find the route to aggregate_cluster
        for (int i = 0; i < virtual_host->routes_size(); i++) {
          auto* route = virtual_host->mutable_routes(i);
          if (route->match().prefix() == "/aggregatecluster") {
            auto* retry_policy = route->mutable_route()->mutable_retry_policy();
            // Add the missing retry policy components
            retry_policy->set_retry_on("5xx");
            retry_policy->mutable_per_try_timeout()->set_seconds(1);
            break;
          }
        }
      });

  // Add circuit breaker config to aggregate cluster
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* cluster = static_resources->mutable_clusters(1);
    auto* threshold = cluster->mutable_circuit_breakers()->mutable_thresholds()->Add();
    threshold->set_track_remaining(true);
    threshold->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    threshold->mutable_max_retries()->set_value(2);
  });

  initialize();

  // Create an updated version of cluster1_ with circuit breakers
  auto* threshold_cluster1 = cluster1_.mutable_circuit_breakers()->mutable_thresholds()->Add();
  threshold_cluster1->set_track_remaining(true);
  threshold_cluster1->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  threshold_cluster1->mutable_max_retries()->set_value(1);

  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "413");

  // before creating a connection - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 1);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);

  // before creating a connection - aggregate_cluster
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 2);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open",
                               0);
  // Create multiple HTTP/1.1 connections
  std::vector<IntegrationCodecClientPtr> clients;
  std::vector<IntegrationStreamDecoderPtr> responses;

  // Create 2 connections
  for (int i = 0; i < 2; i++) {
    clients.push_back(makeHttpConnection(lookupPort("http")));
  }

  // Send requests concurrently
  for (int i = 0; i < 2; i++) {
    auto& client = clients[i];
    auto response =
        client->makeRequestWithBody(Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                                                   {":path", "/aggregatecluster"},
                                                                   {":scheme", "http"},
                                                                   {":authority", "host"}},
                                    1024);
    responses.push_back(std::move(response));
  };

  std::vector<FakeHttpConnectionPtr> upstream_connections;
  std::vector<FakeStreamPtr> upstream_requests;

  for (int i = 0; i < 2; i++) {
    // Use waitForNextUpstreamRequest to handle the connection and request
    waitForNextUpstreamRequest(FirstUpstreamIndex);

    // Store the connection and request (move them to avoid having them overwritten)
    upstream_connections.push_back(std::move(fake_upstream_connection_));
    upstream_requests.push_back(std::move(upstream_request_));

    // Send 503 response to trigger retry
    upstream_requests[i]->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

    // Close the connection to ensure a new one is created for the retry
    ASSERT_TRUE(upstream_connections[i]->close());
    ASSERT_TRUE(upstream_connections[i]->waitForDisconnect());
  }

  // after creating a connection - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 1);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);

  // after creating a connection - aggregate_cluster
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 1);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open",
                               0);
  // Clean up
  for (auto& client : clients) {
    client->close();
  }

  cleanupUpstreamAndDownstream();
}

TEST_P(AggregateIntegrationTest, CircuitBreakingAggregateLimitLowerThanChildMaxRetries) {
  config_helper_.addConfigModifier(
      [](envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager&
             hcm) {
        auto* virtual_host = hcm.mutable_route_config()->mutable_virtual_hosts(0);

        // Find the route to aggregate_cluster
        for (int i = 0; i < virtual_host->routes_size(); i++) {
          auto* route = virtual_host->mutable_routes(i);
          if (route->match().prefix() == "/aggregatecluster") {
            auto* retry_policy = route->mutable_route()->mutable_retry_policy();
            // Add the missing retry policy components
            retry_policy->set_retry_on("5xx");
            retry_policy->mutable_per_try_timeout()->set_seconds(1);
            break;
          }
        }
      });

  // Add circuit breaker config to aggregate cluster
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    auto* static_resources = bootstrap.mutable_static_resources();
    auto* cluster = static_resources->mutable_clusters(1);
    auto* threshold = cluster->mutable_circuit_breakers()->mutable_thresholds()->Add();
    threshold->set_track_remaining(true);
    threshold->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    threshold->mutable_max_retries()->set_value(2);
  });

  initialize();

  // Create an updated version of cluster1_ with circuit breakers
  auto* threshold_cluster1 = cluster1_.mutable_circuit_breakers()->mutable_thresholds()->Add();
  threshold_cluster1->set_track_remaining(true);
  threshold_cluster1->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  threshold_cluster1->mutable_max_connections()->set_value(3);

  // Send the updated cluster via CDS
  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                             {cluster1_}, {cluster1_}, {}, "413");

  // before creating a connection - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 3);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);

  // before creating a connection - aggregate_cluster
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 2);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open",
                               0);

  // Create multiple HTTP/1.1 connections
  std::vector<IntegrationCodecClientPtr> clients;
  std::vector<IntegrationStreamDecoderPtr> responses;

  // Create 2 connections
  for (int i = 0; i < 2; i++) {
    clients.push_back(makeHttpConnection(lookupPort("http")));
  }

  // Send requests concurrently
  for (int i = 0; i < 2; i++) {
    auto& client = clients[i];
    auto response =
        client->makeRequestWithBody(Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                                                   {":path", "/aggregatecluster"},
                                                                   {":scheme", "http"},
                                                                   {":authority", "host"}},
                                    1024);
    responses.push_back(std::move(response));
  };

  std::vector<FakeHttpConnectionPtr> upstream_connections;
  std::vector<FakeStreamPtr> upstream_requests;

  for (int i = 0; i < 2; i++) {
    // Use waitForNextUpstreamRequest to handle the connection and request
    waitForNextUpstreamRequest(FirstUpstreamIndex);

    // Store the connection and request (move them to avoid having them overwritten)
    upstream_connections.push_back(std::move(fake_upstream_connection_));
    upstream_requests.push_back(std::move(upstream_request_));

    // Send 503 response to trigger retry
    upstream_requests[i]->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "503"}}, true);

    // Close the connection to ensure a new one is created for the retry
    ASSERT_TRUE(upstream_connections[i]->close());
    ASSERT_TRUE(upstream_connections[i]->waitForDisconnect());
  }

  // after creating a connection - cluster_1
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.remaining_retries", 3);
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_retry_open", 0);

  // after creating a connection - aggregate_cluster
  test_server_->waitForGaugeEq(
      "cluster.aggregate_cluster.circuit_breakers.default.remaining_retries", 1);
  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open",
                               0);
  // Clean up
  for (auto& client : clients) {
    client->close();
  }

  cleanupUpstreamAndDownstream();
}

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

} // namespace
} // namespace Envoy
