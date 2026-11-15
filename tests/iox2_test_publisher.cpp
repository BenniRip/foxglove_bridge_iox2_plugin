// Usage: iox2_test_publisher <service_name> <schema_path> <safe_overflow 0|1>
//
// Creates the service with non-default settings and publishes a TestPayload every 20 ms. Prints
// "ready", "subscribers=<n>" on every change, or "error=<message>". Stops on SIGTERM or SIGINT.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <iox2/bb/file_path.hpp>
#include <iox2/enum_translation.hpp>
#include <iox2/node.hpp>
#include <iox2/port_factory_publish_subscribe.hpp>
#include <iox2/publisher.hpp>
#include <iox2/sample_mut.hpp>
#include <iox2/sample_mut_uninit.hpp>
#include <iox2/service_name.hpp>
#include <iox2/signal_handling_mode.hpp>

#include "test_payload_generated.h"

namespace {

std::atomic<bool> g_stop = false;

void requestStop(int /*signal*/) {
  g_stop = true;
}

int fail(const std::string& message) {
  std::cout << "error=" << message << std::endl;
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    return fail("usage: iox2_test_publisher <service_name> <schema_path> <safe_overflow 0|1>");
  }
  std::signal(SIGTERM, requestStop);
  std::signal(SIGINT, requestStop);

  using Payload = iox2::Flatbuffer<foxglove_bridge_iox2_plugin::test::TestPayload>;
  auto node = iox2::NodeBuilder()
                .signal_handling_mode(iox2::SignalHandlingMode::Disabled)
                .create<iox2::ServiceType::Ipc>();
  if (!node.has_value()) {
    return fail(iox2::bb::into<const char*>(node.error()));
  }
  auto serviceName = iox2::ServiceName::create(argv[1]);
  auto schemaPath = iox2::bb::FilePath::create(argv[2]);
  if (!serviceName.has_value() || !schemaPath.has_value()) {
    return fail("invalid service name or schema path");
  }
  auto service = node.value()
                   .service_builder(serviceName.value())
                   .publish_subscribe<Payload>()
                   .flatbuffer_schema_path(schemaPath.value())
                   .history_size(1)
                   .subscriber_max_buffer_size(4)
                   .max_subscribers(4)
                   .enable_safe_overflow(std::string(argv[3]) == "1")
                   .create();
  if (!service.has_value()) {
    return fail(iox2::bb::into<const char*>(service.error()));
  }
  auto publisher = service.value().publisher_builder().initial_reserved_memory(1024).create();
  if (!publisher.has_value()) {
    return fail(iox2::bb::into<const char*>(publisher.error()));
  }

  std::cout << "ready" << std::endl;
  const std::vector<uint8_t> data = {1, 2, 3, 4, 5};
  uint64_t lastSubscriberCount = 0;
  for (uint32_t seq = 0; !g_stop; ++seq) {
    const auto subscriberCount = service.value().dynamic_config().number_of_subscribers();
    if (subscriberCount != lastSubscriberCount) {
      lastSubscriberCount = subscriberCount;
      std::cout << "subscribers=" << subscriberCount << std::endl;
    }
    auto sample = publisher.value().loan_flatbuffer();
    if (sample.has_value()) {
      auto root = foxglove_bridge_iox2_plugin::test::CreateTestPayloadDirect(
        sample.value().flatbuffer_builder(), seq, &data);
      static_cast<void>(iox2::send(iox2::assume_init(std::move(sample.value()), root)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return 0;
}
