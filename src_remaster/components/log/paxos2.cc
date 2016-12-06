// Author: Alex Thomson (thomson@cs.yale.edu)
// Author: Kun  Ren (kun.ren@yale.edu)
//

#include "components/log/paxos2.h"

#include <atomic>
#include <glog/logging.h>
#include <google/protobuf/message.h>
#include <queue>
#include <set>
#include <utility>
#include <vector>

#include "common/types.h"
#include "components/log/log.h"
#include "components/log/log_reader.pb.h"
#include "components/log/local_mem_log.h"
#include "machine/machine.h"
#include "machine/message_buffer.h"
#include "proto/header.pb.h"
#include "proto/scalar.pb.h"


using std::atomic;
using std::make_pair;
using std::pair;
using std::queue;
using std::set;
using std::vector;

// Make Paxos2App startable.
REGISTER_APP(Paxos2App) {
  Scalar s;
  s.ParseFromString(ARG);
  return new Paxos2App(new LocalMemLog(), FromScalar<uint64>(s));
}

REGISTER_APP(Paxos2App2) {
  StartAppProto sap;
  sap.ParseFromString(ARG);
  vector<uint64> participants;
  for (int i = 0; i < sap.participants_size(); i++) {
    participants.push_back(sap.participants(i));
  }
  return new Paxos2App(new LocalMemLog(), participants);
}

Paxos2App::Paxos2App(Log* log, const vector<uint64>& participants)
    : participants_(participants), go_(true), going_(false), count_(0) {
  log_ = log;
}

Paxos2App::Paxos2App(Log* log, uint64 count)
    : go_(true), going_(false), count_(0) {
  log_ = log;
  for (uint64 i = 0; i < count; i++) {
    participants_.push_back(i);
  }
}

bool Paxos2App::IsLeader() {
  return machine()->machine_id() == participants_[0];
}

void Paxos2App::Append(uint64 blockid, uint64 count) {
  // Forward append request to leader.
  Header* header = new Header();
  header->set_from(machine()->machine_id());
  header->set_to(participants_[0]);
  header->set_type(Header::RPC);
  header->set_app(name());
  header->set_rpc("APPEND");
  header->add_misc_int(blockid);
  header->add_misc_int(count);
  machine()->SendMessage(header, new MessageBuffer());
}

void Paxos2App::Start() {
  going_ = true;
  replica_count = (machine()->config().size() >= 3) ? 3 : 1;
  partitions_per_replica = machine()->config().size() / replica_count;
  
  local_sequences_index = 0;

  if (IsLeader()) {
    RunLeader();
  } else {
    RunFollower();
  }
  going_ = false;
}

void Paxos2App::Stop() {
  go_ = false;
  while (going_.load()) {
    // Wait for main loop to stop.
    usleep(10);
  }
}

void Paxos2App::HandleOtherMessages(Header* header, MessageBuffer* message) {
  if (header->rpc() == "APPEND") {
    Lock l(&mutex_);
    UInt64Pair* p = sequence_.add_pairs();
    p->set_first(header->misc_int(0));
    p->set_second(header->misc_int(1));
    count_ += p->second();
//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< " ++Paxos2 recevie a Append request. block id is:"<< header->misc_int(0)<<"  count is:"<<header->misc_int(1)<<" from machine:"<<header->from();

  } else if (header->rpc() == "NEW-SEQUENCE") {

PairSequence other_sequence;
other_sequence.ParseFromArray((*message)[0].data(), (*message)[0].size());
CHECK(other_sequence.pairs_size() != 0);

MessageBuffer* m = new MessageBuffer(other_sequence);

    Scalar s;
    s.ParseFromArray((*message)[1].data(), (*message)[1].size());
    m->Append(s);
    s.ParseFromArray((*message)[2].data(), (*message)[2].size());
    m->Append(s);
    sequences_other_replicas.Push(m);
//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< " ++Paxos2 recevie a NEW-SEQUENCE  from machine:"<<header->from()<<"  version is:"<<other_sequence.misc();
  } else if (header->rpc() == "NEW-SEQUENCE-ACK") {
//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< " ++Paxos2 recevie a NEW-SEQUENCE-ACK. from machine:"<<header->from();
    // Send next sequence to the from-replica
    Scalar s;
    s.ParseFromArray((*message)[0].data(), (*message)[0].size());
    uint32 from_replica = FromScalar<uint32>(s);

    uint64 next_index = 0;
    bool findit = next_sequences_index.Lookup(from_replica, &next_index);
    
    CHECK(findit == true);

    pair<uint64, uint64> next_sequence_version;
    bool findnext = local_versions_index_table.Lookup(next_index, &next_sequence_version); 

//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< " ++Paxos2 recevie a NEW-SEQUENCE-ACK(--before find next). from machine:"<<header->from();
    while (findnext == false) {
      usleep(10);
      findnext = local_versions_index_table.Lookup(next_index, &next_sequence_version);
    }

//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< " ++Paxos2 recevie a NEW-SEQUENCE-ACK(--already find next). from machine:"<<header->from()<<". next version is: "<<next_sequence_version.first;

    // The number of actions of the current sequence
    uint64 num_actions = next_sequence_version.second;
    ++next_index;

    next_sequences_index.EraseAndPut(from_replica, next_index);
 
    Log::Reader* r = readers_for_local_log[from_replica];
    bool find = r->Seek(next_sequence_version.first);
    CHECK(find == true);

    Header* header2 = new Header();
    header2->set_from(machine()->machine_id());
    header2->set_to(header->from());
    header2->set_type(Header::RPC);
    header2->set_app(name());
    header2->set_rpc("NEW-SEQUENCE");
    MessageBuffer* m = new MessageBuffer();
    m->Append(r->Entry());
    m->Append(ToScalar<uint64>(num_actions));
    m->Append(ToScalar<uint32>(machine()->machine_id()));
    machine()->SendMessage(header2, m);
//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< "=>Paxos2: Send NEW-SEQUENCE(after receive ack) to: "<<from_replica<<" . version: "<<r->Version();
  }  else if (header->rpc() == "FAKEACTIONBATCH") {
    uint64 block_id = header->misc_int(0);
    ActionBatch* batch = new ActionBatch();
    batch->ParseFromArray((*message)[0].data(), (*message)[0].size());
    fakebatches_.Put(block_id, batch);
  } else {
    LOG(FATAL) << "unknown message type: " << header->rpc();
  }

  delete header;
  delete message;
}

