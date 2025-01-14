// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "benchmark/operation.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "benchmark/benchmark.h"
#include "common/helper.h"
#include "fmt/core.h"

DEFINE_string(benchmark, "fillseq", "Benchmark type");
DEFINE_validator(benchmark, [](const char*, const std::string& value) -> bool {
  return dingodb::benchmark::IsSupportBenchmarkType(value);
});

DEFINE_uint32(key_size, 64, "Key size");
DEFINE_uint32(value_size, 256, "Value size");

DEFINE_uint32(batch_size, 1, "Batch size");

DEFINE_uint32(arrange_kv_num, 10000, "The number of kv for read");

DEFINE_bool(is_pessimistic_txn, false, "Optimistic or pessimistic transaction");
DEFINE_string(txn_isolation_level, "SI", "Transaction isolation level");
DEFINE_validator(txn_isolation_level, [](const char*, const std::string& value) -> bool {
  auto isolation_level = dingodb::Helper::ToUpper(value);
  return isolation_level == "SI" || isolation_level == "RC";
});

namespace dingodb {
namespace benchmark {

static const std::string kClientRaw = "w";

static const char kAlphabet[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
                                 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x',
                                 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};

using BuildFuncer = std::function<OperationPtr(std::shared_ptr<sdk::Client> client)>;
using OperationBuilderMap = std::map<std::string, BuildFuncer>;

static OperationBuilderMap support_operations = {
    {"fillseq",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr { return std::make_shared<FillSeqOperation>(client); }},
    {"fillrandom",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr { return std::make_shared<FillRandomOperation>(client); }},
    {"readseq",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr { return std::make_shared<ReadSeqOperation>(client); }},
    {"readrandom",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr { return std::make_shared<ReadRandomOperation>(client); }},
    {"readmissing",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr {
       return std::make_shared<ReadMissingOperation>(client);
     }},
    {"filltxnseq",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr { return std::make_shared<FillTxnSeqOperation>(client); }},
    {"filltxnrandom",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr {
       return std::make_shared<FillTxnRandomOperation>(client);
     }},
    {"readtxnseq",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr { return std::make_shared<TxnReadSeqOperation>(client); }},
    {"readtxnrandom",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr {
       return std::make_shared<TxnReadRandomOperation>(client);
     }},
    {"readtxnmissing",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr {
       return std::make_shared<TxnReadMissingOperation>(client);
     }},
};

static sdk::TransactionIsolation GetTxnIsolationLevel() {
  auto isolation_level = dingodb::Helper::ToUpper(FLAGS_txn_isolation_level);
  if (isolation_level == "SI") {
    return sdk::TransactionIsolation::kSnapshotIsolation;
  } else if (isolation_level == "RC") {
    return sdk::TransactionIsolation::kReadCommitted;
  }

  LOG(FATAL) << fmt::format("Not support transaction isolation level: {}", FLAGS_txn_isolation_level);
  return sdk::TransactionIsolation::kReadCommitted;
}

// rand string
static std::string GenRandomString(int len) {
  std::string result;
  int alphabet_len = sizeof(kAlphabet);

  std::mt19937 rng;
  rng.seed(std::random_device()());
  std::uniform_int_distribution<std::mt19937::result_type> distrib(1, 1000000000);
  for (int i = 0; i < len; ++i) {
    result.append(1, kAlphabet[distrib(rng) % alphabet_len]);
  }

  return result;
}

static std::string GenSeqString(int num, int len) { return fmt::format("{0:0{1}}", num, len); }

static std::string EncodeRawKey(const std::string& str) { return kClientRaw + str; }

BaseOperation::BaseOperation(std::shared_ptr<sdk::Client> client) : client(client) {
  auto status = client->NewRawKV(raw_kv);
  if (!status.IsOK()) {
    LOG(FATAL) << fmt::format("New RawKv failed, error: {}", status.ToString());
  }
}

Operation::Result BaseOperation::KvPut(RegionEntryPtr region_entry, bool is_random) {
  Operation::Result result;
  std::string& prefix = region_entry->prefix;
  auto& counter = region_entry->counter;

  int random_str_len = FLAGS_key_size - prefix.size();

  size_t count = counter.fetch_add(1, std::memory_order_relaxed);
  std::string key;
  if (is_random) {
    key = EncodeRawKey(prefix + GenRandomString(random_str_len));
  } else {
    key = EncodeRawKey(prefix + GenSeqString(count, random_str_len));
  }

  std::string value = GenRandomString(FLAGS_value_size);
  result.write_bytes = key.size() + value.size();

  int64_t start_time = Helper::TimestampUs();

  result.status = raw_kv->Put(key, value);

  result.eplased_time = Helper::TimestampUs() - start_time;

  return result;
}

Operation::Result BaseOperation::KvBatchPut(RegionEntryPtr region_entry, bool is_random) {
  Operation::Result result;
  std::string& prefix = region_entry->prefix;
  auto& counter = region_entry->counter;

  int random_str_len = FLAGS_key_size - prefix.size();

  std::vector<sdk::KVPair> kvs;
  for (int i = 0; i < FLAGS_batch_size; ++i) {
    sdk::KVPair kv;
    if (is_random) {
      kv.key = EncodeRawKey(prefix + GenRandomString(random_str_len));
    } else {
      size_t count = counter.fetch_add(1, std::memory_order_relaxed);
      kv.key = EncodeRawKey(prefix + GenSeqString(count, random_str_len));
    }

    kv.value = GenRandomString(FLAGS_value_size);

    kvs.push_back(kv);
    result.write_bytes += kv.key.size() + kv.value.size();
  }

  int64_t start_time = Helper::TimestampUs();

  result.status = raw_kv->BatchPut(kvs);

  result.eplased_time = Helper::TimestampUs() - start_time;

  return result;
}

Operation::Result BaseOperation::KvGet(std::string key) {
  Operation::Result result;

  int64_t start_time = Helper::TimestampUs();

  std::string value;
  result.status = raw_kv->Get(key, value);

  result.read_bytes += value.size();

  result.eplased_time = Helper::TimestampUs() - start_time;

  return result;
}

Operation::Result BaseOperation::KvBatchGet(const std::vector<std::string>& keys) {
  Operation::Result result;

  int64_t start_time = Helper::TimestampUs();

  std::vector<sdk::KVPair> kvs;
  result.status = raw_kv->BatchGet(keys, kvs);

  for (auto& kv : kvs) {
    result.read_bytes += kv.key.size() + kv.value.size();
  }

  result.eplased_time = Helper::TimestampUs() - start_time;

  return result;
}

Operation::Result BaseOperation::KvTxnPut(std::vector<RegionEntryPtr>& region_entries, bool is_random) {
  std::vector<sdk::KVPair> kvs;
  for (const auto& region_entry : region_entries) {
    int random_str_len = FLAGS_key_size - region_entry->prefix.size();

    sdk::KVPair kv;
    size_t count = region_entry->counter.fetch_add(1, std::memory_order_relaxed);
    kv.key = is_random ? EncodeRawKey(region_entry->prefix + GenRandomString(random_str_len))
                       : EncodeRawKey(region_entry->prefix + GenSeqString(count, random_str_len));

    kv.value = GenRandomString(FLAGS_value_size);
    kvs.push_back(kv);
  }

  return KvTxnPut(kvs);
}

Operation::Result BaseOperation::KvTxnPut(const std::vector<sdk::KVPair>& kvs) {
  Operation::Result result;

  for (const auto& kv : kvs) {
    result.write_bytes += kv.key.size() + kv.value.size();
  }

  int64_t start_time = Helper::TimestampUs();

  sdk::Transaction* txn = nullptr;
  sdk::TransactionOptions options;
  options.kind = FLAGS_is_pessimistic_txn ? sdk::TransactionKind::kPessimistic : sdk::TransactionKind::kOptimistic;
  options.isolation = GetTxnIsolationLevel();

  result.status = client->NewTransaction(options, &txn);
  if (!result.status.IsOK()) {
    goto end;
  }

  for (const auto& kv : kvs) {
    result.status = txn->Put(kv.key, kv.value);
    if (!result.status.IsOK()) {
      goto end;
    }
  }

  result.status = txn->PreCommit();
  if (!result.status.IsOK()) {
    goto end;
  }

  result.status = txn->Commit();

end:
  result.eplased_time = Helper::TimestampUs() - start_time;
  delete txn;

  return result;
}

Operation::Result BaseOperation::KvTxnBatchPut(std::vector<RegionEntryPtr>& region_entries, bool is_random) {
  std::vector<sdk::KVPair> kvs;
  for (const auto& region_entry : region_entries) {
    int random_str_len = FLAGS_key_size - region_entry->prefix.size();

    for (int i = 0; i < FLAGS_batch_size; ++i) {
      sdk::KVPair kv;
      size_t count = region_entry->counter.fetch_add(1, std::memory_order_relaxed);
      kv.key = is_random ? EncodeRawKey(region_entry->prefix + GenRandomString(random_str_len))
                         : EncodeRawKey(region_entry->prefix + GenSeqString(count, random_str_len));
      kv.value = GenRandomString(FLAGS_value_size);

      kvs.push_back(kv);
    }
  }

  return KvTxnBatchPut(kvs);
}

Operation::Result BaseOperation::KvTxnBatchPut(const std::vector<sdk::KVPair>& kvs) {
  Operation::Result result;

  for (const auto& kv : kvs) {
    result.write_bytes += kv.key.size() + kv.value.size();
  }

  int64_t start_time = Helper::TimestampUs();

  sdk::Transaction* txn = nullptr;
  sdk::TransactionOptions options;
  options.kind = FLAGS_is_pessimistic_txn ? sdk::TransactionKind::kPessimistic : sdk::TransactionKind::kOptimistic;
  options.isolation = GetTxnIsolationLevel();

  result.status = client->NewTransaction(options, &txn);
  if (!result.status.IsOK()) {
    goto end;
  }

  result.status = txn->BatchPut(kvs);
  if (!result.status.IsOK()) {
    goto end;
  }

  result.status = txn->PreCommit();
  if (!result.status.IsOK()) {
    goto end;
  }

  result.status = txn->Commit();

end:
  result.eplased_time = Helper::TimestampUs() - start_time;
  delete txn;

  return result;
}

Operation::Result BaseOperation::KvTxnGet(const std::vector<std::string>& keys) {
  Operation::Result result;

  int64_t start_time = Helper::TimestampUs();

  sdk::Transaction* txn = nullptr;
  sdk::TransactionOptions options;
  options.kind = FLAGS_is_pessimistic_txn ? sdk::TransactionKind::kPessimistic : sdk::TransactionKind::kOptimistic;
  options.isolation = GetTxnIsolationLevel();

  result.status = client->NewTransaction(options, &txn);
  if (!result.status.IsOK()) {
    goto end;
  }

  for (const auto& key : keys) {
    std::string value;
    result.status = txn->Get(key, value);
    if (!result.status.IsOK()) {
      goto end;
    }
  }

  result.status = txn->PreCommit();
  if (!result.status.IsOK()) {
    goto end;
  }

  result.status = txn->Commit();

end:
  result.eplased_time = Helper::TimestampUs() - start_time;
  delete txn;

  return result;
}

Operation::Result BaseOperation::KvTxnBatchGet(const std::vector<std::vector<std::string>>& keys) {
  Operation::Result result;

  int64_t start_time = Helper::TimestampUs();

  sdk::Transaction* txn = nullptr;
  sdk::TransactionOptions options;
  options.kind = FLAGS_is_pessimistic_txn ? sdk::TransactionKind::kPessimistic : sdk::TransactionKind::kOptimistic;
  options.isolation = GetTxnIsolationLevel();

  result.status = client->NewTransaction(options, &txn);
  if (!result.status.IsOK()) {
    goto end;
  }

  for (const auto& batch_keys : keys) {
    std::vector<sdk::KVPair> kvs;
    result.status = txn->BatchGet(batch_keys, kvs);
    if (!result.status.IsOK()) {
      goto end;
    }
  }

  result.status = txn->PreCommit();
  if (!result.status.IsOK()) {
    goto end;
  }

  result.status = txn->Commit();

end:
  result.eplased_time = Helper::TimestampUs() - start_time;
  delete txn;

  return result;
}

Operation::Result FillSeqOperation::Execute(RegionEntryPtr region_entry) {
  return FLAGS_batch_size == 1 ? KvPut(region_entry, false) : KvBatchPut(region_entry, false);
}

Operation::Result FillRandomOperation::Execute(RegionEntryPtr region_entry) {
  return FLAGS_batch_size == 1 ? KvPut(region_entry, true) : KvBatchPut(region_entry, true);
}

bool ReadOperation::Arrange(RegionEntryPtr region_entry) {
  std::string& prefix = region_entry->prefix;
  int random_str_len = FLAGS_key_size - prefix.size();

  uint32_t batch_size = 256;
  std::vector<sdk::KVPair> kvs;
  kvs.reserve(batch_size);
  for (uint32_t i = 0; i < FLAGS_arrange_kv_num; ++i) {
    sdk::KVPair kv;
    size_t count = region_entry->counter.fetch_add(1, std::memory_order_relaxed);
    kv.key = EncodeRawKey(prefix + GenSeqString(count, random_str_len));
    kv.value = GenRandomString(FLAGS_value_size);

    kvs.push_back(kv);
    region_entry->keys.push_back(kv.key);

    if ((i + 1) % batch_size == 0 || (i + 1 == FLAGS_arrange_kv_num)) {
      auto status = raw_kv->BatchPut(kvs);
      if (!status.ok()) {
        return false;
      }
      kvs.clear();
      std::cout << '\r' << fmt::format("Region({}) put progress [{}%]", prefix, i * 100 / FLAGS_arrange_kv_num)
                << std::flush;
    }
  }

  std::cout << "\r" << fmt::format("Region({}) put data({}) done", prefix, FLAGS_arrange_kv_num) << '\n';

  return true;
}

Operation::Result ReadSeqOperation::Execute(RegionEntryPtr region_entry) {
  auto& keys = region_entry->keys;

  if (FLAGS_batch_size <= 1) {
    return KvGet(keys[region_entry->read_index++ % keys.size()]);
  } else {
    std::vector<std::string> keys;
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      keys.push_back(keys[region_entry->read_index++ % keys.size()]);
    }
    return KvBatchGet(keys);
  }
}

Operation::Result ReadRandomOperation::Execute(RegionEntryPtr region_entry) {
  auto& keys = region_entry->keys;

  if (FLAGS_batch_size <= 1) {
    uint32_t index = Helper::GenerateRealRandomInteger(0, UINT32_MAX) % keys.size();
    return KvGet(keys[index]);
  } else {
    std::vector<std::string> keys;
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      uint32_t index = Helper::GenerateRealRandomInteger(0, UINT32_MAX) % keys.size();
      keys.push_back(keys[index]);
    }
    return KvBatchGet(keys);
  }
}

