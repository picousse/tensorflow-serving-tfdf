/* Copyright 2019 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow_serving/servables/tensorflow/tflite_session.h"

#include <memory>
#include <string>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "flatbuffers/flexbuffers.h"
#include "tensorflow/cc/saved_model/signature_constants.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/test_benchmark.h"
#include "tensorflow/core/platform/threadpool_options.h"
#include "tensorflow/core/protobuf/config.pb.h"
#include "tensorflow/lite/tools/signature/signature_def_util.h"
#include "tensorflow/lite/util.h"
#include "tensorflow/lite/version.h"
#include "tensorflow_serving/test_util/test_util.h"

namespace tensorflow {
namespace serving {
namespace {

using ::testing::_;
using ::testing::Pair;
using ::testing::UnorderedElementsAre;

constexpr char kTestModel[] =
    "/servables/tensorflow/testdata/saved_model_half_plus_two_tflite/00000123/"
    "model.tflite";

constexpr char kMobileNetModel[] =
    "/servables/tensorflow/testdata/mobilenet_v1_quant_tflite/00000123/"
    "model.tflite";

TEST(TfLiteSession, BasicTest) {
  string model_bytes;
  TF_ASSERT_OK(ReadFileToString(tensorflow::Env::Default(),
                                test_util::TestSrcDirPath(kTestModel),
                                &model_bytes));

  ::google::protobuf::Map<string, SignatureDef> signatures;
  std::unique_ptr<TfLiteSession> session;
  TF_ASSERT_OK(
      TfLiteSession::Create(std::move(model_bytes), &session, &signatures));
  EXPECT_EQ(signatures.size(), 1);
  EXPECT_EQ(signatures.begin()->first, "serving_default");
  EXPECT_THAT(signatures.begin()->second, test_util::EqualsProto(R"(
                inputs {
                  key: "x"
                  value {
                    name: "x"
                    dtype: DT_FLOAT
                    tensor_shape {
                      dim { size: 1 }
                      dim { size: 1 }
                    }
                  }
                }
                outputs {
                  key: "y"
                  value {
                    name: "y"
                    dtype: DT_FLOAT
                    tensor_shape {
                      dim { size: 1 }
                      dim { size: 1 }
                    }
                  }
                }
                method_name: "tensorflow/serving/predict"
              )"));
  Tensor input = test::AsTensor<float>({1.0, 2.0, 3.0}, TensorShape({3}));
  {
    // Use TF Lite tensor names.
    std::vector<Tensor> outputs;
    TF_EXPECT_OK(session->Run({{"x", input}}, {"y"}, {}, &outputs));
    ASSERT_EQ(outputs.size(), 1);
    test::ExpectTensorEqual<float>(
        outputs[0], test::AsTensor<float>({2.5, 3, 3.5}, TensorShape({3})));
  }
  {
    // Use TF tensor names (with `:0` suffix).
    std::vector<Tensor> outputs;
    TF_EXPECT_OK(session->Run({{"x:0", input}}, {"y:0"}, {}, &outputs));
    ASSERT_EQ(outputs.size(), 1);
    test::ExpectTensorEqual<float>(
        outputs[0], test::AsTensor<float>({2.5, 3, 3.5}, TensorShape({3})));
  }
}

constexpr char kTestModelInputList[] = "list";
constexpr char kTestModelInputShape[] = "shape";
constexpr char kTestModelOutput[] = "output";

constexpr char kSignatureInputList[] = "input_list";
constexpr char kSignatureInputShape[] = "input_shape";
constexpr char kSignatureOutput[] = "sigdef_output";

std::map<string, SignatureDef> GetTestSignatureDefMap() {
  auto signature_def = SignatureDef();
  TensorInfo input_list_tensor;
  TensorInfo input_shape_tensor;
  TensorInfo output_tensor;
  *input_list_tensor.mutable_name() = absl::StrCat(kTestModelInputList, ":0");
  *input_shape_tensor.mutable_name() = absl::StrCat(kTestModelInputShape, ":0");
  *output_tensor.mutable_name() = absl::StrCat(kTestModelOutput, ":0");
  *signature_def.mutable_method_name() = kClassifyMethodName;
  (*signature_def.mutable_inputs())[kSignatureInputList] = input_list_tensor;
  (*signature_def.mutable_inputs())[kSignatureInputShape] = input_shape_tensor;
  (*signature_def.mutable_outputs())[kSignatureOutput] = output_tensor;
  std::map<string, SignatureDef> signature_def_map = {
      {kDefaultServingSignatureDefKey, signature_def}};
  return signature_def_map;
}

tensorflow::DataType ToTfTensorType(tflite::TensorType tflite_type) {
  switch (tflite_type) {
    case tflite::TensorType_INT32:
      return tensorflow::DT_INT32;
    case tflite::TensorType_STRING:
      return tensorflow::DT_STRING;
    default:
      LOG(FATAL) << "Unsupported tflite type: " << tflite_type;
  }
}

// Returns a serialized FlatBuffer tflite model.
//
// The model has two inputs (kTestModelInputList|Shape) and one output
// kTestModelOutput. The output is list that is reshaped to shape via
// tf.reshape operator.
//
// Elements of list are expected to be of `tensor_type` type. `use_flex_op`
// sets up the model to use the `Reshape` *flex* op as opposed to using the
// builtin `Reshape` op from TF Lite.
string BuildTestModel(tflite::TensorType tensor_type, bool use_flex_op,
                      std::map<string, SignatureDef>* signature_def_map) {
  std::vector<int32_t> inputs;
  std::vector<int32_t> outputs;
  std::vector<flatbuffers::Offset<tflite::Tensor>> tensors;
  std::vector<flatbuffers::Offset<tflite::OperatorCode>> opcodes;
  std::vector<flatbuffers::Offset<tflite::Operator>> operators;
  std::vector<flatbuffers::Offset<tflite::Buffer>> buffers;
  flatbuffers::FlatBufferBuilder builder;

  // Input list: 1D tensor for list of `tensor_type` elements.
  inputs.push_back(tensors.size());
  tensors.push_back(CreateTensor(builder, builder.CreateVector<int>({1}),
                                 tensor_type, /*buffer=*/0,
                                 builder.CreateString(kTestModelInputList),
                                 /*quantization=*/0, /*is_variable=*/false));

  // Input shape: 1D tensor for shape.
  inputs.push_back(tensors.size());
  tensors.push_back(CreateTensor(builder, builder.CreateVector<int>({1}),
                                 tflite::TensorType_INT32, /*buffer=*/0,
                                 builder.CreateString(kTestModelInputShape),
                                 /*quantization=*/0, /*is_variable=*/false));

  // Output: Reshaped list to shape.
  outputs.push_back(tensors.size());
  tensors.push_back(CreateTensor(builder, builder.CreateVector<int>({1}),
                                 tensor_type, /*buffer=*/0,
                                 builder.CreateString(kTestModelOutput),
                                 /*quantization=*/0, /*is_variable=*/false));

  // Add reshape operator.
  tflite::BuiltinOptions builtin_opts_type =
      tflite::BuiltinOptions_ReshapeOptions;
  flatbuffers::Offset<void> reshape_opts =
      tflite::CreateReshapeOptions(builder, builder.CreateVector<int>({}))
          .Union();
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> custom_opts = 0;
  if (use_flex_op) {
    string flexop = std::string(tflite::kFlexCustomCodePrefix) + "Reshape";
    opcodes.push_back(CreateOperatorCodeDirect(
        builder, tflite::BuiltinOperator_CUSTOM, flexop.data()));
    builtin_opts_type = tflite::BuiltinOptions_NONE;
    reshape_opts = 0;
    NodeDef node_def;
    node_def.set_name("Reshape");
    node_def.set_op("Reshape");
    (*node_def.mutable_attr())["T"].set_type(ToTfTensorType(tensor_type));
    string node_def_str;
    CHECK(node_def.SerializeToString(&node_def_str));
    auto flex_builder = absl::make_unique<flexbuffers::Builder>();
    flex_builder->Vector([&]() {
      flex_builder->String(node_def.op());
      flex_builder->String(node_def_str);
    });
    flex_builder->Finish();
    custom_opts = builder.CreateVector(flex_builder->GetBuffer());
  } else {
    opcodes.push_back(
        CreateOperatorCode(builder, tflite::BuiltinOperator_RESHAPE, 0));
  }

  operators.push_back(CreateOperator(
      builder, /*opcode_index=*/0, builder.CreateVector<int32_t>(inputs),
      builder.CreateVector<int32_t>(outputs), builtin_opts_type, reshape_opts,
      custom_opts, tflite::CustomOptionsFormat_FLEXBUFFERS));

  auto subgraph = CreateSubGraph(builder, builder.CreateVector(tensors),
                                 builder.CreateVector<int32_t>(inputs),
                                 builder.CreateVector<int32_t>(outputs),
                                 builder.CreateVector(operators));
  builder.Finish(CreateModel(
      builder, TFLITE_SCHEMA_VERSION, builder.CreateVector(opcodes),
      builder.CreateVector(&subgraph, 1), builder.CreateString("testmodel"),
      builder.CreateVector(buffers)));

  if (signature_def_map) {
    std::string model_buffer = string(
        reinterpret_cast<char*>(builder.GetBufferPointer()), builder.GetSize());
    std::string model_buffer_with_signature_def;
    auto model = tflite::FlatBufferModel::BuildFromModel(
        flatbuffers::GetRoot<tflite::Model>(model_buffer.data()));
    TF_CHECK_OK(tflite::SetSignatureDefMap(model->GetModel(),
                                           *signature_def_map,
                                           &model_buffer_with_signature_def));
    return model_buffer_with_signature_def;
  }

  return string(reinterpret_cast<char*>(builder.GetBufferPointer()),
                builder.GetSize());
}

