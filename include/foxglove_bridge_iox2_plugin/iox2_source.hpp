#pragma once

#include <chrono>
#include <condition_variable>
#include <expected>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <iox2/deployment/iceoryx2_cxx_feature_flags.hpp>
#include <iox2/marker.hpp>
#include <iox2/port_factory_publish_subscribe.hpp>
#include <iox2/subscriber.hpp>
#include <rclcpp/rclcpp.hpp>

#include <foxglove/channel.hpp>
#include <foxglove/context.hpp>
#include <foxglove_bridge/channel_source_plugin.hpp>

#include "foxglove_bridge_iox2_plugin/iox2_node.hpp"
#include "foxglove_bridge_iox2_plugin/iox2_source_settings.hpp"
#include "foxglove_bridge_iox2_plugin/plugin_support/problem_log.hpp"

#if !IOX2_FEATURE_FLATBUFFERS
#error "foxglove_bridge_iox2_plugin requires iceoryx2-cxx built with IOX2_FEATURE_FLATBUFFERS=ON"
#endif

namespace foxglove_bridge_iox2_plugin {

/// Serves iceoryx2 FlatBuffer services as Foxglove channels; see README.md.
class Iox2Source final : public foxglove_bridge::ChannelSourcePlugin {
public:
  ~Iox2Source() override;

  void initialize(rclcpp::Node& node, const std::string& name,
                  const foxglove::Context& context) override;
  void shutdown() override;

private:
  /// iceoryx2 registers every `iox2::Flatbuffer<T>` under one type name, so any T opens a service.
  struct OpaqueFlatbuffer {};

  using Payload = iox2::Flatbuffer<OpaqueFlatbuffer>;
  using PortFactory = iox2::PortFactoryPublishSubscribe<iox2::ServiceType::Ipc, Payload, void>;
  using Subscriber = iox2::Subscriber<iox2::ServiceType::Ipc, Payload, void>;

  struct ActiveService {
    foxglove::RawChannel channel;
    // Must outlive the subscriber.
    PortFactory service;
    std::optional<Subscriber> subscriber{};
  };

  struct ServiceState {
    ServiceConfig config;
    std::optional<ActiveService> active{};
    ProblemLog problems{};
    bool receiveErrorLogged = false;
  };

  void discoveryLoop(std::stop_token stop);
  void updateServices();
  /// Opens the service and advertises its channel if a publisher exists and is compatible.
  std::expected<void, Problem> tryActivate(ServiceState& state);
  /// Checks the service before open(), which registers the bridge with it.
  std::expected<void, Problem> checkService(const ServiceConfig& config);
  std::expected<PortFactory, Problem> openService(const ServiceConfig& config);
  void reportProblem(ServiceState& state, const Problem& problem);
  void poll();
  /// Creates the subscriber while a client is subscribed and removes it otherwise. Returns
  /// whether a subscriber exists afterwards.
  bool updateSubscriber(ServiceState& state);
  void forwardSamples(ServiceState& state);
  void deactivateAll();

  rclcpp::Logger _logger = rclcpp::get_logger("iox2");
  // The bridge node's clock, so log times follow use_sim_time.
  rclcpp::Clock::SharedPtr _clock;
  foxglove::Context _context;
  bool _allowBlockingPublishers = false;
  std::chrono::seconds _problemRepeatInterval{};

  // Guards _services and the iceoryx2 objects, which are not thread-safe.
  std::mutex _mutex;
  std::optional<iox2::Config> _iox2Config;
  std::optional<IpcNode> _iox2Node;
  std::vector<ServiceState> _services;

  std::mutex _discoveryWakeMutex;
  std::condition_variable_any _discoveryWake;
  std::jthread _discoveryThread;
  rclcpp::CallbackGroup::SharedPtr _callbackGroup;
  rclcpp::TimerBase::SharedPtr _pollTimer;
};

}  // namespace foxglove_bridge_iox2_plugin
