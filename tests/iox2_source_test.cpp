#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <class_loader/class_loader.hpp>
#include <gtest/gtest.h>
#include <iox2/bb/file_path.hpp>
#include <iox2/config.hpp>
#include <iox2/messaging_pattern.hpp>
#include <iox2/service.hpp>
#include <iox2/service_name.hpp>
#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rcutils/logging.h>
#include <rosgraph_msgs/msg/clock.hpp>
#include <sys/wait.h>
#include <websocketpp/base64/base64.hpp>
#include <websocketpp/client.hpp>
// No TLS, so no OpenSSL dependency.
#include <websocketpp/config/asio_no_tls_client.hpp>

#include <foxglove/websocket.hpp>
#include <foxglove_bridge/channel_source_plugin.hpp>

#include "test_payload_generated.h"

namespace {

using Plugin = class_loader::ClassLoader::UniquePtr<foxglove_bridge::ChannelSourcePlugin>;

constexpr auto DEFAULT_TIMEOUT = std::chrono::seconds(10);
// Several 200 ms discovery passes.
constexpr auto SETTLE_TIME = std::chrono::milliseconds(1000);
constexpr char SCHEMA_NAME[] = "foxglove_bridge_iox2_plugin.test.TestPayload";
const std::string TEST_BFBS = std::string(TEST_SCHEMA_DIR) + "/test_payload.bfbs";
const std::string OTHER_BFBS = std::string(TEST_SCHEMA_DIR) + "/other_payload.bfbs";

std::mutex g_logsMutex;
// Severity and message of every log at INFO or above.
std::vector<std::pair<int, std::string>> g_logs;
rcutils_logging_output_handler_t g_defaultLogHandler = nullptr;

void captureLogs(const rcutils_log_location_t* location, int severity, const char* name,
                 rcutils_time_point_value_t timestamp, const char* format, va_list* args) {
  if (severity >= RCUTILS_LOG_SEVERITY_INFO) {
    va_list argsCopy;
    va_copy(argsCopy, *args);
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), format, argsCopy);
    va_end(argsCopy);
    std::lock_guard<std::mutex> lock(g_logsMutex);
    g_logs.emplace_back(severity, buffer);
  }
  g_defaultLogHandler(location, severity, name, timestamp, format, args);
}

size_t countLogsContaining(int severity, const std::string& text) {
  std::lock_guard<std::mutex> lock(g_logsMutex);
  return static_cast<size_t>(std::count_if(g_logs.begin(), g_logs.end(), [&](const auto& log) {
    return log.first == severity && log.second.find(text) != std::string::npos;
  }));
}

size_t countErrorLogsContaining(const std::string& text) {
  return countLogsContaining(RCUTILS_LOG_SEVERITY_ERROR, text);
}

std::string readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// Loads the built plugin like pluginlib does.
Plugin createPlugin() {
  static class_loader::ClassLoader loader(PLUGIN_LIBRARY_PATH);
  return loader.createUniqueInstance<foxglove_bridge::ChannelSourcePlugin>(
    "foxglove_bridge_iox2_plugin::Iox2Source");
}

bool waitFor(const std::function<bool()>& predicate,
             std::chrono::milliseconds timeout = DEFAULT_TIMEOUT) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return predicate();
}

/// Unique per process and test, so runs never share services.
std::string uniqueServiceName() {
  const auto* testInfo = testing::UnitTest::GetInstance()->current_test_info();
  return "foxglove_bridge_iox2_plugin_test/" + std::to_string(getpid()) + "/" + testInfo->name();
}

bool serviceExists(const std::string& serviceName) {
  auto name = iox2::ServiceName::create(serviceName.c_str());
  auto exists = iox2::Service<iox2::ServiceType::Ipc>::does_exist(
    name.value(), iox2::Config::global_config(), iox2::MessagingPattern::PublishSubscribe);
  return exists.has_value() && exists.value();
}