Operation::Result ReadMissingOperation::Execute(RegionEntryPtr region_entry) {
  std::string& prefix = region_entry->prefix;

  int random_str_len = FLAGS_key_size + 4 - prefix.size();
  if (FLAGS_batch_size <= 1) {
    std::string key = EncodeRawKey(prefix + GenRandomString(random_str_len));
    return KvGet(key);
  } else {
    std::vector<std::string> keys;
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      std::string key = EncodeRawKey(prefix + GenRandomString(random_str_len));
      keys.push_back(key);
    }
    return KvBatchGet(keys);
  }
}

Operation::Result FillTxnSeqOperation::Execute(RegionEntryPtr region_entry) {
  std::vector<RegionEntryPtr> region_entries = {region_entry};
  return FLAGS_batch_size == 1 ? KvTxnPut(region_entries, false) : KvTxnBatchPut(region_entries, false);
}

Operation::Result FillTxnSeqOperation::Execute(std::vector<RegionEntryPtr>& region_entries) {
  return FLAGS_batch_size == 1 ? KvTxnPut(region_entries, false) : KvTxnBatchPut(region_entries, false);
}

Operation::Result FillTxnRandomOperation::Execute(RegionEntryPtr region_entry) {
  std::vector<RegionEntryPtr> region_entries = {region_entry};
  return FLAGS_batch_size == 1 ? KvTxnPut(region_entries, true) : KvTxnBatchPut(region_entries, true);
}

