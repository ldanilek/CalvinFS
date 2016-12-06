// Author: Alex Thomson (thomson@cs.yale.edu)
//         Kun  Ren <kun.ren@yale.edu>
//
// TODO(agt): Reduce number of string copies.

#ifndef CALVIN_FS_BLOCK_LOG_H_
#define CALVIN_FS_BLOCK_LOG_H_

#include <google/protobuf/repeated_field.h>
#include <set>
#include <vector>
#include <tr1/unordered_map>

#include "common/types.h"
#include "components/log/log.h"
#include "components/log/log_app.h"
#include "components/log/paxos2.h"
#include "fs/batch.pb.h"
#include "fs/block_store.h"
#include "fs/calvinfs.h"
#include "machine/app/app.h"
#include "proto/action.pb.h"

using std::set;
using std::vector;
using std::make_pair;
using std::tr1::unordered_map;

class Header;
class Machine;
class MessageBuffer;

class SequenceSource : public Source<UInt64Pair*> {
 public:
  explicit SequenceSource(Source<PairSequence*>* source)
      : source_(source), current_(NULL), index_(0), offset_(0) {
  }
  virtual ~SequenceSource() {
    delete source_;
    delete current_;
  }

  virtual bool Get(UInt64Pair** p) {
    // Get a non-empty sequence or return false.
    while (current_ == NULL) {
      if (!source_->Get(&current_)) {
        return false;
      }
      if (current_->pairs_size() == 0) {
        delete current_;
        current_ = NULL;
LOG(ERROR) <<"^^^^^^^^^SequenceSource wrong!!!";

      } else {
        index_ = 0;
        offset_ = current_->misc();
//LOG(ERROR) <<"^^^^^^^^^SequenceSource get a sequence:"<< offset_ << " block_id is: "<<current_->pairs(0).first();
      }
    }

    // Expose next element in sequence.
    *p = new UInt64Pair();
    (*p)->set_first(current_->pairs(index_).first());
    (*p)->set_second(offset_);
    offset_ +=  current_->pairs(index_).second();
    if (++index_ == current_->pairs_size()) {
      delete current_;
      current_ = NULL;
    }
    return true;
  }

 private:
  Source<PairSequence*>* source_;
  PairSequence* current_;
  int index_;
  int offset_;
};

class BlockLogApp : public App {
 public:
  BlockLogApp() : go_(true), going_(false), to_delete_(60) {}

  virtual ~BlockLogApp() {
    Stop();
  }

