#pragma once

#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <utility>

#include "cereal/types/memory.hpp"

class EventStream;
class Reactor;
class System;
class Connector;

extern thread_local Reactor* current_reactor_;

/**
 * Base class for messages.
 */
class Message {
 public:
  virtual ~Message() {}

  template <class Archive>
  void serialize(Archive &) {}

  /** Run-time type identification that is used for callbacks.
   *
   * Warning: this works because of the virtual destructor, don't remove it from this class
   */
  std::type_index GetTypeIndex() {
    return typeid(*this);
  }
};

/**
 * Write-end of a Connector (between two reactors).
 */
class Channel {
 public:
  /**
   * Construct and send the message to the channel.
   */
  template<typename MsgType, typename... Args>
  void Send(Args&&... args) {
    Send(std::unique_ptr<Message>(new MsgType(std::forward<Args>(args)...)));
  }

  virtual void Send(std::unique_ptr<Message> ptr) = 0;

  virtual std::string ReactorName() = 0;

  virtual std::string Name() = 0;

  void operator=(const Channel &) = delete;

  template <class Archive>
  void serialize(Archive &archive) {
    archive(ReactorName(), Name());
  }
};

/**
 * Read-end of a Connector (between two reactors).
 */
class EventStream {
 public:
  /**
   * Blocks until a message arrives.
   */
  virtual std::unique_ptr<Message> AwaitEvent() = 0;

  /**
   * Polls if there is a message available, returning null if there is none.
   */
  virtual std::unique_ptr<Message> PopEvent() = 0;

  /**
   * Get the name of the connector.
   */
  virtual const std::string &ConnectorName() = 0;
  /**
   * Subscription Service.
   *
   * Unsubscribe from a callback. Lightweight object (can copy by value).
   */
  class Subscription {
   public:
    /**
     * Unsubscribe. Call only once.
     */
    void unsubscribe() const;

   private:
    friend class Reactor;
    friend class Connector;

    Subscription(Connector &event_queue, std::type_index tidx, uint64_t cb_uid)
      : event_queue_(event_queue), tidx_(tidx), cb_uid_(cb_uid) { }

    Connector &event_queue_;
    std::type_index tidx_;
    uint64_t cb_uid_;
  };

  /**
   * Register a callback that will be called whenever an event arrives.
   */
  template<typename MsgType>
  void OnEvent(std::function<void(const MsgType&, const Subscription&)> &&cb) {
    OnEventHelper(typeid(MsgType), [cb = move(cb)](const Message &general_msg,
                                                   const Subscription &subscription) {
        const MsgType &correct_msg = dynamic_cast<const MsgType&>(general_msg);
        cb(correct_msg, subscription);
      });
  }

  class OnEventOnceChainer;
  /**
   * Starts a chain to register a callback that fires off only once.
   *
   * This method supports chaining (see the the class OnEventOnceChainer or the tests for examples).
   * Warning: when chaining callbacks, make sure that EventStream does not deallocate before the last
   * chained callback fired.
   */
  OnEventOnceChainer OnEventOnce() {
    return OnEventOnceChainer(*this);
  }

  /**
   * Close this event stream, disallowing further events from getting received.
   *
   * Any subsequent call after Close() to any function will be result in undefined
   * behavior (invalid pointer dereference). Can only be called from the thread
   * associated with the Reactor.
   */
  virtual void Close() = 0;

  /**
   * Convenience class to chain one-off callbacks.
   *
   * Usage: Create this class with OnEventOnce() and then chain callbacks using ChainOnce.
   * A callback will fire only once, unsubscribe and immediately subscribe the next callback to the stream.
   *
   * Example: stream->OnEventOnce().ChainOnce(firstCb).ChainOnce(secondCb);
   *
   * Implementation: This class is a temporary object that remembers the callbacks that are to be installed
   * and finally installs them in the destructor. Not sure is this kosher, is there another way?
   */
  class OnEventOnceChainer {
   public:
    OnEventOnceChainer(EventStream &event_stream) : event_stream_(event_stream) {}
    ~OnEventOnceChainer() {
      InstallCallbacks();
    }

