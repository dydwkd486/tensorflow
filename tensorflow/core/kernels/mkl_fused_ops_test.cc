/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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
#ifdef INTEL_MKL
#include "tensorflow/cc/ops/const_op.h"
#include "tensorflow/cc/ops/image_ops.h"
#include "tensorflow/cc/ops/nn_ops.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/core/common_runtime/kernel_benchmark_testlib.h"
#include "tensorflow/core/framework/fake_input.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/kernels/conv_ops_gpu.h"
#include "tensorflow/core/kernels/ops_testutil.h"
#include "tensorflow/core/kernels/ops_util.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/platform/test_benchmark.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/public/session.h"

namespace tensorflow {

// Helper class for converting MKL tensors to TF tensors and comparing to
// expected values

static const uint8 dummy_tensor[] = {0, 0, 0, 0, 0, 0, 0, 0};
static const TensorShape dummy_shape({8});

using BiasAddGraphRunner =
    std::function<void(const Tensor& input_data, const Tensor& filter_data,
                       const Tensor& bias_data, Tensor* out)>;

template <typename T>
class CommonTestUtilities : public OpsTestBase {
 public:
  void PerformConversion(DataType dtype, const Tensor& tensor,
                         const Tensor& mkl_meta_tensor, Tensor* output) {
    // Create an MKL to TF conversion node and execute it
    TF_EXPECT_OK(NodeDefBuilder("mkl_to_tf_op", "_MklToTf")
                     .Input(FakeInput(dtype))     // Input
                     .Input(FakeInput(DT_UINT8))  // Mkl second tensor
                     .Attr("T", dtype)
                     .Attr("_kernel", "MklOp")
                     .Finalize(node_def()));
    TF_EXPECT_OK(InitOp());
    AddInputFromArray<T>(tensor.shape(), tensor.flat<T>());
    AddInputFromArray<uint8>(mkl_meta_tensor.shape(),
                             mkl_meta_tensor.flat<uint8>());
    TF_ASSERT_OK(RunOpKernel());

    *output = *GetOutput(0);
  }

  // Runs a Tensorflow graph defined by the root scope, and fetches the result
  // of 'fetch' node into the output Tensor.
  static void RunAndFetch(const tensorflow::Scope& root, const string& fetch,
                          Tensor* output) {
    tensorflow::GraphDef graph;
    TF_ASSERT_OK(root.ToGraphDef(&graph));

    std::unique_ptr<tensorflow::Session> session(
        tensorflow::NewSession(tensorflow::SessionOptions()));
    TF_ASSERT_OK(session->Create(graph));

    std::vector<Tensor> unfused_tensors;
    TF_ASSERT_OK(session->Run({}, {fetch}, {}, &unfused_tensors));

    *output = unfused_tensors[0];
  }

  void ConvertAndCompare(DataType dtype, const Tensor& tensor,
                         const Tensor& mkl_meta_tensor,
                         const Tensor& expected) {
    Tensor output;
    PerformConversion(dtype, tensor, mkl_meta_tensor, &output);
    test::ExpectTensorNear<T>(expected, output, 1e-5);
  }
  void TestBody() {}

  static void VerifyBiasAddTensorsClose(int depth, int image_width,
                                        int image_height, int image_batch_count,
                                        int filter_size, int filter_count,
                                        const BiasAddGraphRunner& run_default,
                                        const BiasAddGraphRunner& run_fused) {
    DataType dtype = DataTypeToEnum<T>::v();

    Tensor image(dtype, {image_batch_count, image_height, image_width, depth});
    image.flat<T>() = image.flat<T>().setRandom();

    Tensor filter(dtype, {filter_size, filter_size, depth, filter_count});
    filter.flat<T>() = filter.flat<T>().setRandom();

    const int bias_size = filter_count;
    Tensor bias(dtype, {bias_size});
    bias.flat<T>() = bias.flat<T>().setRandom();

    Tensor conv_2d;
    Tensor fused_conv_2d;

    run_default(image, filter, bias, &conv_2d);
    run_fused(image, filter, bias, &fused_conv_2d);

    ASSERT_EQ(conv_2d.dtype(), fused_conv_2d.dtype());
    ASSERT_EQ(conv_2d.shape(), fused_conv_2d.shape());

    test::ExpectClose(conv_2d, fused_conv_2d);
  }
};

// Testing MKL's fused convolution ops

template <typename T>
class MklFusedConv2DOpTest : public OpsTestBase {
 protected:
  static constexpr int kDepth = 3;
  static constexpr int kImageWidth = 32;
  static constexpr int kImageHeight = 32;
  static constexpr int kImageBatchCount = 8;

