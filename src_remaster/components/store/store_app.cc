// Author: Alex Thomson (thomson@cs.yale.edu)
//

#include "components/store/store_app.h"

#include "components/store/kvstore.h"
#include "machine/app/app.h"

StoreApp::~StoreApp() {
  delete store_;
}

void StoreApp::HandleMessage(Header* header, MessageBuffer* message) {
  HandleMessageBase(header, message);
}

void StoreApp::HandleMessageBase(Header* header, MessageBuffer* message) {
  if (header->rpc() == "GETRWSETS") {
    // Parse Action.
    Action* action = new Action();
    action->ParseFromArray((*message)[0].data(), (*message)[0].size());
    // Compute action's read write sets.
    store_->GetRWSets(action);
    // Reply to request with updated action state.
    machine()->SendReplyMessage(header, new MessageBuffer(*action));
    delete action;

  } else if (header->rpc() == "RUN") {
    // Parse Action.
    Action* action = new Action();
    action->ParseFromArray((*message)[0].data(), (*message)[0].size());
    // Run action.
    Run(action);
    machine()->SendReplyMessage(header, new MessageBuffer(*action));

  } else if (header->rpc() == "RUNLOCAL") {
    // Parse Action.
    Action* action = reinterpret_cast<Action*>(header->misc_int(0));

    // Get result queue.
    AtomicQueue<Action*>* queue =
        reinterpret_cast<AtomicQueue<Action*>*>(header->misc_int(1));

    // Run action and push onto queue.
    Run(action);
    queue->Push(action);

  } else {
    LOG(FATAL) << "unknown RPC type";
  }

  delete message;
}

void StoreApp::GetRWSets(Action* action) {
  store_->GetRWSets(action);
}

bool StoreApp::IsLocal(const string& path) {
  return store_->IsLocal(path);
}

uint32 StoreApp::LookupReplicaByDir(string dir) {
  return store_->LookupReplicaByDir(dir);
}

uint64 StoreApp::GetHeadMachine(uint64 machine_id) {
  return store_->GetHeadMachine(machine_id);
}

uint32 StoreApp::LocalReplica() {
  return store_->LocalReplica();
}

void StoreApp::Run(Action* action) {
  store_->Run(action);

  // Send results to client.
  if (action->has_client_machine()) {
    Header* header = new Header();
    header->set_from(machine()->machine_id());
    header->set_to(action->client_machine());
    header->set_type(Header::DATA);
    header->set_data_channel(action->client_channel());
    machine()->SendMessage(header, new MessageBuffer(*action));
  }
}

void StoreApp::RunAsync(Action* action, AtomicQueue<Action*>* queue) {
  // Address RPC header to self.
  Header* header = new Header();
  header->set_from(machine()->machine_id());
  header->set_to(machine()->machine_id());
  header->set_type(Header::RPC);
  header->set_app(name());
  header->set_rpc("RUNLOCAL");

  // Encode ptrs to action and queue in misc.
  header->add_misc_int(reinterpret_cast<uint64>(action));
  header->add_misc_int(reinterpret_cast<uint64>(queue));

  // Send request.
  machine()->SendMessage(header, new MessageBuffer());
}