Operation::Result FillTxnRandomOperation::Execute(std::vector<RegionEntryPtr>& region_entries) {
  return FLAGS_batch_size == 1 ? KvTxnPut(region_entries, true) : KvTxnBatchPut(region_entries, true);
}

bool TxnReadOperation::Arrange(RegionEntryPtr region_entry) {
  auto& prefix = region_entry->prefix;
  auto& counter = region_entry->counter;
  auto& keys = region_entry->keys;

  int random_str_len = FLAGS_key_size - prefix.size();

  uint32_t batch_size = 256;
  std::vector<sdk::KVPair> kvs;
  kvs.reserve(batch_size);
  for (uint32_t i = 0; i < FLAGS_arrange_kv_num; ++i) {
    sdk::KVPair kv;
    size_t count = counter.fetch_add(1, std::memory_order_relaxed);
    kv.key = EncodeRawKey(prefix + GenSeqString(count, random_str_len));
    kv.value = GenRandomString(FLAGS_value_size);

    kvs.push_back(kv);
    keys.push_back(kv.key);

    if ((i + 1) % batch_size == 0 || (i + 1 == FLAGS_arrange_kv_num)) {
      auto result = KvTxnBatchPut(kvs);
      if (!result.status.ok()) {
        return false;
      }
      kvs.clear();
      std::cout << '\r' << fmt::format("Region({}) put progress [{}%]", prefix, i * 100 / FLAGS_arrange_kv_num)
                << std::flush;
    }
  }

  std::cout << "\r" << fmt::format("Region({}) put data({}) done", prefix, FLAGS_arrange_kv_num) << '\n';

  return true;
}

