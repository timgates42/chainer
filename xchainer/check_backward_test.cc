#include "xchainer/gradient_check.h"

#include <algorithm>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>
#include <gsl/gsl>

#include "xchainer/array.h"
#include "xchainer/check_backward.h"
#include "xchainer/native_backend.h"
#include "xchainer/op_node.h"
#include "xchainer/shape.h"

namespace xchainer {
namespace {

using Arrays = std::vector<Array>;
using Fprop = std::function<std::vector<Array>(const std::vector<Array>&)>;

Arrays IncorrectBackwardUnaryFunc(const Arrays& inputs) {
    const Array& in = inputs[0];
    Array out = Array::EmptyLike(in);

    auto backward_function = [](const Array& gout, const std::vector<GraphId>&) { return gout * gout; };
    internal::SetUpOpNodes("incorrect_unary", {in}, out, {backward_function});

    VisitDtype(in.dtype(), [&](auto pt) {
        using T = typename decltype(pt)::type;
        int64_t total_size = in.GetTotalSize();
        auto* ldata = static_cast<const T*>(in.data().get());
        auto* odata = static_cast<T*>(out.data().get());
        for (int64_t i = 0; i < total_size; i++) {
            odata[i] = ldata[i];
        }
    });

    return {out};
}

Arrays IncorrectBackwardBinaryFunc(const Arrays& inputs) {
    const Array& lhs = inputs[0];
    const Array& rhs = inputs[1];
    CheckEqual(lhs.dtype(), rhs.dtype());
    CheckEqual(lhs.shape(), rhs.shape());
    Array out = Array::EmptyLike(lhs);

    auto lhs_backward_function = [other = rhs](const Array& gout, const std::vector<GraphId>& graph_ids_to_stop_gradient)->Array {
        return gout + other.AsConstant(graph_ids_to_stop_gradient);
    };
    auto rhs_backward_function = lhs_backward_function;
    internal::SetUpOpNodes("incorrect_binary", {lhs, rhs}, out, {lhs_backward_function, rhs_backward_function});

    VisitDtype(lhs.dtype(), [&](auto pt) {
        using T = typename decltype(pt)::type;
        int64_t total_size = lhs.GetTotalSize();
        auto* ldata = static_cast<const T*>(lhs.data().get());
        auto* rdata = static_cast<const T*>(rhs.data().get());
        auto* odata = static_cast<T*>(out.data().get());
        for (int64_t i = 0; i < total_size; i++) {
            odata[i] = ldata[i] * rdata[i];
        }
    });

    return {out};
}

class CheckBackwardBaseTest : public ::testing::Test {
protected:
    virtual void SetUp() {
        backend_ = std::make_unique<NativeBackend>();
        device_id_scope_ = std::make_unique<DeviceIdScope>(backend_.get());
    }

    virtual void TearDown() {
        device_id_scope_.reset();
        backend_.reset();
    }

protected:
    template <typename T>
    Array MakeArray(const Shape& shape, const T* data) const {
        int64_t size = shape.GetTotalSize();
        auto a = std::make_unique<T[]>(size);
        std::copy(data, data + size, a.get());
        return Array::FromBuffer(shape, TypeToDtype<T>, std::move(a));
    }

    void CheckBackwardBaseComputation(bool expect_correct, Fprop fprop, Arrays& inputs, const Arrays& grad_outputs, const Arrays& eps,
                                      double atol, double rtol, const GraphId& graph_id) {
        bool is_none_of_grad_required =
            std::none_of(inputs.begin(), inputs.end(), [graph_id](const Array& input) { return input.IsGradRequired(graph_id); });
        if (expect_correct || is_none_of_grad_required) {
            // We cannot expect any failures in case none of the input std::vector<Array> require gradients
            CheckBackwardComputation(fprop, inputs, grad_outputs, eps, atol, rtol, graph_id);
        } else {
            // Catch the gtest failure expected to be generated by CheckBackwardComputation but without failing this test
            EXPECT_NONFATAL_FAILURE(CheckBackwardComputation(fprop, inputs, grad_outputs, eps, atol, rtol, graph_id),
                                    "Backward check failure");
        }
    }

private:
    std::unique_ptr<Backend> backend_;
    std::unique_ptr<DeviceIdScope> device_id_scope_;
};

class CheckBackwardUnaryTest : public CheckBackwardBaseTest, public ::testing::WithParamInterface<bool> {
protected:
    void SetUp() override {
        CheckBackwardBaseTest::SetUp();
        requires_grad = GetParam();
    }