std::vector<rclcpp::Parameter> pluginParameters(const std::string& serviceName,
                                                const std::string& topic,
                                                bool allowBlockingPublishers = false,
                                                const std::string& bfbsFile = "test_payload.bfbs",
                                                const std::string& schemaName = SCHEMA_NAME) {
  return {
    rclcpp::Parameter("iox2.schema_dir", std::string(TEST_SCHEMA_DIR)),
    rclcpp::Parameter("iox2.service_names", std::vector<std::string>{serviceName}),
    rclcpp::Parameter("iox2.topics", std::vector<std::string>{topic}),
    rclcpp::Parameter("iox2.schema_names", std::vector<std::string>{schemaName}),
    rclcpp::Parameter("iox2.bfbs_files", std::vector<std::string>{bfbsFile}),
    rclcpp::Parameter("iox2.poll_interval_ms", 5),
    rclcpp::Parameter("iox2.allow_blocking_publishers", allowBlockingPublishers),
  };
}

/// Moves parameters from the default "iox2" instance to another instance name.
std::vector<rclcpp::Parameter> forInstance(const std::vector<rclcpp::Parameter>& parameters,
                                           const std::string& instanceName) {
  std::vector<rclcpp::Parameter> renamed;
  for (const auto& parameter : parameters) {
    renamed.emplace_back(instanceName + parameter.get_name().substr(std::strlen("iox2")),
                         parameter.get_parameter_value());
  }
  return renamed;
}

/// Runs iox2_test_publisher. iceoryx2 detects node liveness per process, so a publisher in the
/// test process would look dead to the plugin.
class PublisherProcess {
public:
  PublisherProcess(const std::string& serviceName, const std::string& schemaPath,
                   bool safeOverflow = true) {
    // Prepared before fork(): the child may only make async-signal-safe calls.
    const char* overflow = safeOverflow ? "1" : "0";
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
      throw std::runtime_error("pipe() failed");
    }
    _pid = fork();
    if (_pid < 0) {
      throw std::runtime_error("fork() failed");
    }
    if (_pid == 0) {
      dup2(pipeFds[1], STDOUT_FILENO);
      close(pipeFds[0]);
      close(pipeFds[1]);
      execl(TEST_PUBLISHER_PATH, TEST_PUBLISHER_PATH, serviceName.c_str(), schemaPath.c_str(),
            overflow, static_cast<char*>(nullptr));
      _exit(127);
    }
    close(pipeFds[1]);
    _stdout = fdopen(pipeFds[0], "r");
    _reader = std::thread([this]() {
      char line[256];
      while (std::fgets(line, sizeof(line), _stdout) != nullptr) {
        const std::string text(line);
        if (text.rfind("ready", 0) == 0) {
          _ready = true;
        } else if (text.rfind("subscribers=", 0) == 0) {
          _subscribers = std::stoull(text.substr(std::strlen("subscribers=")));
        } else if (text.rfind("error=", 0) == 0) {
          std::lock_guard<std::mutex> lock(_errorMutex);
          _error = text;
        }
      }
    });
    if (!waitFor([this]() {
          return _ready.load();
        })) {
      std::lock_guard<std::mutex> lock(_errorMutex);
      throw std::runtime_error("publisher process did not start: " + _error);
    }
  }

  ~PublisherProcess() {
    kill(_pid, SIGTERM);
    waitpid(_pid, nullptr, 0);
    _reader.join();
    std::fclose(_stdout);
  }

  uint64_t numberOfSubscribers() const {
    return _subscribers.load();
  }

private:
  pid_t _pid = -1;
  FILE* _stdout = nullptr;
  std::thread _reader;
  std::atomic<bool> _ready = false;
  std::atomic<uint64_t> _subscribers = 0;
  std::mutex _errorMutex;
  std::string _error;
};