Operation::Result TxnReadSeqOperation::Execute(RegionEntryPtr region_entry) {
  auto& keys = region_entry->keys;

  if (FLAGS_batch_size <= 1) {
    return KvTxnGet({keys[region_entry->read_index++ % keys.size()]});
  } else {
    std::vector<std::string> batch_keys;
    batch_keys.reserve(FLAGS_batch_size);
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      batch_keys.push_back(keys[region_entry->read_index++ % keys.size()]);
    }
    return KvTxnBatchGet({batch_keys});
  }
}

Operation::Result TxnReadSeqOperation::Execute(std::vector<RegionEntryPtr>& region_entries) {
  if (FLAGS_batch_size <= 1) {
    std::vector<std::string> tmp_keys;
    for (auto& region_entry : region_entries) {
      auto& keys = region_entry->keys;
      tmp_keys.push_back(keys[region_entry->read_index++ % keys.size()]);
    }

    return KvTxnGet(tmp_keys);
  } else {
    std::vector<std::vector<std::string>> tmp_keys;
    for (auto& region_entry : region_entries) {
      auto& keys = region_entry->keys;

      std::vector<std::string> batch_keys;
      batch_keys.reserve(FLAGS_batch_size);
      for (int i = 0; i < FLAGS_batch_size; ++i) {
        batch_keys.push_back(keys[region_entry->read_index++ % keys.size()]);
      }

      tmp_keys.push_back(batch_keys);
    }

    return KvTxnBatchGet(tmp_keys);
  }
}

