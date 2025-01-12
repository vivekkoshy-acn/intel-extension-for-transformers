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

#include <map>
#include <string>

#include "../../include/common.hpp"
#include "../../include/conf.hpp"
#include "../../include/operators/inner_product.hpp"
#include "gtest/gtest.h"
using executor::AttrConfig;
using executor::MemoryAllocator;
using executor::OperatorConfig;
using executor::Tensor;
using executor::TensorConfig;

struct OpArgs {
  std::vector<Tensor*> input;
  std::vector<Tensor*> output;
  shared_ptr<OperatorConfig> conf;
};

struct TestParams {
  std::pair<OpArgs, Tensor*> args;
  bool expect_to_fail;
};

bool CheckResult(const TestParams& t) {
  const auto& p = t.args.first;
  const auto& q = t.args.second;
  try {
    executor::InnerProductOperator innerproduct(p.conf);
    innerproduct.Prepare(p.input, p.output);
    innerproduct.Reshape(p.input, p.output);
    innerproduct.Forward(p.input, p.output);
  } catch (const dnnl::error& e) {
    if (e.status != dnnl_status_t::dnnl_success && t.expect_to_fail) {
      return true;
    } else {
      return false;
    }
  } catch (const std::string& e) {
    if (e == "Windows" && t.expect_to_fail)
      return true;
    else
      return false;
  }
  if (!t.expect_to_fail) {
    bool is_equal;
    if (q->dtype() == "fp32") {
      is_equal = executor::CompareData<float>(p.output[0]->data(), p.output[0]->size(), q->data(), q->size(), 0.03);
    } else if (q->dtype() == "s8") {
      is_equal = executor::CompareData<int8_t>(p.output[0]->data(), p.output[0]->size(), q->data(), q->size(), 2);
    } else if (q->dtype() == "u8") {
      is_equal = executor::CompareData<uint8_t>(p.output[0]->data(), p.output[0]->size(), q->data(), q->size(), 2);
    }
    return is_equal;
  }
  return false;
}

class InnerProductInt8Test : public testing::TestWithParam<TestParams> {
 protected:
  InnerProductInt8Test() {}
  ~InnerProductInt8Test() {}
  void SetUp() override {}
  void TearDown() override {}
};

TEST_P(InnerProductInt8Test, TestPostfix) {
  TestParams t = testing::TestWithParam<TestParams>::GetParam();
  EXPECT_TRUE(CheckResult(t));
}

Tensor* make_int32_bias_obj(const shared_ptr<TensorConfig>& bias_tensor_config, const float* origin_data,
                            Tensor* weight_fp32, Tensor* weight_min, Tensor* weight_max, Tensor* src_min,
                            Tensor* src_max) {
  Tensor* bias_tensor = new Tensor(*bias_tensor_config);
  bias_tensor->add_tensor_life(1);
  int32_t* bias_data = reinterpret_cast<int32_t*>(bias_tensor->mutable_data());
  const float* weight_scales = reinterpret_cast<const float*>(weight_max->data());
  const float* src_scales = reinterpret_cast<const float*>(src_max->data());
  const float zp = *reinterpret_cast<const float*>(src_min->data());
  const float* weight_data = reinterpret_cast<const float*>(weight_fp32->data());
#pragma omp parallel for
  for (int y = 0; y < weight_fp32->shape()[1]; y++) {
    float compensation = 0;
    for (int x = 0; x < weight_fp32->shape()[0]; x++) compensation += weight_data[x * weight_fp32->shape()[1] + y];
    bias_data[y] = (origin_data[y] + compensation * zp) * src_scales[0] * weight_scales[y];
  }
  return bias_tensor;
}

