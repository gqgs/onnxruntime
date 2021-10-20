// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#if !defined(DISABLE_SPARSE_TENSORS)

#include "core/framework/data_transfer_manager.h"
#include "core/framework/element_type_lists.h"
#include "core/framework/op_kernel.h"
#include "core/framework/sparse_tensor.h"
#include "core/framework/sparse_utils.h"
#include "core/framework/tensor.h"

namespace onnxruntime {
namespace contrib {

class Add final : public OpKernel {
 public:
  explicit Add(const OpKernelInfo& info) : OpKernel(info) {
  }

  Status Compute(OpKernelContext*) const override;
};

ONNX_OPERATOR_KERNEL_EX(
    Add,
    kMSDomain,
    1,
    kCpuExecutionProvider,
    KernelDefBuilder()
        .TypeConstraint("T", BuildKernelDefSparseConstraintsFromTypeList<element_type_lists::AllFixedSizeExceptHalf>()),
    Add);

namespace {

// Can't use SparseTensor::Copy because dense shapes may not match
Status CopySparseToOutput(const SparseTensor& input, SparseTensor& output) {
  const auto& indices = input.AsCoo().Indices();
  const auto output_ndims = output.DenseShape().GetDims().size();
  // if we had 2-D indices and the output shape is bigger than 2-D then
  // we convert to flat indices bc 2-D indices only apply to 2-D tensors
  const auto ind_ndims = indices.Shape().NumDimensions();
  if (ind_ndims == 2 && output_ndims > ind_ndims) {
    const auto indices_size = gsl::narrow<size_t>(indices.Shape().Size());
    ORT_RETURN_IF_NOT(indices_size % 2, "Indices size must be divisible by 2");
    const auto new_indices_size = indices_size / 2;
    auto coo_mutator = output.MakeCooData(input.NumValues(), new_indices_size);
    ORT_RETURN_IF_ERROR(sparse_utils::ConvertIndicesTo1DAndCopy(input, coo_mutator));
    sparse_utils::CopyCpuTensor(input.Values(), coo_mutator.Values());
  } else {
    sparse_utils::CopyCpuSparseCooTensor(input, output);
  }
  return Status::OK();
}

template <typename T>
struct Sum {
  void operator()(const void* lhs, size_t lhs_idx, const void* rhs, size_t rhs_idx, void* result, size_t res_idx) const {
    reinterpret_cast<T*>(result)[res_idx] = reinterpret_cast<const T*>(lhs)[lhs_idx] + reinterpret_cast<const T*>(rhs)[rhs_idx];
  }
};
}  // namespace

Status Add::Compute(OpKernelContext* ctx) const {
  const SparseTensor& input_A = *ctx->Input<SparseTensor>(0);
  const SparseTensor& input_B = *ctx->Input<SparseTensor>(1);

  auto A_dims = input_A.DenseShape().GetDims();
  auto B_dims = input_B.DenseShape().GetDims();
  ORT_RETURN_IF_NOT(!A_dims.empty() && !B_dims.empty(), "Unable to handle empty shapes");

  const auto output_ndims = std::max(A_dims.size(), B_dims.size());
  if (A_dims.size() < output_ndims) {
    A_dims.insert(A_dims.begin(), (output_ndims - A_dims.size()), 1);
  }

  if (B_dims.size() < output_ndims) {
    B_dims.insert(B_dims.begin(), (output_ndims - B_dims.size()), 1);
  }

  assert(A_dims.size() == B_dims.size());

  std::vector<int64_t> output_dims;
  for (size_t i = 0; i < output_ndims; ++i) {
    output_dims.push_back(std::max(A_dims[i], B_dims[i]));
  }
  TensorShape output_shape(std::move(output_dims));
  SparseTensor& output = *ctx->OutputSparse(0, output_shape);

  // Handle fully sparse cases
  if (input_A.NumValues() == 0 && input_B.NumValues() == 0) {
    ORT_UNUSED_PARAMETER(output.MakeCooData(0, 0));
  } else {
    if (input_A.NumValues() == 0) {
      ORT_RETURN_IF_ERROR(CopySparseToOutput(input_B, output));
    } else if (input_B.NumValues() == 0) {
      ORT_RETURN_IF_ERROR(CopySparseToOutput(input_A, output));
    } else {
      // Neither of indices are empty
      sparse_utils::IndicesSpan A_coo_indices;
      ORT_RETURN_IF_ERROR(sparse_utils::GetCoo1DIndicesAndMaybeConvert(input_A, A_coo_indices));
      const auto& A_ind_span = A_coo_indices.Get();

      sparse_utils::IndicesSpan B_coo_indices;
      ORT_RETURN_IF_ERROR(sparse_utils::GetCoo1DIndicesAndMaybeConvert(input_B, B_coo_indices));
      const auto& B_ind_span = B_coo_indices.Get();

      enum Source {
        kInputA = 1,
        kInputB = 2,
        kInputSum = 3
      };

      std::vector<std::tuple<int64_t, Source>> result_indices;
      result_indices.reserve(A_ind_span.size() + B_ind_span.size());
      size_t a_ind = 0;
      size_t b_ind = 0;

      const auto a_span_size = A_ind_span.size();
      const auto b_span_size = B_ind_span.size();
      while (a_ind < a_span_size || b_ind < b_span_size) {
        if (a_ind < a_span_size && b_ind < b_span_size) {
          auto a_val = A_ind_span[a_ind];
          auto b_val = B_ind_span[b_ind];
          if (a_val == b_val) {
            result_indices.push_back(std::make_tuple(a_val, kInputSum));
            a_ind++;
            b_ind++;
          } else if (a_val < b_val) {
            result_indices.push_back(std::make_tuple(A_ind_span[a_ind++], kInputA));
          } else {
            result_indices.push_back(std::make_tuple(B_ind_span[b_ind++], kInputB));
          }
        } else if (a_ind < a_span_size) {
          result_indices.push_back(std::make_tuple(A_ind_span[a_ind++], kInputA));
        } else {
          result_indices.push_back(std::make_tuple(B_ind_span[b_ind++], kInputB));
        }
      }

      const auto element_size = input_A.DataType()->AsPrimitiveDataType()->Size();
      sparse_utils::CopyElementFunc copy_func;
      switch (element_size) {
        case 1: {
          copy_func = sparse_utils::CopyElementAligned<uint8_t>;
          break;
        }
        case 2: {
          copy_func = sparse_utils::CopyElementAligned<uint16_t>;
          break;
        }
        case 4: {
          copy_func = sparse_utils::CopyElementAligned<uint32_t>;
          break;
        }
        case 8: {
          copy_func = sparse_utils::CopyElementAligned<uint64_t>;
          break;
        }
        default:
          return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL,
                                 "Element_size of: ", element_size,
                                 " is not supported.", " data_type: ", input_A.GetElementType());
      }

      const auto* values_A = input_A.Values().DataRaw();
      size_t a_src_idx = 0;
      const auto* values_B = input_B.Values().DataRaw();
      size_t b_src_idx = 0;
      auto coo_mutator = output.MakeCooData(result_indices.size(), result_indices.size());
      auto* output_values = coo_mutator.Values().MutableDataRaw();
      size_t dst_idx = 0;
      auto* output_indices = coo_mutator.Indices().MutableData<int64_t>();
      utils::MLTypeCallDispatcherFromTypeList<element_type_lists::AllFixedSizeExceptHalf>
          t_disp(input_A.GetElementType());
      for (const auto& e : result_indices) {
        auto where = std::get<1>(e);
        switch (where) {
          case kInputA:
            copy_func(output_values, values_A, dst_idx, a_src_idx);
            ++dst_idx;
            ++a_src_idx;
            break;
          case kInputB:
            copy_func(output_values, values_B, dst_idx, b_src_idx);
            ++dst_idx;
            ++b_src_idx;
            break;
          case kInputSum:
            t_disp.Invoke<Sum>(values_A, a_src_idx, values_B, b_src_idx, output_values, dst_idx);
            ++a_src_idx;
            ++b_src_idx;
            ++dst_idx;
            break;
          default:
            ORT_THROW("Unhandled switch for Add");
        }
        *output_indices++ = std::get<0>(e);
      }
    }
  }
  return Status::OK();
}

}  // namespace contrib
}  // namespace onnxruntime

#endif  //  !defined(DISABLE_SPARSE_TENSORS)