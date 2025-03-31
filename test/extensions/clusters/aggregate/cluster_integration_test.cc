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

// uses a config() thats passed in to the constructor
// if we want to use an alternate config we need to do that at construction time(?)
// or can we edit it before initialize(?)

// aggregate_cluster
//   - cluster1
//   - cluster2 (ideally we don't want this here (so we need to remove it WITHOUT affecting the other tests), and want to maybe add it later)

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

// 5. send another request but this time to the /cluster1 route
//    - this request should also fail (because the cluster1 circuit breaker is 1 (open))

TEST_P(AggregateIntegrationTest, CircuitBreakerTest) {
  std::cout << "---------- 00 TEST START" << std::endl;

  std::cout << "---------- 01 CONFIG MODIFY START" << std::endl;
 
  // this is how we can modify the config (from the top of this file) before calling "initialize()"
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    
    // we want to access the "static_resources" to modify the "aggregate_cluster"

    // "my_cds_cluster" and "aggregate_cluster" are in "static_resources" > "clusters"
    auto* static_resources = bootstrap.mutable_static_resources();

    // "my_cds_cluster" is at index 0 of the "static_resources" > "clusters"
    // auto* my_cds_cluster = static_resources->mutable_clusters(0);
    // std::cout << "my_cds_cluster name(): " << my_cds_cluster->name() << std::endl;

    // "aggregate_cluster" is at index 1 of the "static_resources" > "clusters"
    auto* aggregate_cluster = static_resources->mutable_clusters(1);
    std::cout << "aggregate_cluster name(): " << aggregate_cluster->name() << std::endl;

    // ---------

    // we want to reduce the "aggregate_cluster" "clusters" list down to just "cluster_1" (and therefore remove "cluster_2")
    // so we can control our tests
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

    // std::cout << "BEFORE my_cds_cluster has_circuit_breakers(): " << my_cds_cluster->has_circuit_breakers() << std::endl;
    std::cout << "BEFORE aggregate_cluster has_circuit_breakers(): " << aggregate_cluster->has_circuit_breakers() << std::endl;

    auto* aggregate_cluster_circuit_breakers = aggregate_cluster->mutable_circuit_breakers();

    std::cout << "BEFORE aggregate_cluster thresholds_size(): " << aggregate_cluster_circuit_breakers->thresholds_size() << std::endl;

    // set the aggregate_cluster circuit breakers
    auto* aggregate_cluster_circuit_breakers_threshold_default = aggregate_cluster_circuit_breakers->add_thresholds();
    aggregate_cluster_circuit_breakers_threshold_default->set_priority(envoy::config::core::v3::RoutingPriority::DEFAULT);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1);

    auto* aggregate_cluster_circuit_breakers_threshold_high = aggregate_cluster_circuit_breakers->add_thresholds();
    aggregate_cluster_circuit_breakers_threshold_high->set_priority(envoy::config::core::v3::RoutingPriority::HIGH);
    aggregate_cluster_circuit_breakers_threshold_high->mutable_max_connections()->set_value(1);
    aggregate_cluster_circuit_breakers_threshold_high->mutable_max_pending_requests()->set_value(1);
    aggregate_cluster_circuit_breakers_threshold_high->mutable_max_requests()->set_value(1);
    aggregate_cluster_circuit_breakers_threshold_high->mutable_max_retries()->set_value(1);

    // std::cout << "AFTER my_cds_cluster has_circuit_breakers(): " << my_cds_cluster->has_circuit_breakers() << std::endl;
    std::cout << "AFTER aggregate_cluster has_circuit_breakers(): " << aggregate_cluster->has_circuit_breakers() << std::endl;

    std::cout << "AFTER aggregate_cluster thresholds_size(): " << aggregate_cluster_circuit_breakers->thresholds_size() << std::endl;

    // ---------
  });

  std::cout << "---------- 02 CONFIG MODIFY FINISH" << std::endl;

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
  cluster1_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1);
  cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1);
  cluster1_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1);
  cluster1_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1);

  auto* cluster1_circuit_breakers_threshold_high = cluster1_circuit_breakers->add_thresholds();
  cluster1_circuit_breakers_threshold_high->set_priority(envoy::config::core::v3::RoutingPriority::HIGH);
  cluster1_circuit_breakers_threshold_high->mutable_max_connections()->set_value(1);
  cluster1_circuit_breakers_threshold_high->mutable_max_pending_requests()->set_value(1);
  cluster1_circuit_breakers_threshold_high->mutable_max_requests()->set_value(1);
  cluster1_circuit_breakers_threshold_high->mutable_max_retries()->set_value(1);

  std::cout << "AFTER cluster1_ has_circuit_breakers(): " << cluster1_.has_circuit_breakers() << std::endl;
  std::cout << "AFTER cluster1_circuit_breakers thresholds_size(): " << cluster1_circuit_breakers->thresholds_size() << std::endl;

  std::cout << "cluster1_ threshold default max_connections().value(): " << cluster1_circuit_breakers_threshold_default->max_connections().value() << std::endl;
  std::cout << "cluster1_ threshold default max_pending_requests().value(): " << cluster1_circuit_breakers_threshold_default->max_pending_requests().value() << std::endl;
  std::cout << "cluster1_ threshold default max_requests().value(): " << cluster1_circuit_breakers_threshold_default->max_requests().value() << std::endl;
  std::cout << "cluster1_ threshold default max_retries().value(): " << cluster1_circuit_breakers_threshold_default->max_retries().value() << std::endl;

  std::cout << "cluster1_ threshold high max_connections().value(): " << cluster1_circuit_breakers_threshold_high->max_connections().value() << std::endl;
  std::cout << "cluster1_ threshold high max_pending_requests().value(): " << cluster1_circuit_breakers_threshold_high->max_pending_requests().value() << std::endl;
  std::cout << "cluster1_ threshold high max_requests().value(): " << cluster1_circuit_breakers_threshold_high->max_requests().value() << std::endl;
  std::cout << "cluster1_ threshold high max_retries().value(): " << cluster1_circuit_breakers_threshold_high->max_retries().value() << std::endl;

  // LOG OUTPUT :
  // ---------- 00 TEST START
  // ---------- 01 CONFIG MODIFY START
  // ---------- 02 CONFIG MODIFY FINISH
  // ---------- 03 INITIALIZE START
  // aggregate_cluster name(): aggregate_cluster
  // BEFORE temp_aggregate_cluster_typed_config.clusters_size(): 2
  // BEFORE temp_aggregate_cluster_typed_config clusters[0]: cluster_1
  // BEFORE temp_aggregate_cluster_typed_config clusters[1]: cluster_2
  // AFTER temp_aggregate_cluster_typed_config.clusters_size(): 1
  // AFTER temp_aggregate_cluster_typed_config clusters[0]: cluster_1
  // BEFORE aggregate_cluster has_circuit_breakers(): 0
  // BEFORE aggregate_cluster thresholds_size(): 0
  // AFTER aggregate_cluster has_circuit_breakers(): 1
  // AFTER aggregate_cluster thresholds_size(): 2
  // ---------- 04 INITIALIZE FINISH
  // cluster1_ name(): cluster_1
  // BEFORE cluster1_ has_circuit_breakers(): 0
  // BEFORE cluster1_circuit_breakers thresholds_size(): 0
  // AFTER cluster1_ has_circuit_breakers(): 1
  // AFTER cluster1_circuit_breakers thresholds_size(): 2
  // cluster1_ threshold default max_connections().value(): 1
  // cluster1_ threshold default max_pending_requests().value(): 1
  // cluster1_ threshold default max_requests().value(): 1
  // cluster1_ threshold default max_retries().value(): 1
  // cluster1_ threshold high max_connections().value(): 1
  // cluster1_ threshold high max_pending_requests().value(): 1
  // cluster1_ threshold high max_requests().value(): 1
  // cluster1_ threshold high max_retries().value(): 1
  // ---------- 99 TEST END

  // ----------

  // now we want to make the requests and check the circuit breakers behaviour...

  // check the initial circuit breaker stats on both "aggregate_cluster" and "cluster_1"
  test_server_->waitForCounterEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open", 0);
  test_server_->waitForCounterEq("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 0);

  std::cout << "---------- 99 TEST END" << std::endl;

  // codec_client_ = makeHttpConnection(lookupPort("http"));

  // // send the first request (this should go via aggregate_cluster and to cluster_1)
  // auto aggregate_cluster_response1 = codec_client_->makeRequestWithBody(Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}, 1024);
  
  // waitForNextUpstreamRequest(FirstUpstreamIndex);

  // // send the second request (this should go via aggregate_cluster and to cluster_1)
  // auto aggregate_cluster_response2 = codec_client_->makeRequestWithBody(Http::TestRequestHeaderMapImpl{{":method", "GET"},{":path", "/aggregatecluster"},{":scheme", "http"},{":authority", "host"}}, 1024);

  // // aggregate_cluster circuit breaker should still be closed (not tripped)
  // test_server_->waitForCounterEq("cluster.aggregate_cluster.circuit_breakers.default.rq_pending_open", 0);

  // // cluster_1 circuit breaker should be open (tripped)
  // test_server_->waitForCounterGe("cluster.cluster_1.circuit_breakers.default.rq_pending_open", 1);

  // // complete the first request
  // upstream_request_->encodeHeaders(default_response_headers_, true);

  // ASSERT_TRUE(aggregate_cluster_response1->waitForEndStream());
  // EXPECT_TRUE(aggregate_cluster_response1->complete());
  
  // // the first request should have succeeded
  // EXPECT_EQ("200", aggregate_cluster_response1->headers().getStatusValue());

  // // complete the second request
  // ASSERT_TRUE(aggregate_cluster_response2->waitForEndStream());
  // EXPECT_TRUE(aggregate_cluster_response2->complete());

  // // the second request should have failed due to the circuit breaker
  // EXPECT_EQ("503", aggregate_cluster_response2->headers().getStatusValue());
}

} // namespace
} // namespace Envoy