/// Hosts the plugin like foxglove_bridge: a spun node and a WebSocket server on its context.
class PluginHost {
public:
  explicit PluginHost(const std::vector<rclcpp::Parameter>& parameters,
                      const std::string& instanceName = "iox2")
      : _node(std::make_shared<rclcpp::Node>("iox2_plugin_test",
                                             rclcpp::NodeOptions().parameter_overrides(parameters)))
      , _context(foxglove::Context::create()) {
    _plugin->initialize(*_node, instanceName, _context);

    foxglove::WebSocketServerOptions options;
    options.context = _context;
    options.port = 0;
    auto server = foxglove::WebSocketServer::create(std::move(options));
    if (!server.has_value()) {
      throw std::runtime_error("failed to start WebSocket server");
    }
    _server.emplace(std::move(server.value()));

    _executor.add_node(_node);
    _spinThread = std::thread([this]() {
      _executor.spin();
    });
  }

  ~PluginHost() {
    _plugin->shutdown();
    _executor.cancel();
    _spinThread.join();
    _server->stop();
  }

  std::string uri() const {
    return "ws://127.0.0.1:" + std::to_string(_server->port());
  }

  rclcpp::Node& node() {
    return *_node;
  }

private:
  rclcpp::Node::SharedPtr _node;
  foxglove::Context _context;
  // After the node, so it is destroyed first, as in the bridge.
  Plugin _plugin = createPlugin();
  std::optional<foxglove::WebSocketServer> _server;
  rclcpp::executors::SingleThreadedExecutor _executor;
  std::thread _spinThread;
};

/// A minimal Foxglove WebSocket client that tracks the advertised channels.
class FoxgloveClient {
public:
  struct Channel {
    uint64_t id;
    std::string topic;
    std::string encoding;
    std::string schemaName;
    std::string schema;
    std::string schemaEncoding;
  };

  struct Message {
    uint64_t logTime;
    std::vector<uint8_t> payload;
  };

  explicit FoxgloveClient(const std::string& uri) {
    _client.clear_access_channels(websocketpp::log::alevel::all);
    _client.clear_error_channels(websocketpp::log::elevel::all);
    _client.init_asio();
    auto opened = std::make_shared<std::promise<void>>();
    auto openedFuture = opened->get_future();
    _client.set_open_handler([opened](websocketpp::connection_hdl) {
      opened->set_value();
    });
    _client.set_message_handler([this](websocketpp::connection_hdl, WsClient::message_ptr msg) {
      onMessage(msg);
    });
    websocketpp::lib::error_code error;
    auto connection = _client.get_connection(uri, error);
    if (error) {
      throw std::runtime_error("invalid URI " + uri);
    }
    connection->add_subprotocol("foxglove.sdk.v1");
    _connection = connection->get_handle();
    _client.connect(connection);
    _thread = std::thread([this]() {
      _client.run();
    });
    if (openedFuture.wait_for(DEFAULT_TIMEOUT) != std::future_status::ready) {
      _client.stop();
      _thread.join();
      throw std::runtime_error("failed to connect to " + uri);
    }
  }

  ~FoxgloveClient() {
    _client.stop();
    _thread.join();
  }

  std::optional<Channel> find(const std::string& topic) {
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto& [id, channel] : _channels) {
      if (channel.topic == topic) {
        return channel;
      }
    }
    return std::nullopt;
  }

  void subscribe(uint32_t subscriptionId, uint64_t channelId) {
    send({{"op", "subscribe"},
          {"subscriptions", {{{"id", subscriptionId}, {"channelId", channelId}}}}});
  }

  void unsubscribe(uint32_t subscriptionId) {
    send({{"op", "unsubscribe"}, {"subscriptionIds", {subscriptionId}}});
  }

  /// The next message on any subscription.
  std::future<Message> nextMessage() {
    std::lock_guard<std::mutex> lock(_mutex);
    _nextMessage.emplace();
    return _nextMessage->get_future();
  }

