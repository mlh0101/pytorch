#include <torch/csrc/jit/tensorexpr/external_functions.h>

#include <ATen/ATen.h>
#include <ATen/Functions.h>
#include <ATen/NativeFunctions.h>
#include <ATen/core/Tensor.h>
#include <ATen/native/quantized/cpu/conv_packed_params.h>
#include <ATen/native/quantized/cpu/conv_serialization.h>
#include <ATen/native/quantized/cpu/qadd.h>
#include <ATen/native/quantized/cpu/quant_utils.h>
#include <ATen/native/quantized/cpu/quantized_ops.h>
#include <ATen/native/xnnpack/OpContext.h>
#include <ATen/quantized/QTensorImpl.h>
#include <c10/core/TensorOptions.h>
#include <c10/util/ArrayRef.h>
#include <c10/util/irange.h>
#include <torch/csrc/jit/serialization/import_source.h>
#include <torch/csrc/jit/serialization/pickle.h>
#include <torch/csrc/jit/tensorexpr/exceptions.h>
#include <torch/csrc/jit/tensorexpr/external_functions_registry.h>
#include <utility>

namespace torch {
namespace jit {
namespace tensorexpr {

c10::MemoryFormat deduce_memory_format(
    c10::IntArrayRef strides,
    c10::IntArrayRef dims) {
  if (strides.size() == 4 && strides[3] == dims[1] && strides[1] == 1l) {
    return c10::MemoryFormat::ChannelsLast;
  }
  return c10::MemoryFormat::Contiguous;
}

c10::MemoryFormat deduce_memory_format(
    const std::vector<int64_t>& strides,
    const std::vector<int64_t>& dims) {
  return deduce_memory_format(
      c10::IntArrayRef(strides), c10::IntArrayRef(dims));
}

at::Tensor from_blob_quantized(
    void* data,
    at::IntArrayRef sizes,
    at::IntArrayRef strides,
    double qscale,
    int64_t qzero,
    at::ScalarType dtype) {
  auto memory_format = deduce_memory_format(strides, sizes);
  auto qx = at::_empty_affine_quantized(
      sizes,
      dtype,
      c10::kStrided,
      at::kCPU,
      false,
      qscale,
      qzero,
      memory_format);
  auto qtensor_impl = static_cast<at::QTensorImpl*>(qx.unsafeGetTensorImpl());
  auto typeMeta = c10::scalarTypeToTypeMeta(dtype);
  std::size_t size = 1;
  for (std::int64_t s : sizes) {
    size *= static_cast<std::size_t>(s);
  }
  qtensor_impl->ShareExternalPointer(
      c10::InefficientStdFunctionContext::makeDataPtr(
          data, [](void*) {}, at::kCPU),
      typeMeta,
      size * typeMeta.itemsize());
  qtensor_impl->set_sizes_and_strides(sizes, strides);
  return qx;
}

std::vector<at::Tensor> constructTensors(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    c10::optional<std::vector<std::pair<size_t, QIData>>> qdataArg) {
  std::vector<void*> buf_data_vec;
  std::vector<std::vector<int64_t>> buf_dims_vec;
  std::vector<std::vector<int64_t>> buf_strides_vec;
  std::vector<c10::ScalarType> buf_dtypes_vec;
  int64_t buf_dims_idx = 0;
  int64_t buf_strides_idx = 0;
  for (const auto i : c10::irange(bufs_num)) {
    buf_data_vec.push_back(buf_data[i]);
    buf_dims_vec.emplace_back();
    buf_strides_vec.emplace_back();
    for (const auto dim : c10::irange(buf_ranks[i])) {
      (void)dim;
      buf_dims_vec[i].push_back(buf_dims[buf_dims_idx++]);
      buf_strides_vec[i].push_back(buf_strides[buf_strides_idx++]);
    }
    buf_dtypes_vec.push_back(static_cast<c10::ScalarType>(buf_dtypes[i]));
  }

  std::vector<at::Tensor> tensors;
  if (!qdataArg.has_value()) {
    for (const auto i : c10::irange(buf_data_vec.size())) {
      auto options = at::TensorOptions()
                         // NOLINTNEXTLINE
                         .dtype(buf_dtypes_vec[i])
                         .layout(at::kStrided)
                         .device(at::kCPU) // TODO: support GPUs too
                         .memory_format(deduce_memory_format(
                             // NOLINTNEXTLINE
                             buf_strides_vec[i],
                             // NOLINTNEXTLINE
                             buf_dims_vec[i]))
                         .requires_grad(false);
      auto tensor = at::from_blob(
          // NOLINTNEXTLINE
          buf_data_vec[i],
          buf_dims_vec[i],
          buf_strides_vec[i],
          options);
      tensors.emplace_back(tensor);
    }
  } else {
    // handle quantized
    std::vector<c10::optional<QIData>> qdata(bufs_num, c10::nullopt);
    for (const auto& qd : *qdataArg) {
      qdata[qd.first] = qd.second;
    }
    for (const auto i : c10::irange(buf_data_vec.size())) {
      auto options = at::TensorOptions()
                         // NOLINTNEXTLINE
                         .dtype(buf_dtypes_vec[i])
                         .layout(at::kStrided)
                         .device(at::kCPU) // TODO: support GPUs too
                         .memory_format(deduce_memory_format(
                             // NOLINTNEXTLINE
                             buf_strides_vec[i],
                             // NOLINTNEXTLINE
                             buf_dims_vec[i]))
                         .requires_grad(false);
      if (auto qd = qdata[i]) {
        // inplace tensor
        auto tensor = from_blob_quantized(
            // NOLINTNEXTLINE
            buf_data_vec[i],
            buf_dims_vec[i],
            buf_strides_vec[i],
            qd->scale,
            qd->zero,
            qd->scalarType);
        tensors.emplace_back(tensor);
      } else {
        auto tensor = at::from_blob(
            // NOLINTNEXTLINE
            buf_data_vec[i],
            buf_dims_vec[i],
            buf_strides_vec[i],
            options);
        tensors.emplace_back(tensor);
      }
    }
  }
  return tensors;
}

std::vector<at::Tensor> constructTensors(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    std::vector<std::pair<size_t, QIData>> qdata) {
  c10::optional<std::vector<std::pair<size_t, QIData>>> opt = std::move(qdata);
  return constructTensors(
      bufs_num, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes, opt);
}

#ifndef _WIN32
at::Tensor quantized_add_out(
    const at::Tensor& x1,
    const at::Tensor& x2,
    at::Tensor out) {
  const auto qadd_op =
      c10::Dispatcher::singleton()
          .findSchemaOrThrow("quantized::add", "out")
          .typed<at::Tensor(at::Tensor, at::Tensor, at::Tensor)>();
  return qadd_op.call(x1, x2, out);
}

at::Tensor quantized_mul_out(
    const at::Tensor& x1,
    const at::Tensor& x2,
    at::Tensor out) {
  const auto op = c10::Dispatcher::singleton()
                      .findSchemaOrThrow("quantized::mul", "out")
                      .typed<at::Tensor(at::Tensor, at::Tensor, at::Tensor)>();
  return op.call(x1, x2, out);
}

at::Tensor quantized_mul_scalar_out(
    const at::Tensor& x,
    double scalar,
    at::Tensor out) {
  const auto op =
      c10::Dispatcher::singleton()
          .findSchemaOrThrow("quantized::mul", "Scalar_out")
          .typed<at::Tensor(at::Tensor, c10::Scalar const&, at::Tensor)>();
  auto s = c10::Scalar(scalar);
  return op.call(x, s, out);
}

at::Tensor quantized_sigmoid(const at::Tensor& x, double scale, int64_t zero) {
  const auto op = c10::Dispatcher::singleton()
                      .findSchemaOrThrow("quantized::sigmoid", "")
                      .typed<at::Tensor(at::Tensor, double, int64_t)>();
  return op.call(x, scale, zero);
}

at::Tensor quantized_cat_out(
    const c10::List<at::Tensor>& qxs,
    int64_t dim,
    at::Tensor& out) {
  const auto op = c10::Dispatcher::singleton()
                      .findSchemaOrThrow("quantized::cat_out", "")
                      .typed<at::Tensor(
                          c10::List<at::Tensor> const&, int64_t, at::Tensor)>();
  return op.redispatch(
      c10::DispatchKeySet({c10::DispatchKey::QuantizedCPU}), qxs, dim, out);
}

at::Tensor quantized_relu(const at::Tensor& qx) {
  const auto op = c10::Dispatcher::singleton()
                      .findSchemaOrThrow("quantized::relu", "")
                      .typed<at::Tensor(at::Tensor)>();
  return op.call(qx);
}

void quantized_upsample_nearest2d_out(
    const at::Tensor& input,
    c10::optional<at::IntArrayRef> output_size,
    c10::optional<at::ArrayRef<double>> scale_factors,
    at::Tensor& output) {
  const auto op = c10::Dispatcher::singleton()
                      .findSchemaOrThrow("quantized::upsample_nearest2d", "out")
                      .typed<at::Tensor&(
                          const at::Tensor&,
                          c10::optional<at::IntArrayRef>,
                          c10::optional<at::ArrayRef<double>>,
                          at::Tensor&)>();
  op.call(input, output_size, scale_factors, output);
}
#endif // _WIN32

#ifdef C10_MOBILE
extern "C" {
#endif

void nnc_aten_conv2d(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t args_num,
    int64_t* extra_args) {
  auto tensors = constructTensors(
      bufs_num, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes);

  at::Tensor& r = tensors[0];
  const at::Tensor& x = tensors[1];
  const at::Tensor& w = tensors[2];
  if (args_num > 0) {
    // Check that if the extra arguments are provided, then the bias tensor is
    // also present
    TORCH_INTERNAL_ASSERT(args_num == 7 && bufs_num == 4);
    const at::Tensor& b = tensors[3];

    int64_t strideH = extra_args[0];
    int64_t strideW = extra_args[1];
    int64_t paddingH = extra_args[2];
    int64_t paddingW = extra_args[3];
    int64_t dilationH = extra_args[4];
    int64_t dilationW = extra_args[5];
    int64_t groups = extra_args[6];

    try {
      r = at::conv2d(
          x,
          w,
          b,
          {strideH, strideW},
          {paddingH, paddingW},
          {dilationH, dilationW},
          groups);
    } catch (...) {
    }
  } else {
    try {
      r = at::conv2d(x, w);
    } catch (...) {
    }
  }

  // TODO: can i haz an out version of the conv2d?
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

void nnc_aten_quantized_conv1d(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  const double x_qscale = ((double*)extra_args)[0];
  const int64_t x_qzero = extra_args[1];
  const c10::ScalarType x_qdtype = static_cast<c10::ScalarType>(extra_args[2]);
  auto tensors = constructTensors(
      bufs_num,
      buf_data,
      buf_ranks,
      buf_dims,
      buf_strides,
      buf_dtypes,
      {{1u, {x_qscale, x_qzero, toQIntType(x_qdtype)}}});
  auto convPackedParams =
      reinterpret_cast<ConvPackedParamsBase<2>*>(buf_data[2]);
  const double out_qscale = ((double*)extra_args)[3];
  const int64_t out_qzero = extra_args[4];
  // NOLINTNEXTLINE
  auto qx = tensors[1].unsqueeze(quant_utils::kConv1dSqueezeDim + 2);
  auto r = convPackedParams->apply(qx, out_qscale, out_qzero);
  r = r.squeeze_(quant_utils::kConv1dSqueezeDim + 2);
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

void nnc_aten_quantized_conv2d(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  const double x_qscale = ((double*)extra_args)[0];
  const int64_t x_qzero = extra_args[1];
  const c10::ScalarType x_qdtype = static_cast<c10::ScalarType>(extra_args[2]);
  auto tensors = constructTensors(
      bufs_num,
      buf_data,
      buf_ranks,
      buf_dims,
      buf_strides,
      buf_dtypes,
      {{1u, {x_qscale, x_qzero, toQIntType(x_qdtype)}}});
  auto convPackedParams =
      reinterpret_cast<ConvPackedParamsBase<2>*>(buf_data[2]);
  const double out_qscale = ((double*)extra_args)[3];
  const int64_t out_qzero = extra_args[4];
  // NOLINTNEXTLINE
  auto r = convPackedParams->apply(tensors[1], out_qscale, out_qzero);
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

void nnc_aten_quantized_conv2d_relu(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  const double x_qscale = ((double*)extra_args)[0];
  const int64_t x_qzero = extra_args[1];
  const c10::ScalarType x_qdtype = static_cast<c10::ScalarType>(extra_args[2]);
  auto tensors = constructTensors(
      bufs_num,
      buf_data,
      buf_ranks,
      buf_dims,
      buf_strides,
      buf_dtypes,
      {{1u, {x_qscale, x_qzero, toQIntType(x_qdtype)}}});
  auto convPackedParams =
      reinterpret_cast<ConvPackedParamsBase<2>*>(buf_data[2]);
  const double out_qscale = ((double*)extra_args)[3];
  const int64_t out_qzero = extra_args[4];
  // NOLINTNEXTLINE
  auto r = convPackedParams->apply_relu(tensors[1], out_qscale, out_qzero);
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

void nnc_aten_quantized_linear(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  const double x_qscale = ((double*)extra_args)[0];
  const int64_t x_qzero = extra_args[1];
  const c10::ScalarType x_qdtype = static_cast<c10::ScalarType>(extra_args[2]);
  auto tensors = constructTensors(
      bufs_num,
      buf_data,
      buf_ranks,
      buf_dims,
      buf_strides,
      buf_dtypes,
      {{1u, {x_qscale, x_qzero, toQIntType(x_qdtype)}}});
  auto linearPackedParams =
      reinterpret_cast<LinearPackedParamsBase*>(buf_data[2]);
  const double out_qscale = ((double*)extra_args)[3];
  const int64_t out_qzero = extra_args[4];
  // NOLINTNEXTLINE
  auto r = linearPackedParams->apply(tensors[1], out_qscale, out_qzero);
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

void nnc_aten_quantized_linear_relu(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  const double x_qscale = ((double*)extra_args)[0];
  const int64_t x_qzero = extra_args[1];
  const c10::ScalarType x_qdtype = static_cast<c10::ScalarType>(extra_args[2]);
  auto tensors = constructTensors(
      bufs_num,
      buf_data,
      buf_ranks,
      buf_dims,
      buf_strides,
      buf_dtypes,
      {{1u, {x_qscale, x_qzero, toQIntType(x_qdtype)}}});
  auto linearPackedParams =
      reinterpret_cast<LinearPackedParamsBase*>(buf_data[2]);
  const double out_qscale = ((double*)extra_args)[3];
  const int64_t out_qzero = extra_args[4];
  // NOLINTNEXTLINE
  auto r = linearPackedParams->apply_relu(tensors[1], out_qscale, out_qzero);
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

#ifndef _WIN32
void nnc_aten_quantized_add(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  // TORCH_INTERNAL_ASSERT(tensors.size() == 3);

  const double a_qscale = ((double*)extra_args)[0];
  const int64_t a_qzero = extra_args[1];
  const c10::ScalarType a_qdtype = static_cast<c10::ScalarType>(extra_args[2]);
  const double b_qscale = ((double*)extra_args)[3];
  const int64_t b_qzero = extra_args[4];
  const c10::ScalarType b_qdtype = static_cast<c10::ScalarType>(extra_args[5]);
  const double out_qscale = ((double*)extra_args)[6];
  const int64_t out_qzero = extra_args[7];
  auto tensors = constructTensors(
      bufs_num,
      buf_data,
      buf_ranks,
      buf_dims,
      buf_strides,
      buf_dtypes,
      {{0u, {out_qscale, out_qzero, toQIntType(a_qdtype)}},
       {1u, {a_qscale, a_qzero, toQIntType(a_qdtype)}},
       {2u, {b_qscale, b_qzero, toQIntType(b_qdtype)}}});
  // NOLINTNEXTLINE
  quantized_add_out(tensors[1], tensors[2], tensors[0]);
}

void nnc_aten_quantized_mul(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  const double a_qscale = ((double*)extra_args)[0];
  const int64_t a_qzero = extra_args[1];
  const c10::ScalarType a_qdtype = static_cast<c10::ScalarType>(extra_args[2]);
  const double b_qscale = ((double*)extra_args)[3];
  const int64_t b_qzero = extra_args[4];
  const c10::ScalarType b_qdtype = static_cast<c10::ScalarType>(extra_args[5]);
  const double out_qscale = ((double*)extra_args)[6];
  const int64_t out_qzero = extra_args[7];
  auto tensors = constructTensors(
      bufs_num,
      buf_data,
      buf_ranks,
      buf_dims,
      buf_strides,
      buf_dtypes,
      {{0u, {out_qscale, out_qzero, toQIntType(a_qdtype)}},
       {1u, {a_qscale, a_qzero, toQIntType(a_qdtype)}},
       {2u, {b_qscale, b_qzero, toQIntType(b_qdtype)}}});
  // NOLINTNEXTLINE
  quantized_mul_out(tensors[1], tensors[2], tensors[0]);
}

void nnc_aten_quantized_mul_scalar(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  const double x_qscale = ((double*)extra_args)[0];
  const int64_t x_qzero = extra_args[1];
  const c10::ScalarType x_qdtype = static_cast<c10::ScalarType>(extra_args[2]);
  auto tensors = constructTensors(
      bufs_num,
      buf_data,
      buf_ranks,
      buf_dims,
      buf_strides,
      buf_dtypes,
      {{0u, {x_qscale, x_qzero, toQIntType(x_qdtype)}},
       {1u, {x_qscale, x_qzero, toQIntType(x_qdtype)}}});
  const double scalar = ((double*)extra_args)[3];
  // NOLINTNEXTLINE
  quantized_mul_scalar_out(tensors[1], scalar, tensors[0]);
}

void nnc_aten_quantized_relu(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  const double x_qscale = ((double*)extra_args)[0];
  const int64_t x_qzero = extra_args[1];
  const c10::ScalarType x_qdtype = static_cast<c10::ScalarType>(extra_args[2]);
  auto tensors = constructTensors(
      bufs_num,
      buf_data,
      buf_ranks,
      buf_dims,
      buf_strides,
      buf_dtypes,
      {{1u, {x_qscale, x_qzero, toQIntType(x_qdtype)}}});
  auto r = at::relu(tensors[1]);
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

void nnc_aten_quantized_sigmoid(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  const double x_qscale = ((double*)extra_args)[0];
  const int64_t x_qzero = extra_args[1];
  const c10::ScalarType x_qdtype = static_cast<c10::ScalarType>(extra_args[2]);
  auto tensors = constructTensors(
      bufs_num,
      buf_data,
      buf_ranks,
      buf_dims,
      buf_strides,
      buf_dtypes,
      {{1u, {x_qscale, x_qzero, toQIntType(x_qdtype)}}});

  // NOLINTNEXTLINE
  auto r = at::sigmoid(tensors[1]);
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

void nnc_aten_quantized_cat(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  std::vector<std::pair<size_t, QIData>> qdata;
  const auto in_bufs_num = bufs_num - 1;
  const double out_qscale = ((double*)extra_args)[3 * in_bufs_num + 1];
  const int64_t out_qzero = extra_args[3 * in_bufs_num + 2];
  qdata.emplace_back(
      0u,
      QIData{
          out_qscale, out_qzero, static_cast<c10::ScalarType>(extra_args[2])});
  for (const size_t i : c10::irange(in_bufs_num)) {
    const double qscale = ((double*)extra_args)[3 * i + 0];
    const int64_t qzero = extra_args[3 * i + 1];
    const c10::ScalarType qdtype =
        static_cast<c10::ScalarType>(extra_args[3 * i + 2]);
    qdata.emplace_back(i + 1u, QIData{qscale, qzero, qdtype});
  }
  auto tensors = constructTensors(
      bufs_num, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes, qdata);
  const int64_t dim = extra_args[3 * in_bufs_num + 0];
  auto qxs = c10::List<at::Tensor>(
      std::vector<at::Tensor>(tensors.begin() + 1, tensors.end()));
  quantized_cat_out(qxs, dim, tensors[0]);
}
#endif // _WIN32

void nnc_aten_upsample_nearest2d(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  // NOLINTNEXTLINE(facebook-hte-LocalUncheckedArrayBounds)
  const double x_qscale = ((double*)extra_args)[0];
  const int64_t x_qzero = extra_args[1];
  const int64_t x_qdtype = extra_args[2];
  const auto is_quantized = x_qdtype != -1;
  c10::optional<std::vector<std::pair<size_t, QIData>>> qdata;
  if (is_quantized) {
    const auto qdtype = at::toQIntType(static_cast<c10::ScalarType>(x_qdtype));
    qdata = {
        {0u, {x_qscale, x_qzero, qdtype}}, {1u, {x_qscale, x_qzero, qdtype}}};
  }
  auto tensors = constructTensors(
      bufs_num, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes, qdata);
  auto x = tensors[1];

  int64_t output_size_h = extra_args[3];
  int64_t output_size_w = extra_args[4];
  double scale_factor_h = ((double*)extra_args)[5];
  double scale_factor_w = ((double*)extra_args)[6];

  c10::optional<at::IntArrayRef> output_size = (output_size_h != -1)
      ? c10::optional<at::IntArrayRef>({output_size_h, output_size_w})
      : c10::nullopt;
  c10::optional<at::ArrayRef<double>> scale_factors = (scale_factor_h != -1.f)
      ? c10::optional<at::ArrayRef<double>>({scale_factor_h, scale_factor_w})
      : c10::nullopt;
  if (is_quantized) {
    quantized_upsample_nearest2d_out(
        tensors[1], output_size, scale_factors, tensors[0]);
    return;
  }
  // TODO: Use out op variant for non-quantized
  auto r = at::upsample_nearest2d(tensors[1], output_size, scale_factors);
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

void nnc_aten_quantize_per_tensor(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  auto tensors = constructTensors(
      bufs_num, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes);
  // NOLINTNEXTLINE(facebook-hte-LocalUncheckedArrayBounds)
  at::Tensor x = tensors[1];
  const double qscale = ((double*)extra_args)[0];
  const int64_t qzero = extra_args[1];
  const c10::ScalarType qdtype = static_cast<c10::ScalarType>(extra_args[2]);
  auto r = at::quantize_per_tensor(x, qscale, qzero, qdtype);
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

void nnc_aten_dequantize(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t,
    int64_t* extra_args) {
  const double qscale = ((double*)extra_args)[0];
  const int64_t qzero = extra_args[1];
  const int64_t qdtype = extra_args[2];
  auto tensors = constructTensors(
      bufs_num,
      buf_data,
      buf_ranks,
      buf_dims,
      buf_strides,
      buf_dtypes,
      {{1u,
        {qscale, qzero, toQIntType(static_cast<c10::ScalarType>(qdtype))}}});
  // NOLINTNEXTLINE
  auto r = at::dequantize(tensors[1]);
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

void nnc_aten_conv1d(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t args_num,
    int64_t* extra_args) {
  auto tensors = constructTensors(
      bufs_num, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes);

  at::Tensor& r = tensors[0];
  const at::Tensor& x = tensors[1];
  const at::Tensor& w = tensors[2];
  if (args_num > 0) {
    // Check that if the extra arguments are provided, then the bias tensor is
    // also present
    TORCH_INTERNAL_ASSERT(args_num == 4 && bufs_num == 4);
    const at::Tensor& b = tensors[3];

    int64_t stride = extra_args[0];
    int64_t padding = extra_args[1];
    int64_t dilation = extra_args[2];
    int64_t groups = extra_args[3];

    try {
      r = at::conv1d(x, w, b, {stride}, {padding}, {dilation}, groups);
    } catch (...) {
    }
  } else {
    try {
      r = at::conv1d(x, w);
    } catch (...) {
    }
  }

  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

void nnc_aten_adaptive_avg_pool2d(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t args_num,
    int64_t* extra_args) {
  auto tensors = constructTensors(
      bufs_num, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes);

  at::Tensor& r = tensors[0];
  const at::Tensor& x = tensors[1];
  int64_t H = extra_args[0];
  int64_t W = H;
  if (args_num > 1) {
    W = extra_args[1];
  }
  try {
    at::adaptive_avg_pool2d_out(r, x, {H, W});
  } catch (...) {
  }
}

void nnc_aten_mean(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t args_num,
    int64_t* extra_args) {
  auto tensors = constructTensors(
      bufs_num, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes);

  at::Tensor& r = tensors[0];
  const at::Tensor& x = tensors[1];
  std::vector<int64_t> mean_dims(args_num - 1);
  bool keepdim = (bool)extra_args[args_num - 1];
  if (args_num > 1) {
    memcpy(mean_dims.data(), extra_args, sizeof(int64_t) * (args_num - 1));
  }
  try {
    at::mean_out(r, x, mean_dims, keepdim);
  } catch (...) {
  }
}

void nnc_aten_max_red(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t args_num,
    int64_t* extra_args) {
  auto tensors = constructTensors(
      bufs_num, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes);

  at::Tensor& r = tensors[0];
  const at::Tensor& x = tensors[1];
  int64_t max_dim = extra_args[0];
  bool keep_dim = extra_args[1];
  try {
    r = std::get<0>(at::max(x, max_dim, keep_dim));
  } catch (...) {
  }
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

void nnc_aten_addmm(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t args_num,
    int64_t* extra_args) {
  auto tensors = constructTensors(
      bufs_num, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes);

  at::Tensor& r = tensors[0];
  const at::Tensor& x = tensors[1];
  const at::Tensor& y = tensors[2];
  const at::Tensor& z = tensors[3];
  // TODO: handle other alpha and beta dtypes, e.g. alpha=0.6, beta=0.2
  int64_t alpha = extra_args[0], beta = extra_args[1];

  try {
    at::addmm_out(r, x, y, z, alpha, beta);
  } catch (...) {
  }
}

// Only provides first output, the second output is just a copy of one of the
// inputs
void nnc_aten_triangular_solve(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t args_num,
    int64_t* extra_args) {
  auto tensors = constructTensors(
      bufs_num, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes);
  at::Tensor& r = tensors[0];
  at::Tensor r2 = tensors[2].clone();
  const at::Tensor& input = tensors[1];
  const at::Tensor& A = tensors[2];
  try {
    at::triangular_solve_out(
        r, r2, input, A, extra_args[0], extra_args[2], extra_args[3]);
  } catch (...) {
  }
}

#ifdef USE_XNNPACK

void nnc_prepacked_linear_clamp_run(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t args_num,
    int64_t* extra_args) {
  using namespace at::native::xnnpack;

  auto tensors = constructTensors(
      bufs_num - 1, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes);

  const at::Tensor& x = tensors[1];
  auto context = reinterpret_cast<LinearOpContext*>(buf_data[2]);
  at::Tensor output = context->run(x);
  memcpy(
      buf_data[0], output.data_ptr(), output.element_size() * output.numel());
}

void nnc_prepacked_conv2d_clamp_run(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t args_num,
    int64_t* extra_args) {
  using namespace at::native::xnnpack;

  auto tensors = constructTensors(
      bufs_num - 1, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes);

  const at::Tensor& x = tensors[1];
  auto context = reinterpret_cast<Conv2dOpContext*>(buf_data[2]);
  at::Tensor output = context->run(x);
  memcpy(
      buf_data[0], output.data_ptr(), output.element_size() * output.numel());
}

#endif // USE_XNNPACK

void nnc_aten_embedding(
    int64_t bufs_num,
    void** buf_data,
    int64_t* buf_ranks,
    int64_t* buf_dims,
    int64_t* buf_strides,
    int8_t* buf_dtypes,
    int64_t args_num,
    int64_t* extra_args) {
  auto tensors = constructTensors(
      bufs_num, buf_data, buf_ranks, buf_dims, buf_strides, buf_dtypes);

  at::Tensor& r = tensors[0];
  const at::Tensor& weight = tensors[1];
  const at::Tensor& indices = tensors[2];
  try {
    r = at::embedding(weight, indices);
  } catch (...) {
  }
  // TODO: have to copy output because at::embedding doesnt have an out
  // variant and NNC's external calls don't support allocations
  memcpy(buf_data[0], r.data_ptr(), r.element_size() * r.numel());
}

#ifndef C10_MOBILE

const static RegisterNNCExternalFunction nnc_conv2d(
    "nnc_aten_conv2d",
    nnc_aten_conv2d);

const static RegisterNNCExternalFunction nnc_quantized_conv1d(
    "nnc_aten_quantized_conv1d",
    nnc_aten_quantized_conv1d);
const static RegisterNNCExternalFunction nnc_quantized_conv2d(
    "nnc_aten_quantized_conv2d",
    nnc_aten_quantized_conv2d);
const static RegisterNNCExternalFunction nnc_quantized_conv2d_relu(
    "nnc_aten_quantized_conv2d_relu",
    nnc_aten_quantized_conv2d_relu);
const static RegisterNNCExternalFunction nnc_quantized_linear(
    "nnc_aten_quantized_linear",
    nnc_aten_quantized_linear);
#ifndef _WIN32
const static RegisterNNCExternalFunction nnc_quantized_add(
    "nnc_aten_quantized_add",
    nnc_aten_quantized_add);
const static RegisterNNCExternalFunction nnc_quantized_mul(
    "nnc_aten_quantized_mul",
    nnc_aten_quantized_mul);
const static RegisterNNCExternalFunction nnc_quantized_mul_scalar(
    "nnc_aten_quantized_mul_scalar",
    nnc_aten_quantized_mul_scalar);
const static RegisterNNCExternalFunction nnc_quantized_sigmoid(
    "nnc_aten_quantized_sigmoid",
    nnc_aten_quantized_sigmoid);
const static RegisterNNCExternalFunction nnc_quantized_cat(
    "nnc_aten_quantized_cat",
    nnc_aten_quantized_cat);
const static RegisterNNCExternalFunction nnc_quantized_relu(
    "nnc_aten_quantized_relu",
    nnc_aten_quantized_relu);
#endif // _WIN32
const static RegisterNNCExternalFunction nnc_quantize_per_tensor(
    "nnc_aten_quantize_per_tensor",
    nnc_aten_quantize_per_tensor);
const static RegisterNNCExternalFunction nnc_dequantize(
    "nnc_aten_dequantize",
    nnc_aten_dequantize);

const static RegisterNNCExternalFunction nnc_upsample_nearest2d(
    "nnc_aten_upsample_nearest2d",
    nnc_aten_upsample_nearest2d);
const static RegisterNNCExternalFunction nnc_conv1d(
    "nnc_aten_conv1d",
    nnc_aten_conv1d);
const static RegisterNNCExternalFunction nnc_adaptive_avg_pool2d(
    "nnc_aten_adaptive_avg_pool2d",
    nnc_aten_adaptive_avg_pool2d);
const static RegisterNNCExternalFunction nnc_mean(
    "nnc_aten_mean",
    nnc_aten_mean);
const static RegisterNNCExternalFunction nnc_max_red(
    "nnc_aten_max_red",
    nnc_aten_max_red);
const static RegisterNNCExternalFunction nnc_addmm(
    "nnc_aten_addmm",
    nnc_aten_addmm);

const static RegisterNNCExternalFunction nnc_triangular_solve(
    "nnc_aten_triangular_solve",
    nnc_aten_triangular_solve);

const static RegisterNNCExternalFunction nnc_embedding(
    "nnc_aten_embedding",
    nnc_aten_embedding);

#ifdef USE_XNNPACK
const static RegisterNNCExternalFunction reg_nnc_prepacked_linear_clamp_run(
    "nnc_prepacked_linear_clamp_run",
    nnc_prepacked_linear_clamp_run);
const static RegisterNNCExternalFunction reg_nnc_prepacked_conv2d_clamp_run(
    "nnc_prepacked_conv2d_clamp_run",
    nnc_prepacked_conv2d_clamp_run);
#endif // USE_XNNPACK

#endif // C10_MOBILE

#ifdef C10_MOBILE
} // extern "C"
#endif

} // namespace tensorexpr
} // namespace jit
} // namespace torch