Operation::Result TxnReadRandomOperation::Execute(RegionEntryPtr region_entry) {
  auto& keys = region_entry->keys;

  if (FLAGS_batch_size <= 1) {
    uint32_t index = Helper::GenerateRealRandomInteger(0, UINT32_MAX) % keys.size();
    return KvTxnGet({keys[index]});
  } else {
    std::vector<std::string> batch_keys;
    batch_keys.reserve(FLAGS_batch_size);
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      uint32_t index = Helper::GenerateRealRandomInteger(0, UINT32_MAX) % keys.size();
      batch_keys.push_back(keys[index]);
    }
    return KvTxnBatchGet({batch_keys});
  }
}

Operation::Result TxnReadRandomOperation::Execute(std::vector<RegionEntryPtr>& region_entries) {
  if (FLAGS_batch_size <= 1) {
    std::vector<std::string> tmp_keys;
    for (auto& region_entry : region_entries) {
      auto& keys = region_entry->keys;
      uint32_t index = Helper::GenerateRealRandomInteger(0, UINT32_MAX) % keys.size();
      tmp_keys.push_back(keys[index]);
    }

    return KvTxnGet(tmp_keys);
  } else {
    std::vector<std::vector<std::string>> tmp_keys;
    for (auto& region_entry : region_entries) {
      auto& keys = region_entry->keys;

      std::vector<std::string> batch_keys;
      batch_keys.reserve(FLAGS_batch_size);
      for (int i = 0; i < FLAGS_batch_size; ++i) {
        uint32_t index = Helper::GenerateRealRandomInteger(0, UINT32_MAX) % keys.size();
        batch_keys.push_back(keys[index]);
      }

      tmp_keys.push_back(batch_keys);
    }

    return KvTxnBatchGet(tmp_keys);
  }
}