private:
  using WsClient = websocketpp::client<websocketpp::config::asio_client>;

  void send(const nlohmann::json& message) {
    _client.send(_connection, message.dump(), websocketpp::frame::opcode::text);
  }

  void onMessage(const WsClient::message_ptr& msg) {
    const auto& payload = msg->get_payload();
    std::lock_guard<std::mutex> lock(_mutex);
    if (msg->get_opcode() == websocketpp::frame::opcode::text) {
      const auto json = nlohmann::json::parse(payload);
      if (json["op"] == "advertise") {
        for (const auto& channel : json["channels"]) {
          const auto id = channel["id"].get<uint64_t>();
          _channels[id] = {id,
                           channel["topic"].get<std::string>(),
                           channel["encoding"].get<std::string>(),
                           channel["schemaName"].get<std::string>(),
                           channel["schema"].get<std::string>(),
                           channel.value("schemaEncoding", "")};
        }
      } else if (json["op"] == "unadvertise") {
        for (const auto& id : json["channelIds"]) {
          _channels.erase(id.get<uint64_t>());
        }
      }
      return;
    }
    // Opcode (1 byte), subscription ID (4), log time (8, little endian), payload.
    if (!_nextMessage || payload.size() < 13) {
      return;
    }
    Message message{0, {payload.begin() + 13, payload.end()}};
    for (int i = 7; i >= 0; --i) {
      message.logTime = (message.logTime << 8) | static_cast<uint8_t>(payload[5 + i]);
    }
    _nextMessage->set_value(std::move(message));
    _nextMessage.reset();
  }

  WsClient _client;
  websocketpp::connection_hdl _connection;
  std::thread _thread;
  std::mutex _mutex;
  std::map<uint64_t, Channel> _channels;
  std::optional<std::promise<Message>> _nextMessage;
};

}  // namespace

TEST(Iox2SourceTest, doesNotCreateServiceWithoutPublisher) {
  const auto serviceName = uniqueServiceName();
  PluginHost host(pluginParameters(serviceName, "/no_publisher"));
  FoxgloveClient client(host.uri());

  std::this_thread::sleep_for(SETTLE_TIME);
  EXPECT_FALSE(serviceExists(serviceName));
  EXPECT_FALSE(client.find("/no_publisher").has_value());
}

TEST(Iox2SourceTest, advertisesChannelWhilePublisherExists) {
  const auto serviceName = uniqueServiceName();
  PluginHost host(pluginParameters(serviceName, "/points"));
  FoxgloveClient client(host.uri());

  // Started after the plugin, with non-default service settings.
  auto publisher = std::make_unique<PublisherProcess>(serviceName, TEST_BFBS);
  ASSERT_TRUE(waitFor([&]() {
    return client.find("/points").has_value();
  }));
  const auto channel = client.find("/points").value();
  EXPECT_EQ("flatbuffer", channel.encoding);
  EXPECT_EQ(SCHEMA_NAME, channel.schemaName);
  EXPECT_EQ("flatbuffer", channel.schemaEncoding);
  EXPECT_EQ(readFile(TEST_BFBS), websocketpp::base64_decode(channel.schema));

  publisher.reset();
  ASSERT_TRUE(waitFor([&]() {
    return !client.find("/points").has_value();
  }));
}

TEST(Iox2SourceTest, readsParametersAndLogsUnderItsInstanceName) {
  const auto serviceName = uniqueServiceName();
  PublisherProcess publisher(serviceName, TEST_BFBS);
  PluginHost host(forInstance(pluginParameters(serviceName, "/lidar"), "lidar"), "lidar");
  FoxgloveClient client(host.uri());
  ASSERT_TRUE(waitFor([&]() {
    return client.find("/lidar").has_value();
  }));
  EXPECT_EQ(1u, countLogsContaining(RCUTILS_LOG_SEVERITY_INFO, "for topic \"/lidar\""));
  EXPECT_TRUE(host.node().has_parameter("lidar.service_names"));
  EXPECT_FALSE(host.node().has_parameter("iox2.service_names"));
}