TEST(TfLiteSession, ProcessStrings) {
  auto model_signature_def_map = GetTestSignatureDefMap();
  string model_bytes =
      BuildTestModel(tflite::TensorType_STRING, /*use_flex_op=*/false,
                     &model_signature_def_map);
  ::google::protobuf::Map<string, SignatureDef> signatures;
  std::unique_ptr<TfLiteSession> session;
  TF_ASSERT_OK(
      TfLiteSession::Create(std::move(model_bytes), &session, &signatures));
  Tensor input_list =
      test::AsTensor<tstring>({"a", "b", "c", "d"}, TensorShape({4}));
  Tensor input_shape = test::AsTensor<int32>({2, 2}, TensorShape({2}));
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session->Run(
      {{kTestModelInputList, input_list}, {kTestModelInputShape, input_shape}},
      {kTestModelOutput}, {}, &outputs));
  ASSERT_EQ(outputs.size(), 1);
  test::ExpectTensorEqual<tstring>(
      outputs[0],
      test::AsTensor<tstring>({"a", "b", "c", "d"}, TensorShape({2, 2})));
}

TEST(TfLiteSession, ProcessStringsFlex) {
  auto model_signature_def_map = GetTestSignatureDefMap();
  string model_bytes =
      BuildTestModel(tflite::TensorType_STRING, /*use_flex_op=*/true,
                     &model_signature_def_map);
  ::google::protobuf::Map<string, SignatureDef> signatures;
  std::unique_ptr<TfLiteSession> session;
  TF_ASSERT_OK(
      TfLiteSession::Create(std::move(model_bytes), &session, &signatures));
  Tensor input_list =
      test::AsTensor<tstring>({"a", "b", "c", "d"}, TensorShape({4}));
  Tensor input_shape = test::AsTensor<int32>({2, 2}, TensorShape({2}));
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session->Run(
      {{kTestModelInputList, input_list}, {kTestModelInputShape, input_shape}},
      {kTestModelOutput}, {}, &outputs));
  ASSERT_EQ(outputs.size(), 1);
  test::ExpectTensorEqual<tstring>(
      outputs[0],
      test::AsTensor<tstring>({"a", "b", "c", "d"}, TensorShape({2, 2})));
}