  virtual void Start() {
    // Get local config info.
    config_ = new CalvinFSConfigMap(machine());

    // Record local replica id.
    replica_ = config_->LookupReplica(machine()->machine_id());

    // Record local replica's paxos machine.
    uint32 replica_count = config_->config().block_replication_factor();
    local_paxos_leader_ = replica_ * (machine()->config().size() / replica_count);

    // Note what machines contain metadata shards on the same replica.
    for (auto it = config_->mds().begin(); it != config_->mds().end(); ++it) {
      if (it->first.second == replica_) {
        mds_.insert(it->second);
      }
    }

    // Get ptr to local block store app.
    blocks_ = reinterpret_cast<BlockStoreApp*>(machine()->GetApp("blockstore"))
        ->blocks_;

    // Get ptr to paxos leader (maybe).
    if (machine()->machine_id() == local_paxos_leader_) {
      paxos_leader_ =
          reinterpret_cast<Paxos2App*>(machine()->GetApp("paxos2"));
    }

    // Get reader of Paxos output (using closest paxos machine).
    batch_sequence_ =
        new SequenceSource(
            new RemoteLogSource<PairSequence>(machine(), local_paxos_leader_, "paxos2"));

    delay_time_ = 0.1;
    batch_cnt_ = 0;
   
    uint64 delayed_batch_cnt = delay_time_/0.005;

    // Okay, finally, start main loop!
    going_ = true;
    while (go_.load()) {
      // Create new batch once per epoch.
      double next_epoch = GetTime() + 0.005;
  
      batch_cnt_++;

      // Create batch (iff there are any pending requests).
      int count = queue_.Size();
      if (count != 0 || delay_txns_.find(batch_cnt_) != delay_txns_.end()) {
        ActionBatch batch;
        int actual_offset = 0;
        // Handle new actions
        for (int i = 0; i < count; i++) {
          Action* a = NULL;
          queue_.Pop(&a);
          
          // Delay multi-replicas action, and add a fake action
          if (a->single_replica() == false) {
            uint64 active_batch_cnt = batch_cnt_ + delayed_batch_cnt;
            if (delay_txns_.find(active_batch_cnt) == delay_txns_.end()) {
              vector<Action*> actions;
              actions.push_back(a);
              //delay_txns_.insert(make_pair<uint64, vector<Action*>>(active_batch_cnt, actions));
              delay_txns_[active_batch_cnt] = actions;
            } else {
              vector<Action*> actions = delay_txns_[active_batch_cnt];
              actions.push_back(a);
              //delay_txns_.insert(make_pair<uint64, vector<Action*>>(active_batch_cnt, actions));
              delay_txns_[active_batch_cnt] = actions;
            }

            // Add a fake multi-replicas action
            a->set_version_offset(actual_offset++);
	    a->set_origin(config_->LookupReplica(machine()->machine_id()));
            a->set_fake_action(true);
            batch.mutable_entries()->AddAllocated(a);

          }

          a->set_version_offset(actual_offset++);
	  a->set_origin(config_->LookupReplica(machine()->machine_id()));
          batch.mutable_entries()->AddAllocated(a);
        }

        // Add the old multi-replicas actions into batch
        if (delay_txns_.find(batch_cnt_) != delay_txns_.end()) {
          vector<Action*> actions = delay_txns_[batch_cnt_];
          for (uint32 i = 0; i < actions.size(); i++) {
            Action* a = actions[i];
            a->set_version_offset(actual_offset++);
	    a->set_origin(config_->LookupReplica(machine()->machine_id()));
            batch.mutable_entries()->AddAllocated(a);
          }

          delay_txns_.erase(batch_cnt_);
        }

        // Avoid multiple allocation.
        string* block = new string();
        batch.SerializeToString(block);

        // Choose block_id.
        uint64 block_id =
            machine()->GetGUID() * 2 + (block->size() > 1024 ? 1 : 0);

        // Send batch to block stores.
        for (uint64 i = 0; i < config_->config().block_replication_factor();
             i++) {
          Header* header = new Header();
          header->set_from(machine()->machine_id());
          header->set_to(
              config_->LookupBlucket(config_->HashBlockID(block_id), i));
          header->set_type(Header::RPC);
          header->set_app(name());
          header->set_rpc("BATCH");
          header->add_misc_int(block_id);
          machine()->SendMessage(header, new MessageBuffer(Slice(*block)));
        }

        // Scheduler block for eventual deallocation.
        to_delete_.Push(block);
      }

      // Delete old blocks.
      string* block;
      while (to_delete_.Pop(&block)) {
        delete block;
      }

      // Sleep until next epoch.
      SpinUntil(next_epoch);
    }

    going_ = false;
  }

  virtual void Stop() {
    go_ = false;
    while (going_.load()) {
      usleep(10);
    }
  }

  // Takes ownership of '*entry'.
  virtual void Append(Action* entry) {
    entry->set_origin(config_->LookupReplica(machine()->machine_id()));
    queue_.Push(entry);
  }

  virtual void Append(const Slice& entry, uint64 count = 1) {
    CHECK(count == 1);
    Action* a = new Action();
    a->ParseFromArray(entry.data(), entry.size());
    a->set_origin(config_->LookupReplica(machine()->machine_id()));
    queue_.Push(a);
  }