// // ----------- NOTES FOR CONTEXT :

// // the specific constructor used by this class:

// // [0] (in here at the top)
// AggregateIntegrationTest() : HttpIntegrationTest(Http::CodecType::HTTP1, std::get<0>(GetParam()), config()),

// // [1] test/integration/http_integration.h (132-133)
// HttpIntegrationTest(Http::CodecType downstream_protocol, Network::Address::IpVersion version, const std::string& config);

// // [2] test/integration/http_integration.cc (325-334)
// HttpIntegrationTest::HttpIntegrationTest(Http::CodecType downstream_protocol,
//                                          Network::Address::IpVersion version,
//                                          const std::string& config)
//     : HttpIntegrationTest::HttpIntegrationTest(
//           downstream_protocol,
//           [version](int) {
//             return Network::Utility::parseInternetAddressNoThrow(
//                 Network::Test::getLoopbackAddressString(version), 0);
//           },
//           version, config) {}

// // [3] test/integration/http_integration.h (142-144)
// HttpIntegrationTest(Http::CodecType downstream_protocol,
//   const InstanceConstSharedPtrFn& upstream_address_fn,
//   Network::Address::IpVersion version, const std::string& config);

// // [4] test/integration/http_integration.cc (336-372)
// HttpIntegrationTest::HttpIntegrationTest(Http::CodecType downstream_protocol,
//                                          const InstanceConstSharedPtrFn& upstream_address_fn,
//                                          Network::Address::IpVersion version,
//                                          const std::string& config)
//     : BaseIntegrationTest(upstream_address_fn, version, config),
//       downstream_protocol_(downstream_protocol), quic_stat_names_(stats_store_.symbolTable()) {
//   // Legacy integration tests expect the default listener to be named "http" for
//   // lookupPort calls.
//   config_helper_.renameListener("http");
//   config_helper_.setClientCodec(typeToCodecType(downstream_protocol_));
//   // Allow extension lookup by name in the integration tests.
//   config_helper_.addRuntimeOverride("envoy.reloadable_features.no_extension_lookup_by_name",
//                                     "false");

