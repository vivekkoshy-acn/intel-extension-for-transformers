//  Copyright (c) 2021 Intel Corporation
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#ifndef ENGINE_EXECUTOR_INCLUDE_OPERATORS_COSSIN_HPP_
#define ENGINE_EXECUTOR_INCLUDE_OPERATORS_COSSIN_HPP_

#include<Eigen/Dense>
#include <string>
#include <vector>
#include <memory>

#include "../operator.hpp"


namespace executor {


/**
 * @brief A CosSin operator.
 *
 */

class CosSinOperator : public Operator {
 public:
  explicit CosSinOperator(const shared_ptr<OperatorConfig>& conf);
  virtual ~CosSinOperator();

  void Reshape(const vector<Tensor*>& input, const vector<Tensor*>& output) override;
  void Forward(const vector<Tensor*>& input, const vector<Tensor*>& output) override;

 private:
  string output_dtype_ = "fp32";
  string algorithm_ = "cos";
  int array_size_ = 0;
};
}  // namespace executor
#endif  // ENGINE_EXECUTOR_INCLUDE_OPERATORS_COSSIN_HPP_