  void RunConv2DWithBias(const Tensor& input_data, const Tensor& filter_data,
                         const Tensor& bias_data, Tensor* output,
                         int stride = 1) {
    auto root = tensorflow::Scope::NewRootScope();

    auto conv = ops::Conv2D(
        root.WithOpName("conv"),
        ops::Const(root.WithOpName("input"), Input::Initializer(input_data)),
        ops::Const(root.WithOpName("filter"), Input::Initializer(filter_data)),
        {1, stride, stride, 1}, "SAME");

    auto with_bias = ops::BiasAdd(
        root.WithOpName("with_bias"), conv,
        ops::Const(root.WithOpName("bias"), Input::Initializer(bias_data)));

    CommonTestUtilities<T>::RunAndFetch(root, "with_bias", output);
  }

  void RunConv2DWithBiasAndRelu(const Tensor& input_data,
                                const Tensor& filter_data,
                                const Tensor& bias_data, Tensor* output,
                                int stride = 1) {
    auto root = tensorflow::Scope::NewRootScope();

    auto conv = ops::Conv2D(
        root.WithOpName("conv"),
        ops::Const(root.WithOpName("input"), Input::Initializer(input_data)),
        ops::Const(root.WithOpName("filter"), Input::Initializer(filter_data)),
        {1, stride, stride, 1}, "SAME");

    auto with_bias = ops::BiasAdd(
        root.WithOpName("with_bias"), conv,
        ops::Const(root.WithOpName("bias"), Input::Initializer(bias_data)));

    auto with_relu = ops::Relu(root.WithOpName("with_relu"), with_bias);

    CommonTestUtilities<T>::RunAndFetch(root, "with_relu", output);
  }

  void RunMklFusedConv2DOp(const Tensor& image, const Tensor& filter,
                           const std::vector<Tensor>& args,
                           const std::vector<string>& fused_ops, Tensor* output,
                           int stride = 1) {
    DataType dtype = DataTypeToEnum<T>::v();
    int num_args = static_cast<int>(args.size());

    TF_EXPECT_OK(NodeDefBuilder("fused_conv_op", "_MklFusedConv2D")
                     .Input(FakeInput(dtype))
                     .Input(FakeInput(dtype))
                     .Input(FakeInput(num_args, dtype))
                     .Input(FakeInput(DT_UINT8))
                     .Input(FakeInput(DT_UINT8))
                     .Input(FakeInput(num_args, DT_UINT8))
                     .Attr("T", dtype)
                     .Attr("num_args", num_args)
                     .Attr("strides", {1, stride, stride, 1})
                     .Attr("padding", "SAME")
                     .Attr("fused_ops", fused_ops)
                     .Attr("_kernel", "MklOp")
                     .Finalize(node_def()));

    TF_EXPECT_OK(InitOp());

    AddInputFromArray<T>(image.shape(), image.flat<T>());
    AddInputFromArray<T>(filter.shape(), filter.flat<T>());
    for (const Tensor& arg : args)
      AddInputFromArray<T>(arg.shape(), arg.flat<T>());
    AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
    AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
    for (const Tensor& arg : args)
      AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
    TF_ASSERT_OK(RunOpKernel());

    // Compare output to expected results
    const Tensor& output_tensor = *GetOutput(0);
    // Index 2 will need to be changed if the number of outputs produced
    // by MklConv2D change.
    const Tensor& output_meta_tensor = *GetOutput(2);
    CommonTestUtilities<T> test_util;
    test_util.PerformConversion(dtype, output_tensor, output_meta_tensor,
                                output);
  }