//   config_helper_.addConfigModifier(
//       [](envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager&
//              hcm) {
//         auto* range = hcm.mutable_internal_address_config()->add_cidr_ranges();
//         // Set loopback to be trusted so tests can set x-envoy headers.
//         range->set_address_prefix("127.0.0.1");
//         range->mutable_prefix_len()->set_value(32);
//         // Legacy tests also set XFF: 10.0.0.1
//         range->set_address_prefix("10.0.0.0");
//         range->mutable_prefix_len()->set_value(8);
//         range = hcm.mutable_internal_address_config()->add_cidr_ranges();
//         range->set_address_prefix("::1");
//         range->mutable_prefix_len()->set_value(128);
//       });

// #ifdef ENVOY_ENABLE_QUIC
//   if (downstream_protocol_ == Http::CodecType::HTTP3) {
//     // Needed to config QUIC transport socket factory, and needs to be added before base class calls
//     // initialize().
//     config_helper_.addQuicDownstreamTransportSocketConfig();
//   }
// #endif
// }

// // [5] (from above this) test/integration/http_integration.cc
// BaseIntegrationTest(upstream_address_fn, version, config)

// // [6] test/integration/base_integration_test.h (73-75)
// BaseIntegrationTest(const InstanceConstSharedPtrFn& upstream_address_fn,
//   Network::Address::IpVersion version,
//   const std::string& config = ConfigHelper::httpProxyConfig());

