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

#include "yggdrasil_decision_forests/model/gradient_boosted_trees/gradient_boosted_trees.h"

#include <stddef.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/container/fixed_array.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "yggdrasil_decision_forests/dataset/data_spec.pb.h"
#include "yggdrasil_decision_forests/dataset/example.pb.h"
#include "yggdrasil_decision_forests/dataset/vertical_dataset.h"
#include "yggdrasil_decision_forests/metric/metric.pb.h"
#include "yggdrasil_decision_forests/model/abstract_model.h"
#include "yggdrasil_decision_forests/model/abstract_model.pb.h"
#include "yggdrasil_decision_forests/model/decision_tree/decision_tree.h"
#include "yggdrasil_decision_forests/model/decision_tree/decision_tree.pb.h"
#include "yggdrasil_decision_forests/model/decision_tree/decision_tree_io.h"
#include "yggdrasil_decision_forests/model/decision_tree/structure_analysis.h"
#include "yggdrasil_decision_forests/model/gradient_boosted_trees/gradient_boosted_trees.pb.h"
#include "yggdrasil_decision_forests/model/prediction.pb.h"
#include "yggdrasil_decision_forests/utils/compatibility.h"
#include "yggdrasil_decision_forests/utils/distribution.pb.h"
#include "yggdrasil_decision_forests/utils/filesystem.h"
#include "yggdrasil_decision_forests/utils/logging.h"
#include "yggdrasil_decision_forests/utils/status_macros.h"
#include "yggdrasil_decision_forests/utils/usage.h"