void Paxos2App::RunLeader() {
  uint64 next_version = 1;
  int quorum = static_cast<int>(participants_.size()) / 2 + 1;
  set<atomic<int>*> ack_ptrs;
  MessageBuffer* m = NULL;
  bool isFirst = true;
  bool isLocal = false;

  for (uint64 i = 0; i < replica_count; i++) {
    readers_for_local_log[i] = log_->GetReader();
  }

  while (go_.load()) {
    // Sleep while there are NO requests.
    while (count_.load() == 0 && sequences_other_replicas.Size() == 0) {
      usleep(10);
      if (!go_.load()) {
        return;
      }
    }

    string encoded;
    uint64 version;
    uint32 from_machine = machine()->machine_id();
    PairSequence other_sequence;

    if (count_.load() != 0) {
      // Propose a new sequence.
      {
        Lock l(&mutex_);
        version = next_version;
        next_version += count_.load();
        count_ = 0;
        sequence_.set_misc(version);
        sequence_.SerializeToString(&encoded);
        sequence_.Clear();
        isLocal = true;

//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< "=>Paxos2 proposes a new sequence from local: version:"<< version<< " next_version is: "<<next_version;
      }
    } else if (sequences_other_replicas.Size() != 0) {
      sequences_other_replicas.Pop(&m);

      version = next_version;

      other_sequence.ParseFromArray((*m)[0].data(), (*m)[0].size());
CHECK(other_sequence.pairs_size() != 0);
      other_sequence.set_misc(version);
      other_sequence.SerializeToString(&encoded);

      Scalar s;
      s.ParseFromArray((*m)[1].data(), (*m)[1].size());

      next_version += FromScalar<uint64>(s);
      s.ParseFromArray((*m)[2].data(), (*m)[2].size());
      from_machine = FromScalar<uint32>(s);
      isLocal = false;
//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< "=>Paxos2 proposes a new sequence from other replicas: version:"<< other_sequence.misc() << " next_version is: "<<next_version<<". from: "<<from_machine;
    }


    // Append new actions, only need to handle remoted sequences
    if (isLocal == false) {
      ActionBatch* subbatch_;
      Action* new_action;
      for (int i = 0; i < other_sequence.pairs_size();i++) {
        uint64 subbatch_id_ = other_sequence.pairs(i).first();
        fakebatches_.Lookup(subbatch_id_, &subbatch_);

        if (subbatch_->entries_size() == 0) {
          continue;
        }
              
        for (int i = 0; i < subbatch_->entries_size() / 2; i++) {
          subbatch_->mutable_entries()->SwapElements(i, subbatch_->entries_size()-1-i);
        }
        
        for (int i = 0; i < subbatch_->entries_size() / 2; i++) {
          new_action = subbatch_->mutable_entries()->ReleaseLast();

          Header* header = new Header();
          header->set_from(tmachine()->machine_id());
          header->set_to(machine()->machine_id());

          header->add_misc_int(action->distinct_id());
          header->add_misc_int(action->involved_machines());
          header->set_type(Header::RPC);
          header->set_app("blocklog");
          header->set_rpc("CREATE_NEW");
          
          
          new_action->set_fake_action(false);
          new_action->clear_client_machine();
          new_action->clear_client_channel();
          new_action->clear_origin();
          new_action->set_create_new(false);
          string* block = new string();
          new_action->SerializeToString(block);

          machine()->SendMessage(header, new MessageBuffer(Slice(*block)));
        }
      }
    }
  

    atomic<int>* acks = new atomic<int>(1);
    ack_ptrs.insert(acks);
    for (uint32 i = 1; i < participants_.size(); i++) {
      Header* h = new Header();
      h->set_from(machine()->machine_id());
      h->set_to(participants_[i]);
      h->set_type(Header::DATA);
      h->set_data_channel("paxos2");
      m = new MessageBuffer(new string(encoded));
      m->Append(ToScalar<uint64>(version));
      m->Append(ToScalar<uint64>(reinterpret_cast<uint64>(acks)));
      machine()->SendMessage(h, m);
    }

    // Collect Acks.
    while (acks->load() < quorum) {
      usleep(10);
      if (!go_.load()) {
        return;
      }
    }

    // Commit!
    for (uint32 i = 1; i < participants_.size(); i++) {
      Header* h = new Header();
      h->set_from(machine()->machine_id());
      h->set_to(participants_[i]);
      h->set_type(Header::DATA);
      h->set_data_channel("paxos2");
      machine()->SendMessage(h, new MessageBuffer());
    }

    // Actually append the request into the log
    log_->Append(version, encoded);
//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< "=>Paxos2: Actually append the request into the log: version:"<< version;
     
    if (isLocal == true) {
      local_versions_index_table.Put(local_sequences_index, make_pair(version, next_version - version));
      local_sequences_index++;
    }
   
    if (isLocal == true && isFirst == true) {
      // Send the sequence to the LeaderPaxosApp of all the other replicas;

      for (uint64 i = 0; i < replica_count; i++) {
        if (i != machine()->machine_id()/partitions_per_replica) {
          Header* header = new Header();
          header->set_from(machine()->machine_id());
          header->set_to(i*partitions_per_replica);
          header->set_type(Header::RPC);
          header->set_app(name());
          header->set_rpc("NEW-SEQUENCE");
          m = new MessageBuffer(new string(encoded));
	  m->Append(ToScalar<uint64>(next_version - version));
          m->Append(ToScalar<uint32>(machine()->machine_id()));
          machine()->SendMessage(header, m);

          next_sequences_index.Put(i, 1);
//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< "=>Paxos2: Send NEW-SEQUENCE to: "<<i*partitions_per_replica;
	}
      }

      isFirst = false;

    } else if (isLocal == false) {
      Header* header = new Header();
      header->set_from(machine()->machine_id());
      header->set_to(from_machine);
      header->set_type(Header::RPC);
      header->set_app(name());
      header->set_rpc("NEW-SEQUENCE-ACK");
      m = new MessageBuffer();
      m->Append(ToScalar<uint32>(machine()->machine_id()/partitions_per_replica));
      machine()->SendMessage(header, m);
//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< "=>Paxos2: Send NEW-SEQUENCE-ACK to: "<<from_machine;
    }

  }
}

