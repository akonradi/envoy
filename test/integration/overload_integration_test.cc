#include "envoy/config/bootstrap/v3/bootstrap.pb.h"
#include "envoy/config/overload/v3/overload.pb.h"
#include "envoy/server/resource_monitor_config.h"
#include "envoy/registry/registry.h"
#include "envoy/server/resource_monitor.h"

#include "test/common/config/dummy_config.pb.h"
#include "test/integration/http_protocol_integration.h"

#include "absl/strings/str_cat.h"
#include <unordered_map>

namespace Envoy {

namespace Extensions {
namespace ResourceMonitors {

class FakeResourceMonitorFactory;

class FakeResourceMonitor : public Server::ResourceMonitor {
public:
  static constexpr absl::string_view kName =
      "envoy.resource_monitors.testonly.fake_resource_monitor";

  FakeResourceMonitor(FakeResourceMonitorFactory& factory) : factory_(factory), pressure_(0.0) {}
  ~FakeResourceMonitor();
  void updateResourceUsage(Callbacks& callbacks) override;

  void setResourcePressure(double pressure) { pressure_ = pressure; }

private:
  FakeResourceMonitorFactory& factory_;
  double pressure_;
};

class FakeResourceMonitorFactory : public Server::Configuration::ResourceMonitorFactory {
public:
  FakeResourceMonitor* monitor() const { return monitor_; }
  Server::ResourceMonitorPtr
  createResourceMonitor(const Protobuf::Message& config,
                        Server::Configuration::ResourceMonitorFactoryContext& context) override;

  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<test::common::config::DummyConfig>();
  }

  std::string name() const override { return std::string(FakeResourceMonitor::kName); }

  void onMonitorDestroyed(FakeResourceMonitor* monitor);

private:
  FakeResourceMonitor* monitor_{nullptr};
};

REGISTER_FACTORY(FakeResourceMonitorFactory, Server::Configuration::ResourceMonitorFactory);

FakeResourceMonitor::~FakeResourceMonitor() { factory_.onMonitorDestroyed(this); }

void FakeResourceMonitor::updateResourceUsage(Callbacks& callbacks) {
  Server::ResourceUsage usage;
  usage.resource_pressure_ = pressure_;
  callbacks.onSuccess(usage);
}

void FakeResourceMonitorFactory::onMonitorDestroyed(FakeResourceMonitor* monitor) {
  if (monitor_ == monitor) {
    monitor_ = nullptr;
  }
}

Server::ResourceMonitorPtr FakeResourceMonitorFactory::createResourceMonitor(
    const Protobuf::Message&, Server::Configuration::ResourceMonitorFactoryContext&) {
  auto monitor = std::make_unique<FakeResourceMonitor>(*this);
  monitor_ = monitor.get();
  return monitor;
}

} // namespace ResourceMonitors
} // namespace Extensions

class OverloadIntegrationTest : public HttpProtocolIntegrationTest {
protected:
  void initialize() override {
    config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
      const std::string overload_config = R"EOF(
        refresh_interval:
          seconds: 0
          nanos: 1000000
        resource_monitors:
          - name: "envoy.resource_monitors.testonly.fake_resource_monitor"
            typed_config:
              "@type": type.googleapis.com/google.protobuf.Empty
        actions:
          - name: "envoy.overload_actions.stop_accepting_requests"
            triggers:
              - name: "envoy.resource_monitors.testonly.fake_resource_monitor"
                threshold:
                  value: 0.9
          - name: "envoy.overload_actions.disable_http_keepalive"
            triggers:
              - name: "envoy.resource_monitors.testonly.fake_resource_monitor"
                threshold:
                  value: 0.8
          - name: "envoy.overload_actions.stop_accepting_connections"
            triggers:
              - name: "envoy.resource_monitors.testonly.fake_resource_monitor"
                threshold:
                  value: 0.95
      )EOF";
      *bootstrap.mutable_overload_manager() =
          TestUtility::parseYaml<envoy::config::overload::v3::OverloadManager>(overload_config);
    });
    updateResource(0);
    HttpIntegrationTest::initialize();
  }

  void updateResource(double pressure) {
    auto factory = dynamic_cast<Extensions::ResourceMonitors::FakeResourceMonitorFactory*>(
        Registry::FactoryRegistry<Server::Configuration::ResourceMonitorFactory>::getFactory(
            Extensions::ResourceMonitors::FakeResourceMonitor::kName));
    ASSERT(factory != nullptr);
    auto* monitor = factory->monitor();
    if (monitor != nullptr) {
      monitor->setResourcePressure(pressure);
    }
  }
};