  // Verifies that computing Conv2D+BiasAdd in a graph is identical to
  // FusedConv2D.
  void VerifyConv2DWithBias(int filter_size, int filter_count,
                            int depth = kDepth, int image_width = kImageWidth,
                            int image_height = kImageHeight,
                            int image_batch_count = kImageBatchCount) {
    const BiasAddGraphRunner run_default =
        [this](const Tensor& input_data, const Tensor& filter_data,
               const Tensor& bias_data, Tensor* out) {
          RunConv2DWithBias(input_data, filter_data, bias_data, out);
        };

    const BiasAddGraphRunner run_fused =
        [this](const Tensor& input_data, const Tensor& filter_data,
               const Tensor& bias_data, Tensor* out) {
          RunMklFusedConv2DOp(input_data, filter_data, {bias_data}, {"BiasAdd"},
                              out);
        };

    CommonTestUtilities<T>::VerifyBiasAddTensorsClose(
        depth, image_width, image_height, image_batch_count, filter_size,
        filter_count, run_default, run_fused);
  }

  // Verifies that computing Conv2D+BiasAdd+Relu in a graph is identical to
  // FusedConv2D.
  void VerifyConv2DWithBiasAndRelu(int filter_size, int filter_count,
                                   int depth = kDepth,
                                   int image_width = kImageWidth,
                                   int image_height = kImageHeight,
                                   int image_batch_count = kImageBatchCount) {
    const BiasAddGraphRunner run_default =
        [this](const Tensor& input_data, const Tensor& filter_data,
               const Tensor& bias_data, Tensor* out) {
          RunConv2DWithBiasAndRelu(input_data, filter_data, bias_data, out);
        };

    const BiasAddGraphRunner run_fused =
        [this](const Tensor& input_data, const Tensor& filter_data,
               const Tensor& bias_data, Tensor* out) {
          RunMklFusedConv2DOp(input_data, filter_data, {bias_data},
                              {"BiasAdd", "Relu"}, out);
        };

    CommonTestUtilities<T>::VerifyBiasAddTensorsClose(
        depth, image_width, image_height, image_batch_count, filter_size,
        filter_count, run_default, run_fused);
  }
};

template <typename T>
class MklFusedConv2DWithBiasOpTest : public MklFusedConv2DOpTest<T> {};

TYPED_TEST_CASE_P(MklFusedConv2DWithBiasOpTest);

// -------------------------------------------------------------------------- //
// Conv2D + BiasAdd + {Relu}                                                  //
// -------------------------------------------------------------------------- //

TYPED_TEST_P(MklFusedConv2DWithBiasOpTest, OneByOneConvolution) {
  const int filter_size = 1;
  const int filter_count = 12;
  this->VerifyConv2DWithBias(filter_size, filter_count);
}

TYPED_TEST_P(MklFusedConv2DWithBiasOpTest, SpatialConvolution) {
  const int filter_size = 3;
  const int filter_count = 12;
  this->VerifyConv2DWithBias(filter_size, filter_count);
}

TYPED_TEST_P(MklFusedConv2DWithBiasOpTest, OneByOneConvolutionAndRelu) {
  const int filter_size = 1;
  const int filter_count = 12;
  this->VerifyConv2DWithBiasAndRelu(filter_size, filter_count);
}

TYPED_TEST_P(MklFusedConv2DWithBiasOpTest, SpatialConvolutionAndRelu) {
  const int filter_size = 3;
  const int filter_count = 12;
  this->VerifyConv2DWithBiasAndRelu(filter_size, filter_count);
}

REGISTER_TYPED_TEST_CASE_P(MklFusedConv2DWithBiasOpTest,  //
                           OneByOneConvolution,           //
                           SpatialConvolution,            //
                           OneByOneConvolutionAndRelu,    //
                           SpatialConvolutionAndRelu);

using MklFusedBiasAddDataTypes = ::testing::Types<float>;
INSTANTIATE_TYPED_TEST_CASE_P(Test, MklFusedConv2DWithBiasOpTest,
                              MklFusedBiasAddDataTypes);
// Testing fusion of pad and convolution

class FusedPadConvOpTest : public OpsTestBase {
 public:
  template <typename T>
  void Run(DataType dtype, Tensor& image, Tensor& filter, Tensor& padding,
           Tensor& expected, const string data_format) {
    const int stride = 1;

    // Create a fused pad+conv2d node
    TF_EXPECT_OK(NodeDefBuilder("fused_pad_conv_op", "_MklPadWithConv2D")
                     .Input(FakeInput(dtype))     // Input
                     .Input(FakeInput(dtype))     // Filter
                     .Input(FakeInput(DT_INT32))  // Padding
                     .Input(FakeInput(DT_UINT8))  // MKl second tensor
                     .Input(FakeInput(DT_UINT8))  // MKl second tensor
                     .Input(FakeInput(DT_UINT8))  // MKl second tensor
                     .Attr("padding", "VALID")
                     .Attr("data_format", data_format)
                     .Attr("T", dtype)
                     .Attr("strides", {1, stride, stride, 1})
                     .Attr("_kernel", "MklOp")
                     .Finalize(node_def()));
    TF_EXPECT_OK(InitOp());

    // Setting up inputs and execute
    AddInputFromArray<T>(image.shape(), image.flat<T>());
    AddInputFromArray<T>(filter.shape(), filter.flat<T>());
    AddInputFromArray<int32>(padding.shape(), padding.flat<int32>());
    AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
    AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
    AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
    TF_ASSERT_OK(RunOpKernel());

    // Compare output to expected results
    const Tensor& first = *GetOutput(0);
    const Tensor& second = *GetOutput(2);
    CommonTestUtilities<T> test_util;
    test_util.ConvertAndCompare(dtype, first, second, expected);
  }
};

TEST_F(FusedPadConvOpTest, PaddingConvTest) {
  const int depth = 1;
  const int image_width = 4;
  const int image_height = 3;
  const int image_batch_count = 1;
  Tensor image(DT_FLOAT, {image_batch_count, image_height, image_width, depth});
  test::FillValues<float>(&image, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});