void Paxos2App::RunFollower() {
  auto channel = machine()->DataChannel("paxos2");
  queue<MessageBuffer*> uncommitted;
  while (go_.load()) {
    // Get message from leader.
    MessageBuffer* m = NULL;
    while (!channel->Pop(&m)) {
      usleep(10);
      if (!go_.load()) {
        return;
      }
    }
    if (m->size() == 3) {
//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< "=>Paxos2(Follower): Receive a new proposal:";
      // New proposal.
      uncommitted.push(m);
      // Send ack to leader.
      Header* h = new Header();
      h->set_from(machine()->machine_id());
      h->set_to(participants_[0]);
      h->set_type(Header::ACK);
      Scalar s;
      s.ParseFromArray((*m)[2].data(), (*m)[2].size());
      h->set_ack_counter(FromScalar<uint64>(s));
      machine()->SendMessage(h, new MessageBuffer());
    } else {
//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< "=>Paxos2(Follower): Receive a commit message:";
      // Commit message.
      CHECK(!uncommitted.empty());
      delete m;
      m = uncommitted.front();
      uncommitted.pop();
      Scalar s;
      s.ParseFromArray((*m)[1].data(), (*m)[1].size());
      log_->Append(FromScalar<uint64>(s), (*m)[0]);
      delete m;
    }
  }
}