TEST(TfLiteSession, ThreadPoolOptions) {
  auto model_signature_def_map = GetTestSignatureDefMap();
  string model_bytes =
      BuildTestModel(tflite::TensorType_STRING, /*use_flex_op=*/false,
                     &model_signature_def_map);
  ::google::protobuf::Map<string, SignatureDef> signatures;
  std::unique_ptr<TfLiteSession> session;
  TF_ASSERT_OK(
      TfLiteSession::Create(std::move(model_bytes), &session, &signatures));
  Tensor input_list =
      test::AsTensor<tstring>({"a", "b", "c", "d"}, TensorShape({4}));
  Tensor input_shape = test::AsTensor<int32>({2, 2}, TensorShape({2}));
  std::vector<Tensor> outputs;
  RunMetadata run_metadata;
  thread::ThreadPoolOptions thread_pool_options;
  test_util::CountingThreadPool inter_op_threadpool(Env::Default(), "InterOp",
                                                    /*num_threads=*/1);
  test_util::CountingThreadPool intra_op_threadpool(Env::Default(), "IntraOp",
                                                    /*num_threads=*/1);
  thread_pool_options.inter_op_threadpool = &inter_op_threadpool;
  thread_pool_options.intra_op_threadpool = &intra_op_threadpool;
  TF_EXPECT_OK(session->Run(
      RunOptions(),
      {{kTestModelInputList, input_list}, {kTestModelInputShape, input_shape}},
      {kTestModelOutput}, {}, &outputs, &run_metadata, thread_pool_options));
  ASSERT_EQ(outputs.size(), 1);
  test::ExpectTensorEqual<tstring>(
      outputs[0],
      test::AsTensor<tstring>({"a", "b", "c", "d"}, TensorShape({2, 2})));
  // TfLiteSession does not use the ThreadPoolOptions.
  EXPECT_EQ(inter_op_threadpool.NumScheduled(), 0);
  EXPECT_EQ(intra_op_threadpool.NumScheduled(), 0);
}