INSTANTIATE_TEST_SUITE_P(Protocols, OverloadIntegrationTest,
                         testing::ValuesIn(HttpProtocolIntegrationTest::getProtocolTestParams()),
                         HttpProtocolIntegrationTest::protocolTestParamsToString);

TEST_P(OverloadIntegrationTest, CloseStreamsWhenOverloaded) {
  initialize();

  // Put envoy in overloaded state and check that it drops new requests.
  // Test both header-only and header+body requests since the code paths are slightly different.
  updateResource(0.9);
  test_server_->waitForGaugeEq("overload.envoy.overload_actions.stop_accepting_requests.active", 1);

  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/test/long/url"}, {":scheme", "http"}, {":authority", "host"}};
  codec_client_ = makeHttpConnection(makeClientConnection((lookupPort("http"))));
  auto response = codec_client_->makeRequestWithBody(request_headers, 10);
  response->waitForEndStream();

  EXPECT_TRUE(response->complete());
  EXPECT_EQ("503", response->headers().getStatusValue());
  EXPECT_EQ("envoy overloaded", response->body());
  codec_client_->close();

  codec_client_ = makeHttpConnection(makeClientConnection((lookupPort("http"))));
  response = codec_client_->makeHeaderOnlyRequest(request_headers);
  response->waitForEndStream();

  EXPECT_TRUE(response->complete());
  EXPECT_EQ("503", response->headers().getStatusValue());
  EXPECT_EQ("envoy overloaded", response->body());
  codec_client_->close();

  // Deactivate overload state and check that new requests are accepted.
  updateResource(0.8);
  test_server_->waitForGaugeEq("overload.envoy.overload_actions.stop_accepting_requests.active", 0);

  codec_client_ = makeHttpConnection(makeClientConnection((lookupPort("http"))));
  response = sendRequestAndWaitForResponse(request_headers, 0, default_response_headers_, 0);

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0U, upstream_request_->bodyLength());
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
  EXPECT_EQ(0U, response->body().size());
}

TEST_P(OverloadIntegrationTest, DisableKeepaliveWhenOverloaded) {
  if (downstreamProtocol() != Http::CodecClient::Type::HTTP1) {
    return; // only relevant for downstream HTTP1.x connections
  }

  initialize();

  // Put envoy in overloaded state and check that it disables keepalive
  updateResource(0.8);
  test_server_->waitForGaugeEq("overload.envoy.overload_actions.disable_http_keepalive.active", 1);

  codec_client_ = makeHttpConnection(makeClientConnection((lookupPort("http"))));
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/test/long/url"}, {":scheme", "http"}, {":authority", "host"}};
  auto response = sendRequestAndWaitForResponse(request_headers, 1, default_response_headers_, 1);
  ASSERT_TRUE(codec_client_->waitForDisconnect());

  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
  EXPECT_EQ("close", response->headers().getConnectionValue());

  // Deactivate overload state and check that keepalive is not disabled
  updateResource(0.7);
  test_server_->waitForGaugeEq("overload.envoy.overload_actions.disable_http_keepalive.active", 0);

  codec_client_ = makeHttpConnection(makeClientConnection((lookupPort("http"))));
  response = sendRequestAndWaitForResponse(request_headers, 1, default_response_headers_, 1);

  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
  EXPECT_EQ(nullptr, response->headers().Connection());
}

TEST_P(OverloadIntegrationTest, StopAcceptingConnectionsWhenOverloaded) {
  initialize();

  // Put envoy in overloaded state and check that it doesn't accept the new client connection.
  updateResource(0.95);
  test_server_->waitForGaugeEq("overload.envoy.overload_actions.stop_accepting_connections.active",
                               1);
  codec_client_ = makeHttpConnection(makeClientConnection((lookupPort("http"))));
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/test/long/url"}, {":scheme", "http"}, {":authority", "host"}};
  auto response = codec_client_->makeRequestWithBody(request_headers, 10);
  EXPECT_FALSE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_,
                                                         std::chrono::milliseconds(1000)));

  // Reduce load a little to allow the connection to be accepted but then immediately reject the
  // request.
  updateResource(0.9);
  test_server_->waitForGaugeEq("overload.envoy.overload_actions.stop_accepting_connections.active",
                               0);
  response->waitForEndStream();

  EXPECT_TRUE(response->complete());
  EXPECT_EQ("503", response->headers().getStatusValue());
  EXPECT_EQ("envoy overloaded", response->body());
  codec_client_->close();
}

} // namespace Envoy
