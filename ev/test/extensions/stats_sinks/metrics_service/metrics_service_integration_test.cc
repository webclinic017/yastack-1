#include "envoy/config/metrics/v2/metrics_service.pb.h"
#include "envoy/service/metrics/v2/metrics_service.pb.h"

#include "common/common/version.h"
#include "common/grpc/codec.h"
#include "common/grpc/common.h"
#include "common/stats/histogram_impl.h"

#include "test/common/grpc/grpc_client_integration.h"
#include "test/integration/http_integration.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

using testing::AssertionResult;

namespace Envoy {
namespace {

class MetricsServiceIntegrationTest : public HttpIntegrationTest,
                                      public Grpc::GrpcClientIntegrationParamTest {
public:
  MetricsServiceIntegrationTest()
      : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, ipVersion(), realTime()) {}

  void createUpstreams() override {
    HttpIntegrationTest::createUpstreams();
    fake_upstreams_.emplace_back(
        new FakeUpstream(0, FakeHttpConnection::Type::HTTP2, version_, timeSystem()));
    fake_upstreams_.back()->set_allow_unexpected_disconnects(true);
  }

  void initialize() override {
    config_helper_.addConfigModifier([this](envoy::config::bootstrap::v2::Bootstrap& bootstrap) {
      // metrics_service cluster for Envoy gRPC.
      auto* metrics_service_cluster = bootstrap.mutable_static_resources()->add_clusters();
      metrics_service_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
      metrics_service_cluster->set_name("metrics_service");
      metrics_service_cluster->mutable_http2_protocol_options();
      // metrics_service gRPC service definition.
      auto* metrics_sink = bootstrap.add_stats_sinks();
      metrics_sink->set_name("envoy.metrics_service");
      envoy::config::metrics::v2::MetricsServiceConfig config;
      setGrpcService(*config.mutable_grpc_service(), "metrics_service",
                     fake_upstreams_.back()->localAddress());
      MessageUtil::jsonConvert(config, *metrics_sink->mutable_config());
      // Shrink reporting period down to 1s to make test not take forever.
      bootstrap.mutable_stats_flush_interval()->CopyFrom(
          Protobuf::util::TimeUtil::MillisecondsToDuration(100));
    });

    HttpIntegrationTest::initialize();
  }

  ABSL_MUST_USE_RESULT
  AssertionResult waitForMetricsServiceConnection() {
    ENVOY_LOG_MISC(critical, "tid [{}] calling fake_upstreams_[1]->waitForHttpConnection(*dispatcher_)", std::this_thread::get_id());
    return fake_upstreams_[1]->waitForHttpConnection(*dispatcher_,
                                                     fake_metrics_service_connection_);
  }

  ABSL_MUST_USE_RESULT
  AssertionResult waitForMetricsStream() {
    return fake_metrics_service_connection_->waitForNewStream(*dispatcher_,
                                                              metrics_service_request_);
  }

  ABSL_MUST_USE_RESULT
  AssertionResult waitForMetricsRequest() {
    bool known_histogram_exists = false;
    bool known_counter_exists = false;
    bool known_gauge_exists = false;
    // Sometimes stats do not come in the first flush cycle, this loop ensures that we wait till
    // required stats are flushed.
    // TODO(ramaraochavali): Figure out a more robust way to find out all required stats have been
    // flushed.
    while (!(known_counter_exists && known_gauge_exists && known_histogram_exists)) {
      envoy::service::metrics::v2::StreamMetricsMessage request_msg;
      VERIFY_ASSERTION(metrics_service_request_->waitForGrpcMessage(*dispatcher_, request_msg));
      EXPECT_STREQ("POST", metrics_service_request_->headers().Method()->value().c_str());
      EXPECT_STREQ("/envoy.service.metrics.v2.MetricsService/StreamMetrics",
                   metrics_service_request_->headers().Path()->value().c_str());
      EXPECT_STREQ("application/grpc",
                   metrics_service_request_->headers().ContentType()->value().c_str());
      EXPECT_TRUE(request_msg.envoy_metrics_size() > 0);
      const Protobuf::RepeatedPtrField<::io::prometheus::client::MetricFamily>& envoy_metrics =
          request_msg.envoy_metrics();

      for (::io::prometheus::client::MetricFamily metrics_family : envoy_metrics) {
        if (metrics_family.name() == "cluster.cluster_0.membership_change" &&
            metrics_family.type() == ::io::prometheus::client::MetricType::COUNTER) {
          known_counter_exists = true;
          EXPECT_EQ(1, metrics_family.metric(0).counter().value());
        }
        if (metrics_family.name() == "cluster.cluster_0.membership_total" &&
            metrics_family.type() == ::io::prometheus::client::MetricType::GAUGE) {
          known_gauge_exists = true;
          EXPECT_EQ(1, metrics_family.metric(0).gauge().value());
        }
        if (metrics_family.name() == "cluster.cluster_0.upstream_rq_time" &&
            metrics_family.type() == ::io::prometheus::client::MetricType::SUMMARY) {
          known_histogram_exists = true;
          Stats::HistogramStatisticsImpl empty_statistics;
          EXPECT_EQ(metrics_family.metric(0).summary().quantile_size(),
                    empty_statistics.supportedQuantiles().size());
        }
        ASSERT(metrics_family.metric(0).has_timestamp_ms());
        if (known_counter_exists && known_gauge_exists && known_histogram_exists) {
          break;
        }
      }
    }
    EXPECT_TRUE(known_counter_exists);
    EXPECT_TRUE(known_gauge_exists);
    EXPECT_TRUE(known_histogram_exists);

    return AssertionSuccess();
  }