  virtual void HandleMessage(Header* header, MessageBuffer* message) {
    // Don't run any RPCs before Start() is called.
    while (!going_.load()) {
      usleep(10);
      if (!go_.load()) {
        return;
      }
    }

    if (header->rpc() == "APPEND") {
      Action* a = new Action();
      a->ParseFromArray((*message)[0].data(), (*message)[0].size());
      a->set_origin(replica_);
      queue_.Push(a);
//LOG(ERROR) << "Machine: "<<machine()->machine_id() <<" =>Block log recevie a APPEND request. distinct id is:"<< a->distinct_id()<<" from machine:"<<header->from();
    } else if (header->rpc() == "BATCH") {
      // Write batch block to local block store.
      uint64 block_id = header->misc_int(0);
      blocks_->Put(block_id, (*message)[0]);
//LOG(ERROR) << "Machine: "<<machine()->machine_id() << " =>Block log recevie a BATCH request. block id is:"<< block_id <<" from machine:"<<header->from();
      // Parse batch.
      ActionBatch batch;
      batch.ParseFromArray((*message)[0].data(), (*message)[0].size());

      //  If (This batch come from this replica) → send SUBMIT to the Sequencer(LogApp) on the master node of the local paxos participants
      if (config_->LookupReplica(header->from()) == replica_) {
        Header* header = new Header();
        header->set_from(machine()->machine_id());
        header->set_to(local_paxos_leader_);  // Local Paxos leader.
        header->set_type(Header::RPC);
        header->set_app(name());
        header->set_rpc("SUBMIT");
        header->add_misc_int(block_id);
        header->add_misc_int(batch.entries_size());
        machine()->SendMessage(header, new MessageBuffer());
      }

      // Forward sub-batches to relevant readers (same replica only).
      map<uint64, ActionBatch> subbatches;
      for (int i = 0; i < batch.entries_size(); i++) {
        set<uint64> recipients;
        
        if (batch.entries(i).fake_action() == true) {
          continue;
        }

        for (int j = 0; j < batch.entries(i).readset_size(); j++) {
          if (config_->LookupReplicaByDir(batch.entries(i).readset(j)) == batch.entries(i).origin()) {
            uint64 mds = config_->HashFileName(batch.entries(i).readset(j));
            recipients.insert(config_->LookupMetadataShard(mds, replica_));
          }
        }
        for (int j = 0; j < batch.entries(i).writeset_size(); j++) {
          if (config_->LookupReplicaByDir(batch.entries(i).writeset(j)) == batch.entries(i).origin()) {
            uint64 mds = config_->HashFileName(batch.entries(i).writeset(j));
            recipients.insert(config_->LookupMetadataShard(mds, replica_));
          }
        }

        for (auto it = recipients.begin(); it != recipients.end(); ++it) {
          subbatches[*it].add_entries()->CopyFrom(batch.entries(i));
        }
      }

      for (auto it = mds_.begin(); it != mds_.end(); ++it) {
        header = new Header();
        header->set_from(machine()->machine_id());
        header->set_to(*it);
        header->set_type(Header::RPC);
        header->set_app(name());
        header->set_rpc("SUBBATCH");
        header->add_misc_int(block_id);
        machine()->SendMessage(header, new MessageBuffer(subbatches[*it]));
      }


      // Forward "fake multi-replica action" to the head node 
      if (config_->LookupReplica(header->from()) != config_->LookupReplica(machine()->machine_id())) {
        ActionBatch fake_action_batch;
        for (int i = 0; i < batch.entries_size(); i++) {
          if (batch.entries(i).fake_action() == true) {
            fake_action_batch.add_entries()->CopyFrom(batch.entries(i));
          }
        }

        // Send to the head node
        header = new Header();
        header->set_from(machine()->machine_id());
        header->set_to(local_paxos_leader_);
        header->set_type(Header::RPC);
        header->set_app("paxos2");
        header->set_rpc("FAKEACTIONBATCH");
        header->add_misc_int(block_id);
        machine()->SendMessage(header, new MessageBuffer(fake_action_batch));
      }
      

    } else if (header->rpc() == "SUBMIT") {

      uint64 block_id = header->misc_int(0);

      uint64 count = header->misc_int(1);
      paxos_leader_->Append(block_id, count);
//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< "=>Block log recevie a SUBMIT request. block id is:"<< block_id<<" from machine:"<<header->from();
    } else if (header->rpc() == "SUBBATCH") {
      uint64 block_id = header->misc_int(0);
      ActionBatch* batch = new ActionBatch();
      batch->ParseFromArray((*message)[0].data(), (*message)[0].size());
      subbatches_.Put(block_id, batch);
//LOG(ERROR) << "Machine: "<<machine()->machine_id()<< "=>Block log recevie a SUBBATCH request. block id is:"<< block_id<<" from machine:"<<header->from();
    } else if (header->rpc() == "CREATE_NEW") {
      { 
        Lock l(&create_new_action_mutex_);
        uint64 distinct_id = header->misc_int(0);
        uint32 cnt = header->misc_int(1);
        uint32 current;
        if (create_new_actions_.Lookup(distinct_id, &current) == true) {
          if (current == cnt - 1) {
            Action* a = new Action();
            a->ParseFromArray((*message)[0].data(), (*message)[0].size());
            a->set_origin(replica_);
            queue_.Push(a);
            create_new_actions_.Erase(distinct_id);
          } else {
            create_new_actions_.EraseAndPut(distinct_id, current+1);
          }
        } else {
          if (cnt == 1) {
            Action* a = new Action();
            a->ParseFromArray((*message)[0].data(), (*message)[0].size());
            a->set_origin(replica_);
            queue_.Push(a);
          } else {
            create_new_actions_.Put(distinct_id, 1);
          }
        }
      }
    } else {
      LOG(FATAL) << "unknown RPC type: " << header->rpc();
    }
  }

  Source<Action*>* GetActionSource() {
    return new ActionSource(this);
  }

 private:
  // True iff main thread SHOULD run.
  std::atomic<bool> go_;

  // True iff main thread IS running.
  std::atomic<bool> going_;

