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

#include "yggdrasil_decision_forests/learner/gradient_boosted_trees/gradient_boosted_trees_loss.h"

#include <stddef.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/fixed_array.h"
#include "absl/container/flat_hash_map.h"
#include "absl/memory/memory.h"
#include "yggdrasil_decision_forests/dataset/data_spec.pb.h"
#include "yggdrasil_decision_forests/dataset/vertical_dataset.h"
#include "yggdrasil_decision_forests/learner/abstract_learner.pb.h"
#include "yggdrasil_decision_forests/learner/decision_tree/training.h"
#include "yggdrasil_decision_forests/learner/decision_tree/utils.h"
#include "yggdrasil_decision_forests/learner/gradient_boosted_trees/gradient_boosted_trees.pb.h"
#include "yggdrasil_decision_forests/metric/metric.h"
#include "yggdrasil_decision_forests/metric/ranking_ndcg.h"
#include "yggdrasil_decision_forests/model/abstract_model.pb.h"
#include "yggdrasil_decision_forests/model/decision_tree/decision_tree.h"
#include "yggdrasil_decision_forests/model/decision_tree/decision_tree.pb.h"
#include "yggdrasil_decision_forests/model/gradient_boosted_trees/gradient_boosted_trees.pb.h"
#include "yggdrasil_decision_forests/utils/compatibility.h"
#include "yggdrasil_decision_forests/utils/logging.h"
#include "yggdrasil_decision_forests/utils/random.h"

