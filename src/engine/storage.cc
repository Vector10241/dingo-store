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

#include "engine/storage.h"

#include "proto/common.pb.h"

namespace dingodb {

Storage::Storage(Engine* engine): engine_(engine) {
}

Storage::~Storage() {
}

int Storage::AddRegion(uint64_t region_id, const dingodb::pb::common::RegionInfo& region) {
  return engine_->AddRegion(region_id, region);
}

int Storage::DestroyRegion(uint64_t region_id) {

}

Snapshot* Storage::GetSnapshot() {
  return nullptr;
}

void Storage::ReleaseSnapshot() {

}

std::shared_ptr<std::string> Storage::KvGet(std::shared_ptr<Context> ctx, const std::string& key) {
  return engine_->KvGet(ctx, key);
}

int Storage::KvPut(std::shared_ptr<Context> ctx, const std::string& key, const std::string& value) {
  return engine_->KvPut(ctx, key, value);
}


} // namespace dingodb