Tensor* get_fp32_dst(const shared_ptr<TensorConfig>& dst_tensor_config, vector<Tensor*> inputs, string append_op = "") {
  using dnnl::matmul;
  using dnnl::memory;
  Tensor* dst_tensor = new Tensor(*dst_tensor_config);
  dst_tensor->add_tensor_life(1);
  dnnl::engine engine(dnnl::engine::kind::cpu, 0);
  dnnl::stream engine_stream = dnnl::stream(engine);
  Tensor* src = inputs[0];
  Tensor* weight = inputs[1];
  Tensor* bias = inputs[2];
  Tensor* post = inputs[3];
  dnnl::primitive_attr attr;
  dnnl::post_ops po;
  memory::desc binary_md;
  memory binary_m_;
  auto src_md = memory::desc(src->shape(), dnnl::memory::data_type::f32, dnnl::memory::format_tag::ab);
  auto weights_md = memory::desc(weight->shape(), dnnl::memory::data_type::f32, dnnl::memory::format_tag::ab);
  auto bias_md = memory::desc({1, bias->shape()[0]}, dnnl::memory::data_type::f32, dnnl::memory::format_tag::ab);
  auto dst_md = memory::desc(dst_tensor->shape(), dnnl::memory::data_type::f32, dnnl::memory::format_tag::ab);
  auto src_mem = memory(src_md, engine, src->mutable_data());
  auto weights_mem = memory(weights_md, engine, weight->mutable_data());
  auto bias_mem = memory(bias_md, engine, bias->mutable_data());
  if (post != nullptr) {
    po.append_sum(1.0);
    memcpy(dst_tensor->mutable_data(), post->mutable_data(), dst_tensor->size() * sizeof(float));
  }
  if (append_op == "gelu_tanh") po.append_eltwise(1.0f, dnnl::algorithm::eltwise_gelu_tanh, 0.0f, 0.0f);
  attr.set_post_ops(po);
  auto dst_mem = memory(dst_md, engine, dst_tensor->mutable_data());
  auto matmul_d = matmul::desc(src_md, weights_md, bias_md, dst_md);
  auto matmul_pd = matmul::primitive_desc(matmul_d, attr, engine);
  auto matmul_prim = matmul(matmul_pd);
  std::unordered_map<int, memory> matmul_args;
  matmul_args.insert({DNNL_ARG_SRC, src_mem});
  matmul_args.insert({DNNL_ARG_WEIGHTS, weights_mem});
  matmul_args.insert({DNNL_ARG_BIAS, bias_mem});
  matmul_args.insert({DNNL_ARG_DST, dst_mem});
  matmul_prim.execute(engine_stream, matmul_args);
  engine_stream.wait();
  return dst_tensor;
}

template <typename T>
void init_vector(T* v, int num_size, float range1 = -10, float range2 = 10, int seed = 5489u) {
  float low_value = std::max(range1, static_cast<float>(std::numeric_limits<T>::lowest()) + 1);
  std::mt19937 gen(seed);
  std::uniform_real_distribution<float> u(low_value, range2);
  for (int i = 0; i < num_size; ++i) {
    v[i] = u(gen);
  }
}

Tensor* make_fp32_tensor_obj(const shared_ptr<TensorConfig>& a_tensor_config, float bound1 = -10, float bound2 = 10) {
  // step1: set shape
  Tensor* a_tensor = new Tensor(*a_tensor_config);
  // step2: set tensor life
  a_tensor->add_tensor_life(1);
  // step3: library buffer can only be obtained afterwards
  auto tensor_data = a_tensor->mutable_data();
  static int seed = 0;
  init_vector(static_cast<float*>(tensor_data), a_tensor->size(), bound1, bound2, seed++);
  return a_tensor;
}

vector<Tensor*> quantize2int8_tensor_obj(const vector<shared_ptr<TensorConfig>>& tensor_configs,
                                         const float* origin_fp32_data, bool per_channel = false) {
  vector<Tensor*> tensors(3);
  for (int i = 0; i < 3; i++) {
    tensors[i] = new Tensor(*tensor_configs[i]);
    tensors[i]->add_tensor_life(1);
  }
  float* min_data = reinterpret_cast<float*>(tensors[1]->mutable_data());
  float* max_data = reinterpret_cast<float*>(tensors[2]->mutable_data());
  void* dst_data = tensors[0]->mutable_data();
  if (per_channel) {
    for (int y = 0; y < tensors[0]->shape()[1]; y++) {
      min_data[y] = origin_fp32_data[y];
      max_data[y] = origin_fp32_data[y];
      for (int x = 1; x < tensors[0]->shape()[0]; x++) {
        min_data[y] = std::min(min_data[y], origin_fp32_data[x * tensors[0]->shape()[1] + y]);
        max_data[y] = std::max(max_data[y], origin_fp32_data[x * tensors[0]->shape()[1] + y]);
      }
      vector<float> scales = executor::GetScales(min_data + y, max_data + y, 1, tensors[0]->dtype());
      for (int x = 0; x < tensors[0]->shape()[0]; x++)
        if (tensors[0]->dtype() == "u8") {
          uint8_t* dst_data_ = reinterpret_cast<uint8_t*>(dst_data) + x * tensors[0]->shape()[1] + y;
          int32_t data = nearbyint((origin_fp32_data[x * tensors[0]->shape()[1] + y] - min_data[y]) * scales[0]);
          data = data < 0 ? 0 : data;
          data = data > 255 ? 255 : data;
          *dst_data_ = static_cast<uint8_t>(data);
        } else if (tensors[0]->dtype() == "s8") {
          int8_t* dst_data_ = reinterpret_cast<int8_t*>(dst_data) + x * tensors[0]->shape()[1] + y;
          int32_t data = nearbyint(origin_fp32_data[x * tensors[0]->shape()[1] + y] * scales[0]);
          data = data < -128 ? -128 : data;
          data = data > 127 ? 127 : data;
          *dst_data_ = static_cast<int8_t>(data);
        }
      memcpy(max_data + y, scales.data(), 1 * sizeof(float));
    }
  } else {
    executor::runtime_minmax(origin_fp32_data, tensors[0]->size(), min_data, max_data);
    vector<float> scales = executor::GetScales(min_data, max_data, 1, tensors[0]->dtype());
#if __AVX512F__
    executor::Quantize_avx512(tensors[0]->size(), tensors[0]->dtype(), origin_fp32_data, min_data, scales, dst_data);
#else
    executor::Quantize(tensors[0]->size(), tensors[0]->dtype(), origin_fp32_data, min_data, scales, dst_data);
#endif
    memcpy(max_data, scales.data(), 1 * sizeof(float));
  }
  return tensors;
}