TEST(Iox2SourceTest, forwardsPayloadOnlyWhileClientIsSubscribed) {
  const auto serviceName = uniqueServiceName();
  PublisherProcess publisher(serviceName, TEST_BFBS);
  PluginHost host(pluginParameters(serviceName, "/points"));
  FoxgloveClient client(host.uri());
  ASSERT_TRUE(waitFor([&]() {
    return client.find("/points").has_value();
  }));
  const auto channel = client.find("/points").value();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(0u, publisher.numberOfSubscribers());

  auto message = client.nextMessage();
  client.subscribe(1, channel.id);
  ASSERT_TRUE(waitFor([&]() {
    return publisher.numberOfSubscribers() == 1;
  }));
  EXPECT_EQ(1u, countLogsContaining(RCUTILS_LOG_SEVERITY_INFO,
                                    "Created iceoryx2 subscriber on service '" + serviceName));

  // The publisher process publishes {1, 2, 3, 4, 5} every 20 ms.
  const std::vector<uint8_t> data = {1, 2, 3, 4, 5};
  ASSERT_EQ(std::future_status::ready, message.wait_for(DEFAULT_TIMEOUT));
  const auto bytes = message.get().payload;
  flatbuffers::Verifier verifier(bytes.data(), bytes.size());
  ASSERT_TRUE(foxglove_bridge_iox2_plugin::test::VerifyTestPayloadBuffer(verifier));
  const auto* payload = foxglove_bridge_iox2_plugin::test::GetTestPayload(bytes.data());
  ASSERT_NE(nullptr, payload->data());
  EXPECT_EQ(data, std::vector<uint8_t>(payload->data()->begin(), payload->data()->end()));

  client.unsubscribe(1);
  EXPECT_TRUE(waitFor([&]() {
    return publisher.numberOfSubscribers() == 0;
  }));
  EXPECT_EQ(1u, countLogsContaining(RCUTILS_LOG_SEVERITY_INFO,
                                    "Removed iceoryx2 subscriber on service '" + serviceName));
}