Operation::Result TxnReadMissingOperation::Execute(RegionEntryPtr region_entry) {
  auto& prefix = region_entry->prefix;

  int random_str_len = FLAGS_key_size + 4 - prefix.size();
  if (FLAGS_batch_size <= 1) {
    std::string key = EncodeRawKey(prefix + GenRandomString(random_str_len));
    return KvTxnGet({key});
  } else {
    std::vector<std::string> batch_keys;
    batch_keys.reserve(FLAGS_batch_size);
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      std::string key = EncodeRawKey(prefix + GenRandomString(random_str_len));
      batch_keys.push_back(key);
    }
    return KvTxnBatchGet({batch_keys});
  }
}

Operation::Result TxnReadMissingOperation::Execute(std::vector<RegionEntryPtr>& region_entries) {
  if (FLAGS_batch_size <= 1) {
    std::vector<std::string> tmp_keys;
    for (auto& region_entry : region_entries) {
      auto& prefix = region_entry->prefix;
      int random_str_len = FLAGS_key_size + 4 - prefix.size();

      std::string key = EncodeRawKey(prefix + GenRandomString(random_str_len));
      tmp_keys.push_back(key);
    }

    return KvTxnGet(tmp_keys);
  } else {
    std::vector<std::vector<std::string>> tmp_keys;
    for (auto& region_entry : region_entries) {
      auto& keys = region_entry->keys;
      auto& prefix = region_entry->prefix;
      int random_str_len = FLAGS_key_size + 4 - prefix.size();

      std::vector<std::string> batch_keys;
      batch_keys.reserve(FLAGS_batch_size);
      for (int i = 0; i < FLAGS_batch_size; ++i) {
        std::string key = EncodeRawKey(prefix + GenRandomString(random_str_len));
        batch_keys.push_back(key);
      }

      tmp_keys.push_back(batch_keys);
    }

    return KvTxnBatchGet(tmp_keys);
  }
}

bool IsSupportBenchmarkType(const std::string& benchmark) {
  auto it = support_operations.find(benchmark);
  return it != support_operations.end();
}

std::string GetSupportBenchmarkType() {
  std::string benchmarks;
  for (auto& [benchmark, _] : support_operations) {
    benchmarks += benchmark;
    benchmarks += " ";
  }

  return benchmarks;
}

OperationPtr NewOperation(std::shared_ptr<sdk::Client> client) {
  auto it = support_operations.find(FLAGS_benchmark);
  if (it == support_operations.end()) {
    return nullptr;
  }

  return it->second(client);
}

}  // namespace benchmark
}  // namespace dingodb