std::pair<OpArgs, Tensor*> GenerateInt8Case(const std::vector<std::vector<int64_t>>& input_shape, bool is_dynamic,
                                            std::string input_type = "u8", std::string output_type = "u8",
                                            std::string append_op = "") {
  // support s8s8fp32 and u8s8u8 without bias
  // Step 1: Construct Tensor config ptr
  const auto& src0_shape = input_shape[0];
  const auto& src1_shape = input_shape[1];
  std::vector<int64_t> bias_shape = {src1_shape[1]};
  std::vector<int64_t> dst_shape = {src0_shape[0], src1_shape[1]};

  auto src_fp32_config = std::make_shared<TensorConfig>("src_fp32", src0_shape, "fp32");
  auto src_u8_config = std::make_shared<TensorConfig>("src", src0_shape, input_type);
  auto src_min_config = std::make_shared<TensorConfig>("src_min", vector<int64_t>({1}), "fp32");
  auto src_max_config = std::make_shared<TensorConfig>("src_max", vector<int64_t>({1}), "fp32");
  Tensor* src_fp32;
  src_fp32 = make_fp32_tensor_obj(src_fp32_config);
  auto src_tensors = quantize2int8_tensor_obj({src_u8_config, src_min_config, src_max_config},
                                              reinterpret_cast<const float*>(src_fp32->data()), false);

  auto weight_fp32_config = std::make_shared<TensorConfig>("weight_fp32", src1_shape, "fp32");
  auto weight_s8_config = std::make_shared<TensorConfig>("weight", src1_shape, "s8");
  auto weight_min_config = std::make_shared<TensorConfig>("weight_min", bias_shape, "fp32");
  auto weight_max_config = std::make_shared<TensorConfig>("weight_max", bias_shape, "fp32");
  Tensor* weight_fp32 = make_fp32_tensor_obj(weight_fp32_config);
  auto weight_tensors = quantize2int8_tensor_obj({weight_s8_config, weight_min_config, weight_max_config},
                                                 reinterpret_cast<const float*>(weight_fp32->data()), true);

  auto bias_fp32_config = std::make_shared<TensorConfig>("bias", bias_shape, "fp32");
  Tensor* bias_fp32 = make_fp32_tensor_obj(bias_fp32_config);
  auto post_fp32_config = std::make_shared<TensorConfig>("post", dst_shape, "fp32");
  Tensor* post_fp32 = make_fp32_tensor_obj(post_fp32_config);
  // get true fp32 result and calculate min/max
  auto dst_fp32_config = std::make_shared<TensorConfig>("dst_fp32", dst_shape, "fp32");
  auto dst_config = std::make_shared<TensorConfig>("dst", dst_shape, output_type);
  auto dst_min_config = std::make_shared<TensorConfig>("dst_min", vector<int64_t>({1}), "fp32");
  auto dst_max_config = std::make_shared<TensorConfig>("dst_max", vector<int64_t>({1}), "fp32");
  Tensor* dst_fp32 = get_fp32_dst(
      dst_fp32_config,
      {src_fp32, weight_fp32, bias_fp32, ((output_type == "fp32" && append_op == "sum") ? post_fp32 : nullptr)},
      append_op);
  Tensor* dst = new Tensor(*dst_config);
  dst->add_tensor_life(1);
  Tensor* dst_min = new Tensor(*dst_min_config);
  dst_min->add_tensor_life(1);
  Tensor* dst_max = new Tensor(*dst_max_config);
  dst_max->add_tensor_life(1);
  executor::runtime_minmax(reinterpret_cast<float*>(dst_fp32->mutable_data()), dst_fp32->size(),
                           reinterpret_cast<float*>(dst_min->mutable_data()),
                           reinterpret_cast<float*>(dst_max->mutable_data()));
  vector<float> scales = executor::GetScales(dst_min->data(), dst_max->data(), 1, output_type);
  memcpy(dst_max->mutable_data(), scales.data(), 1 * sizeof(float));
  vector<shared_ptr<TensorConfig>> inputs_configs = {src_u8_config,    weight_s8_config, bias_fp32_config,
                                                     src_min_config,   src_max_config,   weight_min_config,
                                                     weight_max_config};
  vector<shared_ptr<TensorConfig>> output_configs = {dst_config};
  vector<Tensor*> inputs = {src_tensors[0], weight_tensors[0], bias_fp32,        src_tensors[1],
                            src_tensors[2], weight_tensors[1], weight_tensors[2]};
  vector<Tensor*> outputs = {dst};
  map<string, string> attr_map = {{"output_dtype", output_type}, {"src1_perm", "1,0"}};
  if (output_type == "fp32" && append_op == "sum") {
    inputs_configs.insert(inputs_configs.begin() + 3, post_fp32_config);
    inputs.insert(inputs.begin() + 3, post_fp32);
    attr_map["append_op"] = append_op;
  }
  if (output_type == "u8" && append_op == "gelu_tanh") {
    attr_map["append_op"] = append_op;
  }
  if (!is_dynamic) {
    inputs_configs.push_back(dst_min_config);
    inputs_configs.push_back(dst_max_config);
    inputs.push_back(dst_min);
    inputs.push_back(dst_max);
  } else {
    output_configs.push_back(dst_min_config);
    output_configs.push_back(dst_max_config);
    outputs.push_back(dst_min);
    outputs.push_back(dst_max);
  }
  // Step 1.1: Construct Operator config obj
  shared_ptr<AttrConfig> op_attr = std::make_shared<AttrConfig>(attr_map);
  auto op_config =
      std::make_shared<OperatorConfig>("innerproduct", output_type, inputs_configs, output_configs, op_attr);

  OpArgs op_args = {inputs, outputs, op_config};
  // src_fp32->print();
  // for (auto tensor : src_tensors) tensor->print();
  // weight_fp32->print();
  // for (auto tensor : weight_tensors) tensor->print();
  // bias_fp32->print();
  // post_fp32->print();
  // dst_fp32->print();
  if (output_type == "fp32") {
    return {op_args, dst_fp32};
  } else {
    Tensor* true_data = new Tensor(*dst_config);
    true_data->add_tensor_life(1);
#if __AVX512F__
    executor::Quantize_avx512(true_data->size(), output_type, dst_fp32->data(),
                              reinterpret_cast<const float*>(dst_min->data()), scales, true_data->mutable_data());
#else
    executor::Quantize(true_data->size(), output_type, dst_fp32->data(),
                       reinterpret_cast<const float*>(dst_min->data()), scales, true_data->mutable_data());
#endif
    // true_data->print();
    return {op_args, true_data};
  }
}

