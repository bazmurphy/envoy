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

// TEST PLAN

// 1. setup the two circuit breakers
//    - one at the aggregate_cluster level
//    - one at cluster1 level
// and then we can confirm that the aggregate_cluster circuit breaker does not get used [this WILL get used for "retries" because i believe that mechanic lives there]

// 2. send one request through to /aggregate_cluster (and prevent it from completing)

// 3. check the circuit breaker states
//    - aggregate_cluster should be 0 (default or normal state == "closed")
//    - cluster1 should be 1 (which means triggered == "open")

// 4. send another request through (and prevent it from completing) 
//    - this request should fail

// 5. send another request but this time directly to the /cluster1 route
//    - this request should also fail (because the cluster1 circuit breaker is 1 (open))


// STATS NAMES :

// cx_open  - connections circuit breaker
// cx_pool_open - connection pool circuit breaker
// rq_open - request circuit breaker
// rq_pending_open - pending request circuit breaker
// rq_retry_open - request retry circuit breaker

// cluster.aggregate_cluster.circuit_breakers.default.cx_open
// cluster.aggregate_cluster.circuit_breakers.default.cx_pool_open
// cluster.aggregate_cluster.circuit_breakers.default.rq_open
// cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open
// cluster.aggregate_cluster.circuit_breakers.default.rq_retry_open

// remaining_cx - remaining connections
// remaining_cx_pools - remaining connection pools
// remaing_pending - remaining pending requests
// remaining_retries - remaining (request) retries
// remaing_req - remaining requests

// cluster.aggregate_cluster.circuit_breakers.default.remaining_cx
// cluster.aggregate_cluster.circuit_breakers.default.remaining_cx_pools
// cluster.aggregate_cluster.circuit_breakers.default.remaining_pending
// cluster.aggregate_cluster.circuit_breakers.default.remaining_retries
// cluster.aggregate_cluster.circuit_breakers.default.remaining_rq