TEST(Iox2SourceTest, logTimeFollowsSimTime) {
  const auto serviceName = uniqueServiceName();
  PublisherProcess publisher(serviceName, TEST_BFBS);
  auto parameters = pluginParameters(serviceName, "/sim_time");
  parameters.emplace_back("use_sim_time", true);
  PluginHost host(parameters);

  // Far from wall time, so a wall-clock log time cannot pass by accident.
  constexpr int64_t SIM_TIME_NS = 1'000'000'000'000;
  auto clockNode = std::make_shared<rclcpp::Node>("sim_clock_publisher");
  auto clockPublisher =
    clockNode->create_publisher<rosgraph_msgs::msg::Clock>("/clock", rclcpp::ClockQoS());
  rosgraph_msgs::msg::Clock clockMsg;
  clockMsg.clock = rclcpp::Time(SIM_TIME_NS);
  std::atomic<bool> stopClock = false;
  std::thread clockThread([&]() {
    while (!stopClock) {
      clockPublisher->publish(clockMsg);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  ASSERT_TRUE(waitFor([&]() {
    return host.node().now().nanoseconds() == SIM_TIME_NS;
  }));

  FoxgloveClient client(host.uri());
  ASSERT_TRUE(waitFor([&]() {
    return client.find("/sim_time").has_value();
  }));
  auto message = client.nextMessage();
  client.subscribe(1, client.find("/sim_time")->id);
  const auto ready = message.wait_for(DEFAULT_TIMEOUT);
  stopClock = true;
  clockThread.join();
  ASSERT_EQ(std::future_status::ready, ready);
  EXPECT_EQ(static_cast<uint64_t>(SIM_TIME_NS), message.get().logTime);
}

TEST(Iox2SourceTest, schemaMismatchIsReportedOnceAndLeavesServiceIntact) {
  const auto serviceName = uniqueServiceName();
  PublisherProcess publisher(serviceName, OTHER_BFBS);
  {
    PluginHost host(pluginParameters(serviceName, "/mismatch"));
    FoxgloveClient client(host.uri());
    std::this_thread::sleep_for(SETTLE_TIME);
    EXPECT_FALSE(client.find("/mismatch").has_value());
    EXPECT_EQ(1u, countErrorLogsContaining("'" + serviceName + "'"));
  }
  // In iceoryx2 v0.10.0, an open() with the wrong schema would have corrupted the service.
  {
    PluginHost host(pluginParameters(serviceName, "/mismatch", false, "other_payload.bfbs",
                                     "foxglove_bridge_iox2_plugin.test.OtherPayload"));
    FoxgloveClient client(host.uri());
    EXPECT_TRUE(waitFor([&]() {
      return client.find("/mismatch").has_value();
    }));
  }
}

TEST(Iox2SourceTest, persistingProblemIsRepeatedPeriodically) {
  const auto serviceName = uniqueServiceName();
  PublisherProcess publisher(serviceName, OTHER_BFBS);
  auto parameters = pluginParameters(serviceName, "/repeated");
  parameters.emplace_back("iox2.problem_repeat_interval_s", 1);
  PluginHost host(parameters);

  // Discovery runs every 200 ms; within 2.5 s an unchanged problem is logged at t=0, 1 and 2 s.
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  const auto errors = countErrorLogsContaining("'" + serviceName + "'");
  EXPECT_GE(errors, 2u);
  EXPECT_LE(errors, 4u);
}

TEST(Iox2SourceTest, nonOverflowingServiceRequiresOptIn) {
  const auto serviceName = uniqueServiceName();
  PublisherProcess publisher(serviceName, TEST_BFBS, /*safeOverflow=*/false);
  {
    PluginHost host(pluginParameters(serviceName, "/blocking"));
    FoxgloveClient client(host.uri());
    std::this_thread::sleep_for(SETTLE_TIME);
    EXPECT_FALSE(client.find("/blocking").has_value());
  }
  {
    PluginHost host(pluginParameters(serviceName, "/blocking", /*allowBlockingPublishers=*/true));
    FoxgloveClient client(host.uri());
    EXPECT_TRUE(waitFor([&]() {
      return client.find("/blocking").has_value();
    }));
  }
}

TEST(Iox2SourceTest, invalidParametersFailInitialization) {
  auto mismatchedLengths = pluginParameters(uniqueServiceName(), "/a");
  mismatchedLengths[2] = rclcpp::Parameter("iox2.topics", std::vector<std::string>{"/a", "/b"});
  {
    auto node = std::make_shared<rclcpp::Node>(
      "iox2_plugin_test", rclcpp::NodeOptions().parameter_overrides(mismatchedLengths));
    auto plugin = createPlugin();
    EXPECT_THROW(plugin->initialize(*node, "iox2", foxglove::Context::create()),
                 std::invalid_argument);
  }

  auto missingSchema = pluginParameters(uniqueServiceName(), "/a");
  missingSchema[4] =
    rclcpp::Parameter("iox2.bfbs_files", std::vector<std::string>{"does_not_exist.bfbs"});
  {
    auto node = std::make_shared<rclcpp::Node>(
      "iox2_plugin_test", rclcpp::NodeOptions().parameter_overrides(missingSchema));
    auto plugin = createPlugin();
    EXPECT_THROW(plugin->initialize(*node, "iox2", foxglove::Context::create()),
                 std::runtime_error);
  }
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  rclcpp::init(argc, argv);
  g_defaultLogHandler = rcutils_logging_get_output_handler();
  rcutils_logging_set_output_handler(captureLogs);
  const int result = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return result;
}
