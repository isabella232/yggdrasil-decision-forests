/*
 * Copyright 2021 Google LLC.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "yggdrasil_decision_forests/dataset/vertical_dataset_io.h"

#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "yggdrasil_decision_forests/dataset/data_spec.pb.h"
#include "yggdrasil_decision_forests/dataset/example.pb.h"
#include "yggdrasil_decision_forests/dataset/example_reader.h"
#include "yggdrasil_decision_forests/dataset/example_reader_interface.h"
#include "yggdrasil_decision_forests/dataset/example_writer.h"
#include "yggdrasil_decision_forests/dataset/example_writer_interface.h"
#include "yggdrasil_decision_forests/dataset/formats.h"
#include "yggdrasil_decision_forests/dataset/formats.pb.h"
#include "yggdrasil_decision_forests/dataset/vertical_dataset.h"
#include "yggdrasil_decision_forests/utils/compatibility.h"
#include "yggdrasil_decision_forests/utils/concurrency_streamprocessor.h"
#include "yggdrasil_decision_forests/utils/logging.h"
#include "yggdrasil_decision_forests/utils/sharded_io.h"
#include "yggdrasil_decision_forests/utils/status_macros.h"

namespace yggdrasil_decision_forests {
namespace dataset {
namespace {

// Loads the datasets using a single thread. This solution is more memory
// efficient that per-shard loading as examples are directly integrated into the
// vertical representation.
absl::Status LoadVerticalDatasetSingleThread(
    const absl::string_view typed_path,
    const proto::DataSpecification& data_spec, VerticalDataset* dataset,
    absl::optional<std::vector<int>> ensure_non_missing) {
  // Initialize dataset.
  dataset->set_data_spec(data_spec);
  RETURN_IF_ERROR(dataset->CreateColumnsFromDataspec());

  // Read and record the examples.
  ASSIGN_OR_RETURN(auto reader, CreateExampleReader(typed_path, data_spec,
                                                    ensure_non_missing));
  proto::Example example;
  utils::StatusOr<bool> status;
  while ((status = reader->Next(&example)).ok() && status.value()) {
    dataset->AppendExample(example);
    LOG_INFO_EVERY_N_SEC(30, _ << dataset->nrow() << " examples scanned.");
  }
  return status.status();
}

// Set of examples extracted by a worker.
struct BlockOfExamples {
  // List of examples. These messages are allocated in "arena".
  std::vector<proto::Example*> examples;
  google::protobuf::Arena arena;
};

// Reads a shard.
utils::StatusOr<std::unique_ptr<BlockOfExamples>> LoadShard(
    const proto::DataSpecification& data_spec, const absl::string_view prefix,
    const absl::optional<std::vector<int>>& ensure_non_missing,
    const absl::string_view shard) {
  auto block = absl::make_unique<BlockOfExamples>();
  ASSIGN_OR_RETURN(auto reader,
                   CreateExampleReader(absl::StrCat(prefix, ":", shard),
                                       data_spec, ensure_non_missing));
  auto* example = google::protobuf::Arena::CreateMessage<proto::Example>(&block->arena);
  utils::StatusOr<bool> status;
  while ((status = reader->Next(example)).ok() && status.value()) {
    block->examples.push_back(example);
    example = google::protobuf::Arena::CreateMessage<proto::Example>(&block->arena);
  }
  return block;
}

}  // namespace

absl::Status LoadVerticalDataset(
    const absl::string_view typed_path,
    const proto::DataSpecification& data_spec, VerticalDataset* dataset,
    absl::optional<std::vector<int>> ensure_non_missing,
    const int num_threads) {
  // Extract the shards from the dataset path.
  std::string path, prefix;
  ASSIGN_OR_RETURN(std::tie(prefix, path), SplitTypeAndPath(typed_path));
  std::vector<std::string> shards;
  CHECK_OK(utils::ExpandInputShards(path, &shards));

  if (shards.size() <= 1 || num_threads <= 1) {
    // Loading in a single thread.
    return LoadVerticalDatasetSingleThread(typed_path, data_spec, dataset,
                                           ensure_non_missing);
  }

  // Initialize dataset.
  dataset->set_data_spec(data_spec);
  RETURN_IF_ERROR(dataset->CreateColumnsFromDataspec());

  // Reads the examples in a shard.
  const auto load_shard = [&](const std::string shard)
      -> utils::StatusOr<std::unique_ptr<BlockOfExamples>> {
    return LoadShard(data_spec, prefix, ensure_non_missing, shard);
  };

  utils::concurrency::StreamProcessor<
      std::string, utils::StatusOr<std::unique_ptr<BlockOfExamples>>>
      processor("DatasetLoader", std::min<int>(shards.size(), num_threads),
                load_shard,
                /*result_in_order=*/true);

  // Configure the shard loading jobs.
  processor.StartWorkers();
  for (const auto& shard : shards) {
    processor.Submit(shard);
  }
  processor.CloseSubmits();

  // Ingest the examples in the vertical dataset.
  int loaded_shards = 0;
  while (true) {
    auto examples = processor.GetResult();
    if (!examples.has_value()) {
      // All the shards have been read.
      break;
    }
    RETURN_IF_ERROR(examples.value().status());
    auto block = std::move(examples.value().value());

    if (loaded_shards == 0) {
      // Reserve the vertical dataset memory by assuming that all the shards
      // have ~ the same number of examples.
      dataset->Reserve(block->examples.size() * shards.size());
    }

    for (const auto* example : block->examples) {
      dataset->AppendExample(*example);
      LOG_INFO_EVERY_N_SEC(30, _ << dataset->nrow() << " examples scanned.");
    }
    loaded_shards++;
  }

  if (loaded_shards != shards.size()) {
    return absl::InternalError("Unexpected number of shards.");
  }

  processor.JoinAllAndStopThreads();
  LOG_INFO_EVERY_N_SEC(30, _ << dataset->nrow() << " examples and "
                             << loaded_shards << " shards scanned in total.");
  return absl::OkStatus();
}

absl::Status SaveVerticalDataset(const VerticalDataset& dataset,
                                 const absl::string_view typed_path,
                                 int64_t num_records_by_shard) {
  ASSIGN_OR_RETURN(auto writer,
                   CreateExampleWriter(typed_path, dataset.data_spec(),
                                       num_records_by_shard));
  proto::Example example;
  for (VerticalDataset::row_t row = 0; row < dataset.nrow(); row++) {
    dataset.ExtractExample(row, &example);
    RETURN_IF_ERROR(writer->Write(example));
  }
  return absl::OkStatus();
}

}  // namespace dataset
}  // namespace yggdrasil_decision_forests