TEST_P(AggregateIntegrationTest, CircuitBreakerTest) {
  std::cout << "---------- 00 TEST START" << std::endl;

  // this is how we can modify the config (from the top of this file) before calling "initialize()"
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    std::cout << "---------- 01 CONFIG MODIFY START" << std::endl;

    // we want to access the "static_resources" to modify the "aggregate_cluster"

    // "aggregate_cluster" is in "static_resources" > "clusters"
    auto* static_resources = bootstrap.mutable_static_resources();

    // "aggregate_cluster" is at index 1 of the "static_resources" > "clusters"
    auto* aggregate_cluster = static_resources->mutable_clusters(1);
    std::cout << "aggregate_cluster name(): " << aggregate_cluster->name() << std::endl;

    // ---------
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

    // ---------
  
    // now we want to set the circuit breaker configuration on the aggregate cluster

    std::cout << "BEFORE aggregate_cluster has_circuit_breakers(): " << aggregate_cluster->has_circuit_breakers() << std::endl;

    auto* aggregate_cluster_circuit_breakers = aggregate_cluster->mutable_circuit_breakers();

    std::cout << "BEFORE aggregate_cluster thresholds_size(): " << aggregate_cluster_circuit_breakers->thresholds_size() << std::endl;

    // set the aggregate_cluster circuit breakers
    auto* aggregate_cluster_circuit_breakers_threshold_default = aggregate_cluster_circuit_breakers->add_thresholds();
    aggregate_cluster_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    // aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1);
    // aggregate_cluster_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1);
    // and we want to set "- track_remaining: true"
    aggregate_cluster_circuit_breakers_threshold_default->set_track_remaining(true);

    std::cout << "AFTER aggregate_cluster thresholds_size(): " << aggregate_cluster_circuit_breakers->thresholds_size() << std::endl;

    // auto* aggregate_cluster_circuit_breakers_threshold_high = aggregate_cluster_circuit_breakers->add_thresholds();
    // aggregate_cluster_circuit_breakers_threshold_high->set_priority(envoy::config::core::v3::RoutingPriority::HIGH);
    // aggregate_cluster_circuit_breakers_threshold_high->mutable_max_connections()->set_value(1);
    // aggregate_cluster_circuit_breakers_threshold_high->mutable_max_pending_requests()->set_value(1);
    // aggregate_cluster_circuit_breakers_threshold_high->mutable_max_requests()->set_value(1);
    // aggregate_cluster_circuit_breakers_threshold_high->mutable_max_retries()->set_value(1);

    std::cout << "AFTER aggregate_cluster has_circuit_breakers(): " << aggregate_cluster->has_circuit_breakers() << std::endl;
    std::cout << "AFTER aggregate_cluster thresholds_size(): " << aggregate_cluster_circuit_breakers->thresholds_size() << std::endl;
    std::cout << "AFTER aggregate_cluster thresholds_size(): " << aggregate_cluster_circuit_breakers->thresholds_size() << std::endl;

    std::cout << "aggregate_cluster threshold default max_connections().value(): " << aggregate_cluster_circuit_breakers_threshold_default->max_connections().value() << std::endl;
    std::cout << "aggregate_cluster threshold default max_pending_requests().value(): " << aggregate_cluster_circuit_breakers_threshold_default->max_pending_requests().value() << std::endl;
    std::cout << "aggregate_cluster threshold default max_requests().value(): " << aggregate_cluster_circuit_breakers_threshold_default->max_requests().value() << std::endl;
    std::cout << "aggregate_cluster threshold default max_retries().value(): " << aggregate_cluster_circuit_breakers_threshold_default->max_retries().value() << std::endl;
    // check the "track_remaining" value
    std::cout << "aggregate_cluster threshold default track_remaining(): " << aggregate_cluster_circuit_breakers_threshold_default->track_remaining() << std::endl;

    // ---------
    std::cout << "---------- 02 CONFIG MODIFY FINISH" << std::endl;
  });

  std::cout << "---------- 03 INITIALIZE START" << std::endl;

  // ---------

  // now call initialize (and that will add cluster_1 to the "dynamic_resources" > "clusters")

  initialize();

  // ---------

  std::cout << "---------- 04 INITIALIZE FINISH" << std::endl;

  std::cout << "cluster1_ name(): " << cluster1_.name() << std::endl;

  std::cout << "BEFORE cluster1_ has_circuit_breakers(): " << cluster1_.has_circuit_breakers() << std::endl;

  auto* cluster1_circuit_breakers = cluster1_.mutable_circuit_breakers();

  std::cout << "BEFORE cluster1_circuit_breakers thresholds_size(): " << cluster1_circuit_breakers->thresholds_size() << std::endl;

  auto* cluster1_circuit_breakers_threshold_default = cluster1_circuit_breakers->add_thresholds();
  cluster1_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
  // cluster1_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1);
  cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1);
  cluster1_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1);
  // cluster1_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1);
  // and we want to set "- track_remaining: true"
  cluster1_circuit_breakers_threshold_default->set_track_remaining(true);

  // auto* cluster1_circuit_breakers_threshold_high = cluster1_circuit_breakers->add_thresholds();
  // cluster1_circuit_breakers_threshold_high->set_priority(envoy::config::core::v3::RoutingPriority::HIGH);
  // cluster1_circuit_breakers_threshold_high->mutable_max_connections()->set_value(1);
  // cluster1_circuit_breakers_threshold_high->mutable_max_pending_requests()->set_value(1);
  // cluster1_circuit_breakers_threshold_high->mutable_max_requests()->set_value(1);
  // cluster1_circuit_breakers_threshold_high->mutable_max_retries()->set_value(1);

  std::cout << "AFTER cluster1_ has_circuit_breakers(): " << cluster1_.has_circuit_breakers() << std::endl;
  std::cout << "AFTER cluster1_circuit_breakers thresholds_size(): " << cluster1_circuit_breakers->thresholds_size() << std::endl;

  std::cout << "cluster1_ threshold default max_connections().value(): " << cluster1_circuit_breakers_threshold_default->max_connections().value() << std::endl;
  std::cout << "cluster1_ threshold default max_pending_requests().value(): " << cluster1_circuit_breakers_threshold_default->max_pending_requests().value() << std::endl;
  std::cout << "cluster1_ threshold default max_requests().value(): " << cluster1_circuit_breakers_threshold_default->max_requests().value() << std::endl;
  std::cout << "cluster1_ threshold default max_retries().value(): " << cluster1_circuit_breakers_threshold_default->max_retries().value() << std::endl;

  // check the "track_remaining" value
  std::cout << "cluster1_ threshold default track_remaining(): " << cluster1_circuit_breakers_threshold_default->track_remaining() << std::endl;

  // std::cout << "cluster1_ threshold high max_connections().value(): " << cluster1_circuit_breakers_threshold_high->max_connections().value() << std::endl;
  // std::cout << "cluster1_ threshold high max_pending_requests().value(): " << cluster1_circuit_breakers_threshold_high->max_pending_requests().value() << std::endl;
  // std::cout << "cluster1_ threshold high max_requests().value(): " << cluster1_circuit_breakers_threshold_high->max_requests().value() << std::endl;
  // std::cout << "cluster1_ threshold high max_retries().value(): " << cluster1_circuit_breakers_threshold_high->max_retries().value() << std::endl;

  // ----------

  // check the initial circuit breaker stats:
 
  // aggregate_cluster max_requests
  std::cout << "BEFORE request/response1 aggregate_cluster rq_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_open")->value() << std::endl;
  std::cout << "BEFORE request/response1 aggregate_cluster remaining_rq: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq")->value() << std::endl;
  // aggregate_cluster max_pending_requests
  std::cout << "BEFORE request/response1 aggregate_cluster rq_pending_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  std::cout << "BEFORE request/response1 aggregate_cluster remaining_pending: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending")->value() << std::endl;
  // cluster_1 max_requests
  std::cout << "BEFORE request/response1 cluster_1 rq_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_open")->value() << std::endl;
  // std::cout << "BEFORE request/response1 cluster_1 remaining_rq: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_rq")->value() << std::endl;
  // cluster_1 max_pending_requests
  std::cout << "BEFORE request/response1 cluster_1 rq_pending_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  // std::cout << "BEFORE request/response1 cluster_1 remaining_pending: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_pending")->value() << std::endl;

  // aggregate_cluster max_requests
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq")->value(), 1);
  // aggregate_cluster max_pending_requests
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending")->value(), 1);
  // cluster_1 max_requests
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_open")->value(), 0);
  // EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_rq")->value(), 1);
  // cluster_1 max_pending_requests
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value(), 0);
  // EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_pending")->value(), 1);

  // test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_open", 0);
  // test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open", 0);
  // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_open", 0);
  // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 0);

  // ----------

  // now we want to make the requests to check the circuit breakers behaviour...

  Envoy::IntegrationCodecClientPtr codec_client_1 = makeHttpConnection(lookupPort("http"));

  // send the first request (this should go via "aggregate_cluster" through to "cluster_1")
  auto aggregate_cluster_response1 = codec_client_1->makeRequestWithBody(
    Http::TestRequestHeaderMapImpl{
      {":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}, 
      1024
    );
  
  // i think this waits for the request to reach the upstream
  waitForNextUpstreamRequest(FirstUpstreamIndex);

  // aggregate_cluster
  std::cout << "DURING request/response1 aggregate_cluster rq_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_open")->value() << std::endl;
  std::cout << "DURING request/response1 aggregate_cluster remaining_rq: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_rq")->value() << std::endl;
  std::cout << "DURING request/response1 aggregate_cluster rq_pending_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  std::cout << "DURING request/response1 aggregate_cluster remaining_pending: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending")->value() << std::endl;

  // cluster_1
  std::cout << "DURING request/response1 cluster_1 rq_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_open")->value() << std::endl;
  // std::cout << "DURING request/response1 cluster_1 remaining_rq: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_rq")->value() << std::endl;
  std::cout << "DURING request/response1 cluster_1 rq_pending_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  // std::cout << "DURING request/response1 cluster_1 remaining_pending: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_pending")->value() << std::endl;

  // test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open", 0);
  // test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 1);

  // check the circuit breaker stats again:
  // aggregate_cluster circuit breaker should still be closed (0)
  // EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value(), 0);
  // cluster_1 circuit breaker should NOW be open (1)
  // EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value(), 1);

  // std::cout << "AFTER request/response1 aggregate_cluster rq_pending_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  // std::cout << "AFTER request/response1 aggregate_cluster remaining_pending: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.remaining_pending")->value() << std::endl;
  // std::cout << "AFTER request/response1 cluster_1 rq_pending_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  // std::cout << "AFTER request/response1 cluster_1 remaining_pending: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.remaining_pending")->value() << std::endl;

  Envoy::IntegrationCodecClientPtr codec_client_2 = makeHttpConnection(lookupPort("http"));

  // send the second request (this should also go via "aggregate_cluster" through to "cluster_1")
  auto aggregate_cluster_response2 = codec_client_2->makeRequestWithBody(
    Http::TestRequestHeaderMapImpl{
      {":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}, 
      1024
    );

  std::cout << "---------- 97 DID WE GET HERE?" << std::endl;

  // check the circuit breaker stats again:
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value(), 0);

  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_open")->value(), 0);
  EXPECT_EQ(test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value(), 0);

  std::cout << "---------- 98 DID WE GET HERE?" << std::endl;

  // the second response should fail (fast?) with 503
  // ASSERT_TRUE(aggregate_cluster_response2->waitForEndStream());
  // EXPECT_EQ("503", aggregate_cluster_response2->headers().getStatusValue());

  // std::cout << "AFTER request/response2 aggregate_cluster rq_pending_open: " << test_server_->gauge("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open")->value() << std::endl;
  // std::cout << "AFTER request/response2 cluster_1 rq_pending_open: " << test_server_->gauge("cluster.cluster_1.circuit_breakers.default.rq_pending_open")->value() << std::endl;

  // completes the first request/response
  // upstream_request_->encodeHeaders(default_response_headers_, true);

  // wait for first response to complete
  // ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());
  // the first response should succeed with 200
  // EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

  // "test requires explicit cleanupUpstreamAndDownstream"
  cleanupUpstreamAndDownstream();

  std::cout << "---------- 99 TEST END" << std::endl;
}

} // namespace
} // namespace Envoy