// // [7] test/integration/base_integration_test.cc (86-89)
// BaseIntegrationTest::BaseIntegrationTest(const InstanceConstSharedPtrFn& upstream_address_fn,
//                                          Network::Address::IpVersion version,
//                                          const std::string& config)
//     : BaseIntegrationTest(upstream_address_fn, version, configToBootstrap(config)) {}

// // [8] from above - delegate constructor
// BaseIntegrationTest(upstream_address_fn, version, configToBootstrap(config))

// // [8-sidequest] test/integration/base_integration_test.cc (32-41)
// // this is the configToBootstrap function (see above) that presumably converts the raw yaml string into a "Bootstrap" typed object(?)
// // ""unmarshal"" the config into the bootstrap (load the yaml from the string, convert that into the bootstrap object)
// envoy::config::bootstrap::v3::Bootstrap configToBootstrap(const std::string& config) {
//   #ifdef ENVOY_ENABLE_YAML
//     envoy::config::bootstrap::v3::Bootstrap bootstrap;
//     TestUtility::loadFromYaml(config, bootstrap);
//     return bootstrap;
//   #else
//     UNREFERENCED_PARAMETER(config);
//     PANIC("YAML support compiled out: can't parse YAML");
//   #endif
//   }

// // [9] test/integration/base_integration_test.cc (52-84)
// BaseIntegrationTest::BaseIntegrationTest(const InstanceConstSharedPtrFn& upstream_address_fn,
//                                          Network::Address::IpVersion version,
//                                          const envoy::config::bootstrap::v3::Bootstrap& bootstrap)
//     : api_(Api::createApiForTest(stats_store_, time_system_)),
//       mock_buffer_factory_(new NiceMock<MockBufferFactory>),
//       dispatcher_(api_->allocateDispatcher("test_thread",
//                                            Buffer::WatermarkFactoryPtr{mock_buffer_factory_})),
//       version_(version), upstream_address_fn_(upstream_address_fn),
//       config_helper_(version, bootstrap),
//       default_log_level_(TestEnvironment::getOptions().logLevel()) {
//   Envoy::Server::validateProtoDescriptors();
//   // This is a hack, but there are situations where we disconnect fake upstream connections and
//   // then we expect the server connection pool to get the disconnect before the next test starts.
//   // This does not always happen. This pause should allow the server to pick up the disconnect
//   // notification and clear the pool connection if necessary. A real fix would require adding fairly
//   // complex test hooks to the server and/or spin waiting on stats, neither of which I think are
//   // necessary right now.
//   timeSystem().realSleepDoNotUseWithoutScrutiny(std::chrono::milliseconds(10));
//   ON_CALL(*mock_buffer_factory_, createBuffer_(_, _, _))
//       .WillByDefault(Invoke([](std::function<void()> below_low, std::function<void()> above_high,
//                                std::function<void()> above_overflow) -> Buffer::Instance* {
//         return new Buffer::WatermarkBuffer(below_low, above_high, above_overflow);
//       }));
//   ON_CALL(factory_context_.server_context_, api()).WillByDefault(ReturnRef(*api_));
//   ON_CALL(factory_context_, statsScope()).WillByDefault(ReturnRef(*stats_store_.rootScope()));
//   ON_CALL(factory_context_, sslContextManager()).WillByDefault(ReturnRef(context_manager_));
//   ON_CALL(factory_context_.server_context_, threadLocal()).WillByDefault(ReturnRef(thread_local_));