  const int filter_size = 3;
  const int filter_count = 1;
  Tensor filter(DT_FLOAT, {filter_size, filter_size, depth, filter_count});
  test::FillValues<float>(&filter, {1, 4, 7, 2, 5, 8, 3, 6, 9});

  const int padding_height = 4;
  const int padding_width = 2;
  Tensor padding(DT_INT32, {padding_height, padding_width});
  test::FillValues<int32>(&padding, {0, 0, 3, 4, 1, 2, 0, 0});

  Tensor expected(DT_FLOAT, TensorShape({1, 8, 5, 1}));
  test::FillValues<float>(
      &expected,
      {0,  0,   0,   0,   0,   24, 42,  60,  33,  12,  105, 150, 183, 95,
       32, 235, 312, 357, 178, 56, 187, 234, 261, 121, 32,  106, 126, 138,
       59, 12,  0,   0,   0,   0,  0,   0,   0,   0,   0,   0});

  Run<float>(DT_FLOAT, image, filter, padding, expected, "NHWC");
}

TEST_F(FusedPadConvOpTest, PaddingConvTestNchw) {
  const int depth = 1;
  const int image_width = 4;
  const int image_height = 3;
  const int image_batch_count = 1;
  Tensor image(DT_FLOAT, {image_batch_count, depth, image_height, image_width});
  test::FillValues<float>(&image, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});

  const int filter_size = 3;
  const int filter_count = 1;
  Tensor filter(DT_FLOAT, {filter_size, filter_size, depth, filter_count});
  test::FillValues<float>(&filter, {1, 4, 7, 2, 5, 8, 3, 6, 9});

  const int padding_height = 4;
  const int padding_width = 2;
  Tensor padding(DT_INT32, {padding_height, padding_width});
  test::FillValues<int32>(&padding, {0, 0, 0, 0, 3, 4, 1, 2});

  Tensor expected(DT_FLOAT, TensorShape({1, 1, 8, 5}));
  test::FillValues<float>(
      &expected,
      {0,  0,   0,   0,   0,   24, 42,  60,  33,  12,  105, 150, 183, 95,
       32, 235, 312, 357, 178, 56, 187, 234, 261, 121, 32,  106, 126, 138,
       59, 12,  0,   0,   0,   0,  0,   0,   0,   0,   0,   0});

  Run<float>(DT_FLOAT, image, filter, padding, expected, "NCHW");
}

