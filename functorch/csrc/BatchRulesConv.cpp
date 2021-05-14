#include <functorch/csrc/BatchRulesHelper.h>

namespace at { namespace functorch {

// batching rules translated from jax: https://github.com/google/jax/blob/master/jax/_src/lax/lax.py#L3143

// Does not support batch_group_count (needed for convolution backwards)
std::tuple<Tensor,optional<int64_t>>
conv2d_batching_rule(const Tensor& lhs, optional<int64_t> lhs_bdim, const Tensor& rhs, optional<int64_t> rhs_bdim, const optional<Tensor>& bias, optional<int64_t> bias_bdim, IntArrayRef stride, IntArrayRef padding, IntArrayRef dilation, int64_t groups) {
  std::vector<int64_t> lhs_spec = {0,1,2,3};
  std::vector<int64_t> rhs_spec = {0,1,2,3};
  std::vector<int64_t> out_spec = {0,1,2,3};

  // If we have a batched bias or weight, we need to perform the computation separately.
  optional<Tensor> unbatched_bias;
  bool separate_bias;
  if ((rhs_bdim && bias) || bias_bdim) {
    TORCH_INTERNAL_ASSERT(bias.has_value());
    unbatched_bias = nullopt;
    separate_bias = true;
  } else {
    unbatched_bias = bias;
    separate_bias = false;
  }
  std::tuple<Tensor, optional<int64_t>> result;
  if (lhs_bdim && !rhs_bdim) {
    auto new_x = reshape_dim_into(*lhs_bdim, lhs_spec[0], lhs);
    auto out = at::conv2d(new_x, rhs, unbatched_bias, stride, padding, dilation, groups);
    out = reshape_dim_outof(out_spec[0], lhs.sizes()[*lhs_bdim], out);
    result = {out, out_spec[0]};
  } else if (!lhs_bdim && rhs_bdim) {
    if (groups == 1) {
      auto new_w = reshape_dim_into(*rhs_bdim, rhs_spec[0], rhs);
      auto out = at::conv2d(lhs, new_w, unbatched_bias, stride, padding, dilation, groups);
      out = reshape_dim_outof(out_spec[1], rhs.sizes()[*rhs_bdim], out);
      result =  {out, out_spec[1]};
    } else {
      auto new_w = reshape_dim_outof(rhs_spec[0] + (*rhs_bdim <= rhs_spec[0]), groups, rhs);
      new_w = reshape_dim_into(*rhs_bdim + (rhs_spec[0] < rhs_bdim), rhs_spec[0] + 1, new_w);
      new_w = reshape_dim_into(rhs_spec[0], rhs_spec[0], new_w);
      auto out = at::conv2d(lhs, new_w, unbatched_bias, stride, padding, dilation, groups);
      out = reshape_dim_outof(out_spec[1], groups, out);
      out = reshape_dim_outof(out_spec[1] + 1, rhs.sizes()[*rhs_bdim], out);
      out = reshape_dim_into(out_spec[1], out_spec[1] + 1, out);
      result = {out, out_spec[1]};
    }
  } else if (lhs_bdim && rhs_bdim) {
    auto new_x = reshape_dim_into(*lhs_bdim, lhs_spec[1], lhs);
    groups *= lhs.sizes()[*lhs_bdim];
    auto new_w = reshape_dim_into(*rhs_bdim, rhs_spec[0], rhs);
    auto out = at::conv2d(new_x, new_w, unbatched_bias, stride, padding, dilation, groups);
    out = reshape_dim_outof(out_spec[1], lhs.sizes()[*lhs_bdim], out);
    result = {out, out_spec[1]};
  } else {
    result = {at::conv2d(lhs, rhs, unbatched_bias, stride, padding, dilation, groups), nullopt};
  }
  if (separate_bias) {
    auto A = std::get<0>(result);
    auto A_batch_dim = std::get<1>(result);
    auto B = *bias;
    auto B_batch_dim = bias_bdim;
    A = moveBatchDimToFront(A, A_batch_dim);
    B = moveBatchDimToFront(B, B_batch_dim);
    for (int i = 0; i < out_spec.size() - 2; i++) {
      B = B.unsqueeze(-1);
    }
    B = maybePadToLogicalRank(B, B_batch_dim, rankWithoutBatchDim(A, A_batch_dim));

    return {at::add(A, B), 0};
  } else {
    return result;
  }
}


TORCH_LIBRARY_IMPL(aten, FT_BATCHED_KEY, m) {
  VMAP_SUPPORT("conv2d", conv2d_batching_rule);
}
}}
