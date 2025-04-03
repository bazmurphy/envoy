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

// --------------------

// TEST PLAN

// 1. setup the two circuit breakers
//    - one at the aggregate_cluster level
//    - one at cluster1 level
// and then we can confirm that the aggregate_cluster circuit breaker does not get used [EXCEPT for retries]

// 2. send request1 through to /aggregate_cluster (and prevent it from completing)

// 3. check the circuit breaker states
//    - aggregate_cluster should be 0 (normal state == "closed")
//    - cluster1 should be 1 (triggered state == "open")

// 4. send request2 through 
//    - this request should automatically fail

// 5. send request3 through directly to /cluster1 at the same time as request2
//    - this request should also automatically fail

// --------------------

// https://www.envoyproxy.io/docs/envoy/latest/configuration/upstream/cluster_manager/cluster_stats#circuit-breakers-statistics 

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
  cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000);  // set this high
  cluster1_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1); // set this to 1
  cluster1_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000);  // set this high
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

  Envoy::IntegrationCodecClientPtr codec_client_ = makeHttpConnection(lookupPort("http"));

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

// the (max_connections) "cx" circuit breaker limits how many of those connections are allowed to exist

// theres an option in envoy to set and object that describes how to do HTTP2

// filter_chains:
//   - filters:
//     - name: envoy.filters.network.http_connection_manager
//       typed_config:
//       "@type": type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.HttpConnectionManager
//         codec_type: http2
//         http2_protocol_options: {}

// clusters:
//    http2_protocol_options: {}

// max_concurrent_streams [FIND AN EXAMPLE IN YAML]

// if we tell the upstream that it can handle at most one stream per connection

// because we are using http2
// set the max streams per connection on the cluster to 1
// then do the same dance as the other one, but check the cx stats bla bla

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
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000);
    aggregate_cluster_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000);
    aggregate_cluster_circuit_breakers_threshold_default->set_track_remaining(true);

    // we need to add the http2 protocol options with max_concurrent_streams set to 1 on BOTH clusters to their configs, like:

    // clusters:
    //  - name: <cluster_name>
    //    http2_protocol_options:
    //      max_concurrent_streams: 1

    // create a new HttpProtocolOptions
    envoy::extensions::upstreams::http::v3::HttpProtocolOptions http_protocol_options; // make a new object
    // set http2_protocol_options max_concurrent_streams to 1
    http_protocol_options.mutable_explicit_http_config()->mutable_http2_protocol_options()->mutable_max_concurrent_streams()->set_value(1);;
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
  cluster1_circuit_breakers_threshold_default->mutable_max_connections()->set_value(1);
  cluster1_circuit_breakers_threshold_default->mutable_max_pending_requests()->set_value(1000000000);
  cluster1_circuit_breakers_threshold_default->mutable_max_requests()->set_value(1000000000);
  cluster1_circuit_breakers_threshold_default->mutable_max_retries()->set_value(1000000000);
  cluster1_circuit_breakers_threshold_default->set_track_remaining(true);

  EXPECT_TRUE(compareDiscoveryRequest(Config::TypeUrl::get().Cluster, "55", {}, {}, {}));

  sendDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster, {cluster1_}, {cluster1_}, {}, "56");

  // make sure we still have 3 active clusters
  test_server_->waitForGaugeEq("cluster_manager.active_clusters", 3);

  // wait for the "remaining" stats to be available on cluster_1
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

  Envoy::IntegrationCodecClientPtr codec_client_ = makeHttpConnection(lookupPort("http"));

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

  test_server_->waitForGaugeEq("cluster.aggregate_cluster.circuit_breakers.default.cx_open", 0);

  std::cout << "---------- 55 TEST ERRORS HERE" << std::endl;
  // THE TEST ERRORS HERE: (the cx_open circuit breaker isn't returning to its default state... so this waitFor times out...)
  // Value of: TestUtility::waitForGaugeEq(statStore(), name, value, time_system_, timeout)
  //   Actual: false (timed out waiting for cluster.cluster_1.circuit_breakers.default.cx_open to be 0, current value 1)
  //   Expected: true
  test_server_->waitForGaugeEq("cluster.cluster_1.circuit_breakers.default.cx_open", 0);

  // apparently this is because envoy will keep the connection around in its connection pool to that upstream...

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

// 1. work out what a pending request
// 2. cry

// https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/circuit_breaking

// Cluster maximum pending requests: 
// The maximum number of requests that will be queued while waiting for a ready connection pool connection. 
// Requests are added to the list of pending requests whenever there aren’t enough upstream connections available to immediately dispatch the request. 
// For HTTP/2 connections, if max concurrent streams and max requests per connection are not configured, all requests will be multiplexed over the same connection 
// so this circuit breaker will only be hit when no connection is already established. 
// If this circuit breaker overflows the upstream_rq_pending_overflow counter for the cluster will increment. 
// For HTTP/3 the equivalent to HTTP/2’s max concurrent streams is max concurrent streams

// max concurrent streams
// max request per connection

// idea: set the max connections circuit breaker to zero so there can't be any connections
// and all requests will become pneding until the circuit breaker trips

// if we want to have 1 connection and then hold that up, we can use the max_concurrent_streams from above^
// then that 1 request will fill that 1 connection and so more connections are not available to trigger it
// and also max_connections, there is a circuit breaker that limits the max streams per connection (max_concurrent_streams)

// and together they put a bound on the number of in-flight requests to the upstream
// clusters:
//  http2_protocol_options:
//    max_concurrent_streams: 100

// note: you can close the connection forcefully on the makehttp bla bla connection (codec_client_) [janky]

// --------------------

// https://www.envoyproxy.io/docs/envoy/latest/api-v3/config/core/v3/protocol.proto#envoy-v3-api-msg-config-core-v3-http2protocoloptions

// Http2 Protocol Options

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

// clusters:
//  - name: <cluster_name>
//    http2_protocol_options:
//      max_concurrent_streams: 1

// --------------------

// (std::numeric_limits.max() if we want max values for the circuit breaker limits)

// --------------------


} // namespace
} // namespace Envoy