// #ifndef ENVOY_ADMIN_FUNCTIONALITY
//   config_helper_.addConfigModifier(
//       [&](envoy::config::bootstrap::v3::Bootstrap& bootstrap) -> void { bootstrap.clear_admin(); });
// #endif
// }

// // [11]

// // TODO


// // ----------
// // Member VARIABLES in HttpIntegrationTest that maybe of use
// // test/integration/http_integration.h (359+)

// // The client making requests to Envoy.
// IntegrationCodecClientPtr codec_client_;
// // A placeholder for the first upstream connection.
// FakeHttpConnectionPtr fake_upstream_connection_;
// // A placeholder for the first request received at upstream.
// FakeStreamPtr upstream_request_;

// // The response headers sent by sendRequestAndWaitForResponse() by default.
// Http::TestResponseHeaderMapImpl default_response_headers_{{":status", "200"}};
// Http::TestRequestHeaderMapImpl default_request_headers_{{":method", "GET"},
//                                                         {":path", "/test/long/url"},
//                                                         {":scheme", "http"},
//                                                         {":authority", "sni.lyft.com"}};

// // The codec type for the client-to-Envoy connection [this is overriden to HTTP2 in our tests]
// Http::CodecType downstream_protocol_{Http::CodecType::HTTP1};

// // ----------
// // Member METHODS in HttpIntegrationTest that maybe of use

// // TODO


// // ----------
// // Member VARIABLES in BaseIntegrationTest that maybe of use
// // test/integration/base_integration_test.h (??)

// // work out what this is: (line 151)
// Api::ApiPtr api_;

// // Make sure the test server will be torn down after any fake client.
// // The test server owns the runtime, which is often accessed by client and
// // fake upstream codecs and must outlast them.
// IntegrationTestServerPtr test_server_;

// // ^^ because from above in this file we can see:
// test_server_->waitForGaugeGe("cluster_manager.active_clusters", 4);
// // and thats how we check the circuit breaker triggered or not...

// // IP Address to use when binding sockets on upstreams.
// InstanceConstSharedPtrFn upstream_address_fn_;

// // [! LOOK INTO WHAT THIS IS/HAS]
// // The config for envoy start-up.
// ConfigHelper config_helper_;

// // The fake upstreams_ are created using the context_manager, so make sure
// // they are destroyed before it is.
// std::vector<std::unique_ptr<FakeUpstream>> fake_upstreams_;

// // Target number of upstreams.
// uint32_t fake_upstreams_count_{1};

// // The number of worker threads that the test server uses.
// uint32_t concurrency_{1};

// // Configuration for the fake upstream.
// FakeUpstreamConfig upstream_config_{time_system_};

// // ----------
// // Member METHODS in BaseIntegrationTest that maybe of use
// // test/integration/base_integration_test.h (??)

// // Initialize the basic proto configuration, create fake upstreams, and start Envoy.
// virtual void initialize();
// // Set up the fake upstream connections. This is called by initialize() and
// // is virtual to allow subclass overrides.
// virtual void createUpstreams();
// // Create a single upstream, based on the supplied config.
// void createUpstream(Network::Address::InstanceConstSharedPtr endpoint, FakeUpstreamConfig& config);
// // Sets upstream_protocol_ and alters the upstream protocol in the config_helper_
// void setUpstreamProtocol(Http::CodecType protocol);


