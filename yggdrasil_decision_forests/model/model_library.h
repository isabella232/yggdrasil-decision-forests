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

// Abstract classes for model and model builder (called learner).

#ifndef YGGDRASIL_DECISION_FORESTS_MODEL_MODEL_LIBRARY_H_
#define YGGDRASIL_DECISION_FORESTS_MODEL_MODEL_LIBRARY_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "yggdrasil_decision_forests/model/abstract_model.h"

namespace yggdrasil_decision_forests {
namespace model {

// Creates an empty model (the semantic depends on the model) from a model name.
absl::Status CreateEmptyModel(absl::string_view model_name,
                              std::unique_ptr<AbstractModel>* model);

// Returns the list of all registered model names.
std::vector<std::string> AllRegisteredModels();

// Save the model into a directory. The directory should not exist already.
absl::Status SaveModel(absl::string_view directory,
                       const AbstractModel* const mdl);

// Load a model from a directory previously created with "SaveModel".
absl::Status LoadModel(absl::string_view directory,
                       std::unique_ptr<AbstractModel>* model);

// Checks if a model exist i.e. if the "done" file (see kModelDoneFileName) is
// present.
utils::StatusOr<bool> ModelExist(absl::string_view directory);

}  // namespace model
}  // namespace yggdrasil_decision_forests

#endif  // YGGDRASIL_DECISION_FORESTS_MODEL_MODEL_LIBRARY_H_
