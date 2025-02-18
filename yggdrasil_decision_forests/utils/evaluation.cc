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

#if defined YGG_TFRECORD_PREDICTIONS
#include "yggdrasil_decision_forests/utils/sharded_io_tfrecord.h"
#endif

#include "yggdrasil_decision_forests/dataset/data_spec.h"
#include "yggdrasil_decision_forests/dataset/example_writer.h"
#include "yggdrasil_decision_forests/dataset/formats.h"
#include "yggdrasil_decision_forests/utils/distribution.pb.h"
#include "yggdrasil_decision_forests/utils/evaluation.h"
#include "yggdrasil_decision_forests/utils/status_macros.h"

namespace yggdrasil_decision_forests {
namespace utils {

absl::Status ExportPredictions(
    const std::vector<model::proto::Prediction>& predictions,
    model::proto::Task task, const dataset::proto::Column& label_column,
    absl::string_view typed_prediction_path,
    const int num_records_by_shard_in_output) {
  // Determines the container for the predictions.
  std::string prediction_path, prediction_format;
  ASSIGN_OR_RETURN(std::tie(prediction_format, prediction_path),
                   dataset::SplitTypeAndPath(typed_prediction_path));

#if defined YGG_TFRECORD_PREDICTIONS
  if (prediction_format == "tfrecord+pred") {
    // Save the prediction as a tfrecord of proto::Predictions.
    auto prediction_writer =
        absl::make_unique<TFRecordShardedWriter<model::proto::Prediction>>();
    CHECK_OK(prediction_writer->Open(prediction_path,
                                     num_records_by_shard_in_output));
    for (const auto& prediction : predictions) {
      CHECK_OK(prediction_writer->Write(prediction));
    }
  } else
#endif
  {
    // Save the prediction as a collection (e.g. tfrecord or csv) of
    // proto::Examples.
    ASSIGN_OR_RETURN(auto dataspec, PredictionDataspec(task, label_column));
    ASSIGN_OR_RETURN(auto writer, dataset::CreateExampleWriter(
                                      typed_prediction_path, dataspec,
                                      num_records_by_shard_in_output));
    dataset::proto::Example prediction_as_example;
    for (const auto& prediction : predictions) {
      // Convert the prediction into an example.
      RETURN_IF_ERROR(PredictionToExample(task, label_column, prediction,
                                          &prediction_as_example));
      RETURN_IF_ERROR(writer->Write(prediction_as_example));
    }
  }
  return absl::OkStatus();
}

absl::Status PredictionToExample(
    model::proto::Task task, const dataset::proto::Column& label_col,
    const model::proto::Prediction& prediction,
    dataset::proto::Example* prediction_as_example) {
  prediction_as_example->clear_attributes();
  switch (task) {
    case model::proto::Task::CLASSIFICATION: {
      const int num_label_values =
          static_cast<int>(label_col.categorical().number_of_unique_values());
      if (num_label_values !=
          prediction.classification().distribution().counts_size()) {
        return absl::InvalidArgumentError("Wrong number of classes.");
      }
      for (int label_value = 1; label_value < num_label_values; label_value++) {
        const float prediction_proba =
            prediction.classification().distribution().counts(label_value) /
            prediction.classification().distribution().sum();
        prediction_as_example->add_attributes()->set_numerical(
            prediction_proba);
      }
    } break;
    case model::proto::Task::REGRESSION:
      prediction_as_example->add_attributes()->set_numerical(
          prediction.regression().value());
      break;
    case model::proto::Task::RANKING:
      prediction_as_example->add_attributes()->set_numerical(
          prediction.ranking().relevance());
      break;
    default:
      return absl::InvalidArgumentError("Non supported class");
  }
  return absl::OkStatus();
}

utils::StatusOr<dataset::proto::DataSpecification> PredictionDataspec(
    const model::proto::Task task, const dataset::proto::Column& label_col) {
  dataset::proto::DataSpecification dataspec;

  switch (task) {
    case model::proto::Task::CLASSIFICATION: {
      // Note: label_value starts at 1 since we don't predict the OOV
      // (out-of-dictionary) item.
      const int num_label_values =
          static_cast<int>(label_col.categorical().number_of_unique_values());
      for (int label_value = 1; label_value < num_label_values; label_value++) {
        dataset::AddColumn(absl::StrCat(dataset::CategoricalIdxToRepresentation(
                               label_col, label_value)),
                           dataset::proto::ColumnType::NUMERICAL, &dataspec);
      }
    } break;
    case model::proto::Task::REGRESSION:
    case model::proto::Task::RANKING:
      dataset::AddColumn(label_col.name(),
                         dataset::proto::ColumnType::NUMERICAL, &dataspec);
      break;
    default:
      LOG(FATAL) << "Non supported task.";
      break;
  }
  return dataspec;
}

}  // namespace utils
}  // namespace yggdrasil_decision_forests
