#include "foxglove_bridge_iox2_plugin/iox2_source.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <format>
#include <utility>

#include <iox2/enum_translation.hpp>
#include <iox2/messaging_pattern.hpp>
#include <iox2/service.hpp>
#include <pluginlib/class_list_macros.hpp>

#include <foxglove/error.hpp>

#include "foxglove_bridge_iox2_plugin/iox2_service_storage.hpp"

namespace foxglove_bridge_iox2_plugin {
namespace {

// Matches the bridge's ROS graph polling interval.
constexpr auto DISCOVERY_INTERVAL = std::chrono::milliseconds(200);
// So a fast publisher cannot monopolize the executor.
constexpr size_t MAX_SAMPLES_PER_POLL = 64;

Problem schemaMismatch(const std::string& bfbsPath) {
  return {Problem::Severity::Error,
          std::format("schema '{}' is not byte-identical to the schema the publisher registered "
                      "with iceoryx2",
                      bfbsPath)};
}

/// Fails unless the publisher registered exactly our schema; see readRegisteredSchema().
std::expected<void, Problem> checkRegisteredSchema(iox2::Config& iox2Config,
                                                   const std::string& serviceId,
                                                   const ServiceConfig& config) {
  const auto registeredSchema = readRegisteredSchema(iox2Config, serviceId);
  if (!registeredSchema) {
    return std::unexpected(Problem{
      Problem::Severity::Error,
      std::format("cannot read the schema registered with the service ({}), so it is not opened. "
                  "The bridge must run as the same user as the publisher",
                  registeredSchema.error())});
  }
  if (*registeredSchema != config.bfbs) {
    return std::unexpected(schemaMismatch(config.bfbsPath));
  }
  return {};
}

std::expected<foxglove::RawChannel, Problem> createChannel(const ServiceConfig& config,
                                                           const foxglove::Context& context) {
  const foxglove::Schema schema{config.schemaName, "flatbuffer",
                                reinterpret_cast<const std::byte*>(config.bfbs.data()),
                                config.bfbs.size()};
  auto channel = foxglove::RawChannel::create(config.topic, "flatbuffer", schema, context);
  if (!channel.has_value()) {
    return std::unexpected(
      Problem{Problem::Severity::Error,
              std::format("failed to create channel: {}", foxglove::strerror(channel.error()))});
  }
  return std::move(channel.value());
}

}  // namespace

Iox2Source::~Iox2Source() {
  shutdown();
}

void Iox2Source::initialize(rclcpp::Node& node, const std::string& name,
                            const foxglove::Context& context) {
  _logger = node.get_logger().get_child(name);
  _clock = node.get_clock();
  _context = context;

  InstanceParameters parameters(node, name);
  auto settings = loadSettings(parameters);
  _allowBlockingPublishers = settings.allowBlockingPublishers;
  _problemRepeatInterval = settings.problemRepeatInterval;
  if (settings.services.empty()) {
    RCLCPP_INFO(_logger, "No iceoryx2 services configured (%s is empty)",
                parameters.name(PARAM_SERVICE_NAMES).c_str());
    return;
  }
  _iox2Config = loadIox2Config(settings.configFile);
  _iox2Node.emplace(createIox2Node(*_iox2Config));

  for (auto& config : settings.services) {
    _services.push_back(ServiceState{.config = std::move(config)});
  }

  _callbackGroup = node.create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  _pollTimer = node.create_wall_timer(
    settings.pollInterval,
    [this]() {
      poll();
    },
    _callbackGroup);

  _discoveryThread = std::jthread([this](std::stop_token stop) {
    discoveryLoop(stop);
  });
  RCLCPP_INFO(_logger, "Bridging up to %zu iceoryx2 service(s)", _services.size());
}

void Iox2Source::discoveryLoop(std::stop_token stop) {
  while (!stop.stop_requested()) {
    try {
      updateServices();
    } catch (const std::exception& ex) {
      RCLCPP_ERROR(_logger, "Exception while updating iceoryx2 services: %s", ex.what());
    }
    std::unique_lock lock(_discoveryWakeMutex);
    _discoveryWake.wait_for(lock, stop, DISCOVERY_INTERVAL, [] {
      return false;
    });
  }
}

void Iox2Source::updateServices() {
  // Closed outside _mutex, since close() runs the server's unsubscribe callbacks.
  std::vector<foxglove::RawChannel> channelsToClose;
  {
    std::lock_guard lock(_mutex);
    for (auto& state : _services) {
      if (state.active.has_value()) {
        if (state.active->service.dynamic_config().number_of_publishers() > 0) {
          continue;
        }
        RCLCPP_INFO(_logger,
                    "Removing channel %" PRIu64
                    " for topic \"%s\": iceoryx2 service '%s' has no publishers left",
                    state.active->channel.id(), state.config.topic.c_str(),
                    state.config.serviceName.c_str());
        channelsToClose.push_back(std::move(state.active->channel));
        state.active.reset();
        state.problems.clear();
        continue;
      }
      if (auto activated = tryActivate(state); !activated) {
        reportProblem(state, activated.error());
      }
    }
  }
  for (auto& channel : channelsToClose) {
    channel.close();
  }
}

std::expected<void, Problem> Iox2Source::tryActivate(ServiceState& state) {
  const auto& config = state.config;
  if (auto compatible = checkService(config); !compatible) {
    return compatible;
  }
  auto service = openService(config);
  if (!service) {
    return std::unexpected(service.error());
  }
  auto channel = createChannel(config, _context);
  if (!channel) {
    return std::unexpected(channel.error());
  }
  RCLCPP_INFO(_logger,
              "Advertising new channel %" PRIu64 " for topic \"%s\" (iceoryx2 service '%s')",
              channel->id(), config.topic.c_str(), config.serviceName.c_str());
  state.active.emplace(
    ActiveService{.channel = std::move(*channel), .service = std::move(*service)});
  state.problems.clear();
  state.receiveErrorLogged = false;
  return {};
}

std::expected<void, Problem> Iox2Source::checkService(const ServiceConfig& config) {
  using enum Problem::Severity;
  auto details = iox2::Service<iox2::ServiceType::Ipc>::details(
    config.iox2ServiceName, _iox2Node->config(), iox2::MessagingPattern::PublishSubscribe);
  if (!details.has_value()) {
    return std::unexpected(Problem{
      Warn,
      std::format("service discovery failed: {}", iox2::bb::into<const char*>(details.error()))});
  }
  if (!details.value().has_value()) {
    return std::unexpected(Problem{Debug, "service does not exist"});
  }
  const auto pubSubConfig = details.value()->static_details.publish_subscribe();
  const std::string payloadType = pubSubConfig.message_type_details().payload().type_name();
  if (payloadType != Payload::IOX2_TYPE_NAME) {
    return std::unexpected(Problem{
      Error, std::format("payload type is '{}', not an iox2::Flatbuffer payload", payloadType)});
  }
  if (!pubSubConfig.has_safe_overflow() && !_allowBlockingPublishers) {
    return std::unexpected(
      Problem{Error, std::format("safe overflow is disabled, so its publishers could block when "
                                 "the bridge falls behind. Set {} to bridge it anyway",
                                 PARAM_ALLOW_BLOCKING_PUBLISHERS)});
  }
  return checkRegisteredSchema(*_iox2Config, details.value()->static_details.id(), config);
}

std::expected<Iox2Source::PortFactory, Problem> Iox2Source::openService(
  const ServiceConfig& config) {
  using enum Problem::Severity;
  // Never created: publishers own the service.
  auto service = _iox2Node->service_builder(config.iox2ServiceName)
                   .publish_subscribe<Payload>()
                   .flatbuffer_schema_path(config.iox2BfbsPath)
                   .open();
  if (!service.has_value()) {
    switch (service.error()) {
      case iox2::PublishSubscribeOpenError::DoesNotExist:
        return std::unexpected(Problem{Debug, "service does not exist"});
      case iox2::PublishSubscribeOpenError::IncompatibleTypes:
        return std::unexpected(schemaMismatch(config.bfbsPath));
      default:
        return std::unexpected(Problem{
          Error,
          std::format("failed to open service: {}", iox2::bb::into<const char*>(service.error()))});
    }
  }
  if (service.value().dynamic_config().number_of_publishers() == 0) {
    return std::unexpected(Problem{Debug, "service has no publishers"});
  }
  return std::move(service.value());
}

void Iox2Source::reportProblem(ServiceState& state, const Problem& problem) {
  state.problems.report(_logger, std::format("iceoryx2 service '{}'", state.config.serviceName),
                        problem, _problemRepeatInterval);
}

void Iox2Source::poll() {
  std::lock_guard lock(_mutex);
  for (auto& state : _services) {
    if (state.active.has_value() && updateSubscriber(state)) {
      forwardSamples(state);
    }
  }
}

bool Iox2Source::updateSubscriber(ServiceState& state) {
  auto& active = *state.active;
  const bool watched = active.channel.hasSinks();
  if (watched == active.subscriber.has_value()) {
    return watched;
  }
  if (!watched) {
    active.subscriber.reset();
    RCLCPP_INFO(_logger,
                "Removed iceoryx2 subscriber on service '%s' for channel %" PRIu64
                ": no client subscribed",
                state.config.serviceName.c_str(), active.channel.id());
    return false;
  }
  auto subscriber = active.service.subscriber_builder().create();
  if (!subscriber.has_value()) {
    reportProblem(state, Problem{Problem::Severity::Error,
                                 std::format("failed to create subscriber: {}",
                                             iox2::bb::into<const char*>(subscriber.error()))});
    return false;
  }
  active.subscriber.emplace(std::move(subscriber.value()));
  RCLCPP_INFO(_logger, "Created iceoryx2 subscriber on service '%s' for channel %" PRIu64,
              state.config.serviceName.c_str(), active.channel.id());
  return true;
}

void Iox2Source::forwardSamples(ServiceState& state) {
  auto& active = *state.active;
  for (size_t i = 0; i < MAX_SAMPLES_PER_POLL; ++i) {
    auto sample = active.subscriber->receive();
    if (!sample.has_value()) {
      // E.g. right after a publisher crashed; the next poll retries.
      if (!state.receiveErrorLogged) {
        state.receiveErrorLogged = true;
        RCLCPP_WARN(_logger, "Receiving from iceoryx2 service '%s' failed (%s); retrying",
                    state.config.serviceName.c_str(), iox2::bb::into<const char*>(sample.error()));
      }
      return;
    }
    state.receiveErrorLogged = false;
    if (!sample.value().has_value()) {
      return;
    }
    const auto logTime = _clock->now().nanoseconds();
    const auto bytes = sample.value()->payload_bytes();
    active.channel.log(reinterpret_cast<const std::byte*>(bytes.data()), bytes.number_of_bytes(),
                       static_cast<uint64_t>(std::max<int64_t>(logTime, 0)));
  }
}

void Iox2Source::shutdown() {
  if (_pollTimer) {
    _pollTimer->cancel();
  }
  if (_discoveryThread.joinable()) {
    _discoveryThread.request_stop();
    _discoveryThread.join();
  }
  deactivateAll();
}

void Iox2Source::deactivateAll() {
  std::vector<foxglove::RawChannel> channelsToClose;
  {
    std::lock_guard lock(_mutex);
    for (auto& state : _services) {
      if (state.active.has_value()) {
        channelsToClose.push_back(std::move(state.active->channel));
        state.active.reset();
      }
    }
    _iox2Node.reset();
    _iox2Config.reset();
  }
  for (auto& channel : channelsToClose) {
    channel.close();
  }
}

}  // namespace foxglove_bridge_iox2_plugin

PLUGINLIB_EXPORT_CLASS(foxglove_bridge_iox2_plugin::Iox2Source,
                       foxglove_bridge::ChannelSourcePlugin)
