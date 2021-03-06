// Copyright 2017 Qihoo
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http:// www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef SRC_NODE_ZP_DATA_SERVER_H_
#define SRC_NODE_ZP_DATA_SERVER_H_

#include <set>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>

#include "rocksdb/options.h"

#include "pink/include/bg_thread.h"
#include "pink/include/server_thread.h"
#include "pink/include/pb_conn.h"

#include "slash/include/slash_status.h"
#include "slash/include/slash_mutex.h"

#include "include/zp_conf.h"
#include "include/zp_const.h"
#include "include/zp_binlog.h"
#include "include/zp_meta_utils.h"
#include "include/zp_util.h"
#include "src/node/zp_data_command.h"
#include "src/node/zp_metacmd_bgworker.h"
#include "src/node/zp_ping_thread.h"
#include "src/node/zp_trysync_thread.h"
#include "src/node/zp_binlog_sender.h"
#include "src/node/zp_binlog_receive_bgworker.h"
#include "src/node/zp_data_table.h"
#include "src/node/zp_data_partition.h"

using slash::Status;

class ZPDataServer;
// class ZPDataServerConn;

extern ZpConf* g_zp_conf;

// For now, we only have 2 kinds of Statistics:
//  stats_[0] is client stats_;
//  stats_[1] is sync stats_;
enum StatType {
  kClient = 0,
  kSync = 1,
};

class ZPDataServer  {
 public:
  ZPDataServer();
  virtual ~ZPDataServer();
  Status Start();

  std::string meta_ip() {
    slash::RWLock l(&meta_state_rw_, false);
    return meta_ip_;
  }
  int meta_port() {
    slash::RWLock l(&meta_state_rw_, false);
    return meta_port_;
  }
  std::string local_ip() {
    return g_zp_conf->local_ip();
  }
  int local_port() {
    return g_zp_conf->local_port();
  }

  bool IsSelf(const Node& node) {
    return (g_zp_conf->local_ip() == node.ip
        && g_zp_conf->local_port() == node.port);
  }

  std::string db_sync_path() {
    return g_zp_conf->data_path() + "/sync_"
      + std::to_string(g_zp_conf->local_port()) + "/";
  }

  std::string bgsave_path() {
    return g_zp_conf->data_path() + "/dump/";
  }

  const rocksdb::Options* db_options() const {
    return &db_options_;
  }

  size_t binlog_sender_count() {
    return binlog_send_workers_.size();
  }

  void Exit() {
    should_exit_ = true;
  }

  // Meta related
  bool ShouldJoinMeta();
  void MetaConnected();
  void MetaDisconnect();
  void PickMeta();
  void NextMeta(std::string* ip, long* port);
  int64_t meta_epoch() {
    slash::MutexLock l(&mutex_epoch_);
    return meta_epoch_;
  }
  void TryUpdateEpoch(int64_t epoch);
  void FinishPullMeta(int64_t epoch);
  bool ShouldPullMeta() {
    slash::MutexLock l(&mutex_epoch_);
    return should_pull_meta_;
  }
  bool Availible() {
    slash::MutexLock l(&mutex_epoch_);
    return meta_epoch_ >= 0;
  }

  // Table related
  std::shared_ptr<Table> GetOrAddTable(const std::string &table_name);
  void DeleteTable(const std::string &table_name);

  std::shared_ptr<Partition> GetTablePartition(
      const std::string &table_name, const std::string &key);
  std::shared_ptr<Partition> GetTablePartitionById(
      const std::string &table_name, const int partition_id);
  int KeyToPartition(const std::string& table_name, const std::string &key);

  void DumpTablePartitions();
  void DumpBinlogSendTask();