  // CalvinFS configuration.
  CalvinFSConfigMap* config_;

  // Replica to which we belong.
  uint64 replica_;

  // This machine's local block store.
  BlockStore* blocks_;

  // List of machine ids that have metadata shards with replica == replica_.
  set<uint64> mds_;

  // Local paxos app (used only by machine 0).
  Paxos2App* paxos_leader_;

  // Number of votes for each batch (used only by machine 0).
  map<uint64, int> batch_votes_;
  Mutex create_new_action_mutex_;

  // Subbatches received.
  AtomicMap<uint64, ActionBatch*> subbatches_;

  // Paxos log output.
  Source<UInt64Pair*>* batch_sequence_;

  // Pending append requests.
  AtomicQueue<Action*> queue_;

  // Delayed deallocation queue.
  // TODO(agt): Ugh this is horrible, we should replace this with ref counting!
  DelayQueue<string*> to_delete_;

  uint64 local_paxos_leader_;

  AtomicMap<uint64, uint32> create_new_actions_;

  double delay_time_;
  unordered_map<uint64, vector<Action*> > delay_txns_;
  uint64 batch_cnt_;


  friend class ActionSource;
  class ActionSource : public Source<Action*> {
   public:
    virtual ~ActionSource() {}
    virtual bool Get(Action** a) {
      while (true) {
        // Make sure we have a valid (i.e. non-zero) subbatch_id_, or return
        // false if we can't get one.
        if (subbatch_id_ == 0) {
          UInt64Pair* p = NULL;
          if (log_->batch_sequence_->Get(&p)) {
            subbatch_id_ = p->first();
            subbatch_version_ = p->second();
            delete p;
//LOG(ERROR) <<"*********Blocklog subbatch_id:"<< subbatch_id_ << " subbatch_version_ "<<subbatch_version_;
          } else {
            return false;
          }
        }

        // Make sure we have a valid pointer to the current (nonempty)
        // subbatch, or return false if we can't get one.
        if (subbatch_ == NULL) {
          // Have we received the subbatch corresponding to subbatch_id_?
          if (!log_->subbatches_.Lookup(subbatch_id_, &subbatch_)) {
            // Nope. Gotta try again later.
//LOG(ERROR) <<"*********Have we received the subbatch corresponding to subbatch_id_?";
            return false;
          } else {
            // Got the subbatch! Is it empty?
            if (subbatch_->entries_size() == 0) {
              // Doh, the batch was empty! Throw it away and keep looking.
//LOG(ERROR) <<"*********Doh, the batch was empty! Throw it away and keep looking.";
              delete subbatch_;
              log_->subbatches_.Erase(subbatch_id_);
              subbatch_ = NULL;
              subbatch_id_ = 0;
            } else {
              // Okay, got a non-empty subbatch! Reverse the order of elements
              // so we can now repeatedly call ReleaseLast on the entries.
//LOG(ERROR) <<"*********Okay, got a non-empty subbatch! Reverse the order of elements.";
              for (int i = 0; i < subbatch_->entries_size() / 2; i++) {
                subbatch_->mutable_entries()->SwapElements(
                    i,
                    subbatch_->entries_size()-1-i);
              }
              // Now we are ready to start returning actions from this subbatch.
              break;
            }
          }
        } else {
          // Already had a good subbatch. Onward.
//LOG(ERROR) <<"*********Already had a good subbatch. Onward.";
          break;
        }
      }

      // Should be good to go now.
      CHECK(subbatch_->entries_size() != 0);
      *a = subbatch_->mutable_entries()->ReleaseLast();
      (*a)->set_version(subbatch_version_ + (*a)->version_offset());
      (*a)->clear_version_offset();
//LOG(ERROR) <<"^^^^^^^^^ActionSource get a txn: distinct_id is: "<<(*a)->distinct_id();
      if (subbatch_->entries_size() == 0) {
        // Okay, NOW the batch is empty.
        delete subbatch_;
        log_->subbatches_.Erase(subbatch_id_);
        subbatch_ = NULL;
        subbatch_id_ = 0;
      }
      return true;
    }

   private:
    friend class BlockLogApp;
    ActionSource(BlockLogApp* log)
      : log_(log), subbatch_id_(0), subbatch_(NULL) {
    }

    // Pointer to underlying BlockLogApp.
    BlockLogApp* log_;

    // ID of (and pointer to) current subbatch (which is technically owned by
    // log_->subbatches_).
    uint64 subbatch_id_;
    ActionBatch* subbatch_;

    // Version of initial action in subbatch_.
    uint64 subbatch_version_;
  };
};

#endif  // CALVIN_FS_BLOCK_LOG_H_