namespace yggdrasil_decision_forests {
namespace model {
namespace gradient_boosted_trees {

constexpr char GradientBoostedTreesModel::kRegisteredName[];

namespace {
using ::yggdrasil_decision_forests::model::decision_tree::
    StrAppendForestStructureStatistics;
// Basename for the shards containing the nodes.
constexpr char kNodeBaseFilename[] = "nodes";
// Filename containing the gradient boosted trees header.
constexpr char kHeaderFilename[] = "gradient_boosted_trees_header.pb";

}  // namespace

absl::Status GradientBoostedTreesModel::Save(
    absl::string_view directory) const {
  RETURN_IF_ERROR(file::RecursivelyCreateDir(directory, file::Defaults()));

  // Format used to store the nodes.
  std::string format;
  if (node_format_.has_value()) {
    format = node_format_.value();
  } else {
    ASSIGN_OR_RETURN(format, decision_tree::RecommendedSerializationFormat());
  }

  int num_shards;
  RETURN_IF_ERROR(decision_tree::SaveTreesToDisk(
      directory, kNodeBaseFilename, decision_trees_, format, &num_shards));
  proto::Header header;
  header.set_node_format(format);
  header.set_num_node_shards(num_shards);
  header.set_num_trees(decision_trees_.size());
  header.set_loss(loss_);
  header.set_num_trees_per_iter(num_trees_per_iter_);
  header.set_validation_loss(validation_loss_);
  *header.mutable_initial_predictions() = google::protobuf::RepeatedField<float>(
      initial_predictions_.begin(), initial_predictions_.end());
  *header.mutable_training_logs() = training_logs_;
  RETURN_IF_ERROR(file::SetBinaryProto(
      file::JoinPath(directory, kHeaderFilename), header, file::Defaults()));
  return absl::OkStatus();
}

absl::Status GradientBoostedTreesModel::Load(absl::string_view directory) {
  proto::Header header;
  RETURN_IF_ERROR(file::GetBinaryProto(
      file::JoinPath(directory, kHeaderFilename), &header, file::Defaults()));
  RETURN_IF_ERROR(decision_tree::LoadTreesFromDisk(
      directory, kNodeBaseFilename, header.num_node_shards(),
      header.num_trees(), header.node_format(), &decision_trees_));
  node_format_ = header.node_format();
  loss_ = header.loss();
  initial_predictions_.assign(header.initial_predictions().begin(),
                              header.initial_predictions().end());
  num_trees_per_iter_ = header.num_trees_per_iter();
  validation_loss_ = header.validation_loss();
  training_logs_ = header.training_logs();
  return absl::OkStatus();
}

absl::Status GradientBoostedTreesModel::Validate() const {
  RETURN_IF_ERROR(AbstractModel::Validate());

  const auto validate_leaf =
      [](const decision_tree::proto::Node& node) -> absl::Status {
    if (!node.has_regressor()) {
      return absl::InvalidArgumentError("Regressor missing");
    }
    return absl::OkStatus();
  };

  for (const auto& tree : decision_trees_) {
    RETURN_IF_ERROR(tree->Validate(data_spec(), validate_leaf));
  }

  if ((decision_trees_.size() % num_trees_per_iter_) != 0) {
    return absl::InvalidArgumentError("Invalid number of trees in GBDT");
  }

  int expected_initial_predictions_size = -1;
  switch (task()) {
    case model::proto::Task::CLASSIFICATION:
      if (loss_ == proto::Loss::MULTINOMIAL_LOG_LIKELIHOOD) {
        expected_initial_predictions_size =
            label_col_spec().categorical().number_of_unique_values() - 1;
      } else if (loss_ == proto::Loss::BINOMIAL_LOG_LIKELIHOOD) {
        expected_initial_predictions_size = 1;
      } else {
        return absl::InvalidArgumentError("Invalid loss in GBDT");
      }
      break;
    case model::proto::Task::REGRESSION:
      expected_initial_predictions_size = 1;
      break;
    case model::proto::Task::RANKING:
      expected_initial_predictions_size = 1;
      if (ranking_group_col_idx() == -1) {
        return absl::InvalidArgumentError("Invalid ranking_group_col in GBDT");
      }
      break;
    default:
      return absl::InvalidArgumentError("Unknown task in GBDT");
  }
  if (initial_predictions_.size() != expected_initial_predictions_size) {
    return absl::InvalidArgumentError("Invalid initial_predictions in GBDT");
  }
  if (expected_initial_predictions_size != num_trees_per_iter_) {
    return absl::InvalidArgumentError("Invalid num_trees_per_iter_ in GBDT");
  }
  return absl::OkStatus();
}

size_t GradientBoostedTreesModel::EstimateModelSizeInByte() const {
  return sizeof(GradientBoostedTreesModel) +
         decision_tree::EstimateSizeInByte(decision_trees_);
}

int64_t GradientBoostedTreesModel::NumNodes() const {
  return decision_tree::NumberOfNodes(decision_trees_);
}

bool GradientBoostedTreesModel::
    IsMissingValueConditionResultFollowGlobalImputation() const {
  return decision_tree::IsMissingValueConditionResultFollowGlobalImputation(
      data_spec(), decision_trees_);
}

// Add a new tree to the model.
void GradientBoostedTreesModel::AddTree(
    std::unique_ptr<decision_tree::DecisionTree> decision_tree) {
  decision_trees_.push_back(std::move(decision_tree));
}

void GradientBoostedTreesModel::CountFeatureUsage(
    std::unordered_map<int32_t, int64_t>* feature_usage) const {
  for (const auto& tree : decision_trees_) {
    tree->CountFeatureUsage(feature_usage);
  }
}

void GradientBoostedTreesModel::Predict(
    const dataset::VerticalDataset& dataset,
    dataset::VerticalDataset::row_t row_idx,
    model::proto::Prediction* prediction) const {
  utils::usage::OnInference(1);
  switch (loss_) {
    case proto::Loss::BINOMIAL_LOG_LIKELIHOOD: {
      double accumulator = initial_predictions_[0];
      CallOnAllLeafs(dataset, row_idx,
                     [&accumulator](const decision_tree::proto::Node& node) {
                       accumulator += node.regressor().top_value();
                     });
      const float proba_true = 1.f / (1.f + std::exp(-accumulator));
      prediction->mutable_classification()->set_value(proba_true > 0.5f ? 2
                                                                        : 1);
      auto* dist = prediction->mutable_classification()->mutable_distribution();
      dist->mutable_counts()->Resize(3, 0.f);
      dist->set_sum(1.f);
      dist->set_counts(1, 1.f - proba_true);
      dist->set_counts(2, proba_true);
    } break;
    case proto::Loss::MULTINOMIAL_LOG_LIKELIHOOD: {
      absl::FixedArray<float> accumulator(num_trees_per_iter_);
      // Zero initial prediction for the MULTINOMIAL_LOG_LIKELIHOOD.
      std::fill(accumulator.begin(), accumulator.end(), 0);

      {
        int accumulator_cell_idx = 0;
        CallOnAllLeafs(dataset, row_idx,
                       [&accumulator, &accumulator_cell_idx,
                        this](const decision_tree::proto::Node& node) {
                         accumulator[accumulator_cell_idx] +=
                             node.regressor().top_value();
                         accumulator_cell_idx++;
                         if (accumulator_cell_idx == num_trees_per_iter_) {
                           accumulator_cell_idx = 0;
                         }
                       });
      }

      auto* dist = prediction->mutable_classification()->mutable_distribution();
      dist->mutable_counts()->Resize(num_trees_per_iter_ + 1, 0.f);

      float sum_exp = 0;
      for (int accumulator_idx = 0; accumulator_idx < num_trees_per_iter_;
           accumulator_idx++) {
        const float exp_val = std::exp(accumulator[accumulator_idx]);
        sum_exp += exp_val;
        // The offset of 1 between the class idx and the accumulator_idx is to
        // skill the special OOD value with index 0.
        dist->set_counts(accumulator_idx + 1, exp_val);
      }
      const float normalization = (sum_exp > 0) ? (1.f / sum_exp) : 0.f;

      float highest_cell_value = 0;
      int highest_cell_idx = 0;

      for (int accumulator_idx = 0; accumulator_idx < num_trees_per_iter_;
           accumulator_idx++) {
        const float value = dist->counts(accumulator_idx + 1);
        if (value > highest_cell_value) {
          highest_cell_value = value;
          highest_cell_idx = accumulator_idx;
        }
        dist->set_counts(accumulator_idx + 1, value * normalization);
      }
      dist->set_sum(1.f);
      prediction->mutable_classification()->set_value(highest_cell_idx + 1);
    } break;
    case proto::Loss::SQUARED_ERROR: {
      double accumulator = initial_predictions_[0];
      CallOnAllLeafs(dataset, row_idx,
                     [&accumulator](const decision_tree::proto::Node& node) {
                       accumulator += node.regressor().top_value();
                     });
      if (task() == model::proto::RANKING) {
        prediction->mutable_ranking()->set_relevance(accumulator);
      } else if (task() == model::proto::REGRESSION) {
        prediction->mutable_regression()->set_value(accumulator);
      } else {
        LOG(FATAL) << "Non supported task";
      }
    } break;
    case proto::Loss::LAMBDA_MART_NDCG5:
    case proto::Loss::XE_NDCG_MART: {
      double accumulator = initial_predictions_[0];
      CallOnAllLeafs(dataset, row_idx,
                     [&accumulator](const decision_tree::proto::Node& node) {
                       accumulator += node.regressor().top_value();
                     });
      prediction->mutable_ranking()->set_relevance(accumulator);
    } break;
    default:
      LOG(FATAL) << "Not implemented";
  }
}

void GradientBoostedTreesModel::Predict(
    const dataset::proto::Example& example,
    model::proto::Prediction* prediction) const {
  utils::usage::OnInference(1);
  switch (loss_) {
    case proto::Loss::BINOMIAL_LOG_LIKELIHOOD: {
      double accumulator = initial_predictions_[0];
      CallOnAllLeafs(example,
                     [&accumulator](const decision_tree::proto::Node& node) {
                       accumulator += node.regressor().top_value();
                     });
      const float proba_true = 1.f / (1.f + std::exp(-accumulator));
      prediction->mutable_classification()->set_value(proba_true > 0.5f ? 2
                                                                        : 1);
      auto* dist = prediction->mutable_classification()->mutable_distribution();
      dist->mutable_counts()->Resize(3, 0.f);
      dist->set_sum(1.f);
      dist->set_counts(1, 1.f - proba_true);
      dist->set_counts(2, proba_true);
    } break;

    case proto::Loss::MULTINOMIAL_LOG_LIKELIHOOD: {
      absl::FixedArray<float> accumulator(num_trees_per_iter_);
      // Zero initial prediction for the MULTINOMIAL_LOG_LIKELIHOOD.
      std::fill(accumulator.begin(), accumulator.end(), 0);

      {
        int accumulator_cell_idx = 0;
        CallOnAllLeafs(example, [&accumulator, &accumulator_cell_idx,
                                 this](const decision_tree::proto::Node& node) {
          accumulator[accumulator_cell_idx] += node.regressor().top_value();
          accumulator_cell_idx++;
          if (accumulator_cell_idx == num_trees_per_iter_) {
            accumulator_cell_idx = 0;
          }
        });
        CHECK_EQ(accumulator_cell_idx, 0);
      }

      // Note: Why the "+1"? : "prediction" reserves the first value for the out
      // of vocabulary which is not taken into account in "accumulator'.

      auto* dist = prediction->mutable_classification()->mutable_distribution();
      dist->mutable_counts()->Resize(num_trees_per_iter_ + 1, 0.f);

      float sum_exp = 0;
      for (int accumulator_idx = 0; accumulator_idx < num_trees_per_iter_;
           accumulator_idx++) {
        const float exp_val = std::exp(accumulator[accumulator_idx]);
        sum_exp += exp_val;
        dist->set_counts(accumulator_idx + 1, exp_val);
      }

      const float normalization = 1.f / sum_exp;

      float highest_cell_value = 0;
      int highest_cell_idx = 0;

      for (int accumulator_idx = 0; accumulator_idx < num_trees_per_iter_;
           accumulator_idx++) {
        const float value = dist->counts(accumulator_idx + 1);
        if (value > highest_cell_value) {
          highest_cell_value = value;
          highest_cell_idx = accumulator_idx;
        }
        dist->set_counts(accumulator_idx + 1, value * normalization);
      }
      dist->set_sum(1.f);
      prediction->mutable_classification()->set_value(highest_cell_idx + 1);
    } break;

    case proto::Loss::SQUARED_ERROR: {
      double accumulator = initial_predictions_[0];
      CallOnAllLeafs(example,
                     [&accumulator](const decision_tree::proto::Node& node) {
                       accumulator += node.regressor().top_value();
                     });
      prediction->mutable_regression()->set_value(accumulator);
    } break;
    case proto::Loss::LAMBDA_MART_NDCG5:
    case proto::Loss::XE_NDCG_MART: {
      double accumulator = initial_predictions_[0];
      CallOnAllLeafs(example,
                     [&accumulator](const decision_tree::proto::Node& node) {
                       accumulator += node.regressor().top_value();
                     });
      prediction->mutable_ranking()->set_relevance(accumulator);
    } break;
    default:
      LOG(FATAL) << "Not implemented";
  }
}

void GradientBoostedTreesModel::CallOnAllLeafs(
    const dataset::VerticalDataset& dataset,
    dataset::VerticalDataset::row_t row_idx,
    const std::function<void(const decision_tree::proto::Node& node)>& callback)
    const {
  for (const auto& tree : decision_trees_) {
    callback(tree->GetLeaf(dataset, row_idx));
  }
}

void GradientBoostedTreesModel::CallOnAllLeafs(
    const dataset::proto::Example& example,
    const std::function<void(const decision_tree::proto::Node& node)>& callback)
    const {
  for (const auto& tree : decision_trees_) {
    callback(tree->GetLeaf(example));
  }
}

void GradientBoostedTreesModel::IterateOnNodes(
    const std::function<void(const decision_tree::NodeWithChildren& node,
                             const int depth)>& call_back) const {
  for (auto& tree : decision_trees_) {
    tree->IterateOnNodes(call_back);
  }
}

void GradientBoostedTreesModel::IterateOnMutableNodes(
    const std::function<void(decision_tree::NodeWithChildren* node,
                             const int depth)>& call_back) const {
  for (auto& tree : decision_trees_) {
    tree->IterateOnMutableNodes(call_back);
  }
}

void GradientBoostedTreesModel::AppendModelStructure(
    std::string* description) const {
  decision_tree::AppendModelStructure(decision_trees_, data_spec(),
                                      label_col_idx_, description);
}

metric::proto::EvaluationResults
GradientBoostedTreesModel::ValidationEvaluation() const {
  if (std::isnan(validation_loss_)) {
    LOG(FATAL) << "Validation evaluation not available for the Gradient "
                  "Boosted Tree model as no validation dataset was provided "
                  "for training (i.e. validation_set_ratio == 0).";
  }
  metric::proto::EvaluationResults validation_evaluation;
  validation_evaluation.set_loss_value(validation_loss_);
  validation_evaluation.set_loss_name(proto::Loss_Name(loss_));
  return validation_evaluation;
}

void GradientBoostedTreesModel::AppendDescriptionAndStatistics(
    bool full_definition, std::string* description) const {
  AbstractModel::AppendDescriptionAndStatistics(full_definition, description);
  absl::StrAppend(description, "\n");

  absl::StrAppend(description, "Loss: ", proto::Loss_Name(loss_), "\n");
  if (!std::isnan(validation_loss_)) {
    absl::StrAppend(description, "Validation loss value: ", validation_loss_,
                    "\n");
  }
  absl::StrAppend(description,
                  "Number of trees per iteration: ", num_trees_per_iter_, "\n");

  absl::StrAppend(description,
                  "Node format: ", node_format_.value_or("NOT_SET"), "\n");

  StrAppendForestStructureStatistics(data_spec(), decision_trees(),
                                     description);

  if (full_definition) {
    absl::StrAppend(description, "\nModel Structure:\n");
    absl::StrAppend(description, "Initial predictions: $0\n",
                    absl::StrJoin(initial_predictions_, ","));
    absl::StrAppend(description, "\n");
    AppendModelStructure(description);
  }
}

std::vector<std::string>
GradientBoostedTreesModel::AvailableVariableImportances() const {
  auto variable_importances = AbstractModel::AvailableVariableImportances();
  variable_importances.push_back(
      decision_tree::kVariableImportanceNumberOfNodes);
  variable_importances.push_back(
      decision_tree::kVariableImportanceNumberOfTimesAsRoot);
  variable_importances.push_back(decision_tree::kVariableImportanceSumScore);
  variable_importances.push_back(
      decision_tree::kVariableImportanceMeanMinDepth);
  return variable_importances;
}

utils::StatusOr<std::vector<model::proto::VariableImportance>>
GradientBoostedTreesModel::GetVariableImportance(absl::string_view key) const {
  // Tree structure variable importances.
  if (key == decision_tree::kVariableImportanceNumberOfNodes) {
    return decision_tree::StructureNumberOfTimesInNode(decision_trees());
  } else if (key == decision_tree::kVariableImportanceNumberOfTimesAsRoot) {
    return decision_tree::StructureNumberOfTimesAsRoot(decision_trees());
  } else if (key == decision_tree::kVariableImportanceSumScore) {
    return decision_tree::StructureSumScore(decision_trees());
  } else if (key == decision_tree::kVariableImportanceMeanMinDepth) {
    return decision_tree::StructureMeanMinDepth(decision_trees(),
                                                data_spec().columns_size());
  } else {
    return AbstractModel::GetVariableImportance(key);
  }
}

REGISTER_AbstractModel(GradientBoostedTreesModel,
                       GradientBoostedTreesModel::kRegisteredName);

}  // namespace gradient_boosted_trees
}  // namespace model
}  // namespace yggdrasil_decision_forests