class FilterCacheTest : public OpsTestBase {
 public:
  template <typename T>
  void Run(DataType dtype, Tensor& image, Tensor& filter, Tensor& expected,
           const bool is_filter_const) {
    const int stride = 1;

    TF_EXPECT_OK(NodeDefBuilder("conv2d_filter_cache", "_MklConv2D")
                     .Input(FakeInput(dtype))     // Input
                     .Input(FakeInput(dtype))     // Filter
                     .Input(FakeInput(DT_UINT8))  // MKl second tensor
                     .Input(FakeInput(DT_UINT8))  // MKl second tensor
                     .Attr("padding", "VALID")
                     .Attr("data_format", "NHWC")
                     .Attr("is_filter_const", is_filter_const)
                     .Attr("T", dtype)
                     .Attr("strides", {1, stride, stride, 1})
                     .Attr("_kernel", "MklOp")
                     .Finalize(node_def()));
    TF_EXPECT_OK(InitOp());

    // Setting up inputs and execute
    AddInputFromArray<T>(image.shape(), image.flat<T>());
    AddInputFromArray<T>(filter.shape(), filter.flat<T>());
    AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
    AddInputFromArray<uint8>(dummy_shape, dummy_tensor);

    TF_ASSERT_OK(RunOpKernel());

    // Compare outputs to expected results
    const Tensor& output = *GetOutput(0);
    const Tensor& output_layout = *GetOutput(2);
    CommonTestUtilities<T> conv_comp;
    conv_comp.ConvertAndCompare(dtype, output, output_layout, expected);

    // TODO(bhavanis): For now, we rely on internal performance tests to
    // determine if filter data is being cached and reused.
    // However, we still need to add a check here to determine if this is
    // still the case by inspecting the contents of the persistent tensor.
    TF_ASSERT_OK(RunOpKernel());

    // Compare output to expected results
    const Tensor& output_new = *GetOutput(0);
    const Tensor& output_layout_new = *GetOutput(2);
    CommonTestUtilities<T> conv_comp_new;
    conv_comp_new.ConvertAndCompare(dtype, output_new, output_layout_new,
                                    expected);
  }
};

TEST_F(FilterCacheTest, Conv2DFilterCacheTest) {
  const int depth = 1;
  const int image_width = 4;
  const int image_height = 3;
  const int image_batch_count = 1;
  Tensor image(DT_FLOAT, {image_batch_count, image_height, image_width, depth});
  test::FillValues<float>(&image, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});

  const int filter_size = 3;
  const int filter_count = 1;
  Tensor filter(DT_FLOAT, {filter_size, filter_size, depth, filter_count});
  test::FillValues<float>(&filter, {1, 4, 7, 2, 5, 8, 3, 6, 9});

  Tensor expected(DT_FLOAT, TensorShape({1, 1, 2, 1}));
  test::FillValues<float>(&expected, {312, 357});

  Run<float>(DT_FLOAT, image, filter, expected, true);
}

// Testing fusion of pad and fusedconv2d
template <typename T>
class MklPadWithFusedConv2DOpTest : public OpsTestBase {
 protected:
  static constexpr int kDepth = 3;
  static constexpr int kImageWidth = 30;
  static constexpr int kImageHeight = 28;
  static constexpr int kImageBatchCount = 8;

  // 0: top pad, 1: bottom pad, 2: left pad, 3: right pad
  int padding_list_[4];

  // Verifies that computing Pad+Conv2D+BiasAdd in a graph is identical to
  // FusedConv2D.
  void VerifyPadAndConv2DWithBias(int filter_size, int filter_count,
                                  int depth = kDepth,
                                  int image_width = kImageWidth,
                                  int image_height = kImageHeight,
                                  int image_batch_count = kImageBatchCount) {
    const BiasAddGraphRunner run_default = [this](const Tensor& input_data,
                                                  const Tensor& filter_data,
                                                  const Tensor& bias_data,
                                                  Tensor* out) {
      RunMklPadWithFusedConv2DAndBias(input_data, filter_data, bias_data, out);
    };

    const BiasAddGraphRunner run_fused =
        [this](const Tensor& input_data, const Tensor& filter_data,
               const Tensor& bias_data, Tensor* out) {
          RunMklFusedConv2DWithPadOp(input_data, filter_data, {bias_data},
                                     {"BiasAdd"}, out);
        };

    CommonTestUtilities<T>::VerifyBiasAddTensorsClose(
        depth, image_width, image_height, image_batch_count, filter_size,
        filter_count, run_default, run_fused);
  }

