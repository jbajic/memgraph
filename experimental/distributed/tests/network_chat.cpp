#include "communication.hpp"

class ChatMessage : public SenderMessage {
 public:
  ChatMessage() : SenderMessage(), message_("") {}

  ChatMessage(std::string reactor, std::string channel, std::string message)
      : SenderMessage(reactor, channel), message_(message) {}

  std::string Message() const { return message_; }

  template <class Archive>
  void serialize(Archive &ar) {
    ar(cereal::base_class<SenderMessage>(this), message_);
  }

 private:
  std::string message_;
};
CEREAL_REGISTER_TYPE(ChatMessage);

class ChatACK : public ChatMessage {
 public:
  ChatACK() : ChatMessage() {}

  ChatACK(std::string reactor, std::string channel, std::string message)
      : ChatMessage(reactor, channel, message) {}

  template <class Archive>
  void serialize(Archive &ar) {
    ar(cereal::base_class<ChatMessage>(this));
  }
};
CEREAL_REGISTER_TYPE(ChatACK);

class ChatServer : public Reactor {
 public:
  ChatServer(System *system, std::string name) : Reactor(system, name) {}

  virtual void Run() {
    std::cout << "ChatServer is active" << std::endl;

    auto chat = Open("chat").first;

    while (true) {
      auto m = chat->AwaitEvent();
      if (ChatACK *ack = dynamic_cast<ChatACK *>(m.get())) {
        std::cout << "Received ACK from " << ack->Address() << ":"
                  << ack->Port() << " -> '" << ack->Message() << "'"
                  << std::endl;
      } else if (ChatMessage *msg = dynamic_cast<ChatMessage *>(m.get())) {
        std::cout << "Received message from " << msg->Address() << ":"
                  << msg->Port() << " -> '" << msg->Message() << "'"
                  << std::endl;
        auto channel = msg->GetChannelToSender(system_);
        if (channel != nullptr) {
          channel->Send<ChatACK>("server", "chat", msg->Message());
        }
      } else {
        std::cerr << "Unknown message received!\n";
        exit(1);
      }
    }
  }
};

class ChatClient : public Reactor {
 public:
  ChatClient(System *system, std::string name) : Reactor(system, name) {}

  virtual void Run() {
    std::cout << "ChatClient is active" << std::endl;

    std::string address, message;
    uint16_t port;
    while (true) {
      std::cout << "Enter IP, port and message to send." << std::endl;
      std::cin >> address >> port >> message;

      auto channel =
          system_->network().Resolve(address, port, "server", "chat");
      if (channel != nullptr) {
        channel->Send<ChatMessage>("server", "chat", message);
      } else {
        std::cerr << "Couldn't resolve that server!" << std::endl;
      }
    }
  }
};

int main(int argc, char *argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  System system;
  system.StartServices();
  system.Spawn<ChatServer>("server");
  system.Spawn<ChatClient>("client");
  system.AwaitShutdown();
  return 0;
}