TEST(TfLiteSession, SimpleSignatureDef) {
  auto model_signature_def_map = GetTestSignatureDefMap();
  string model_bytes =
      BuildTestModel(tflite::TensorType_STRING, /*use_flex_op=*/false,
                     &model_signature_def_map);

  ::google::protobuf::Map<string, SignatureDef> signatures;
  // Fill an entry in the output signatures map, to check that it gets cleared
  string kResidualSignatureKey = "residual_signature";
  signatures[kResidualSignatureKey] = SignatureDef();

  std::unique_ptr<TfLiteSession> session;
  TF_ASSERT_OK(
      TfLiteSession::Create(std::move(model_bytes), &session, &signatures));

  ASSERT_THAT(signatures,
              UnorderedElementsAre(Pair(kDefaultServingSignatureDefKey, _)));

  auto sigdef = signatures[kDefaultServingSignatureDefKey];
  EXPECT_EQ(sigdef.inputs().at(kSignatureInputList).name(),
            kTestModelInputList);
  EXPECT_EQ(sigdef.inputs().at(kSignatureInputShape).name(),
            kTestModelInputShape);
  EXPECT_EQ(sigdef.outputs().at(kSignatureOutput).name(), kTestModelOutput);
  EXPECT_EQ(sigdef.method_name(), kClassifyMethodName);
}