// // this is why we pass in "http" and get back the port:
// // (from above in this file: codec_client_ = makeHttpConnection(lookupPort("http"));)
// // Test-wide port map.
// void registerPort(const std::string& key, uint32_t port);
// uint32_t lookupPort(const std::string& key);


// Network::ClientConnectionPtr makeClientConnection(uint32_t port);

// // Functions for testing reloadable config (xDS)
// virtual void createXdsUpstream();
// void createXdsConnection();
// void cleanUpXdsConnection(); // this is used in TearDown (in this file)

// // ---------


// // IntegrationTestServer
// // test/integration/server.h


// // VARIABLES


// // METHODS
// std::vector<Stats::GaugeSharedPtr> gauges() override { return statStore().gauges(); }



// // Protobufs

// // bazel-bin/external/envoy_api/envoy/config/cluster/v3/cluster.pb.h (6233-6246)

// public:
// // .envoy.config.cluster.v3.CircuitBreakers circuit_breakers = 10;
// bool has_circuit_breakers() const;
// void clear_circuit_breakers() ;
// const ::envoy::config::cluster::v3::CircuitBreakers& circuit_breakers() const;
// PROTOBUF_NODISCARD ::envoy::config::cluster::v3::CircuitBreakers* release_circuit_breakers();
// ::envoy::config::cluster::v3::CircuitBreakers* mutable_circuit_breakers();
// void set_allocated_circuit_breakers(::envoy::config::cluster::v3::CircuitBreakers* value);
// void unsafe_arena_set_allocated_circuit_breakers(::envoy::config::cluster::v3::CircuitBreakers* value);
// ::envoy::config::cluster::v3::CircuitBreakers* unsafe_arena_release_circuit_breakers();

// private:
// const ::envoy::config::cluster::v3::CircuitBreakers& _internal_circuit_breakers() const;
// ::envoy::config::cluster::v3::CircuitBreakers* _internal_mutable_circuit_breakers();


// // bazel-bin/external/envoy_api/envoy/config/cluster/v3/circuit_breaker.pb.h 

// // TOO MUCH TO PASTE HERE...


// ----------

// LIST OF MORE THINGS/EXAMPLES I FOUND THAT I THINK MAY BE USEFUL :

// codec_client_ = makeHttpConnection(lookupPort("http"));

// auto response = codec_client_->makeRequestWithBody(
//                                     Http::TestRequestHeaderMapImpl{
//                                       {":method", "GET"},
//                                       {":path", "/aggregatecluster"},
//                                       {":scheme", "http"},
//                                       {":authority", "host"},
//                                       {"x-forwarded-for", "10.0.0.1"},
//                                       {"x-envoy-retry-on", "5xx"}
//                                     }
//                               );

// auto response = codec_client_->makeRequestWithBody(default_request_headers_, 1024);

// ASSERT_TRUE(response->waitForEndStream());

// ----------

// config_helper_.addRuntimeOverride("circuit_breakers.cluster_0.default.max_requests", "0");
// config_helper_.addRuntimeOverride("circuit_breakers.cluster_0.default.max_retries", "1024");

// ----------

// test_server_->waitForGaugeEq("cluster.cluster_0.upstream_rq_active", 0);
// test_server_->waitForGaugeEq("cluster.cluster_0.upstream_rq_pending_active", 0);

// EXPECT_EQ(test_server_->counter("cluster.cluster_0.upstream_rq_pending_overflow")->value(), 1);

// ----------

// config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
//   // BOOTSTRAP METHODS THAT MIGHT BE USEFUL
//   bootstrap.mutable_static_resources()->add_clusters();
// });

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

// IntegrationCodecClientPtr makeHttpConnection(uint32_t port);
// IntegrationCodecClientPtr makeHttpConnection(Network::ClientConnectionPtr&& conn);

// IntegrationStreamDecoderPtr makeHeaderOnlyRequest(const Http::RequestHeaderMap& headers);