    template<typename MsgType>
    OnEventOnceChainer &ChainOnce(std::function<void(const MsgType&)> &&cb) {
      std::function<void(const Message&, const Subscription&)> wrap =
        [cb = std::move(cb)](const Message &general_msg, const Subscription &subscription) {
          const MsgType &correct_msg = dynamic_cast<const MsgType&>(general_msg);
          subscription.unsubscribe();
          cb(correct_msg); // Warning: this can close the Channel, be careful what you put after it!
      };
      cbs_.emplace_back(typeid(MsgType), std::move(wrap));
      return *this;
    }

  private:
    void InstallCallbacks() {
      int num_callbacks = cbs_.size();
      assert(num_callbacks > 0); // We should install at least one callback, otherwise the usage is wrong?
      std::function<void(const Message&, const Subscription&)> next_cb = nullptr;
      std::type_index next_type = typeid(nullptr);

      for (int i = num_callbacks - 1; i >= 0; --i) {
        std::function<void(const Message&, const Subscription&)> tmp_cb = nullptr;
        tmp_cb = [cb = std::move(cbs_[i].second),
                  next_type,
                  next_cb = std::move(next_cb),
                  es_ptr = &this->event_stream_](const Message &msg, const Subscription &subscription) {
          cb(msg, subscription);
          if (next_cb != nullptr) {
            es_ptr->OnEventHelper(next_type, std::move(next_cb));
          }
        };
        next_cb = std::move(tmp_cb);
        next_type = cbs_[i].first;
      }

      event_stream_.OnEventHelper(next_type, std::move(next_cb));
    }

    EventStream &event_stream_;
    std::vector<std::pair<std::type_index, std::function<void(const Message&, const Subscription&)>>> cbs_;
  };
  typedef std::function<void(const Message&, const Subscription&)> Callback;

private:
  virtual void OnEventHelper(std::type_index tidx, Callback callback) = 0;
};

/**
 * Implementation of a connector.
 *
 * This class is an internal data structure that represents the state of the connector.
 * This class is not meant to be used by the clients of the messaging framework.
 * The Connector class wraps the event queue data structure, the mutex that protects
 * concurrent access to the event queue, the local channel and the event stream.
 * The class is owned by the Reactor. It gets closed when the owner reactor
 * (the one that owns the read-end of a connector) removes/closes it.
 */
class Connector {
 struct Params;

 public:
  friend class Reactor; // to create a Params initialization object
  friend class EventStream::Subscription;

  Connector(Params params)
      : connector_name_(params.connector_name),
        reactor_name_(params.reactor_name),
        mutex_(params.mutex),
        cvar_(params.cvar),
        stream_(mutex_, connector_name_, this) {}

  /**
   * LocalChannel represents the channels to reactors living in the same reactor system (write-end of the connectors).
   *
   * Sending messages to the local channel requires acquiring the mutex.
   * LocalChannel holds a (weak) pointer to the enclosing Connector object.
   * Messages sent to a closed channel are ignored.
   * There can be multiple LocalChannels refering to the same stream if needed.
   */
  class LocalChannel : public Channel {
   public:
    friend class Connector;

    LocalChannel(std::shared_ptr<std::mutex> mutex, std::string reactor_name,
                 std::string connector_name, std::weak_ptr<Connector> queue)
        : mutex_(mutex),
          reactor_name_(reactor_name),
          connector_name_(connector_name),
          weak_queue_(queue) {}

    virtual void Send(std::unique_ptr<Message> m) {
      std::shared_ptr<Connector> queue_ = weak_queue_.lock(); // Atomic, per the standard.
      if (queue_) {
        // We guarantee here that the Connector is not destroyed.
        std::unique_lock<std::mutex> lock(*mutex_);
        queue_->LockedPush(std::move(m));
      }
    }

    virtual std::string ReactorName();

    virtual std::string Name();

   private:
    std::shared_ptr<std::mutex> mutex_;
    std::string reactor_name_;
    std::string connector_name_;
    std::weak_ptr<Connector> weak_queue_;
  };

  /**
   * Implementation of the event stream.
   *
   * After the enclosing Connector object is destroyed (by a call to CloseChannel or Close).
   */
  class LocalEventStream : public EventStream {
   public:
    friend class Connector;