  // Backgroud thread
  void BGSaveTaskSchedule(void (*function)(void*), void* arg);
  void BGPurgeTaskSchedule(void (*function)(void*), void* arg);
  void AddSyncTask(const std::string& table, int partition_id,
      uint64_t delay = 0);
  void AddMetacmdTask();
  Status AddBinlogSendTask(const std::string &table, int parititon_id,
      const std::string& binlog_filename, const Node& node, int32_t filenum,
      int64_t offset);
  Status RemoveBinlogSendTask(const std::string &table, int parititon_id,
      const Node& node);
  int32_t GetBinlogSendFilenum(const std::string &table, int partition_id,
      const Node& node);
  void DispatchBinlogBGWorker(ZPBinlogReceiveTask *task);

  // Command related
  Cmd* CmdGet(const int op) {
    return GetCmdFromTable(op, cmds_);
  }
  void DumpTableBinlogOffsets(const std::string &table_name,
      TablePartitionOffsets *all_offset);

  // Statistic related
  void PlusStat(const StatType type, const std::string &table);
  void ResetLastStat(const StatType type);
  bool GetTotalStat(const StatType type, Statistic* stat);

  bool GetAllTableName(std::set<std::string>* table_names);
  bool GetTableStat(const StatType type, const std::string& table_name,
      std::vector<Statistic>* stats);
  bool GetTableCapacity(const std::string& table_name,
      std::vector<Statistic>* capacity_stats);
  bool GetTableReplInfo(const std::string& table_name,
      std::unordered_map<std::string, client::CmdResponse_InfoRepl>* repls);
  bool GetServerInfo(client::CmdResponse_InfoServer* info_server);

 private:
  slash::Mutex server_mutex_;
  std::unordered_map<int, Cmd*> cmds_;

  // Table and Partition
  // Note: this lock only protect table map,
  // rather than certain partiton which should keep thread safety itself
  pthread_rwlock_t table_rw_;
  std::atomic<int> table_count_;
  std::unordered_map<std::string, std::shared_ptr<Table>> tables_;
  std::shared_ptr<Table> GetTable(const std::string &table_name);

  // Binlog Send related
  ZPBinlogSendTaskPool binlog_send_pool_;
  std::vector<ZPBinlogSendThread*> binlog_send_workers_;

  // Server related
  ZPMetacmdBGWorker* zp_metacmd_bgworker_;
  ZPTrySyncThread* zp_trysync_thread_;

  std::vector<ZPBinlogReceiveBgWorker*> zp_binlog_receive_bgworkers_;
  pink::ConnFactory* sync_factory_;
  pink::ServerHandle* sync_handle_;
  pink::ServerThread* zp_binlog_receiver_thread_;

  pink::ConnFactory* client_factory_;
  pink::ServerHandle* client_handle_;
  pink::ServerThread* zp_dispatch_thread_;
  ZPPingThread* zp_ping_thread_;

  std::atomic<bool> should_exit_;

  // Meta State related
  pthread_rwlock_t meta_state_rw_;
  std::atomic<int> meta_index_;
  std::string meta_ip_;
  long meta_port_;

  slash::Mutex mutex_epoch_;
  int64_t meta_epoch_;
  bool should_pull_meta_;

  // Cmd related
  void InitClientCmdTable();

  // Background thread
  slash::Mutex bgsave_thread_protector_;
  pink::BGThread bgsave_thread_;
  slash::Mutex bgpurge_thread_protector_;
  pink::BGThread bgpurge_thread_;
  void DoTimingTask();

  // Statistic related
  struct ThreadStatistic {
    slash::Mutex mu;
    uint64_t last_time_us;
    Statistic other_stat;
    std::unordered_map<std::string, Statistic*> table_stats;

    ThreadStatistic()
      : last_time_us(slash::NowMicros()) {}
  };

  ThreadStatistic stats_[2];

  bool GetStat(const StatType type, const std::string &table,
      Statistic* stat);

  rocksdb::Options db_options_;
  void InitDBOptions();
};

#endif  // SRC_NODE_ZP_DATA_SERVER_H_