  // Verifies that computing Pad+Conv2D+BiasAdd+Relu in a graph is identical to
  // FusedConv2D.
  void VerifyPadAndConv2DWithBiasRelu(
      int filter_size, int filter_count, int depth = kDepth,
      int image_width = kImageWidth, int image_height = kImageHeight,
      int image_batch_count = kImageBatchCount) {
    const BiasAddGraphRunner run_default =
        [this](const Tensor& input_data, const Tensor& filter_data,
               const Tensor& bias_data, Tensor* out) {
          RunMklPadWithFusedConv2DAndBiasRelu(input_data, filter_data,
                                              bias_data, out);
        };

    const BiasAddGraphRunner run_fused =
        [this](const Tensor& input_data, const Tensor& filter_data,
               const Tensor& bias_data, Tensor* out) {
          RunMklFusedConv2DWithPadOp(input_data, filter_data, {bias_data},
                                     {"BiasAdd", "Relu"}, out);
        };

    CommonTestUtilities<T>::VerifyBiasAddTensorsClose(
        depth, image_width, image_height, image_batch_count, filter_size,
        filter_count, run_default, run_fused);
  }

  void RunMklPadWithFusedConv2DAndBias(const Tensor& input_data,
                                       const Tensor& filter_data,
                                       const Tensor& bias_data, Tensor* output,
                                       int stride = 1) {
    auto root = tensorflow::Scope::NewRootScope();

    // FusedConv2D only supports NHWC format so we use NHWC here.
    auto padding = ops::Const(root.WithOpName("padding"),
                              {0, 0, padding_list_[0], padding_list_[1],
                               padding_list_[2], padding_list_[3], 0, 0},
                              {4, 2});
    auto pad = ops::Pad(
        root.WithOpName("pad"),
        ops::Const(root.WithOpName("input"), Input::Initializer(input_data)),
        padding);

    auto conv = ops::Conv2D(
        root.WithOpName("conv"), pad,
        ops::Const(root.WithOpName("filter"), Input::Initializer(filter_data)),
        {1, stride, stride, 1}, "VALID");

    auto with_bias = ops::BiasAdd(
        root.WithOpName("with_bias"), conv,
        ops::Const(root.WithOpName("bias"), Input::Initializer(bias_data)));

    CommonTestUtilities<T>::RunAndFetch(root, "with_bias", output);
  }

  void RunMklPadWithFusedConv2DAndBiasRelu(const Tensor& input_data,
                                           const Tensor& filter_data,
                                           const Tensor& bias_data,
                                           Tensor* output, int stride = 1) {
    auto root = tensorflow::Scope::NewRootScope();

    // FusedConv2D only supports NHWC format so we use NHWC here.
    auto padding = ops::Const(root.WithOpName("padding"),
                              {0, 0, padding_list_[0], padding_list_[1],
                               padding_list_[2], padding_list_[3], 0, 0},
                              {4, 2});
    auto pad = ops::Pad(
        root.WithOpName("pad"),
        ops::Const(root.WithOpName("input"), Input::Initializer(input_data)),
        padding);

    auto conv = ops::Conv2D(
        root.WithOpName("conv"), pad,
        ops::Const(root.WithOpName("filter"), Input::Initializer(filter_data)),
        {1, stride, stride, 1}, "VALID");

    auto with_bias = ops::BiasAdd(
        root.WithOpName("with_bias"), conv,
        ops::Const(root.WithOpName("bias"), Input::Initializer(bias_data)));

    auto with_relu = ops::Relu(root.WithOpName("with_relu"), with_bias);

    CommonTestUtilities<T>::RunAndFetch(root, "with_relu", output);
  }

