// Author: Kun Ren <kun@cs.yale.edu>
//

#include <stdio.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>

#include <glog/logging.h>
#include <string>
#include <map>
#include <utility>
#include <vector>

#include "machine/cluster_manager.h"
#include "common/utils.h"

using std::string;

const string& ClusterManager::ssh_key(uint64 m) {
  if (config_.size() < 3) {
    return ssh_key_;
  }
  int repsize = config_.size() / 3;
  if (m / repsize == 0) {
    return ssh_key_;
  } else if (m / repsize == 1) {
    return ssh_key2_;
  } else if (m / repsize == 2) {
    return ssh_key3_;
  }
  LOG(FATAL) << "bad machine id: " << m;
}

void* SystemFunction(void* arg) {
  // Run the specified command.
  int status = system(reinterpret_cast<string*>(arg)->c_str());
  if(status == -1){
    LOG(FATAL)<<"system error";
  } else if(WIFEXITED(status) && (WEXITSTATUS(status) == 0)){
    // printf("run command successful\n");
  } else {
    LOG(FATAL) << "run command fail and exit code is " << WEXITSTATUS(status);
  }

  delete reinterpret_cast<string*>(arg);
  return NULL;
}

void ClusterManager::PutConfig() {
  // Copy config file to all machines.
  vector<pthread_t> threads;
  for (map<uint64, MachineInfo>::const_iterator it =
          config_.machines().begin();
       it != config_.machines().end(); ++it) {
    threads.resize(threads.size()+1);
    string* ssh_command = new string(
         "scp " + ssh_key(it->first)  + " " + config_file_ +
         " "+ ssh_username_ + "@" + it->second.host() + ":" + calvin_path_
         + "/" + config_file_);
    pthread_create(
        &threads[threads.size()-1],
        NULL,
        SystemFunction,
        reinterpret_cast<void*>(ssh_command));
  }
  for (uint32 i = 0; i < threads.size(); i++) {
    pthread_join(threads[i], NULL);
  }
}

void ClusterManager::GetTempFiles(const string& base) {
  vector<pthread_t> threads;
  for (map<uint64, MachineInfo>::const_iterator it =
       config_.machines().begin();
       it != config_.machines().end(); ++it) {
    threads.resize(threads.size()+1);
    string* ssh_command = new string(
      "scp " + ssh_key(it->first)  + " "+ ssh_username_ + "@" + it->second.host() +
      ":/tmp/" + base + IntToString(threads.size()-1) + " data/");
    pthread_create(
        &threads[threads.size()-1],
        NULL,
        SystemFunction,
        reinterpret_cast<void*>(ssh_command));
  }
  for (uint32 i = 0; i < threads.size(); i++) {
    pthread_join(threads[i], NULL);
  }
}

void ClusterManager::Update() {
  // Next, Run "git pull ;make clean;make -j" to get the latest code and compile.
  vector<pthread_t> threads;
  for (map<uint64, MachineInfo>::const_iterator it =
       config_.machines().begin();
       it != config_.machines().end(); ++it) {
    threads.resize(threads.size()+1);
    string* ssh_command = new string(
      "ssh " + ssh_key(it->first)  + " "+ ssh_username_ + "@" + it->second.host() +
      " 'cd " + calvin_path_ + "; git pull; cd src_remaster; cp Makefile.default Makefile; make clean; make -j'");
    pthread_create(
        &threads[threads.size()-1],
        NULL,
        SystemFunction,
        reinterpret_cast<void*>(ssh_command));
  }
  for (uint32 i = 0; i < threads.size(); i++) {
    pthread_join(threads[i], NULL);
  }
  threads.clear();
}