TEST(TfLiteSession, MultipleSignatureDef) {
  TensorInfo input_list_tensor;
  TensorInfo input_shape_tensor;
  TensorInfo output_tensor;
  *input_list_tensor.mutable_name() = kTestModelInputList;
  *input_shape_tensor.mutable_name() = kTestModelInputShape;
  *output_tensor.mutable_name() = kTestModelOutput;
  SignatureDef signature1 = SignatureDef();
  *signature1.mutable_method_name() = kClassifyMethodName;
  (*signature1.mutable_inputs())[kSignatureInputList] = input_list_tensor;
  (*signature1.mutable_outputs())[kSignatureOutput] = output_tensor;
  SignatureDef signature2 = SignatureDef();
  *signature2.mutable_method_name() = kClassifyMethodName;
  (*signature2.mutable_inputs())[kSignatureInputShape] = input_shape_tensor;
  (*signature2.mutable_outputs())[kSignatureOutput] = output_tensor;
  constexpr char kSignatureKey1[] = "signature1";
  constexpr char kSignatureKey2[] = "signature2";
  std::map<string, SignatureDef> signature_def_map = {
      {kSignatureKey1, signature1}, {kSignatureKey2, signature2}};

  string model_bytes = BuildTestModel(
      tflite::TensorType_STRING, /*use_flex_op=*/false, &signature_def_map);
  ::google::protobuf::Map<string, SignatureDef> signatures;
  std::unique_ptr<TfLiteSession> session;
  TF_EXPECT_OK(
      TfLiteSession::Create(std::move(model_bytes), &session, &signatures));

  ASSERT_THAT(signatures, UnorderedElementsAre(Pair(kSignatureKey1, _),
                                               Pair(kSignatureKey2, _)));
  auto result_signature1 = signatures[kSignatureKey1];
  EXPECT_THAT(result_signature1.inputs().at(kSignatureInputList).name(),
              kTestModelInputList);
  EXPECT_EQ(result_signature1.outputs().at(kSignatureOutput).name(),
            kTestModelOutput);
  EXPECT_EQ(result_signature1.method_name(), kClassifyMethodName);
  auto result_signature2 = signatures[kSignatureKey2];
  EXPECT_EQ(result_signature2.inputs().at(kSignatureInputShape).name(),
            kTestModelInputShape);
  EXPECT_EQ(result_signature2.outputs().at(kSignatureOutput).name(),
            kTestModelOutput);
  EXPECT_EQ(result_signature2.method_name(), kClassifyMethodName);
}

TEST(TfLiteSession, SimpleSignatureDefAndRun) {
  auto model_signature_def_map = GetTestSignatureDefMap();
  string model_bytes =
      BuildTestModel(tflite::TensorType_STRING, /*use_flex_op=*/false,
                     &model_signature_def_map);
  ::google::protobuf::Map<string, SignatureDef> signatures;
  std::unique_ptr<TfLiteSession> session;
  TF_EXPECT_OK(
      TfLiteSession::Create(std::move(model_bytes), &session, &signatures));

  auto sigdef = signatures[kDefaultServingSignatureDefKey];
  ASSERT_EQ(sigdef.inputs().at(kSignatureInputList).name(),
            kTestModelInputList);
  ASSERT_EQ(sigdef.inputs().at(kSignatureInputShape).name(),
            kTestModelInputShape);
  ASSERT_EQ(sigdef.outputs().at(kSignatureOutput).name(), kTestModelOutput);
  ASSERT_EQ(sigdef.method_name(), kClassifyMethodName);

  Tensor input_list =
      test::AsTensor<tstring>({"a", "b", "c", "d"}, TensorShape({4}));
  Tensor input_shape = test::AsTensor<int32>({2, 2}, TensorShape({2}));
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session->Run(
      {{kTestModelInputList, input_list}, {kTestModelInputShape, input_shape}},
      {kTestModelOutput}, {}, &outputs));
  ASSERT_EQ(outputs.size(), 1);
  test::ExpectTensorEqual<tstring>(
      outputs[0],
      test::AsTensor<tstring>({"a", "b", "c", "d"}, TensorShape({2, 2})));
}

