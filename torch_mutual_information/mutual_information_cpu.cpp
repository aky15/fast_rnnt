#include <math.h>  // for log1p, log1pf
#include <torch/extension.h>


inline double Exp(double x) {
  return exp(x);
}
inline double Exp(float x) {
  return expf(x);
}

// returns log(exp(x) + exp(y)).
inline double LogAdd(double x, double y) {
  double diff;

  if (x < y) {
    diff = x - y;
    x = y;
  } else {
    diff = y - x;
  }
  // diff is negative.  x is now the larger one.
  if (diff >= -1000) {
    double res;
    res = x + log1p(exp(diff));
    return res;
  }

  return x;  // return the larger one.
}

// returns log(exp(x) + exp(y)).
inline float LogAdd(float x, float y) {
  float diff;

  if (x < y) {
    diff = x - y;
    x = y;
  } else {
    diff = y - x;
  }
  // diff is negative.  x is now the larger one.
  if (diff >= -200) {
    float res;
    res = x + log1pf(expf(diff));
    return res;
  }
  return x;  // return the larger one.
}


// forward of mutual_information.  See """... """ comment of `mutual_information` in
// mutual_information.py for documentation of the behavior of this function.
// px: of shape [B, S, T+1] where
torch::Tensor mutual_information_cpu(torch::Tensor px,
                                     torch::Tensor py,
                                     torch::Tensor boundary,
                                     torch::Tensor p) {
  TORCH_CHECK(px.dim() == 3, "px must be 3-dimensional");
  TORCH_CHECK(py.dim() == 3, "py must be 3-dimensional.");
  TORCH_CHECK(p.dim() == 3, "p must be 3-dimensional.");
  TORCH_CHECK(boundary.dim() == 2, "boundary must be 2-dimensional.");
  TORCH_CHECK(px.device().is_cpu() && py.device().is_cpu() && p.device().is_cpu(),
              "inputs must be CPU tensors");

  auto scalar_t = px.scalar_type();
  auto opts = torch::TensorOptions().dtype(scalar_t).device(px.device());

  const int B = px.size(0),
      S = px.size(1),
      T = px.size(2) - 1;
  TORCH_CHECK(py.size(0) == B && py.size(1) == S + 1 && py.size(2) == T);
  TORCH_CHECK(p.size(0) == B && p.size(1) == S + 1 && p.size(2) == T + 1);
  TORCH_CHECK((boundary.size(0) == 0 && boundary.size(1) == 0) ||
              (boundary.size(0) == B && boundary.size(1) == 4));
  TORCH_CHECK(boundary.device().is_cpu() &&
              boundary.dtype() == torch::kInt64);

  torch::Tensor ans = torch::empty({B}, opts);

  bool has_boundary = (boundary.size(0) != 0);


  AT_DISPATCH_FLOATING_TYPES(px.scalar_type(), "mutual_information_cpu_loop", ([&] {
        auto px_a = px.packed_accessor32<scalar_t, 3>(),
            py_a = py.packed_accessor32<scalar_t, 3>(),
            p_a = p.packed_accessor32<scalar_t, 3>();
        auto boundary_a = boundary.packed_accessor32<int64_t, 2>();
        auto ans_a = ans.packed_accessor32<scalar_t, 1>();

        for (int b = 0; b < B; b++) {
          int s_begin, s_end, t_begin, t_end;
          if (has_boundary) {
            s_begin = boundary_a[b][0];
            t_begin = boundary_a[b][1];
            s_end = boundary_a[b][2];
            t_end = boundary_a[b][3];
          } else {
            s_begin = 0;
            s_end = S;
            t_begin = 0;
            t_end = T;
          }
          p_a[b][s_begin][t_begin] = 0.0;
          for (int s = s_begin + 1; s <= s_end; ++s)
            p_a[b][s][t_begin] = p_a[b][s - 1][t_begin] + px_a[b][s - 1][t_begin];
          for (int t = t_begin + 1; t <= t_end; ++t)
            p_a[b][s_begin][t] = p_a[b][s_begin][t - 1] + py_a[b][s_begin][t - 1];
          for (int s = s_begin + 1; s <= s_end; ++s) {
            scalar_t p_s_t1 = p_a[b][s][t_begin];
            for (int t = t_begin + 1; t <= t_end; ++t) {
              // The following statement is a small optimization of:
              // p_a[b][s][t] = LogAdd(p_a[b][s - 1][t] + px_a[b][s - 1][t],
              //                       p_a[b][s][t - 1] + py_a[b][s][t - 1]);
              // .. which obtains p_a[b][s][t - 1] from a register.
              p_a[b][s][t] = p_s_t1 = LogAdd(p_a[b][s - 1][t] + px_a[b][s - 1][t],
                                             p_s_t1 + py_a[b][s][t - 1]);
            }
          }
          ans_a[b] = p_a[b][s_end][t_end];
        }
      }));
  return ans;
}