    template <typename T>
    void CheckBackwardUnaryComputation(bool expect_correct, Fprop fprop, const Shape& shape, const T* input_data, const T* grad_output_data,
                                       const T* eps_data, double atol, double rtol, const GraphId& graph_id) {
        Arrays inputs{MakeArray(shape, input_data)};
        if (requires_grad) {
            inputs[0].RequireGrad(graph_id);
        }

        Arrays grad_outputs{MakeArray(shape, grad_output_data)};
        Arrays eps{MakeArray(shape, eps_data)};
        CheckBackwardBaseComputation(expect_correct, fprop, inputs, grad_outputs, eps, atol, rtol, graph_id);
    }

private:
    bool requires_grad;
};

class CheckBackwardBinaryTest : public CheckBackwardBaseTest, public ::testing::WithParamInterface<std::tuple<bool, bool>> {
protected:
    void SetUp() override {
        CheckBackwardBaseTest::SetUp();
        requires_grads = {std::get<0>(GetParam()), std::get<1>(GetParam())};
    }

    template <typename T>
    void CheckBackwardBinaryComputation(bool expect_correct, Fprop fprop, const Shape& shape, const T* input_data1, const T* input_data2,
                                        const T* grad_output_data, const T* eps_data1, const T* eps_data2, double atol, double rtol,
                                        const GraphId& graph_id) {
        Arrays inputs{MakeArray(shape, input_data1), MakeArray(shape, input_data2)};
        if (requires_grads[0]) {
            inputs[0].RequireGrad(graph_id);
        }
        if (requires_grads[1]) {
            inputs[1].RequireGrad(graph_id);
        }

        Arrays grad_outputs{MakeArray(shape, grad_output_data)};
        Arrays eps{MakeArray(shape, eps_data1), MakeArray(shape, eps_data2)};
        CheckBackwardBaseComputation(expect_correct, fprop, inputs, grad_outputs, eps, atol, rtol, graph_id);
    }

private:
    std::vector<bool> requires_grads;
};

TEST_P(CheckBackwardUnaryTest, CorrectBackward) {
    float input_data[]{1.f, 2.f, 1.f};
    float grad_output_data[]{0.f, -2.f, 1.f};
    float eps_data[]{1e-3f, 1e-3f, 1e-3f};
    Fprop fprop = [](const Arrays& inputs) -> Arrays { return {inputs[0] * inputs[0]}; };
    CheckBackwardUnaryComputation(true, fprop, {1, 3}, input_data, grad_output_data, eps_data, 1e-5, 1e-4, "graph_1");
}

TEST_P(CheckBackwardUnaryTest, IncorrectBackward) {
    float input_data[]{-2.f, 3.f, 1.f};
    float grad_output_data[]{0.f, -2.f, 1.f};
    float eps_data[]{1e-3f, 1e-3f, 1e-3f};
    CheckBackwardUnaryComputation(false, &IncorrectBackwardUnaryFunc, {1, 3}, input_data, grad_output_data, eps_data, 1e-5, 1e-4,
                                  "graph_1");
}

TEST_P(CheckBackwardBinaryTest, CorrectBackward) {
    float input_data1[]{1.f, 2.f, 1.f};
    float input_data2[]{0.f, 1.f, 2.f};
    float eps_data1[]{1e-3f, 1e-3f, 1e-3f};
    float eps_data2[]{1e-3f, 1e-3f, 1e-3f};
    float grad_output_data[]{1.f, -2.f, 3.f};
    Fprop fprop = [](const Arrays& inputs) -> Arrays { return {inputs[0] * inputs[1]}; };
    CheckBackwardBinaryComputation(true, fprop, {1, 3}, input_data1, input_data2, grad_output_data, eps_data1, eps_data2, 1e-5, 1e-4,
                                   "graph_1");
}

TEST_P(CheckBackwardBinaryTest, IncorrectBackward) {
    float input_data1[]{1.f, -2.f, 1.f};
    float input_data2[]{0.f, 1.4f, 2.f};
    float eps_data1[]{1e-3f, 1e-3f, 1e-3f};
    float eps_data2[]{1e-3f, 1e-3f, 1e-3f};
    float grad_output_data[]{4.f, -2.f, 3.f};
    CheckBackwardBinaryComputation(false, &IncorrectBackwardBinaryFunc, {1, 3}, input_data1, input_data2, grad_output_data, eps_data1,
                                   eps_data2, 1e-5, 1e-4, "graph_1");
}

INSTANTIATE_TEST_CASE_P(ForEachSingleSetRequiresGrad, CheckBackwardUnaryTest, ::testing::Bool());
INSTANTIATE_TEST_CASE_P(ForEachCombinedSetRequiresGrad, CheckBackwardBinaryTest, ::testing::Combine(::testing::Bool(), ::testing::Bool()));

}  // namespace
}  // namespace xchainer