  void RunMklFusedConv2DWithPadOp(const Tensor& image, const Tensor& filter,
                                  const std::vector<Tensor>& args,
                                  const std::vector<string>& fused_ops,
                                  Tensor* output, int stride = 1) {
    DataType dtype = DataTypeToEnum<T>::v();
    const int num_args = static_cast<int>(args.size());
    Tensor padding(DT_INT32, {4, 2});
    test::FillValues<int32>(
        &padding, {0, 0, padding_list_[0], padding_list_[1], padding_list_[2],
                   padding_list_[3], 0, 0});

    TF_EXPECT_OK(NodeDefBuilder("pad_fused_conv_op", "_MklPadWithFusedConv2D")
                     .Input(FakeInput(dtype))
                     .Input(FakeInput(dtype))
                     .Input(FakeInput(num_args, dtype))
                     .Input(FakeInput(DT_INT32))
                     .Input(FakeInput(DT_UINT8))
                     .Input(FakeInput(DT_UINT8))
                     .Input(FakeInput(num_args, DT_UINT8))
                     .Input(FakeInput(DT_UINT8))
                     .Attr("T", dtype)
                     .Attr("num_args", num_args)
                     .Attr("strides", {1, stride, stride, 1})
                     .Attr("padding", "VALID")
                     .Attr("fused_ops", fused_ops)
                     .Attr("_kernel", "MklOp")
                     .Finalize(node_def()));

    TF_EXPECT_OK(InitOp());

    AddInputFromArray<T>(image.shape(), image.flat<T>());
    AddInputFromArray<T>(filter.shape(), filter.flat<T>());
    for (const Tensor& arg : args)
      AddInputFromArray<T>(arg.shape(), arg.flat<T>());
    AddInputFromArray<int32>(padding.shape(), padding.flat<int32>());
    // Add MKL meta input for input, filter, pad and agrs.
    for (int i = 0; i < args.size() + 3; ++i)
      AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
    TF_ASSERT_OK(RunOpKernel());

    // Compare output to expected results
    const Tensor& output_tensor = *GetOutput(0);
    // Index 2 will need to be changed if the number of outputs produced
    // by MklConv2D change.
    const Tensor& output_meta_tensor = *GetOutput(2);
    CommonTestUtilities<T> test_util;
    test_util.PerformConversion(dtype, output_tensor, output_meta_tensor,
                                output);
  }

 public:
  void SetPaddingList(int top, int bottom, int left, int right) {
    padding_list_[0] = top;
    padding_list_[1] = bottom;
    padding_list_[2] = left;
    padding_list_[3] = right;
  }
};

TYPED_TEST_CASE_P(MklPadWithFusedConv2DOpTest);

TYPED_TEST_P(MklPadWithFusedConv2DOpTest, WithBiasAndRoundPad) {
  const int filter_size = 1;
  const int filter_count = 12;
  this->SetPaddingList(2, 2, 1, 1);
  this->VerifyPadAndConv2DWithBias(filter_size, filter_count);
}

TYPED_TEST_P(MklPadWithFusedConv2DOpTest, WithBiasAndPartialPad) {
  const int filter_size = 1;
  const int filter_count = 12;
  this->SetPaddingList(4, 0, 2, 0);
  this->VerifyPadAndConv2DWithBias(filter_size, filter_count);
}

TYPED_TEST_P(MklPadWithFusedConv2DOpTest, WithBiasReluAndRoundPad) {
  const int filter_size = 1;
  const int filter_count = 12;
  this->SetPaddingList(2, 2, 1, 1);
  this->VerifyPadAndConv2DWithBiasRelu(filter_size, filter_count);
}

TYPED_TEST_P(MklPadWithFusedConv2DOpTest, WithBiasReluAndPartialPad) {
  const int filter_size = 1;
  const int filter_count = 12;
  this->SetPaddingList(4, 0, 2, 0);
  this->VerifyPadAndConv2DWithBiasRelu(filter_size, filter_count);
}

REGISTER_TYPED_TEST_CASE_P(MklPadWithFusedConv2DOpTest,  //
                           WithBiasAndRoundPad,          //
                           WithBiasAndPartialPad,        //
                           WithBiasReluAndRoundPad,      //
                           WithBiasReluAndPartialPad);

using MklPadWithFusedConv2DDataTypes = ::testing::Types<float>;
INSTANTIATE_TYPED_TEST_CASE_P(Test, MklPadWithFusedConv2DOpTest,
                              MklPadWithFusedConv2DDataTypes);

}  // namespace tensorflow
#endif  // INTEL_MKL