// backward of mutual_information.  Returns (px_grad, py_grad).
// p corresponds to what we computed in the forward pass.
std::vector<torch::Tensor> mutual_information_backward_cpu(
    torch::Tensor px,
    torch::Tensor py,
    torch::Tensor boundary,
    torch::Tensor p,
    torch::Tensor ans_grad) {
  TORCH_CHECK(px.dim() == 3, "px must be 3-dimensional");
  TORCH_CHECK(py.dim() == 3, "py must be 3-dimensional.");
    TORCH_CHECK(p.dim() == 3, "p must be 3-dimensional.");
  TORCH_CHECK(boundary.dim() == 2, "boundary must be 2-dimensional.");
  TORCH_CHECK(ans_grad.dim() == 1, "ans_grad must be 3-dimensional.");

  TORCH_CHECK(px.device().is_cpu() && py.device().is_cpu() && p.device().is_cpu()
              && ans_grad.device().is_cpu(),
              "inputs must be CPU tensors");

  auto scalar_t = px.scalar_type();
  auto opts = torch::TensorOptions().dtype(scalar_t).device(px.device());

  const int B = px.size(0),
      S = px.size(1),
      T = px.size(2) - 1;
  TORCH_CHECK(py.size(0) == B && py.size(1) == S + 1 && py.size(2) == T);
  TORCH_CHECK(p.size(0) == B && p.size(1) == S + 1 && p.size(2) == T + 1);
  TORCH_CHECK((boundary.size(0) == 0 && boundary.size(1) == 0) ||
              (boundary.size(0) == B && boundary.size(1) == 4));
  TORCH_CHECK(boundary.device().is_cpu() &&
              boundary.dtype() == torch::kInt64);

  bool has_boundary = (boundary.size(0) != 0);

  torch::Tensor p_grad = torch::zeros({B, S + 1, T + 1}, opts),
      px_grad = (has_boundary ? torch::zeros({B, S, T + 1}, opts) :
                 torch::empty({B, S, T + 1}, opts)),
      py_grad = (has_boundary ? torch::zeros({B, S + 1, T}, opts) :
                 torch::empty({B, S + 1, T}, opts));

  AT_DISPATCH_FLOATING_TYPES(px.scalar_type(), "mutual_information_cpu_backward_loop", ([&] {
        auto px_a = px.packed_accessor32<scalar_t, 3>(),
            // py_a = py.packed_accessor32<scalar_t, 3>(),
            p_a = p.packed_accessor32<scalar_t, 3>(),
            p_grad_a = p_grad.packed_accessor32<scalar_t, 3>(),
            px_grad_a = px_grad.packed_accessor32<scalar_t, 3>(),
            py_grad_a = py_grad.packed_accessor32<scalar_t, 3>();

        auto ans_grad_a = ans_grad.packed_accessor32<scalar_t, 1>();
        auto boundary_a = boundary.packed_accessor32<int64_t, 2>();

        for (int b = 0; b < B; b++) {
          int s_begin, s_end, t_begin, t_end;
          if (has_boundary) {
            s_begin = boundary_a[b][0];
            t_begin = boundary_a[b][1];
            s_end = boundary_a[b][2];
            t_end = boundary_a[b][3];
          } else {
            s_begin = 0;
            s_end = S;
            t_begin = 0;
            t_end = T;
          }
          // Backprop for: ans_a[b] = p_a[b][s_end][t_end];
          p_grad_a[b][s_end][t_end] = ans_grad_a[b];

          for (int s = s_end; s > s_begin; --s) {
            for (int t = t_end; t > t_begin; --t) {
              // The statement we are backpropagating here is:
              // p_a[b][s][t] = LogAdd(p_a[b][s - 1][t] + px_a[b][s - 1][t],
              //                       p_a[b][s][t - 1] + py_a[b][s][t - 1]);
              // .. which obtains p_a[b][s][t - 1] from a register.

              scalar_t term1 = p_a[b][s - 1][t] + px_a[b][s - 1][t],
                  // term2 = p_a[b][s][t - 1] + py_a[b][s][t - 1], <-- not
                  // actually needed..
                  total = p_a[b][s][t],
                  term1_deriv = exp(term1 - total),
                  term2_deriv = 1.0 - term1_deriv,
                  grad = p_grad_a[b][s][t],
                  term1_grad = term1_deriv * grad,
                  term2_grad = term2_deriv * grad;
              px_grad_a[b][s - 1][t] = term1_grad;
              p_grad_a[b][s - 1][t] = term1_grad;
              py_grad_a[b][s][t - 1] = term2_grad;
              p_grad_a[b][s][t - 1] += term2_grad;
            }
          }
          for (int t = t_end; t >= t_begin; --t) {
            // Backprop for:
            // p_a[b][s_begin][t] = p_a[b][s_begin][t - 1] + py_a[b][s_begin][t - 1];
            scalar_t this_p_grad = p_grad_a[b][s_begin][t];
            p_grad_a[b][s_begin][t - 1] += this_p_grad;
            py_grad_a[b][s_begin][t - 1] += this_p_grad;
          }
          for (int s = s_end; s >= s_begin; --s) {
            // Backprop for:
            // p_a[b][s][t_begin] = p_a[b][s - 1][t_begin] + px_a[b][s - 1][t_begin];
            scalar_t this_p_grad = p_grad_a[b][s][s_begin];
            p_a[b][s - 1][t_begin] += this_p_grad;
            px_a[b][s - 1][t_begin] += this_p_grad;
          }
          // There is no backprop for:
          // p_a[b][s_begin][t_begin] = 0.0;
          // .. but we can use this for a check, that the grad at the beginning
          // of the sequence is equal to the grad at the end of the sequence.
          if (ans_grad_a[b] != 0.0) {
            float grad_ratio = p_a[b][s_begin][t_begin] / ans_grad_a[b];
            if (grad_ratio - 1.0 > 0.01) {
              printf("Warning: mutual_information backprop: expected these numbers to be the same: %f vs. %f\n",
                     (float)p_a[b][s_begin][t_begin], (float)ans_grad_a[b]);
            }
          }
        }
      }));

  std::cout << "p_grad = " << p_grad;
  return std::vector<torch::Tensor>({px_grad, py_grad});
}




PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("mutual_information_cpu", &mutual_information_cpu, "Integrated convolution forward function (CPU)");
  m.def("mutual_information_backward_cpu", &mutual_information_backward_cpu, "Integrated convolution backward function (CPU)");
}