    LocalEventStream(std::shared_ptr<std::mutex> mutex, std::string connector_name,
      Connector *queue) : mutex_(mutex), connector_name_(connector_name), queue_(queue) {}
    std::unique_ptr<Message> AwaitEvent() {
      std::unique_lock<std::mutex> lock(*mutex_);
      return queue_->LockedAwaitPop(lock);
    }
    std::unique_ptr<Message> PopEvent() {
      std::unique_lock<std::mutex> lock(*mutex_);
      return queue_->LockedPop();
    }
    void OnEventHelper(std::type_index tidx, Callback callback) {
      std::unique_lock<std::mutex> lock(*mutex_);
      queue_->LockedOnEventHelper(tidx, callback);
    }
    const std::string &ConnectorName() {
      return queue_->connector_name_;
    }
    void Close();

   private:
    std::shared_ptr<std::mutex> mutex_;
    std::string connector_name_;
    Connector *queue_;
  };

  Connector(const Connector &other) = delete;
  Connector(Connector &&other) = default;
  Connector &operator=(const Connector &other) = delete;
  Connector &operator=(Connector &&other) = default;

private:
  /**
   * Initialization parameters to Connector.
   * Warning: do not forget to initialize self_ptr_ individually. Private because it shouldn't be created outside of a Reactor.
   */
  struct Params {
    std::string reactor_name;
    std::string connector_name;
    std::shared_ptr<std::mutex> mutex;
    std::shared_ptr<std::condition_variable> cvar;
  };


  void LockedPush(std::unique_ptr<Message> m) {
    queue_.emplace(std::move(m));
    // This is OK because there is only one Reactor (thread) that can wait on this Connector.
    cvar_->notify_one();
  }

  std::shared_ptr<LocalChannel> LockedOpenChannel() {
    assert(!self_ptr_.expired()); // TODO(zuza): fix this using this answer https://stackoverflow.com/questions/45507041/how-to-check-if-weak-ptr-is-empty-non-assigned
    return std::make_shared<LocalChannel>(mutex_, reactor_name_, connector_name_, self_ptr_);
  }

  std::unique_ptr<Message> LockedAwaitPop(std::unique_lock<std::mutex> &lock) {
    while (true) {
      std::unique_ptr<Message> m = LockedRawPop();
      if (!m) {
        cvar_->wait(lock);
      } else {
        return m;
      }
    }
  }

  std::unique_ptr<Message> LockedPop() {
    return LockedRawPop();
  }

  void LockedOnEventHelper(std::type_index tidx, EventStream::Callback callback) {
    uint64_t cb_uid = next_cb_uid++;
    callbacks_[tidx][cb_uid] = callback;
  }

  std::unique_ptr<Message> LockedRawPop() {
    if (queue_.empty()) return nullptr;
    std::unique_ptr<Message> t = std::move(queue_.front());
    queue_.pop();
    return t;
  }

  void RemoveCb(const EventStream::Subscription &subscription) {
    std::unique_lock<std::mutex> lock(*mutex_);
    size_t num_erased = callbacks_[subscription.tidx_].erase(subscription.cb_uid_);
    assert(num_erased == 1);
  }

  std::string connector_name_;
  std::string reactor_name_;
  std::queue<std::unique_ptr<Message>> queue_;
  // Should only be locked once since it's used by a cond. var. Also caught in dctor, so must be recursive.
  std::shared_ptr<std::mutex> mutex_;
  std::shared_ptr<std::condition_variable> cvar_;
  /**
   * A weak_ptr to itself.
   *
   * There are initialization problems with this, check Params.
   */
  std::weak_ptr<Connector> self_ptr_;
  LocalEventStream stream_;
  std::unordered_map<std::type_index, std::unordered_map<uint64_t, EventStream::Callback> > callbacks_;
  uint64_t next_cb_uid = 0;
};

/**
 * A single unit of concurrent execution in the system.
 *
 * E.g. one worker, one client. Owned by System. Has a thread associated with it.
 */
class Reactor {
 public:
  friend class System;

  Reactor(std::string name)
      : name_(name), main_(Open("main")) {}

  virtual ~Reactor() {}

  virtual void Run() = 0;

  std::pair<EventStream*, std::shared_ptr<Channel>> Open(const std::string &s);
  std::pair<EventStream*, std::shared_ptr<Channel>> Open();
  const std::shared_ptr<Channel> FindChannel(const std::string &channel_name);