void ClusterManager::DeployCluster(double time, int experiment, int clients, int max_active, int max_running, int local_percentage) {
  vector<pthread_t> threads;
  // Now ssh into all machines and start 'binary' running.
  for (map<uint64, MachineInfo>::const_iterator it =
          config_.machines().begin();
       it != config_.machines().end(); ++it) {
    string val;
    threads.resize(threads.size()+1);
    string* ssh_command = new string(
         "ssh " + ssh_key(it->first)  + " "+ ssh_username_ + "@" + it->second.host() +
         "  'cd " + calvin_path_ + "; " + " bin/scripts/" + binary_ +
         " --machine_id=" + IntToString(it->second.id()) +
         "  --config=" + config_file_ + " --time=" + DoubleToString(time) + " --experiment=" + IntToString(experiment) + " --clients=" + IntToString(clients) +
         " --max_active=" + IntToString(max_active) + " --max_running=" + IntToString(max_running) + " --local_percentage=" + IntToString(local_percentage) + " ' &");

    pthread_create(
        &threads[threads.size()-1],
        NULL,
        SystemFunction,
        reinterpret_cast<void*>(ssh_command));
  }
  for (uint32 i = 0; i < threads.size(); i++) {
    pthread_join(threads[i], NULL);
  }
}

void ClusterManager::KillCluster() {
  vector<pthread_t> threads;

  for (map<uint64, MachineInfo>::const_iterator it =
          config_.machines().begin();
       it != config_.machines().end(); ++it) {
    threads.resize(threads.size()+1);
    string* ssh_command = new string(
        "ssh " + ssh_key(it->first)  + " " + ssh_username_ + "@" + it->second.host() +
        " killall -9 " + binary_);
    pthread_create(
        &threads[threads.size()-1],
        NULL,
        SystemFunction,
        reinterpret_cast<void*>(ssh_command));
  }
  for (uint32 i = 0; i < threads.size(); i++) {
    pthread_join(threads[i], NULL);
  }
}

void ClusterManager::ClusterStatus() {
  // 0: Unreachable
  // 1: Calvin not found
  // 2: Running
  // 3: Not Running
  vector<int > cluster_status(config_.machines().size());
  int index = 0;

  for (map<uint64, MachineInfo>::const_iterator it =
       config_.machines().begin();
       it != config_.machines().end(); ++it) {
    uint64 machine_id = it->second.id();
    string host = it->second.host();

    // Same stuff with DeployCluster
    string ssh_command = "ssh " + ssh_key(it->first)  + " " + ssh_username_ + "@" + host
                         + "  'cd " + calvin_path_ + "; bin/scripts/" + binary_
                         + "  --calvin_version=true" + "  --machine_id="
                         + IntToString(machine_id) + "'";
    int status = system(ssh_command.c_str());
    if (status == -1 || WIFEXITED(status) == false ||
        WEXITSTATUS(status) != 0) {
      if (WEXITSTATUS(status) == 255 || WIFEXITED(status) == false) {
        cluster_status[index++] = 0;
      } else if (WEXITSTATUS(status) == 127) {
        cluster_status[index++] = 1;
      } else if (WEXITSTATUS(status) == 254) {
        cluster_status[index++] = 2;
      }
    } else {
      cluster_status[index++] = 3;
    }
  }

  printf("----------------------Cluster Status-----------------------------\n");
  printf("machine id                 host:port                      status \n");
  index = 0;
  for (map<uint64, MachineInfo>::const_iterator it =
       config_.machines().begin();
       it != config_.machines().end(); ++it) {
    uint64 machine_id = it->second.id();
    string host = it->second.host();
    int port = it->second.port();

    switch (cluster_status[index++]) {
      case 0:
        printf("%-10d          %16s:%d              Unreachable\n",
               (int)machine_id, host.c_str(), port);
        break;
      case 1:
        printf("%-10d          %16s:%d              Calvin not found\n",
               (int)machine_id, host.c_str(), port);
        break;
      case 2:
        printf("%-10d          %16s:%d              Running\n",
               (int)machine_id, host.c_str(), port);
        break;
      case 3:
        printf("%-10d          %16s:%d              Not Running\n",
               (int)machine_id, host.c_str(), port);
        break;
      default:
        break;
    }
  }
  printf("-----------------------------------------------------------------\n");
}

const ClusterConfig& ClusterManager::GetConfig() {
  return config_;
}

