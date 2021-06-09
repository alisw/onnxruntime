// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "orttraining/training_ops/cuda/nn/batch_norm_grad.h"
#include "core/providers/common.h"
#include "core/providers/cuda/cudnn_common.h"
#include "core/providers/cpu/nn/batch_norm_helper.h"
#include "core/providers/cuda/math/unary_elementwise_ops_impl.h"

using namespace std;
namespace onnxruntime {
namespace cuda {

#define REGISTER_GRADIENT_KERNEL_TYPED(T, U)                                               \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                                           \
      BatchNormalizationGrad,                                                              \
      kMSDomain,                                                                           \
      1,                                                                                   \
      T##_##U,                                                                             \
      kCudaExecutionProvider,                                                              \
      (*KernelDefBuilder::Create()).TypeConstraint("T", DataTypeImpl::GetTensorType<T>())  \
                                   .TypeConstraint("U", DataTypeImpl::GetTensorType<U>()), \
      BatchNormalizationGrad<T, U>);

template <typename T, typename U>
Status BatchNormalizationGrad<T, U>::ComputeInternal(OpKernelContext* ctx) const {
  typedef typename ToCudaType<T>::MappedType CudaT;
  typedef typename ToCudaType<U>::MappedType CudaU;

  const Tensor* dY = ctx->Input<Tensor>(0);
  const Tensor* X = ctx->Input<Tensor>(1);
  const Tensor* Scale = ctx->Input<Tensor>(2);
  const Tensor* saved_mean = ctx->Input<Tensor>(3);
  const Tensor* saved_inv_std = ctx->Input<Tensor>(4);
  const TensorShape input_shape = X->Shape();
  const TensorShape channel_shape = saved_mean->Shape();

  // no B here, but B has same size as Scale, so can validate inputs for gradient with this substitute
  ORT_RETURN_IF_ERROR(BatchNormHelper::ValidateInputs(X, Scale, Scale, saved_mean, saved_inv_std));

  auto dY_data = reinterpret_cast<const CudaT*>(dY->template Data<T>());
  auto X_data = reinterpret_cast<const CudaT*>(X->template Data<T>());
  auto Scale_data = reinterpret_cast<const CudaT*>(Scale->template Data<T>());
  auto saved_mean_data = reinterpret_cast<const CudaU*>(saved_mean->template Data<U>());
  auto saved_inv_std_data = reinterpret_cast<const CudaU*>(saved_inv_std->template Data<U>());

  auto dX_data = reinterpret_cast<CudaT*>(ctx->Output(0, input_shape)->template MutableData<T>());
  auto dScale_data = reinterpret_cast<CudaT*>(ctx->Output(1, channel_shape)->template MutableData<T>());
  auto dBias_data = reinterpret_cast<CudaT*>(ctx->Output(2, channel_shape)->template MutableData<T>());

  const auto alpha = std::is_same<T, MLFloat16>::value ? Consts<float>::One : Consts<CudaT>::One;
  const auto beta = std::is_same<T, MLFloat16>::value ? Consts<float>::Zero : Consts<CudaT>::Zero;

  CudnnTensor input_tensor, scale_bias_tensor;
  vector<int64_t> new_dims;
  BatchNormHelper::NormalizeDims(input_shape, new_dims);
  ORT_RETURN_IF_ERROR(input_tensor.Set(new_dims, CudnnTensor::GetDataType<CudaT>()));
  // for fp16 input, `scale_bias_tensor` will have a float type; otherwise it will be the same as input type.
  ORT_RETURN_IF_ERROR(scale_bias_tensor.Set(input_tensor, cudnn_batch_norm_mode_));

  const int64_t C = input_shape.GetDims()[1];
  auto p_scale = reinterpret_cast<const void*>(Scale_data);
  auto p_saved_mean = reinterpret_cast<const void*>(saved_mean_data);
  auto p_saved_inv_std = reinterpret_cast<const void*>(saved_inv_std_data);
  auto p_dScale = reinterpret_cast<void*>(dScale_data);
  auto p_dBias = reinterpret_cast<void*>(dBias_data);

  IAllocatorUniquePtr<float> p_f_scale, p_f_dScale, p_f_dBias, p_f_saved_mean, p_f_saved_inv_std;

  if (std::is_same<T, MLFloat16>::value) {
    p_f_scale = GetScratchBuffer<float>(C);
    p_f_dScale = GetScratchBuffer<float>(C);
    p_f_dBias = GetScratchBuffer<float>(C);

    Impl_Cast<CudaT, float>(Stream(), Scale_data, p_f_scale.get(), C);

    p_scale = p_f_scale.get();
    p_dScale = p_f_dScale.get();
    p_dBias = p_f_dBias.get();
  }

  if (std::is_same<U, MLFloat16>::value) {
    p_f_saved_mean = GetScratchBuffer<float>(C);
    p_f_saved_inv_std = GetScratchBuffer<float>(C);

    Impl_Cast<CudaU, float>(Stream(), saved_mean_data, p_f_saved_mean.get(), C);
    Impl_Cast<CudaU, float>(Stream(), saved_inv_std_data, p_f_saved_inv_std.get(), C);

    p_saved_mean = p_f_saved_mean.get();
    p_saved_inv_std = p_f_saved_inv_std.get();
  }

  CUDNN_RETURN_IF_ERROR(cudnnBatchNormalizationBackward(
      CudnnHandle(),
      cudnn_batch_norm_mode_,
      &alpha,
      &beta,
      &alpha,
      &beta,
      input_tensor,
      X_data,
      input_tensor,
      dY_data,
      input_tensor,
      dX_data,
      scale_bias_tensor,
      p_scale,
      p_dScale,
      p_dBias,
      epsilon_,
      p_saved_mean,
      p_saved_inv_std));

  if (std::is_same<T, MLFloat16>::value) {
    Impl_Cast<float, CudaT>(Stream(), reinterpret_cast<float*>(p_dScale), dScale_data, C);
    Impl_Cast<float, CudaT>(Stream(), reinterpret_cast<float*>(p_dBias), dBias_data, C);
  }

  return Status::OK();
}

#define SPECIALIZED_GRADIENT(T, U)     \
  REGISTER_GRADIENT_KERNEL_TYPED(T, U) \
  template Status BatchNormalizationGrad<T, U>::ComputeInternal(OpKernelContext* ctx) const;

SPECIALIZED_GRADIENT(float, float)
SPECIALIZED_GRADIENT(double, double)
SPECIALIZED_GRADIENT(MLFloat16, MLFloat16)
SPECIALIZED_GRADIENT(MLFloat16, float)

}  // namespace cuda
}  // namespace onnxruntime