  /**
   * Close a connector by name.
   *
   * Should only be called from the Reactor thread.
   */
  void CloseConnector(const std::string &s);

  /**
   * close all connectors (typically during shutdown).
   *
   * Should only be called from the Reactor thread.
   */
  void CloseAllConnectors();

  /**
   * Get Reactor name
   */
  const std::string &name() { return name_; }

  Reactor(const Reactor &other) = delete;
  Reactor(Reactor &&other) = default;
  Reactor &operator=(const Reactor &other) = delete;
  Reactor &operator=(Reactor &&other) = default;

 protected:
  std::string name_;
  /*
   * Locks all Reactor data, including all Connector's in connectors_.
   *
   * This should be a shared_ptr because LocalChannel can outlive Reactor.
   */
  std::shared_ptr<std::mutex> mutex_ =
      std::make_shared<std::mutex>();
  std::shared_ptr<std::condition_variable> cvar_ =
      std::make_shared<std::condition_variable>();

  /**
   * List of connectors of a reactor indexed by name.
   *
   * While the connectors are owned by the reactor, a shared_ptr to solve the circular reference problem
   * between Channels and EventStreams.
   */
  std::unordered_map<std::string, std::shared_ptr<Connector>> connectors_;
  int64_t connector_name_counter_{0};
  std::pair<EventStream*, std::shared_ptr<Channel>> main_;

 private:
  typedef std::pair<std::unique_ptr<Message>,
                    std::vector<std::pair<EventStream::Callback, EventStream::Subscription> > > MsgAndCbInfo;

  /**
   * Dispatches all waiting messages to callbacks. Shuts down when there are no callbacks left.
   */
  void RunEventLoop();

  // TODO: remove proof of locking evidence ?!
  MsgAndCbInfo LockedGetPendingMessages();
};


/**
 * Global placeholder for all reactors in the system.
 *
 * E.g. holds set of reactors, channels for all reactors.
 * Alive through the entire process lifetime.
 * Singleton class. Created automatically on first use.
 */
class System {
 public:
  friend class Reactor;

  /**
   * Get the (singleton) instance of System.
   *
   * More info: https://stackoverflow.com/questions/1008019/c-singleton-design-pattern
   */
  static System &GetInstance() {
    static System system; // guaranteed to be destroyed, initialized on first use
    return system;
  }

  template <class ReactorType, class... Args>
  const std::shared_ptr<Channel> Spawn(const std::string &name,
                                       Args &&... args) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    auto *raw_reactor =
        new ReactorType(name, std::forward<Args>(args)...);
    std::unique_ptr<Reactor> reactor(raw_reactor);
    // Capturing a pointer isn't ideal, I would prefer to capture a Reactor&, but not sure how to do it.
    std::thread reactor_thread(
        [this, raw_reactor]() { this->StartReactor(*raw_reactor); });
    assert(reactors_.count(name) == 0);
    reactors_.emplace(name, std::pair<std::unique_ptr<Reactor>, std::thread>
                      (std::move(reactor), std::move(reactor_thread)));
    return nullptr;
  }

  const std::shared_ptr<Channel> FindChannel(const std::string &reactor_name,
                                             const std::string &channel_name) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    auto it_reactor = reactors_.find(reactor_name);
    if (it_reactor == reactors_.end()) return nullptr;
    return it_reactor->second.first->FindChannel(channel_name);
  }

  void AwaitShutdown() {
    for (auto &key_value : reactors_) {
      auto &thread = key_value.second.second;
      thread.join();
    }
    reactors_.clear(); // for testing, since System is a singleton now
  }

 private:
  System() {}
  System(const System &) = delete;
  System(System &&) = delete;
  System &operator=(const System &) = delete;
  System &operator=(System &&) = delete;

  void StartReactor(Reactor &reactor) {
    current_reactor_ = &reactor;
    reactor.Run();
    reactor.RunEventLoop();  // Activate callbacks.
  }

  std::recursive_mutex mutex_;
  // TODO: Replace with a map to a reactor Connector map to have more granular
  // locking.
  std::unordered_map<std::string,
                     std::pair<std::unique_ptr<Reactor>, std::thread>>
      reactors_;
};