static auto CasesInt8 = []() {
  MemoryAllocator::InitStrategy();

  std::vector<TestParams> cases;

  // Config
  std::vector<int64_t> src0_shape;
  std::vector<int64_t> src1_shape;
  std::vector<int64_t> bias_shape;

  src0_shape = {3840, 1024};
  src1_shape = {1024, 256};
#ifdef _WIN32
  cases.push_back({GenerateInt8Case({src0_shape, src1_shape}, true, "u8", "s8"), true});
#else
  cases.push_back({GenerateInt8Case({src0_shape, src1_shape}, true, "u8", "s8"), false});
#endif

  src0_shape = {3840, 256};
  src1_shape = {256, 1024};
#ifdef _WIN32
  cases.push_back({GenerateInt8Case({src0_shape, src1_shape}, true, "u8", "u8", "gelu_tanh"), true});
#else
  cases.push_back({GenerateInt8Case({src0_shape, src1_shape}, true, "u8", "u8", "gelu_tanh"), false});
#endif

  src0_shape = {3840, 256};
  src1_shape = {256, 256};
#ifdef _WIN32
  cases.push_back({GenerateInt8Case({src0_shape, src1_shape}, true, "u8", "fp32", "sum"), true});
#else
  cases.push_back({GenerateInt8Case({src0_shape, src1_shape}, true, "u8", "fp32", "sum"), false});
#endif

  return ::testing::ValuesIn(cases);
};

INSTANTIATE_TEST_SUITE_P(Prefix, InnerProductInt8Test, CasesInt8());