// LOG OUTPUT:

// ---------- 00 TEST START
// ---------- 03 INITIALIZE START
// ---------- 01 CONFIG MODIFY START
// aggregate_cluster name(): aggregate_cluster
// BEFORE temp_aggregate_cluster_typed_config.clusters_size(): 2
// BEFORE temp_aggregate_cluster_typed_config clusters[0]: cluster_1
// BEFORE temp_aggregate_cluster_typed_config clusters[1]: cluster_2
// AFTER temp_aggregate_cluster_typed_config.clusters_size(): 1
// AFTER temp_aggregate_cluster_typed_config clusters[0]: cluster_1
// BEFORE aggregate_cluster has_circuit_breakers(): 0
// BEFORE aggregate_cluster thresholds_size(): 0
// AFTER aggregate_cluster thresholds_size(): 1
// AFTER aggregate_cluster has_circuit_breakers(): 1
// AFTER aggregate_cluster thresholds_size(): 1
// AFTER aggregate_cluster thresholds_size(): 1
// aggregate_cluster threshold default max_connections().value(): 0
// aggregate_cluster threshold default max_pending_requests().value(): 1
// aggregate_cluster threshold default max_requests().value(): 1
// aggregate_cluster threshold default max_retries().value(): 0
// aggregate_cluster threshold default track_remaining(): 1
// ---------- 02 CONFIG MODIFY FINISH
// ---------- 04 INITIALIZE FINISH
// cluster1_ name(): cluster_1
// BEFORE cluster1_ has_circuit_breakers(): 0
// BEFORE cluster1_circuit_breakers thresholds_size(): 0
// AFTER cluster1_ has_circuit_breakers(): 1
// AFTER cluster1_circuit_breakers thresholds_size(): 1
// cluster1_ threshold default max_connections().value(): 0
// cluster1_ threshold default max_pending_requests().value(): 1
// cluster1_ threshold default max_requests().value(): 1
// cluster1_ threshold default max_retries().value(): 0
// cluster1_ threshold default track_remaining(): 1
// BEFORE request/response1 aggregate_cluster rq_open: 0
// BEFORE request/response1 aggregate_cluster remaining_rq: 1
// BEFORE request/response1 aggregate_cluster rq_pending_open: 0
// BEFORE request/response1 aggregate_cluster remaining_pending: 1
// BEFORE request/response1 cluster_1 rq_open: 0
// BEFORE request/response1 cluster_1 rq_pending_open: 0
// ---------- 97 DID WE GET HERE?
// ---------- 98 DID WE GET HERE?
// ---------- 99 TEST END
// [2025-04-01 14:19:15.090][12][critical][assert] [source/common/network/connection_impl.cc:122] assert failure: !socket_->isOpen() && delayed_close_timer_ == nullptr. Details: ConnectionImpl was unexpectedly torn down without being closed.
// [2025-04-01 14:19:15.090][12][error][envoy_bug] [./source/common/common/assert.h:38] stacktrace for envoy bug
// [2025-04-01 14:19:15.108][12][error][envoy_bug] [./source/common/common/assert.h:43] #0 Envoy::Network::ConnectionImpl::~ConnectionImpl() [0x604bc1f]
// [2025-04-01 14:19:15.125][12][error][envoy_bug] [./source/common/common/assert.h:43] #1 Envoy::Network::ClientConnectionImpl::~ClientConnectionImpl() [0x606bca8]
// [2025-04-01 14:19:15.143][12][error][envoy_bug] [./source/common/common/assert.h:43] #2 Envoy::Network::ClientConnectionImpl::~ClientConnectionImpl() [0x606aca0]
// [2025-04-01 14:19:15.160][12][error][envoy_bug] [./source/common/common/assert.h:43] #3 Envoy::Network::ClientConnectionImpl::~ClientConnectionImpl() [0x606acf9]
// [2025-04-01 14:19:15.177][12][error][envoy_bug] [./source/common/common/assert.h:43] #4 std::default_delete<>::operator()() [0x30aa8fc]
// [2025-04-01 14:19:15.194][12][error][envoy_bug] [./source/common/common/assert.h:43] #5 std::unique_ptr<>::~unique_ptr() [0x308864a]
// [2025-04-01 14:19:15.211][12][error][envoy_bug] [./source/common/common/assert.h:43] #6 Envoy::Http::CodecClient::~CodecClient() [0x308eb62]
// [2025-04-01 14:19:15.228][12][error][envoy_bug] [./source/common/common/assert.h:43] #7 Envoy::Http::CodecClientProd::~CodecClientProd() [0x3088035]
// [2025-04-01 14:19:15.245][12][error][envoy_bug] [./source/common/common/assert.h:43] #8 Envoy::IntegrationCodecClient::~IntegrationCodecClient() [0x308e749]
// [2025-04-01 14:19:15.262][12][error][envoy_bug] [./source/common/common/assert.h:43] #9 Envoy::IntegrationCodecClient::~IntegrationCodecClient() [0x308e769]
// [2025-04-01 14:19:15.278][12][error][envoy_bug] [./source/common/common/assert.h:43] #10 std::default_delete<>::operator()() [0x2bab31c]
// [2025-04-01 14:19:15.296][12][error][envoy_bug] [./source/common/common/assert.h:43] #11 std::unique_ptr<>::~unique_ptr() [0x2baa8dd]
// [2025-04-01 14:19:15.313][12][error][envoy_bug] [./source/common/common/assert.h:43] #12 Envoy::(anonymous namespace)::AggregateIntegrationTest_CircuitBreakerTest_Test::TestBody() [0x2b5800b]
// [2025-04-01 14:19:15.330][12][error][envoy_bug] [./source/common/common/assert.h:43] #13 testing::internal::HandleSehExceptionsInMethodIfSupported<>() [0x742084b]
// [2025-04-01 14:19:15.347][12][error][envoy_bug] [./source/common/common/assert.h:43] #14 testing::internal::HandleExceptionsInMethodIfSupported<>() [0x74103fd]
// [2025-04-01 14:19:15.364][12][error][envoy_bug] [./source/common/common/assert.h:43] #15 testing::Test::Run() [0x73f8c73]
// [2025-04-01 14:19:15.365][12][critical][backtrace] [./source/server/backtrace.h:129] Caught Aborted, suspect faulting address 0x103d0000000c
// [2025-04-01 14:19:15.365][12][critical][backtrace] [./source/server/backtrace.h:113] Backtrace (use tools/stack_decode.py to get line numbers):
// [2025-04-01 14:19:15.365][12][critical][backtrace] [./source/server/backtrace.h:114] Envoy version: 0/1.34.0-dev/test/DEBUG/BoringSSL
// [2025-04-01 14:19:15.377][12][critical][backtrace] [./source/server/backtrace.h:121] #0: Envoy::SignalAction::sigHandler() [0x646958c]
// [2025-04-01 14:19:15.378][12][critical][backtrace] [./source/server/backtrace.h:123] #1: [0x71ecc3242520]
// ================================================================================
// INFO: Found 1 test target...
// Target //test/extensions/clusters/aggregate:cluster_integration_test up-to-date:
//   bazel-bin/test/extensions/clusters/aggregate/cluster_integration_test
// INFO: Elapsed time: 66.120s, Critical Path: 65.77s
// INFO: 4 processes: 4 linux-sandbox.
// INFO: Build completed, 1 test FAILED, 4 total actions
// //test/extensions/clusters/aggregate:cluster_integration_test            FAILED in 6.2s
//   /home/baz.murphy/.cache/bazel/_bazel_baz.murphy/7fcf1edc087d667522e3815b5db406ee/execroot/envoy/bazel-out/k8-fastbuild/testlogs/test/extensions/clusters/aggregate/cluster_integration_test/test.log

// Executed 1 out of 1 test: 1 fails locally.
