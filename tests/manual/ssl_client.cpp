#include <gflags/gflags.h>
#include <glog/logging.h>

#include "communication/client.hpp"
#include "io/network/endpoint.hpp"
#include "utils/timer.hpp"

DEFINE_string(address, "127.0.0.1", "Server address");
DEFINE_int32(port, 54321, "Server port");
DEFINE_string(cert_file, "", "Certificate file to use.");
DEFINE_string(key_file, "", "Key file to use.");

bool EchoMessage(communication::Client &client, const std::string &data) {
  uint16_t size = data.size();
  if (!client.Write(reinterpret_cast<const uint8_t *>(&size), sizeof(size))) {
    LOG(WARNING) << "Couldn't send data size!";
    return false;
  }
  if (!client.Write(data)) {
    LOG(WARNING) << "Couldn't send data!";
    return false;
  }

  client.ClearData();
  if (!client.Read(size)) {
    LOG(WARNING) << "Couldn't receive data!";
    return false;
  }
  if (std::string(reinterpret_cast<const char *>(client.GetData()), size) !=
      data) {
    LOG(WARNING) << "Received data isn't equal to sent data!";
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  communication::Init();

  io::network::Endpoint endpoint(FLAGS_address, FLAGS_port);

  communication::ClientContext context(FLAGS_key_file, FLAGS_cert_file);
  communication::Client client(&context);

  if (!client.Connect(endpoint)) return 1;

  bool success = true;
  while (true) {
    std::string s;
    std::getline(std::cin, s);
    if (s == "") break;
    if (!EchoMessage(client, s)) {
      success = false;
      break;
    }
  }

  // Send server shutdown signal. The call will fail, we don't care.
  EchoMessage(client, "");

  return success ? 0 : 1;
}