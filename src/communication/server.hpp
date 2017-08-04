#pragma once

#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <glog/logging.h>

#include "communication/worker.hpp"
#include "io/network/event_listener.hpp"
#include "utils/assert.hpp"

namespace communication {

/**
 * TODO (mferencevic): document methods
 */

/**
 * Communication server.
 * Listens for incomming connections on the server port and assings them in a
 * round-robin manner to it's workers.
 *
 * Current Server achitecture:
 * incomming connection -> server -> worker -> session
 *
 * @tparam Session the server can handle different Sessions, each session
 *         represents a different protocol so the same network infrastructure
 *         can be used for handling different protocols
 * @tparam Socket the input/output socket that should be used
 * @tparam SessionData the class with objects that will be forwarded to the session
 */
template <typename Session, typename Socket, typename SessionData>
class Server
    : public io::network::EventListener<Server<Session, Socket, SessionData>> {
  using Event = io::network::Epoll::Event;

 public:
  Server(Socket &&socket, SessionData &session_data)
      : socket_(std::forward<Socket>(socket)),
        session_data_(session_data) {
    event_.data.fd = socket_;

    // TODO: EPOLLET is hard to use -> figure out how should EPOLLET be used
    // event.events = EPOLLIN | EPOLLET;
    event_.events = EPOLLIN;

    this->listener_.Add(socket_, &event_);
  }

  void Start(size_t n) {
    std::cout << fmt::format("Starting {} workers", n) << std::endl;
    workers_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      workers_.push_back(
          std::make_unique<Worker<Session, Socket, SessionData>>(
              session_data_));
      workers_.back()->Start(alive_);
    }
    std::cout << "Server is fully armed and operational" << std::endl;
    std::cout << fmt::format("Listening on {} at {}",
                             socket_.endpoint().address(),
                             socket_.endpoint().port())
              << std::endl;
    while (alive_) {
      this->WaitAndProcessEvents();
    }

    std::cout << "Shutting down..." << std::endl;
    for (auto &worker : workers_) worker->thread_.join();
  }

  void Shutdown() {
    // This should be as simple as possible, so that it can be called inside a
    // signal handler.
    alive_.store(false);
  }

  void OnConnect() {
    debug_assert(idx_ < workers_.size(), "Invalid worker id.");

    DLOG(INFO) << "on connect";

    if (UNLIKELY(!workers_[idx_]->Accept(socket_))) return;

    idx_ = idx_ == static_cast<int>(workers_.size()) - 1 ? 0 : idx_ + 1;
  }

  void OnWaitTimeout() {}

  void OnDataEvent(Event &event) {
    if (UNLIKELY(socket_ != event.data.fd)) return;

    this->derived().OnConnect();
  }

  template <class... Args>
  void OnExceptionEvent(Event &, Args &&...) {
    // TODO: Do something about it
    DLOG(WARNING) << "epoll exception";
  }

  void OnCloseEvent(Event &event) { close(event.data.fd); }

  void OnErrorEvent(Event &event) { close(event.data.fd); }

 private:
  std::vector<typename Worker<Session, Socket, SessionData>::uptr> workers_;
  std::atomic<bool> alive_{true};
  int idx_{0};

  Socket socket_;
  Event event_;
  SessionData &session_data_;
};

}  // namespace communication