namespace yggdrasil_decision_forests {
namespace model {
namespace gradient_boosted_trees {
namespace {
using row_t = dataset::VerticalDataset::row_t;

// Maximum number of items in a ranking group (e.g. maximum number of queries
// for a document). While possible, it is very unlikely that a user would exceed
// this value. A most likely scenario would be a
// configuration/dataset-preparation error.
constexpr int64_t kMaximumItemsInRankingGroup = 2000;

constexpr int kNDCG5Truncation = 5;

// Index of the secondary metrics according to the type of loss.
constexpr int kBinomialLossSecondaryMetricClassificationIdx = 0;

// Minimum length of the hessian (i.e. denominator) in the Newton step
// optimization.
constexpr float kMinHessianForNewtonStep = 0.001f;

// Ensures that the value is finite i.e. not NaN and not infinite.
// This is a no-op in release mode.
template <typename T>
void DCheckIsFinite(T v) {
  DCHECK(!std::isnan(v) && !std::isinf(v));
}

void UpdatePredictionWithSingleUnivariateTree(
    const dataset::VerticalDataset& dataset,
    const decision_tree::DecisionTree& tree, std::vector<float>* predictions,
    double* mean_abs_prediction) {
  double sum_abs_predictions = 0;
  for (row_t example_idx = 0; example_idx < dataset.nrow(); example_idx++) {
    const auto& leaf = tree.GetLeaf(dataset, example_idx);
    (*predictions)[example_idx] += leaf.regressor().top_value();
    sum_abs_predictions += std::abs(leaf.regressor().top_value());
  }
  if (mean_abs_prediction) {
    *mean_abs_prediction = sum_abs_predictions / dataset.nrow();
  }
}

void UpdatePredictionWithMultipleUnivariateTrees(
    const dataset::VerticalDataset& dataset,
    const std::vector<const decision_tree::DecisionTree*>& trees,
    std::vector<float>* predictions, double* mean_abs_prediction) {
  double sum_abs_predictions = 0;
  const int num_trees = trees.size();
  for (row_t example_idx = 0; example_idx < dataset.nrow(); example_idx++) {
    for (int grad_idx = 0; grad_idx < num_trees; grad_idx++) {
      const auto& leaf = trees[grad_idx]->GetLeaf(dataset, example_idx);
      (*predictions)[grad_idx + example_idx * num_trees] +=
          leaf.regressor().top_value();
      sum_abs_predictions += std::abs(leaf.regressor().top_value());
    }
  }
  if (mean_abs_prediction) {
    *mean_abs_prediction = sum_abs_predictions / dataset.nrow();
  }
}

}  // namespace

utils::StatusOr<std::unique_ptr<AbstractLoss>> CreateLoss(
    proto::Loss loss, model::proto::Task task,
    const dataset::proto::Column& label_column,
    const proto::GradientBoostedTreesTrainingConfig& config) {
  std::unique_ptr<AbstractLoss> loss_imp;
  switch (loss) {
    case proto::BINOMIAL_LOG_LIKELIHOOD:
      loss_imp = absl::make_unique<BinomialLogLikelihoodLoss>(config, task,
                                                              label_column);
      break;
    case proto::SQUARED_ERROR:
      loss_imp =
          absl::make_unique<MeanSquaredErrorLoss>(config, task, label_column);
      break;
    case proto::MULTINOMIAL_LOG_LIKELIHOOD:
      loss_imp = absl::make_unique<MultinomialLogLikelihoodLoss>(config, task,
                                                                 label_column);
      break;
    case proto::LAMBDA_MART_NDCG5:
      loss_imp = absl::make_unique<NDCGLoss>(config, task, label_column);
      break;
    case proto::XE_NDCG_MART:
      loss_imp =
          absl::make_unique<CrossEntropyNDCGLoss>(config, task, label_column);
      break;
    default:
      return absl::UnimplementedError("Non implemented loss");
  }
  RETURN_IF_ERROR(loss_imp->Status());
  return loss_imp;
}

BinomialLogLikelihoodLoss::BinomialLogLikelihoodLoss(
    const proto::GradientBoostedTreesTrainingConfig& gbt_config,
    const model::proto::Task task, const dataset::proto::Column& label_column)
    : gbt_config_(gbt_config), task_(task), label_column_(label_column) {}

absl::Status BinomialLogLikelihoodLoss::Status() const {
  if (task_ != model::proto::Task::CLASSIFICATION)
    return absl::InvalidArgumentError(
        "Binomial log likelihood loss is only compatible with a "
        "classification task");
  if (label_column_.categorical().number_of_unique_values() != 3)
    return absl::InvalidArgumentError(
        "Binomial log likelihood loss is only compatible with a BINARY "
        "classification task");
  return absl::OkStatus();
}

utils::StatusOr<std::vector<float>>
BinomialLogLikelihoodLoss::InitialPredictions(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& weights) const {
  // Return: log(y/(1-y)) with y the ratio of positive labels.
  double weighted_sum_positive = 0;
  double sum_weights = 0;
  const auto* labels =
      dataset.ColumnWithCast<dataset::VerticalDataset::CategoricalColumn>(
          label_col_idx);
  for (row_t example_idx = 0; example_idx < dataset.nrow(); example_idx++) {
    sum_weights += weights[example_idx];
    weighted_sum_positive +=
        weights[example_idx] * (labels->values()[example_idx] == 2);
  }
  const auto ratio_positive = weighted_sum_positive / sum_weights;
  if (ratio_positive == 0.0) {
    return std::vector<float>{-std::numeric_limits<float>::max()};
  } else if (ratio_positive == 1.0) {
    return std::vector<float>{std::numeric_limits<float>::max()};
  } else {
    return std::vector<float>{
        static_cast<float>(std::log(ratio_positive / (1. - ratio_positive)))};
  }
}

absl::Status BinomialLogLikelihoodLoss::UpdateGradients(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& predictions,
    const RankingGroupsIndices* ranking_index,
    std::vector<GradientData>* gradients, utils::RandomEngine* random) const {
  // Set the gradient to:
  //   label - 1/(1 + exp(-prediction))
  // where "label" is in {0,1} and prediction is the probability of
  // label=1.
  if (gradients->size() != 1) {
    return absl::InternalError("Wrong gradient shape");
  }
  const auto* labels =
      dataset.ColumnWithCast<dataset::VerticalDataset::CategoricalColumn>(
          label_col_idx);
  std::vector<float>& gradient_data = (*gradients)[0].gradient;
  std::vector<float>* hessian_data = nullptr;
  if (gbt_config_.use_hessian_gain()) {
    hessian_data = (*gradients)[0].hessian;
  }
  for (row_t example_idx = 0; example_idx < dataset.nrow(); example_idx++) {
    const float label = (labels->values()[example_idx] == 2) ? 1.f : 0.f;
    const float prediction = predictions[example_idx];
    const float prediction_proba = 1.f / (1.f + std::exp(-prediction));
    DCheckIsFinite(prediction);
    DCheckIsFinite(prediction_proba);
    gradient_data[example_idx] = label - prediction_proba;
    if (hessian_data) {
      (*hessian_data)[example_idx] = prediction_proba * (1 - prediction_proba);
    }
  }
  return absl::OkStatus();
}

decision_tree::CreateSetLeafValueFunctor
BinomialLogLikelihoodLoss::SetLeafFunctor(
    const std::vector<float>& predictions,
    const std::vector<GradientData>& gradients, const int label_col_idx) const {
  return
      [this, &predictions, label_col_idx](
          const dataset::VerticalDataset& train_dataset,
          const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
          const std::vector<float>& weights,
          const model::proto::TrainingConfig& config,
          const model::proto::TrainingConfigLinking& config_link,
          decision_tree::NodeWithChildren* node) {
        return SetLeaf(train_dataset, selected_examples, weights, config,
                       config_link, predictions, label_col_idx, node);
      };
}

void BinomialLogLikelihoodLoss::SetLeaf(
    const dataset::VerticalDataset& train_dataset,
    const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
    const std::vector<float>& weights,
    const model::proto::TrainingConfig& config,
    const model::proto::TrainingConfigLinking& config_link,
    const std::vector<float>& predictions, const int label_col_idx,
    decision_tree::NodeWithChildren* node) const {
  if (!gbt_config_.use_hessian_gain()) {
    decision_tree::SetRegressionLabelDistribution(
        train_dataset, selected_examples, weights, config_link,
        node->mutable_node());
    // Even if "use_hessian_gain" is not enabled for the splits. We use a Newton
    // step in the leafs i.e. if "use_hessian_gain" is false, we need all the
    // information.
  }

  // Set the value of the leaf to:
  //   (\sum_i weight[i] * (label[i] - p[i]) ) / (\sum_i weight[i] * p[i] *
  //   (1-p[i]))
  // with: p[i] = 1/(1+exp(-prediction)
  const auto* labels =
      train_dataset.ColumnWithCast<dataset::VerticalDataset::CategoricalColumn>(
          label_col_idx);
  double numerator = 0;
  double denominator = 0;
  double sum_weights = 0;
  static const float bool_to_float[] = {0.f, 1.f};
  for (const auto example_idx : selected_examples) {
    const float weight = weights[example_idx];
    const float label = bool_to_float[labels->values()[example_idx] == 2];
    const float prediction = predictions[example_idx];
    const float p = 1.f / (1.f + std::exp(-prediction));
    numerator += weight * (label - p);
    denominator += weight * p * (1.f - p);
    sum_weights += weight;
    DCheckIsFinite(numerator);
    DCheckIsFinite(denominator);
  }

  if (denominator <= kMinHessianForNewtonStep) {
    denominator = kMinHessianForNewtonStep;
  }

  if (gbt_config_.use_hessian_gain()) {
    auto* reg = node->mutable_node()->mutable_regressor();
    reg->set_sum_gradients(numerator);
    reg->set_sum_hessians(denominator);
    reg->set_sum_weights(sum_weights);
  }

  const auto leaf_value =
      gbt_config_.shrinkage() *
      static_cast<float>(decision_tree::l1_threshold(
                             numerator, gbt_config_.l1_regularization()) /
                         (denominator + gbt_config_.l2_regularization()));

  node->mutable_node()->mutable_regressor()->set_top_value(
      utils::clamp(leaf_value, -gbt_config_.clamp_leaf_logit(),
                   gbt_config_.clamp_leaf_logit()));
}

absl::Status BinomialLogLikelihoodLoss::UpdatePredictions(
    const std::vector<const decision_tree::DecisionTree*>& new_trees,
    const dataset::VerticalDataset& dataset, std::vector<float>* predictions,
    double* mean_abs_prediction) const {
  if (new_trees.size() != 1) {
    return absl::InternalError("Wrong number of trees");
  }
  UpdatePredictionWithSingleUnivariateTree(dataset, *new_trees.front(),
                                           predictions, mean_abs_prediction);
  return absl::OkStatus();
}

std::vector<std::string> BinomialLogLikelihoodLoss::SecondaryMetricNames()
    const {
  return {"accuracy"};
}

absl::Status BinomialLogLikelihoodLoss::Loss(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& predictions, const std::vector<float>& weights,
    const RankingGroupsIndices* ranking_index, float* loss_value,
    std::vector<float>* secondary_metric) const {
  const auto* labels =
      dataset.ColumnWithCast<dataset::VerticalDataset::CategoricalColumn>(
          label_col_idx);
  double sum_loss = 0;
  double count_correct_predictions = 0;
  double sum_weights = 0;
  for (row_t example_idx = 0; example_idx < dataset.nrow(); example_idx++) {
    const bool pos_label = labels->values()[example_idx] == 2;
    const float label = pos_label ? 1.f : 0.f;
    const float prediction = predictions[example_idx];
    const float weight = weights[example_idx];
    const bool pos_prediction = prediction >= 0;
    sum_weights += weight;
    if (pos_label == pos_prediction) {
      count_correct_predictions += weight;
    }
    // Loss:
    //   -2 * ( label * prediction - log(1+exp(prediction)))
    sum_loss -=
        2 * weight * (label * prediction - std::log(1 + std::exp(prediction)));
    DCheckIsFinite(sum_loss);
  }
  secondary_metric->resize(1);
  if (sum_weights > 0) {
    *loss_value = static_cast<float>(sum_loss / sum_weights);
    (*secondary_metric)[kBinomialLossSecondaryMetricClassificationIdx] =
        static_cast<float>(count_correct_predictions / sum_weights);
  } else {
    *loss_value =
        (*secondary_metric)[kBinomialLossSecondaryMetricClassificationIdx] =
            std::numeric_limits<float>::quiet_NaN();
  }
  return absl::OkStatus();
}

MeanSquaredErrorLoss::MeanSquaredErrorLoss(
    const proto::GradientBoostedTreesTrainingConfig& gbt_config,
    const model::proto::Task task, const dataset::proto::Column& label_column)
    : task_(task), gbt_config_(gbt_config) {
  if (task != model::proto::Task::REGRESSION &&
      task != model::proto::Task::RANKING) {
    LOG(FATAL) << "Mean squared error loss is only compatible with a "
                  "regression or ranking task";
  }
}

absl::Status MeanSquaredErrorLoss::Status() const {
  if (task_ != model::proto::Task::REGRESSION &&
      task_ != model::proto::Task::RANKING) {
    return absl::InvalidArgumentError(
        "Mean squared error loss is only compatible with a "
        "regression or ranking task");
  }
  return absl::OkStatus();
}

utils::StatusOr<std::vector<float>> MeanSquaredErrorLoss::InitialPredictions(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& weights) const {
  // Note: The initial value is the weighted mean of the labels.
  double weighted_sum_values = 0;
  double sum_weights = 0;
  const auto* labels =
      dataset.ColumnWithCast<dataset::VerticalDataset::NumericalColumn>(
          label_col_idx);
  for (row_t example_idx = 0; example_idx < dataset.nrow(); example_idx++) {
    sum_weights += weights[example_idx];
    weighted_sum_values += weights[example_idx] * labels->values()[example_idx];
  }
  // Note: Null and negative weights are detected by the dataspec
  // computation.
  if (sum_weights <= 0) {
    return absl::InvalidArgumentError(
        "The sum of weights are null. The dataset is "
        "either empty or contains null weights.");
  }
  return std::vector<float>{
      static_cast<float>(weighted_sum_values / sum_weights)};
}

absl::Status MeanSquaredErrorLoss::UpdateGradients(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& predictions,
    const RankingGroupsIndices* ranking_index,
    std::vector<GradientData>* gradients, utils::RandomEngine* random) const {
  // Set the gradient to:
  //   label - prediction
  if (gradients->size() != 1) {
    return absl::InternalError("Wrong gradient shape");
  }
  const auto* labels =
      dataset.ColumnWithCast<dataset::VerticalDataset::NumericalColumn>(
          label_col_idx);
  std::vector<float>& gradient_data = (*gradients)[0].gradient;
  for (row_t example_idx = 0; example_idx < dataset.nrow(); example_idx++) {
    const float label = labels->values()[example_idx];
    const float prediction = predictions[example_idx];
    gradient_data[example_idx] = label - prediction;
  }
  return absl::OkStatus();
}

absl::Status MeanSquaredErrorLoss::UpdatePredictions(
    const std::vector<const decision_tree::DecisionTree*>& new_trees,
    const dataset::VerticalDataset& dataset, std::vector<float>* predictions,
    double* mean_abs_prediction) const {
  if (new_trees.size() != 1) {
    return absl::InternalError("Wrong number of trees");
  }
  UpdatePredictionWithSingleUnivariateTree(dataset, *new_trees.front(),
                                           predictions, mean_abs_prediction);
  return absl::OkStatus();
}

decision_tree::CreateSetLeafValueFunctor MeanSquaredErrorLoss::SetLeafFunctor(
    const std::vector<float>& predictions,
    const std::vector<GradientData>& gradients, const int label_col_idx) const {
  return
      [this, &predictions, label_col_idx](
          const dataset::VerticalDataset& train_dataset,
          const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
          const std::vector<float>& weights,
          const model::proto::TrainingConfig& config,
          const model::proto::TrainingConfigLinking& config_link,
          decision_tree::NodeWithChildren* node) {
        return SetLeaf(train_dataset, selected_examples, weights, config,
                       config_link, predictions, label_col_idx, node);
      };
}

void MeanSquaredErrorLoss::SetLeaf(
    const dataset::VerticalDataset& train_dataset,
    const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
    const std::vector<float>& weights,
    const model::proto::TrainingConfig& config,
    const model::proto::TrainingConfigLinking& config_link,
    const std::vector<float>& predictions, const int label_col_idx,
    decision_tree::NodeWithChildren* node) const {
  decision_tree::SetRegressionLabelDistribution(
      train_dataset, selected_examples, weights, config_link,
      node->mutable_node());

  // Set the value of the leaf to be the residual:
  //   label[i] - prediction
  const auto* labels =
      train_dataset.ColumnWithCast<dataset::VerticalDataset::NumericalColumn>(
          label_col_idx);
  double sum_weighted_values = 0;
  double sum_weights = 0;
  for (const auto example_idx : selected_examples) {
    const float label = labels->values()[example_idx];
    const float prediction = predictions[example_idx];
    sum_weighted_values += weights[example_idx] * (label - prediction);
    sum_weights += weights[example_idx];
  }
  if (sum_weights <= 0) {
    LOG(WARNING) << "Zero or negative weights in node";
  }
  // Note: The "sum_weights" terms carries an implicit 2x factor that is
  // integrated in the shrinkage. We don't integrate this factor here not to
  // change the behavior of existing training configurations.
  node->mutable_node()->mutable_regressor()->set_top_value(
      gbt_config_.shrinkage() * sum_weighted_values /
      (sum_weights + gbt_config_.l2_regularization() / 2));
}

std::vector<std::string> MeanSquaredErrorLoss::SecondaryMetricNames() const {
  if (task_ == model::proto::Task::RANKING) {
    return {"rmse", "NDCG@5"};
  } else {
    return {"rmse"};
  }
}

absl::Status MeanSquaredErrorLoss::Loss(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& predictions, const std::vector<float>& weights,
    const RankingGroupsIndices* ranking_index, float* loss_value,
    std::vector<float>* secondary_metric) const {
  const auto* labels =
      dataset.ColumnWithCast<dataset::VerticalDataset::NumericalColumn>(
          label_col_idx);
  const float rmse = metric::RMSE(labels->values(), predictions, weights);
  // The RMSE is also the loss.
  *loss_value = rmse;

  if (task_ == model::proto::Task::RANKING) {
    secondary_metric->resize(2);
    (*secondary_metric)[0] = rmse;
    (*secondary_metric)[1] =
        ranking_index->NDCG(predictions, weights, kNDCG5Truncation);
  } else {
    secondary_metric->resize(1);
    (*secondary_metric)[0] = rmse;
  }
  return absl::OkStatus();
}

MultinomialLogLikelihoodLoss::MultinomialLogLikelihoodLoss(
    const proto::GradientBoostedTreesTrainingConfig& gbt_config,
    const model::proto::Task task, const dataset::proto::Column& label_column)
    : gbt_config_(gbt_config), task_(task), label_column_(label_column) {
  dimension_ = label_column_.categorical().number_of_unique_values() - 1;
}

absl::Status MultinomialLogLikelihoodLoss::Status() const {
  if (task_ != model::proto::Task::CLASSIFICATION) {
    return absl::InvalidArgumentError(
        "Multinomial log-likelihood loss is only compatible with a "
        "classification task");
  }
  return absl::OkStatus();
}

utils::StatusOr<std::vector<float>>
MultinomialLogLikelihoodLoss::InitialPredictions(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& weights) const {
  // In Friedman paper (https://statweb.stanford.edu/~jhf/ftp/trebst.pdf),
  // the initial prediction is 0 for multi-class classification (algorithm
  // 6).
  return std::vector<float>(dimension_, 0);
}

absl::Status MultinomialLogLikelihoodLoss::UpdateGradients(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& predictions,
    const RankingGroupsIndices* ranking_index,
    std::vector<GradientData>* gradients, utils::RandomEngine* random) const {
  // Set the gradient to:
  //   label_i - pred_i
  // where "label_i" is in {0,1}.
  const auto* labels =
      dataset.ColumnWithCast<dataset::VerticalDataset::CategoricalColumn>(
          label_col_idx);
  absl::FixedArray<float> accumulator(gradients->size());
  for (row_t example_idx = 0; example_idx < dataset.nrow(); example_idx++) {
    // Compute normalization term.
    float sum_exp = 0;
    for (int grad_idx = 0; grad_idx < gradients->size(); grad_idx++) {
      float exp_val =
          std::exp(predictions[grad_idx + example_idx * gradients->size()]);
      accumulator[grad_idx] = exp_val;
      sum_exp += exp_val;
    }
    const float normalization = 1.f / sum_exp;
    // Update gradient.
    const int label_cat = labels->values()[example_idx];
    for (int grad_idx = 0; grad_idx < gradients->size(); grad_idx++) {
      const float label = (label_cat == (grad_idx + 1)) ? 1.f : 0.f;
      DCheckIsFinite(label);
      const float prediction = accumulator[grad_idx] * normalization;
      DCheckIsFinite(prediction);
      const float grad = label - prediction;
      const float abs_grad = std::abs(grad);
      DCheckIsFinite(grad);
      (*gradients)[grad_idx].gradient[example_idx] = grad;
      if (gbt_config_.use_hessian_gain()) {
        (*(*gradients)[grad_idx].hessian)[example_idx] =
            abs_grad * (1 - abs_grad);
        DCheckIsFinite(abs_grad * (1 - abs_grad));
      }
    }
  }
  return absl::OkStatus();
}

decision_tree::CreateSetLeafValueFunctor
MultinomialLogLikelihoodLoss::SetLeafFunctor(
    const std::vector<float>& predictions,
    const std::vector<GradientData>& gradients, const int label_col_idx) const {
  return
      [this, &predictions, label_col_idx](
          const dataset::VerticalDataset& train_dataset,
          const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
          const std::vector<float>& weights,
          const model::proto::TrainingConfig& config,
          const model::proto::TrainingConfigLinking& config_link,
          decision_tree::NodeWithChildren* node) {
        return SetLeaf(train_dataset, selected_examples, weights, config,
                       config_link, predictions, label_col_idx, node);
      };
}

void MultinomialLogLikelihoodLoss::SetLeaf(
    const dataset::VerticalDataset& train_dataset,
    const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
    const std::vector<float>& weights,
    const model::proto::TrainingConfig& config,
    const model::proto::TrainingConfigLinking& config_link,
    const std::vector<float>& predictions, const int label_col_idx,
    decision_tree::NodeWithChildren* node) const {
  // Initialize the distribution (as the "top_value" is overridden right
  // after.
  if (!gbt_config_.use_hessian_gain()) {
    decision_tree::SetRegressionLabelDistribution(
        train_dataset, selected_examples, weights, config_link,
        node->mutable_node());
  }

  // Set the value of the leaf to:
  //  (dim-1) / dim * ( \sum_i weight[i] grad[i] ) / (\sum_i |grad[i]| *
  //  (1-|grad[i]|))
  //
  // Note: The leaf value does not depends on the label value (directly).
  const auto& grad =
      train_dataset
          .ColumnWithCast<dataset::VerticalDataset::NumericalColumn>(
              config_link.label())
          ->values();
  double numerator = 0;
  double denominator = 0;
  double sum_weights = 0;
  for (const auto example_idx : selected_examples) {
    const float weight = weights[example_idx];
    numerator += weight * grad[example_idx];
    const float abs_grad = std::abs(grad[example_idx]);
    denominator += weight * abs_grad * (1 - abs_grad);
    sum_weights += weight;
    DCheckIsFinite(numerator);
    DCheckIsFinite(denominator);
  }

  if (denominator <= kMinHessianForNewtonStep) {
    denominator = kMinHessianForNewtonStep;
  }

  if (gbt_config_.use_hessian_gain()) {
    auto* reg = node->mutable_node()->mutable_regressor();
    reg->set_sum_gradients(numerator);
    reg->set_sum_hessians(denominator);
    reg->set_sum_weights(sum_weights);
  }

  numerator *= dimension_ - 1;
  denominator *= dimension_;
  const auto leaf_value =
      gbt_config_.shrinkage() *
      static_cast<float>(decision_tree::l1_threshold(
                             numerator, gbt_config_.l1_regularization()) /
                         (denominator + gbt_config_.l2_regularization()));
  DCheckIsFinite(leaf_value);

  node->mutable_node()->mutable_regressor()->set_top_value(
      utils::clamp(leaf_value, -gbt_config_.clamp_leaf_logit(),
                   gbt_config_.clamp_leaf_logit()));
}

absl::Status MultinomialLogLikelihoodLoss::UpdatePredictions(
    const std::vector<const decision_tree::DecisionTree*>& new_trees,
    const dataset::VerticalDataset& dataset, std::vector<float>* predictions,
    double* mean_abs_prediction) const {
  if (new_trees.size() != dimension_) {
    return absl::InternalError("Wrong number of trees");
  }
  UpdatePredictionWithMultipleUnivariateTrees(dataset, new_trees, predictions,
                                              mean_abs_prediction);
  return absl::OkStatus();
}

std::vector<std::string> MultinomialLogLikelihoodLoss::SecondaryMetricNames()
    const {
  return {"accuracy"};
}

absl::Status MultinomialLogLikelihoodLoss::Loss(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& predictions, const std::vector<float>& weights,
    const RankingGroupsIndices* ranking_index, float* loss_value,
    std::vector<float>* secondary_metric) const {
  const auto* labels =
      dataset.ColumnWithCast<dataset::VerticalDataset::CategoricalColumn>(
          label_col_idx);
  double sum_loss = 0;
  double count_correct_predictions = 0;
  double sum_weights = 0;
  for (row_t example_idx = 0; example_idx < dataset.nrow(); example_idx++) {
    const int label = labels->values()[example_idx];
    const float weight = weights[example_idx];
    sum_weights += weight;

    int predicted_class = -1;
    float predicted_class_exp_value = 0;
    float sum_exp = 0;
    for (int grad_idx = 0; grad_idx < dimension_; grad_idx++) {
      const float exp_val =
          std::exp(predictions[grad_idx + example_idx * dimension_]);
      sum_exp += exp_val;
      DCheckIsFinite(sum_exp);
      if (exp_val > predicted_class_exp_value) {
        predicted_class_exp_value = exp_val;
        predicted_class = grad_idx + 1;
      }
    }
    if (label == predicted_class) {
      count_correct_predictions += weight;
    }
    // Loss:
    //   - log(predict_proba[true_label])
    const float tree_label_exp_value =
        std::exp(predictions[(label - 1) + example_idx * dimension_]);
    sum_loss -= weight * std::log(tree_label_exp_value / sum_exp);
    DCheckIsFinite(sum_loss);
    DCheckIsFinite(sum_weights);
  }

  secondary_metric->resize(1);
  if (sum_weights > 0) {
    *loss_value = static_cast<float>(sum_loss / sum_weights);
    DCheckIsFinite(*loss_value);
    (*secondary_metric)[kBinomialLossSecondaryMetricClassificationIdx] =
        static_cast<float>(count_correct_predictions / sum_weights);
  } else {
    *loss_value =
        (*secondary_metric)[kBinomialLossSecondaryMetricClassificationIdx] =
            std::numeric_limits<float>::quiet_NaN();
  }
  return absl::OkStatus();
}

NDCGLoss::NDCGLoss(const proto::GradientBoostedTreesTrainingConfig& gbt_config,
                   const model::proto::Task task,
                   const dataset::proto::Column& label_column)
    : gbt_config_(gbt_config), task_(task) {}

absl::Status NDCGLoss::Status() const {
  if (task_ != model::proto::Task::RANKING) {
    return absl::InvalidArgumentError(
        "NDCG loss is only compatible with a ranking task.");
  }
  return absl::OkStatus();
}

utils::StatusOr<std::vector<float>> NDCGLoss::InitialPredictions(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& weights) const {
  return std::vector<float>{0.f};
}

absl::Status NDCGLoss::UpdateGradients(
    const dataset::VerticalDataset& dataset, const int label_col_idx,
    const std::vector<float>& predictions,
    const RankingGroupsIndices* ranking_index,
    std::vector<GradientData>* gradients, utils::RandomEngine* random) const {
  std::vector<float>& gradient_data = (*gradients)[0].gradient;
  std::vector<float>& second_order_derivative_data = *(*gradients)[0].hessian;
  metric::NDCGCalculator ndcg_calculator(kNDCG5Truncation);

  const float lambda_loss = gbt_config_.lambda_loss();
  const float lambda_loss_squared = lambda_loss * lambda_loss;

  // Reset gradient accumulators.
  std::fill(gradient_data.begin(), gradient_data.end(), 0.f);
  std::fill(second_order_derivative_data.begin(),
            second_order_derivative_data.end(), 0.f);

  // "pred_and_in_ground_idx[j].first" is the prediction for the example
  // "group[pred_and_in_ground_idx[j].second].example_idx".
  std::vector<std::pair<float, int>> pred_and_in_ground_idx;
  for (const auto& group : ranking_index->groups()) {
    // Extract predictions.
    const int group_size = group.items.size();
    pred_and_in_ground_idx.resize(group_size);
    for (int item_idx = 0; item_idx < group_size; item_idx++) {
      pred_and_in_ground_idx[item_idx] = {
          predictions[group.items[item_idx].example_idx], item_idx};
    }

    // NDCG normalization term.
    // Note: At this point, "pred_and_in_ground_idx" is sorted by relevance
    // i.e. ground truth.
    float utility_norm_factor = 1.;
    if (!gbt_config_.lambda_mart_ndcg().gradient_use_non_normalized_dcg()) {
      const int max_rank = std::min(kNDCG5Truncation, group_size);
      float max_ndcg = 0;
      for (int rank = 0; rank < max_rank; rank++) {
        max_ndcg += ndcg_calculator.Term(group.items[rank].relevance, rank);
      }
      utility_norm_factor = 1.f / max_ndcg;
    }

    // Sort by decreasing predicted value.
    // Note: We shuffle the predictions so that the expected gradient value is
    // aligned with the metric value with ties taken into account (which is too
    // expensive to do here).
    std::shuffle(pred_and_in_ground_idx.begin(), pred_and_in_ground_idx.end(),
                 *random);
    std::sort(pred_and_in_ground_idx.begin(), pred_and_in_ground_idx.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    const int num_pred_and_in_ground = pred_and_in_ground_idx.size();

    // Compute the "force" that each item apply on each other items.
    for (int item_1_idx = 0; item_1_idx < num_pred_and_in_ground;
         item_1_idx++) {
      const float pred_1 = pred_and_in_ground_idx[item_1_idx].first;
      const int in_ground_idx_1 = pred_and_in_ground_idx[item_1_idx].second;
      const float relevance_1 = group.items[in_ground_idx_1].relevance;
      const auto example_1_idx = group.items[in_ground_idx_1].example_idx;

      // Accumulator for the gradient and second order derivative of the
      // example
      // "group[pred_and_in_ground_idx[item_1_idx].second].example_idx".
      float& grad_1 = gradient_data[example_1_idx];
      float& second_order_1 = second_order_derivative_data[example_1_idx];

      for (int item_2_idx = item_1_idx + 1; item_2_idx < num_pred_and_in_ground;
           item_2_idx++) {
        const float pred_2 = pred_and_in_ground_idx[item_2_idx].first;
        const int in_ground_idx_2 = pred_and_in_ground_idx[item_2_idx].second;
        const float relevance_2 = group.items[in_ground_idx_2].relevance;
        const auto example_2_idx = group.items[in_ground_idx_2].example_idx;

        // Skip examples with the same relevance value.
        if (relevance_1 == relevance_2) {
          continue;
        }

        // "delta_utility" corresponds to "Z_{i,j}" in the paper.
        float delta_utility = 0;
        if (item_1_idx < kNDCG5Truncation) {
          delta_utility += ndcg_calculator.Term(relevance_2, item_1_idx) -
                           ndcg_calculator.Term(relevance_1, item_1_idx);
        }
        if (item_2_idx < kNDCG5Truncation) {
          delta_utility += ndcg_calculator.Term(relevance_1, item_2_idx) -
                           ndcg_calculator.Term(relevance_2, item_2_idx);
        }
        delta_utility = std::abs(delta_utility) * utility_norm_factor;

        // "sign" correspond to the sign in front of the lambda_{i,j} terms
        // in the equation defining lambda_i, in section 7 of "From RankNet
        // to LambdaRank to LambdaMART: An Overview".
        // The "sign" is also used to reverse the {i,j} or {j,i} in the
        // "lambda" term i.e. "s_i" and "s_j" in the sigmoid.

        // sign = in_ground_idx_1 < in_ground_idx_2 ? +1.f : -1.f;
        // signed_lambda_loss = sign * lambda_loss;

        const float signed_lambda_loss =
            lambda_loss -
            2.f * lambda_loss * (in_ground_idx_1 >= in_ground_idx_2);

        // "sigmoid" corresponds to "rho_{i,j}" in the paper.
        const float sigmoid =
            1.f / (1.f + std::exp(signed_lambda_loss * (pred_1 - pred_2)));

        // "unit_grad" corresponds to "lambda_{i,j}" in the paper.
        // Note: We want to minimize the loss function i.e. go in opposite
        // side of the gradient.
        const float unit_grad = signed_lambda_loss * sigmoid * delta_utility;
        const float unit_second_order =
            delta_utility * sigmoid * (1.f - sigmoid) * lambda_loss_squared;

        grad_1 += unit_grad;
        second_order_1 += unit_second_order;

        DCheckIsFinite(grad_1);
        DCheckIsFinite(second_order_1);

        gradient_data[example_2_idx] -= unit_grad;
        second_order_derivative_data[example_2_idx] += unit_second_order;
      }
    }
  }
  return absl::OkStatus();
}

decision_tree::CreateSetLeafValueFunctor NDCGLoss::SetLeafFunctor(
    const std::vector<float>& predictions,
    const std::vector<GradientData>& gradients, const int label_col_idx) const {
  return
      [this, &predictions, &gradients, label_col_idx](
          const dataset::VerticalDataset& train_dataset,
          const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
          const std::vector<float>& weights,
          const model::proto::TrainingConfig& config,
          const model::proto::TrainingConfigLinking& config_link,
          decision_tree::NodeWithChildren* node) {
        return SetLeafStatic(train_dataset, selected_examples, weights, config,
                             config_link, predictions, gbt_config_, gradients,
                             label_col_idx, node);
      };
}

void NDCGLoss::SetLeafStatic(
    const dataset::VerticalDataset& train_dataset,
    const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
    const std::vector<float>& weights,
    const model::proto::TrainingConfig& config,
    const model::proto::TrainingConfigLinking& config_link,
    const std::vector<float>& predictions,
    const proto::GradientBoostedTreesTrainingConfig& gbt_config,
    const std::vector<GradientData>& gradients, const int label_col_idx,
    decision_tree::NodeWithChildren* node) {
  if (!gbt_config.use_hessian_gain()) {
    decision_tree::SetRegressionLabelDistribution(
        train_dataset, selected_examples, weights, config_link,
        node->mutable_node());
  }

  const auto& gradient_data = gradients.front().gradient;
  const auto& second_order_derivative_data = *(gradients.front().hessian);

  double sum_weighted_gradient = 0;
  double sum_weighted_second_order_derivative = 0;
  double sum_weights = 0;
  for (const auto example_idx : selected_examples) {
    const float weight = weights[example_idx];
    sum_weighted_gradient += weight * gradient_data[example_idx];
    sum_weighted_second_order_derivative +=
        weight * second_order_derivative_data[example_idx];
    sum_weights += weight;
  }
  DCheckIsFinite(sum_weighted_gradient);
  DCheckIsFinite(sum_weighted_second_order_derivative);

  if (sum_weighted_second_order_derivative <= kMinHessianForNewtonStep) {
    sum_weighted_second_order_derivative = kMinHessianForNewtonStep;
  }

  if (gbt_config.use_hessian_gain()) {
    auto* reg = node->mutable_node()->mutable_regressor();
    reg->set_sum_gradients(sum_weighted_gradient);
    reg->set_sum_hessians(sum_weighted_second_order_derivative);
    reg->set_sum_weights(sum_weights);
  }

  node->mutable_node()->mutable_regressor()->set_top_value(
      gbt_config.shrinkage() *
      decision_tree::l1_threshold(sum_weighted_gradient,
                                  gbt_config.l1_regularization()) /
      (sum_weighted_second_order_derivative + gbt_config.l2_regularization()));
}

absl::Status NDCGLoss::UpdatePredictions(
    const std::vector<const decision_tree::DecisionTree*>& new_trees,
    const dataset::VerticalDataset& dataset, std::vector<float>* predictions,
    double* mean_abs_prediction) const {
  if (new_trees.size() != 1) {
    return absl::InternalError("Wrong number of trees");
  }
  UpdatePredictionWithSingleUnivariateTree(dataset, *new_trees.front(),
                                           predictions, mean_abs_prediction);
  return absl::OkStatus();
}

std::vector<std::string> NDCGLoss::SecondaryMetricNames() const {
  return {"NDCG@5"};
}

absl::Status NDCGLoss::Loss(const dataset::VerticalDataset& dataset,
                            int label_col_idx,
                            const std::vector<float>& predictions,
                            const std::vector<float>& weights,
                            const RankingGroupsIndices* ranking_index,
                            float* loss_value,
                            std::vector<float>* secondary_metric) const {
  if (ranking_index == nullptr) {
    return absl::InternalError("Missing ranking index");
  }
  const auto ndcg = ranking_index->NDCG(predictions, weights, kNDCG5Truncation);

  // The loss is -1 * the ndcg.
  *loss_value = -ndcg;

  secondary_metric->resize(1);
  (*secondary_metric)[0] = ndcg;
  return absl::OkStatus();
}

CrossEntropyNDCGLoss::CrossEntropyNDCGLoss(
    const proto::GradientBoostedTreesTrainingConfig& gbt_config,
    const model::proto::Task task, const dataset::proto::Column& label_column)
    : gbt_config_(gbt_config), task_(task) {}

absl::Status CrossEntropyNDCGLoss::Status() const {
  if (task_ != model::proto::Task::RANKING) {
    return absl::InvalidArgumentError(
        "Cross Entropy NDCG loss is only compatible with a ranking task.");
  }
  return absl::OkStatus();
}

utils::StatusOr<std::vector<float>> CrossEntropyNDCGLoss::InitialPredictions(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& weights) const {
  return std::vector<float>{0.f};
}

absl::Status CrossEntropyNDCGLoss::UpdateGradients(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& predictions,
    const RankingGroupsIndices* ranking_index,
    std::vector<GradientData>* gradients, utils::RandomEngine* random) const {
  std::vector<float>& gradient_data = (*gradients)[0].gradient;
  std::vector<float>& second_order_derivative_data = *((*gradients)[0].hessian);

  std::uniform_real_distribution<float> distribution(0.0, 1.0);

  // Reset gradient accumulators.
  std::fill(gradient_data.begin(), gradient_data.end(), 0.f);
  std::fill(second_order_derivative_data.begin(),
            second_order_derivative_data.end(), 0.f);

  // A vector of predictions for items in a group.
  std::vector<float> preds;
  // An auxiliary buffer of parameters used to form the ground-truth
  // distribution and compute the loss.
  std::vector<float> params;

  for (const auto& group : ranking_index->groups()) {
    const size_t group_size = group.items.size();

    // Skip groups with too few items.
    if (group_size <= 1) {
      continue;
    }

    // Extract predictions.
    preds.resize(group_size);
    params.resize(group_size);

    switch (gbt_config_.xe_ndcg().gamma()) {
      case proto::GradientBoostedTreesTrainingConfig::XeNdcg::ONE:
        std::fill(params.begin(), params.end(), 1.f);
        break;
      case proto::GradientBoostedTreesTrainingConfig::XeNdcg::AUTO:
      case proto::GradientBoostedTreesTrainingConfig::XeNdcg::UNIFORM:
        for (int item_idx = 0; item_idx < group_size; item_idx++) {
          params[item_idx] = distribution(*random);
        }
        break;
    }
    for (int item_idx = 0; item_idx < group_size; item_idx++) {
      preds[item_idx] = predictions[group.items[item_idx].example_idx];
    }

    // Turn scores into a probability distribution with Softmax.
    const float max_pred = *std::max_element(preds.begin(), preds.end());
    float sum_exp = 0.0f;
    for (int idx = 0; idx < group_size; idx++) {
      sum_exp += std::exp(preds[idx] - max_pred);
    }
    float log_sum_exp = max_pred + std::log(sum_exp + 1e-20f);
    for (int idx = 0; idx < group_size; idx++) {
      float probability = std::exp(preds[idx] - log_sum_exp);
      preds[idx] = utils::clamp(probability, 1e-5f, .99999f);
    }

    // Approximate Newton's step.
    // First-order terms.
    float inv_denominator = 0;
    for (int idx = 0; idx < group_size; idx++) {
      // Params is currently a \gamma but becomes the numerator of the
      // first-order approximation terms.
      params[idx] = std::exp2f(group.items[idx].relevance) - params[idx];
      inv_denominator += params[idx];
    }
    if (inv_denominator == 0.f) {
      continue;
    }
    inv_denominator = 1.f / inv_denominator;

    float sum_l1 = 0.f;
    for (int idx = 0; idx < group_size; idx++) {
      const auto example_idx = group.items[idx].example_idx;
      const auto term = -params[idx] * inv_denominator + preds[idx];
      gradient_data[example_idx] = -term;

      // Params will now store terms needed to compute second-order terms.
      params[idx] = term / (1.f - preds[idx]);
      sum_l1 += params[idx];
    }
    // Second-order terms.
    float sum_l2 = 0.f;
    for (int idx = 0; idx < group_size; idx++) {
      const auto example_idx = group.items[idx].example_idx;
      const auto term = preds[idx] * (sum_l1 - params[idx]);
      gradient_data[example_idx] -= term;

      // Params will now store terms needed to compute third-order terms.
      params[idx] = term / (1.f - preds[idx]);
      sum_l2 += params[idx];
    }

    // Third-order terms and the Hessian.
    for (int idx = 0; idx < group_size; idx++) {
      const auto example_idx = group.items[idx].example_idx;
      gradient_data[example_idx] -= preds[idx] * (sum_l2 - params[idx]);
      second_order_derivative_data[example_idx] =
          preds[idx] * (1.f - preds[idx]);
    }
  }
  return absl::OkStatus();
}

decision_tree::CreateSetLeafValueFunctor CrossEntropyNDCGLoss::SetLeafFunctor(
    const std::vector<float>& predictions,
    const std::vector<GradientData>& gradients, const int label_col_idx) const {
  return
      [this, &predictions, &gradients, label_col_idx](
          const dataset::VerticalDataset& train_dataset,
          const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
          const std::vector<float>& weights,
          const model::proto::TrainingConfig& config,
          const model::proto::TrainingConfigLinking& config_link,
          decision_tree::NodeWithChildren* node) {
        return NDCGLoss::SetLeafStatic(
            train_dataset, selected_examples, weights, config, config_link,
            predictions, gbt_config_, gradients, label_col_idx, node);
      };
}

absl::Status CrossEntropyNDCGLoss::UpdatePredictions(
    const std::vector<const decision_tree::DecisionTree*>& new_trees,
    const dataset::VerticalDataset& dataset, std::vector<float>* predictions,
    double* mean_abs_prediction) const {
  if (new_trees.size() != 1) {
    return absl::InternalError("Wrong number of trees");
  }
  UpdatePredictionWithSingleUnivariateTree(dataset, *new_trees.front(),
                                           predictions, mean_abs_prediction);
  return absl::OkStatus();
}

std::vector<std::string> CrossEntropyNDCGLoss::SecondaryMetricNames() const {
  return {};
}

absl::Status CrossEntropyNDCGLoss::Loss(
    const dataset::VerticalDataset& dataset, int label_col_idx,
    const std::vector<float>& predictions, const std::vector<float>& weights,
    const RankingGroupsIndices* ranking_index, float* loss_value,
    std::vector<float>* secondary_metric) const {
  if (ranking_index == nullptr) {
    return absl::InternalError("Missing ranking index");
  }
  *loss_value = -ranking_index->NDCG(predictions, weights, kNDCG5Truncation);
  return absl::OkStatus();
}

void RankingGroupsIndices::Initialize(const dataset::VerticalDataset& dataset,
                                      int label_col_idx, int group_col_idx) {
  // Access to raw label and group values.
  const auto& label_values =
      dataset
          .ColumnWithCast<dataset::VerticalDataset::NumericalColumn>(
              label_col_idx)
          ->values();

  const auto* group_categorical_values =
      dataset.ColumnWithCastOrNull<dataset::VerticalDataset::CategoricalColumn>(
          group_col_idx);
  const auto* group_hash_values =
      dataset.ColumnWithCastOrNull<dataset::VerticalDataset::HashColumn>(
          group_col_idx);

  // Fill index.
  absl::flat_hash_map<uint64_t, std::vector<Item>> tmp_groups;
  for (row_t example_idx = 0; example_idx < dataset.nrow(); example_idx++) {
    // Get the value of the group.
    uint64_t group_value;
    if (group_categorical_values) {
      group_value = group_categorical_values->values()[example_idx];
    } else if (group_hash_values) {
      group_value = group_hash_values->values()[example_idx];
    } else {
      LOG(FATAL) << "Invalid group type";
    }

    tmp_groups[group_value].push_back(
        {/*.relevance =*/label_values[example_idx],
         /*.example_idx =*/example_idx});
  }
  num_items_ = dataset.nrow();

  // Sort the group items by decreasing ground truth relevance.
  groups_.reserve(tmp_groups.size());
  for (auto& group : tmp_groups) {
    std::sort(group.second.begin(), group.second.end(),
              [](const Item& a, const Item& b) {
                if (a.relevance == b.relevance) {
                  return a.example_idx > b.example_idx;
                }
                return a.relevance > b.relevance;
              });

    if (group.second.size() > kMaximumItemsInRankingGroup) {
      LOG(FATAL) << "The number of items in the group \"" << group.first
                 << "\" is " << group.second.size()
                 << " and is greater than kMaximumItemsInRankingGroup="
                 << kMaximumItemsInRankingGroup
                 << ". This is likely a mistake in the generation of the "
                    "configuration of the group column.";
    }

    groups_.push_back(
        {/*.group_idx =*/group.first, /*.items =*/std::move(group.second)});
  }

  // Sort the group by example index to improve the data locality.
  std::sort(groups_.begin(), groups_.end(), [](const Group& a, const Group& b) {
    if (a.items.front().example_idx == b.items.front().example_idx) {
      return a.group_idx < b.group_idx;
    }
    return a.items.front().example_idx < b.items.front().example_idx;
  });
  LOG(INFO) << "Found " << groups_.size() << " groups in " << dataset.nrow()
            << " examples.";
}

double RankingGroupsIndices::NDCG(const std::vector<float>& predictions,
                                  const std::vector<float>& weights,
                                  const int truncation) const {
  DCHECK_EQ(predictions.size(), num_items_);
  DCHECK_EQ(weights.size(), num_items_);

  metric::NDCGCalculator ndcg_calculator(truncation);
  std::vector<metric::RankingLabelAndPrediction> pred_and_label_relevance;

  double sum_weighted_ndcg = 0;
  double sum_weights = 0;
  for (auto& group : groups_) {
    DCHECK(!group.items.empty());
    const float weight = weights[group.items.front().example_idx];

    ExtractPredAndLabelRelevance(group.items, predictions,
                                 &pred_and_label_relevance);

    sum_weighted_ndcg +=
        weight * ndcg_calculator.NDCG(pred_and_label_relevance);
    sum_weights += weight;
  }
  return sum_weighted_ndcg / sum_weights;
}

void RankingGroupsIndices::ExtractPredAndLabelRelevance(
    const std::vector<Item>& group, const std::vector<float>& predictions,
    std::vector<metric::RankingLabelAndPrediction>* pred_and_label_relevance) {
  pred_and_label_relevance->resize(group.size());
  for (int item_idx = 0; item_idx < group.size(); item_idx++) {
    (*pred_and_label_relevance)[item_idx] = {
        /*.prediction =*/predictions[group[item_idx].example_idx],
        /*.label =*/group[item_idx].relevance};
  }
}

}  // namespace gradient_boosted_trees
}  // namespace model
}  // namespace yggdrasil_decision_forests