  void cleanup() {
    if (fake_metrics_service_connection_ != nullptr) {
      AssertionResult result = fake_metrics_service_connection_->close();
      RELEASE_ASSERT(result, result.message());
      result = fake_metrics_service_connection_->waitForDisconnect();
      RELEASE_ASSERT(result, result.message());
    }
  }

  FakeHttpConnectionPtr fake_metrics_service_connection_;
  FakeStreamPtr metrics_service_request_;
};

// TODO
INSTANTIATE_TEST_CASE_P(IpVersionsClientType, MetricsServiceIntegrationTest,
                        GRPC_CLIENT_INTEGRATION_PARAMS);
//
// This creates a fake upstream/server that involves threading. Since currently threading is not supported
// in fs, we have disabled the creation of infra in ev/test/integration/server.cc
//
// This test depends on that. If ev/test/integration/server.cc is not correctly initialized, it doesn't
// find a stats_store_ object and hence crashes.
//
// Test a basic metric service flow.
TEST_P(MetricsServiceIntegrationTest, BasicFlow) {
  // After this call, there are a total of 3 threads and 3 dispatchers
  // - server thread/dispatcher (IntegrationTestServer::threadRoutine)
  // - fake upstream thread/dispatcher (FakeUpstream::threadRoutine)
  // - current thread/dispatcher (BaseIntegrationTest::dispatcher_)
  ENVOY_LOG_MISC(critical, "tid [{}] calling initialize()",std::this_thread::get_id());
  initialize();
  // Send an empty request so that histogram values merged for cluster_0.
  // BaseIntegrationTest::makeClientConnection uses BaseIntegrationTest::dispatcher_
  ENVOY_LOG_MISC(critical, "tid [{}] calling makeHttpConnection(makeClientConnection())",std::this_thread::get_id());
  codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
  Http::TestHeaderMapImpl request_headers{{":method", "GET"},
                                          {":path", "/test/long/url"},
                                          {":scheme", "http"},
                                          {":authority", "host"},
                                          {"x-lyft-user-id", "123"}};
  ENVOY_LOG_MISC(critical, "tid [{}] calling sendRequestAndWaitForResponse()",std::this_thread::get_id());
  sendRequestAndWaitForResponse(request_headers, 0, default_response_headers_, 0);

  ASSERT_TRUE(waitForMetricsServiceConnection());
  ASSERT_TRUE(waitForMetricsStream());
  ASSERT_TRUE(waitForMetricsRequest());

  // Send an empty response and end the stream. This should never happen but make sure nothing
  // breaks and we make a new stream on a follow up request.
  metrics_service_request_->startGrpcStream();
  envoy::service::metrics::v2::StreamMetricsResponse response_msg;
  metrics_service_request_->sendGrpcMessage(response_msg);
  metrics_service_request_->finishGrpcStream(Grpc::Status::Ok);

  switch (clientType()) {
  case Grpc::ClientType::EnvoyGrpc:
    test_server_->waitForGaugeEq("cluster.metrics_service.upstream_rq_active", 0);
    break;
  case Grpc::ClientType::GoogleGrpc:
    test_server_->waitForCounterGe("grpc.metrics_service.streams_closed_0", 1);
    break;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
  cleanup();
}

} // namespace
} // namespace Envoy