#ifdef PLATFORM_GOOGLE
// These benchmarks rely on https://github.com/google/benchmark features,
// not available in open-sourced TF codebase.

static void BM_Reshape(benchmark::State& state, bool use_flex_op) {
  static TfLiteSession* session;
  if (state.thread_index() == 0) {
    auto model_signature_def_map = GetTestSignatureDefMap();
    string model_bytes = BuildTestModel(tflite::TensorType_INT32, use_flex_op,
                                        &model_signature_def_map);
    ::google::protobuf::Map<string, SignatureDef> signatures;
    std::unique_ptr<TfLiteSession> sess;
    TF_ASSERT_OK(
        TfLiteSession::Create(std::move(model_bytes), &sess, &signatures));
    session = sess.release();
  }
  Tensor input = test::AsTensor<int32>({1, 2, 3, 4, 5, 6}, TensorShape({6}));
  Tensor input_shape = test::AsTensor<int32>({3, 2}, TensorShape({2}));
  std::vector<Tensor> outputs;
  testing::UseRealTime();
  for (auto _ : state) {
    outputs.clear();
    TF_ASSERT_OK(session->Run(
        {{kTestModelInputList, input}, {kTestModelInputShape, input_shape}},
        {kTestModelOutput}, {}, &outputs));
  }
}

static void BM_Reshape_Builtin(benchmark::State& state) {
  BM_Reshape(state, /*use_flex_op=*/false);
}
BENCHMARK(BM_Reshape_Builtin)->ThreadRange(1, 64);

static void BM_Reshape_Flex(benchmark::State& state) {
  BM_Reshape(state, /*use_flex_op=*/true);
}
BENCHMARK(BM_Reshape_Flex)->ThreadRange(1, 64);

void BM_HalfPlusTwo(benchmark::State& state) {
  static TfLiteSession* session;
  if (state.thread_index() == 0) {
    string model_bytes;
    TF_ASSERT_OK(ReadFileToString(
        Env::Default(), test_util::TestSrcDirPath(kTestModel), &model_bytes));
    ::google::protobuf::Map<string, SignatureDef> signatures;
    std::unique_ptr<TfLiteSession> sess;
    TF_ASSERT_OK(
        TfLiteSession::Create(std::move(model_bytes), &sess, &signatures));
    session = sess.release();
  }
  Tensor input = test::AsTensor<float>({1.0, 2.0, 3.0}, TensorShape({3}));
  std::vector<Tensor> outputs;
  testing::UseRealTime();
  for (auto _ : state) {
    outputs.clear();
    TF_ASSERT_OK(session->Run({{"x", input}}, {"y"}, {}, &outputs));
  }
}
BENCHMARK(BM_HalfPlusTwo)->ThreadRange(1, 64);

void BM_MobileNet(benchmark::State& state) {
  static TfLiteSession* session;
  if (state.thread_index() == 0) {
    string model_bytes;
    TF_ASSERT_OK(ReadFileToString(Env::Default(),
                                  test_util::TestSrcDirPath(kMobileNetModel),
                                  &model_bytes));
    ::google::protobuf::Map<string, SignatureDef> signatures;
    std::unique_ptr<TfLiteSession> sess;
    TF_ASSERT_OK(
        TfLiteSession::Create(std::move(model_bytes), &sess, &signatures));
    session = sess.release();
  }
  std::vector<int8> x_data(1 * 224 * 224 * 3, 1);
  Tensor x = test::AsTensor<int8>(x_data, TensorShape({1, 224, 224, 3}));
  std::vector<Tensor> outputs;
  testing::UseRealTime();
  for (auto _ : state) {
    outputs.clear();
    TF_ASSERT_OK(session->Run(
        {{"x:0", x}}, {"MobilenetV1/Predictions/Reshape_1"}, {}, &outputs));
  }
}
BENCHMARK(BM_MobileNet)->ThreadRange(1, 64);

#endif  // PLATFORM_GOOGLE

}  // namespace
}  // namespace serving
}  // namespace tensorflow
