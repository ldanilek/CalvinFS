// Author: Alex Thomson
//
// A Machine is the basic unit of participation in a distributed application.
//
// Generally a machine deployment will have one Machine object per process, and
// will run one process per physical server.
//
// A machine essentially consists of a pool of worker threads that execute
// RPCs, plus connections to all other machines in the cluster.
//

#ifndef CALVIN_MACHINE_MACHINE_H_
#define CALVIN_MACHINE_MACHINE_H_

#include <atomic>
#include <map>
#include <string>

#include "common/atomic.h"
#include "common/types.h"
#include "machine/cluster_config.h"
#include "proto/header.pb.h"
#include "proto/start_app.pb.h"

class App;
class Connection;
class MessageBuffer;
class ThreadPool;

class Machine {
 public:
  // Constructs a Machine for the particular ClusterConfig.
  // Requires: 'config' contains an machine with id 'id'
  Machine(uint64 machine_id, const ClusterConfig& config);

  // Optionally, pass existing ThreadPool/connection objects to the Machine,
  // which takes ownership of these provided components rather than creating
  // its own. Mainly useful for testing.
  Machine(
      uint64 machine_id,
      const ClusterConfig& config,
      ThreadPool* tp,
      Connection* connection);

  // Default: cluster consists of only this machine.
  Machine();

  ~Machine();

  // Returns this Machine's id.
  inline uint64 machine_id() { return machine_id_; }

  // Returns the config for this Machine's cluster.
  inline const ClusterConfig& config() { return config_; }

  // Sends '*message' to another machine, according to specification in
  // '*header'. Takes ownership of both args.
  void SendMessage(Header* header, MessageBuffer* message);

  // TODO(kun): Please document this method!
  void SendReplyClient(Header* header, MessageBuffer* message);

  // Replies to a message initially sent with 'header'.
  void SendReplyMessage(Header* header, MessageBuffer* message);

  // Return a pointer to the inbox associated with the specified channel.
  // Creates a new one if none exists for 'channel'.
  AtomicQueue<MessageBuffer*>* DataChannel(const string& channel);

  // Remove the channel. Erases anything currently in the channel.
  void CloseDataChannel(const string& channel);

  // Starts an app running on THIS machine (replaces old AppStarter
  // functionality).
  void AddApp(const StartAppProto& sap);
  void AddApp(const string& app, const string& name) {
    StartAppProto sap;
    sap.set_app(app);
    sap.set_app_name(name);
    AddApp(sap);
  }

  // Returns a pointer to the App object registered under the specified name,
  // or NULL if no such App is registered.
  //
  // TODO(agt): This is not thread-safe if new apps are being registered!
  App* GetApp(const string& name);

  // TODO(kun): This copies the entire map! Maybe instead return something
  //            of type:
  //                     const map<string, App*>&
  //
  map<string, App*> GetApps();

  // Returns a globally unique ID (no ordering guarantees though).
  uint64 GetGUID() {
    return 1 + machine_id_ + (config_.size() * (next_guid_++));
  }

  bool Stopped() {
    return stop_;
  }

  void Stop() {
    stop_ = true;
  }

  AtomicMap<string, string>* AppData() {
    return &app_data_;
  }

  void GlobalBarrier();

 private:
  friend class AppStarter;
  friend class Reporter;
  friend class ConnectionLoopMessageHandler;
  friend class WorkerThreadMessageHandler;

  // Initialization method called by constructor.
  void InitializeComponents();

  // Locally adds app to the app list..
  void AddAppInternal(const StartAppProto& sap);

  // Internal (not-necessarily terminating) app starting function.
  void StartAppInternal(const StartAppProto& sap);

  // This Machine's unique id.
  uint64 machine_id_;

  // Config for the cluster in which the Machine participates.
  ClusterConfig config_;

  // Apps can store shared (but machine-local) config data here.
  AtomicMap<string, string> app_data_;

  // Pool of RPC execution threads. Owned by the Machine object.
  ThreadPool* thread_pool_;

  // Object managing communications with all other Machines in the cluster.
  // Owned by the Machine object.
  Connection* connection_;

  // App registry.
  // TODO(agt): Make this an AtomicMap.
  map<string, App*> apps_;

  // Collection of data message queues, each associated with a string 'channel'
  // identifier.
  AtomicMap<string, AtomicQueue<MessageBuffer*>*> inboxes_;

  // Globally unique ID source.
  std::atomic<uint64> next_guid_;

  // True iff machine has received an external 'stop' request and the server
  // needs to exit gracefully.
  bool stop_;

  uint64 next_barrier_;

  // DISALLOW_COPY_AND_ASSIGN
  Machine(const Machine&);
  Machine& operator=(const Machine&);
};

#endif  // CALVIN_MACHINE_MACHINE_H_

