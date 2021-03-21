#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>

#include <torch/library.h>

namespace at { namespace native {

void checkLongTensor(const Tensor& tensor) {
  TORCH_CHECK(tensor.dim() == 1 && tensor.device().type() == at::kCPU && tensor.scalar_type() == at::kLong,
           "'lengths' argument should be a 1D CPU int64 tensor, but got ",
            tensor.dim(), "D ", tensor.device().str(), " ", tensor.scalar_type(), " tensor");
}

// This method returns `(data, batch_sizes)`, which are then passed into a
// `PackedSequence` constructor.
// `data` can be on arbitrary device and of arbitrary dtype, but `batch_sizes`
// must be a CPU int64 tensor.
// See NOTE [ device and dtype of a PackedSequence ]
std::tuple<Tensor, Tensor> _pack_padded_sequence(const Tensor& _input, const Tensor& _lengths, bool batch_first) {
  auto input = batch_first ? _input.transpose(0, 1) : _input;
  auto lengths_t = _lengths.contiguous();
  checkLongTensor(lengths_t);

  int64_t batch_size = input.size(1);
  int64_t * lengths = lengths_t.data_ptr<int64_t>();
  TORCH_CHECK(input.numel() > 0, "Cannot pack empty tensors.");
  TORCH_CHECK(lengths_t.size(0) == batch_size,
           "Expected `len(lengths)` to be equal to batch_size, but got ", lengths_t.size(0),
           " (batch_size=", batch_size, ")");
  TORCH_CHECK(lengths[batch_size - 1] > 0,
           "Length of all samples has to be greater than 0, but found an element "
           "in 'lengths' that is <= 0");
  for(auto i = 0; i < batch_size - 1; i++) {
    if (lengths[batch_size - 1 - i] > lengths[batch_size - 2 - i]) {
      // NB: enforce_sorted is implemented at a Python level, but the sortedness
      // check lives here. If enforce_sorted=False then this error should never
      // get called.
      AT_ERROR("`lengths` array must be sorted in decreasing order when "
               "`enforce_sorted` is True. You can pass `enforce_sorted=False` "
               "to pack_padded_sequence and/or pack_sequence to sidestep this "
               "requirement if you do not need ONNX exportability.");
    }
  }

  std::vector<at::Tensor> steps;
  steps.reserve(batch_size);
  at::Tensor batch_sizes_t = at::empty(lengths[0], _lengths.options());
  int64_t * batch_sizes = batch_sizes_t.data_ptr<int64_t>();

  std::vector<int64_t> step_shape; // == [-1, *input.shape[2:]]
  {
    auto input_sizes = input.sizes();
    step_shape.reserve(input_sizes.size());
    auto s_input_sizes = input_sizes.slice(2);
    step_shape.push_back(-1);
    step_shape.insert(step_shape.end(), s_input_sizes.begin(), s_input_sizes.end());
  }

  // To understand what's going on in this loop imagine that the input is a padded 2D
  // array that looks like this (x = valid entry, . = padding)
  //
  //  1 1 1 1 1
  //  2 2 2 . .
  //  2 2 2 . .
  //  4 . . . .
  //  4 . . . .
  //
  // Where the vertical dimension corresponds to time, and horizontal dim to batch.
  // In this example, the lengths array will be equal to [5, 3, 3, 1, 1], and we will
  // iterate over them in reverse order (from the rightmost column to the left).
  // We want to avoid eager slicing of the input at every time step, and wait for
  // the moments where the length increases. In this example, that will happen at the
  // first, second and fourth steps. Then, we slice out the whole block of the input
  // that corresponds to this length, and hasn't been sliced yet (the steps at which each
  // element is sliced are annotated in the array above).  You can think of this as if we
  // were scanning the sequences from the shortest one, and every time we realize there's
  // more elements below in our column, we lower the counter (prev_l), and append the new
  // block to the output.
  int64_t prev_l = 0;
  for (int64_t i = 0; i < batch_size; ++i) {
    int64_t l = lengths[batch_size - 1 - i];
    if (l > prev_l) {
      auto current_batch_size = batch_size - i;
      steps.push_back(input.slice(0, prev_l, l).slice(1, 0, current_batch_size).contiguous().view(step_shape));
      for (int64_t j = 0; j < (l - prev_l); ++j) {
        (*batch_sizes++) = current_batch_size;
      }
      prev_l = l;
    }
    TORCH_CHECK(l >= prev_l);
  }

  return std::make_tuple(at::cat(steps), batch_sizes_t);
}

// `grad` could be on arbitrary device and of arbitrary dtype, but `_batch_sizes`
// is guaranteed to be a CPU int64 tensor.
// See NOTE [ device and dtype of a PackedSequence ]
Tensor _pack_padded_sequence_backward(const Tensor& grad, at::IntArrayRef input_size, const Tensor& _batch_sizes, bool batch_first) {
  std::vector<int64_t> input_size_after_t = input_size.vec();
  if (batch_first) {
    TORCH_CHECK(input_size.size() >= 2);
    std::swap(input_size_after_t[0], input_size_after_t[1]);
  }
  auto grad_input = at::zeros(input_size_after_t, grad.options());
  auto batch_sizes_t = _batch_sizes.contiguous();
  checkLongTensor(batch_sizes_t);

  int64_t offset = 0;
  int64_t max_seq_len = batch_sizes_t.size(0);
  int64_t * batch_sizes = batch_sizes_t.data_ptr<int64_t>();
  for (int64_t i = 0; i < max_seq_len; ++i) {
    grad_input[i].slice(0, 0, batch_sizes[i]).copy_(grad.slice(0, offset, offset + batch_sizes[i]));
    offset += batch_sizes[i];
  }

  if (batch_first) {
    grad_input = grad_input.transpose(0, 1);
  }

  return grad_input;
}

std::tuple<Tensor, Tensor> _pad_packed_sequence(const Tensor& data, const Tensor& _batch_sizes, bool batch_first, const Scalar& padding_value, int64_t total_length) {
  auto batch_sizes_t = _batch_sizes.contiguous();
  checkLongTensor(batch_sizes_t);

  int64_t * batch_sizes = batch_sizes_t.data_ptr<int64_t>();
  int64_t max_batch_size = batch_sizes[0];
  int64_t max_real_seq_length = batch_sizes_t.size(0);
  int64_t max_seq_length = max_real_seq_length;
  if (total_length > 0) {
    TORCH_CHECK(total_length >= max_seq_length,
             "Expected total_length to be at least the length of the longest "
             "sequence in input, but got total_length=", total_length, " and "
             "max sequence length being ", max_seq_length);
    max_seq_length = total_length;
  }

  std::vector<int64_t> output_size; // == [max_seq_length, max_batch_size, *var_data.size()[1:]]
  {
    output_size.reserve(data.dim() + 1);
    output_size.push_back(max_seq_length);
    output_size.push_back(max_batch_size);
    auto s_data_size = data.sizes().slice(1);
    output_size.insert(output_size.end(), s_data_size.begin(), s_data_size.end());
  }
  auto output = at::full(output_size, padding_value, data.options());

  // This will be modified at every iteration, but we reserve memory for it now.
  std::vector<int64_t> tmp_view_size = std::move(output_size); // == [-1, -1, *var_data.size()[1:]]

  at::Tensor lengths_t = at::empty(max_batch_size, batch_sizes_t.options());
  int64_t * lengths = lengths_t.data_ptr<int64_t>() + max_batch_size - 1;
  int64_t data_offset = 0;
  int64_t prev_batch_size = max_batch_size;
  int64_t prev_i = 0;
  for (int64_t i = 0; i <= max_real_seq_length; ++i) {
    int64_t batch_size = i != max_real_seq_length ? batch_sizes[i] : 0;
    if (batch_size != prev_batch_size) {
      int64_t l = prev_batch_size * (i - prev_i);
      // The lines below are equivalent to this:
      // output[prev_i:i, :prev_batch_size] = tmp.view(i - prev_i, prev_batch_size, *input.shape[2:])
      auto tmp = data.slice(0, data_offset, data_offset + l);
      tmp_view_size[0] = i - prev_i;
      tmp_view_size[1] = prev_batch_size;
      output.slice(0, prev_i, i).slice(1, 0, prev_batch_size).copy_(tmp.view(tmp_view_size));
      data_offset += l;
      prev_i = i;
    }
    int64_t dec = prev_batch_size - batch_size;
    if (dec > 0) {
      for (int64_t j = 0; j < dec; ++j) {
        (*lengths--) = i;
      }
    }
    prev_batch_size = batch_size;
  }

  if (batch_first) {
    output = output.transpose(0, 1);
  }

  return std::make_tuple(output, lengths_t);
}


std::tuple<Tensor, Tensor> ragged_indices_to_padded_tensor_and_mask(
    const std::vector<std::vector<int64_t>>& indices,
    int64_t pad_value,
    int64_t max_batch_len,
    int64_t max_seq_len) {
  // TODO: potential optimization: if there is a reliable way to allocate a
  // tensor with storage backed by zero-scrubbed pages, then we can use that
  // in the case that `pad_value` = 0 (common case). Then we don't have to
  // touch the padded entries, as the OS has done that for us already.
  // Similarly for the mask tensor we would only have to touch the
  // elements that are past the end of each sequence.
  TORCH_CHECK(indices.size() >= max_batch_len);
  Tensor padded_tensor = at::empty({max_batch_len, max_seq_len}, at::kLong);
  Tensor mask_tensor = at::empty({max_batch_len, max_seq_len}, at::kLong);

  int64_t *padded_buf = padded_tensor.data_ptr<int64_t>();
  int64_t *mask_buf = mask_tensor.data_ptr<int64_t>();

  for (size_t i = 0; i < max_batch_len; ++i) {
    const std::vector<int64_t>& sequence = indices[i];
    int64_t *padded_row_ptr = padded_buf + max_seq_len * i;
    int64_t *mask_row_ptr = mask_buf + max_seq_len * i;
    int64_t numel_to_copy = std::min(static_cast<int64_t>(sequence.size()), max_seq_len);

    // Beg the dragon for autovectorization
    size_t j = 0;
    for (; j < numel_to_copy; ++j) {
      padded_row_ptr[j] = sequence[j];
      mask_row_ptr[j] = static_cast<int64_t>(1);
    }

    // Now write out remainder values. padded_buf gets `pad_value`
    // values and mask gets 0's
    for (; j < max_seq_len; ++j) {
      padded_row_ptr[j] = pad_value;
      mask_row_ptr[j] = static_cast<int64_t>(0);
    }
  }

  return std::tuple<Tensor, Tensor>{padded_tensor, mask_tensor};
}

TORCH_LIBRARY(padded, m) {
  m.def("ragged_indices_to_padded_tensor_and_mask", ragged_indices_to_padded_tensor_and_mask);
}

}} // namespace at::native