// IntegrationStreamDecoderPtr makeRequestWithBody(const Http::RequestHeaderMap& headers, uint64_t body_size, bool end_stream = true);
// IntegrationStreamDecoderPtr makeRequestWithBody(const Http::RequestHeaderMap& headers, const std::string& body, bool end_stream = true);

// test/integration/http_integration.cc
// IntegrationCodecClient

// makeHttpConnections (variant 1)
// codec_client_->makeHttpConnection(...)
// IntegrationCodecClientPtr HttpIntegrationTest::makeHttpConnection(uint32_t port) {
//   return makeHttpConnection(makeClientConnection(port));
// }

// makeHttpConnections (variant 2)
// codec_client_->makeHttpConnection(...)
// IntegrationCodecClientPtr HttpIntegrationTest::makeHttpConnection(Network::ClientConnectionPtr&& conn) {
//   auto codec = makeRawHttpConnection(std::move(conn), absl::nullopt);
//   EXPECT_TRUE(codec->connected()) << codec->connection()->transportFailureReason();
//   return codec;
// }

// makeHeaderOnlyRequest
// codec_client_->makeHeaderOnlyRequest(...)
// IntegrationStreamDecoderPtr IntegrationCodecClient::makeHeaderOnlyRequest(const Http::RequestHeaderMap& headers) {
//   auto response = std::make_unique<IntegrationStreamDecoder>(dispatcher_);
//   Http::RequestEncoder& encoder = newStream(*response);
//   encoder.getStream().addCallbacks(*response);
//   encoder.encodeHeaders(headers, true).IgnoreError();
//   flushWrite();
//   return response;
// }

// makeRequestWithBody (variant 1)
// codec_client_->makeRequestWithBody(...)
// IntegrationStreamDecoderPtr IntegrationCodecClient::makeRequestWithBody(const Http::RequestHeaderMap& headers, uint64_t body_size, bool end_stream) {
//   return makeRequestWithBody(headers, std::string(body_size, 'a'), end_stream);
// }

// makeRequestWithBody (variant 2)
// codec_client_->makeRequestWithBody(...)
// IntegrationStreamDecoderPtr IntegrationCodecClient::makeRequestWithBody(const Http::RequestHeaderMap& headers, const std::string& body, bool end_stream) {
//   auto response = std::make_unique<IntegrationStreamDecoder>(dispatcher_);
//   Http::RequestEncoder& encoder = newStream(*response);
//   encoder.getStream().addCallbacks(*response);
//   encoder.encodeHeaders(headers, false).IgnoreError();
//   Buffer::OwnedImpl data(body);
//   encoder.encodeData(data, end_stream);
//   flushWrite();
//   return response;
// }

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
// absl::optional<uint64_t> waitForNextUpstreamRequest(const std::vector<uint64_t>& upstream_indices,std::chrono::milliseconds connection_wait_timeout = TestUtility::DefaultTimeout);
// void waitForNextUpstreamRequest(uint64_t upstream_index = 0, std::chrono::milliseconds connection_wait_timeout = TestUtility::DefaultTimeout);
// waitForEndStream(std::chrono::milliseconds timeout = TestUtility::DefaultTimeout);

// test/integration/integration_stream_decoder.cc
// IntegrationStreamDecoder

// AssertionResult IntegrationStreamDecoder::waitForEndStream(std::chrono::milliseconds timeout) {
//   bool timer_fired = false;
//   while (!saw_end_stream_) {
//     Event::TimerPtr timer(dispatcher_.createTimer([this, &timer_fired]() -> void {
//       timer_fired = true;
//       dispatcher_.exit();
//     }));
//     timer->enableTimer(timeout);
//     waiting_for_end_stream_ = true;
//     dispatcher_.run(Event::Dispatcher::RunType::Block);
//     if (!saw_end_stream_) {
//       ENVOY_LOG_MISC(warn, "non-end stream event.");
//     }
//     if (timer_fired) {
//       return AssertionFailure() << "Timed out waiting for end stream\n";
//     }
//   }
//   return AssertionSuccess();
// }

// ----------
