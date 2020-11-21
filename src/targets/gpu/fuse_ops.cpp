#include <migraphx/pass_manager.hpp>
#include <migraphx/dead_code_elimination.hpp>
#include <migraphx/gpu/fuse_ops.hpp>
#include <migraphx/matcher.hpp>
#include <migraphx/gpu/miopen.hpp>
#include <migraphx/gpu/clip.hpp>
#include <migraphx/gpu/convolution.hpp>
#include <migraphx/gpu/oper.hpp>
#include <migraphx/gpu/add.hpp>
#include <migraphx/gpu/mul.hpp>
#include <migraphx/gpu/gemm.hpp>
#include <migraphx/gpu/device/layernorm.hpp>
#include <migraphx/gpu/device/gelu.hpp>
#include <migraphx/gpu/device/mul_add.hpp>
#include <migraphx/gpu/device/add_clip.hpp>
#include <migraphx/gpu/device/add_relu.hpp>
#include <migraphx/gpu/device/add_sigmoid.hpp>
#include <migraphx/gpu/device/add_tanh.hpp>
#include <migraphx/gpu/device/mul_add_relu.hpp>
#include <migraphx/gpu/device/add.hpp>
#include <migraphx/instruction.hpp>
#include <migraphx/register_op.hpp>
#include <migraphx/array.hpp>
#include <migraphx/op/clip.hpp>
#include <cmath>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {
namespace gpu {

MIGRAPHX_DECLARE_ENV_VAR(MIGRAPHX_DISABLE_MIOPEN_FUSION)

struct fusion
{
    using op_t = miopenFusionOpDescriptor_t;
    shared<fusion_plan_descriptor> fp;

    // Used as a temporary hack to keep descriptor references alive
    std::vector<std::shared_ptr<void>> storage;

    template <class T>
    auto keep_alive(T x)
    {
        auto result = share(std::move(x));
        storage.push_back(result);
        return result;
    }

    fusion() = default;

    fusion(const shape& input)
    {
        assert(input.standard());
        auto t = make_tensor(input);
        fp     = make_fusion_plan(t);
        assert(fp);
        keep_alive(std::move(t));
    }

    op_t operator[](std::size_t i) const
    {
        assert(fp);
        op_t result;
        auto status = miopenFusionPlanGetOp(fp.get(), i, &result);
        if(status != miopenStatusSuccess)
            MIGRAPHX_THROW("Failed retrieving operator at " + std::to_string(i));
        return result;
    }

    auto get() const
    {
        assert(fp);
        return fp.get();
    }

    op_t create_bias(const shape& bias)
    {
        assert(fp);
        op_t result;
        auto b      = shape{bias.type(), {1, bias.lens().at(1), 1, 1}};
        auto t      = keep_alive(make_tensor(b));
        auto status = miopenCreateOpBiasForward(fp.get(), &result, t.get());
        if(status != miopenStatusSuccess)
            MIGRAPHX_THROW("Creating operator failed");
        return result;
    }

    op_t create_relu()
    {
        assert(fp);
        op_t result;
        auto status = miopenCreateOpActivationForward(fp.get(), &result, miopenActivationRELU);
        if(status != miopenStatusSuccess)
            MIGRAPHX_THROW("Creating operator failed");
        return result;
    }

    op_t create_conv(const op::convolution& op, const shape& weights)
    {
        assert(fp);
        op_t result;
        auto cd     = keep_alive(make_conv(op));
        auto t      = keep_alive(make_tensor(weights));
        auto status = miopenCreateOpConvForward(fp.get(), &result, cd.get(), t.get());
        if(status != miopenStatusSuccess)
            MIGRAPHX_THROW("Creating operator failed");
        return result;
    }

    shape get_workspace(context&)
    {
        // assert(fp);
        // TODO: Use zero workspace for now
        std::size_t ws_size = 0;
        // int algo_count = 1;
        // miopenConvFwdAlgorithm_t algo;
        // miopenFusionPlanConvolutionGetAlgo(fp.get(), 1, &algo_count, &algo);
        // miopenFusionPlanGetWorkSpaceSize(ctx.get_stream().get_miopen(), fp.get(), &ws_size,
        // algo);
        return shape{shape::int8_type, {ws_size}};
    }

    void compile(context& ctx)
    {
        assert(fp);
        auto status = miopenCompileFusionPlan(ctx.get_stream().get_miopen(), fp.get());
        if(status != miopenStatusSuccess)
            MIGRAPHX_THROW("Compiling fusion plan failed");
    }

    argument execute(context& ctx,
                     const fused_operator_args& fargs,
                     const argument& x,
                     const argument& y) const
    {
        assert(fp);
        auto x_td   = make_tensor(x.get_shape());
        auto y_td   = make_tensor(y.get_shape());
        auto status = miopenExecuteFusionPlan(ctx.get_stream().get_miopen(),
                                              fp.get(),
                                              x_td.get(),
                                              x.implicit(),
                                              y_td.get(),
                                              y.implicit(),
                                              fargs.get());
        if(status != miopenStatusSuccess)
            MIGRAPHX_THROW("Failed to execute fusion plan");
        return y;
    }
};

MIGRAPHX_PRED_MATCHER(bias_shape, instruction_ref ins)
{
    auto&& s = ins->get_shape();
    return s.broadcasted() and s.strides().size() == 4 and s.strides()[0] == 0 and
           s.strides()[1] != 0 and s.strides()[2] == 0 and s.strides()[3] == 0;
}

MIGRAPHX_PRED_MATCHER(fusable_conv, instruction_ref ins)
{
    if(enabled(MIGRAPHX_DISABLE_MIOPEN_FUSION{}))
        return false;
    if(ins->name() != "gpu::convolution")
        return false;
    if(ins->get_shape().type() != shape::float_type)
        return false;
    auto wei = ins->inputs().at(1)->get_shape();
    assert(wei.lens().size() == 4);
    auto conv = any_cast<miopen_convolution>(ins->get_operator());
    if(conv.op.group > 1)
        return false;
    if(wei.lens()[1] > 512 and conv.algo != miopenConvolutionFwdAlgoWinograd)
        return false;

    // Do not fuse non-symmetric input
    auto input_lens = ins->inputs().at(0)->get_shape().lens();
    if(input_lens[2] != input_lens[3] or wei.lens()[2] != wei.lens()[3])
        return false;

    auto op = conv.op;
    // Dont fuse winograd for non-3x3s since there is no fused windograd for those configs
    if(conv.algo == miopenConvolutionFwdAlgoWinograd and wei.lens()[2] != 3 and
       wei.lens()[3] != 3 and contains({{1, 1}}, op.stride))
        return false;
    return contains({{0, 0}, {1, 1}, {2, 2}}, op.padding) and
           contains({{0, 0}, {1, 1}}, op.stride) and contains({{1, 1}}, op.dilation);
}

struct hip_triadd : ternary_device<hip_triadd, &device::add>
{
};
MIGRAPHX_REGISTER_OP(hip_triadd)

struct hip_triadd_clip : quinary_device<hip_triadd_clip, &device::add_clip>
{
};
MIGRAPHX_REGISTER_OP(hip_triadd_clip)

struct hip_add_clip : quaternary_device<hip_add_clip, &device::add_clip>
{
};
MIGRAPHX_REGISTER_OP(hip_add_clip)

struct hip_triadd_relu : ternary_device<hip_triadd_relu, &device::add_relu>
{
};
MIGRAPHX_REGISTER_OP(hip_triadd_relu)

struct hip_triadd_sigmoid : ternary_device<hip_triadd_sigmoid, &device::add_sigmoid>
{
};
MIGRAPHX_REGISTER_OP(hip_triadd_sigmoid)

struct hip_triadd_tanh : ternary_device<hip_triadd_tanh, &device::add_tanh>
{
};
MIGRAPHX_REGISTER_OP(hip_triadd_tanh)

struct hip_add_relu : binary_device<hip_add_relu, &device::add_relu>
{
};
MIGRAPHX_REGISTER_OP(hip_add_relu)

struct hip_add_sigmoid : binary_device<hip_add_relu, &device::add_sigmoid>
{
};
MIGRAPHX_REGISTER_OP(hip_add_sigmoid)

struct hip_add_tanh : binary_device<hip_add_tanh, &device::add_tanh>
{
};
MIGRAPHX_REGISTER_OP(hip_add_tanh)

struct hip_layernorm : unary_device<hip_layernorm, &device::layernorm>
{
    // Empty finalize to skip dimension reduction
    void finalize(context&, const shape&, const std::vector<shape>&) {}
};
MIGRAPHX_REGISTER_OP(hip_layernorm)

struct hip_triadd_layernorm : ternary_device<hip_triadd_layernorm, &device::triadd_layernorm>
{
    // Empty finalize to skip dimension reduction
    void finalize(context&, const shape&, const std::vector<shape>&) {}
};
MIGRAPHX_REGISTER_OP(hip_triadd_layernorm)

struct hip_gelu : unary_device<hip_gelu, &device::gelu>
{
};
MIGRAPHX_REGISTER_OP(hip_gelu)

struct hip_add_gelu : binary_device<hip_add_gelu, &device::add_gelu>
{
};
MIGRAPHX_REGISTER_OP(hip_add_gelu)

struct hip_gelu_new : unary_device<hip_gelu_new, &device::gelu_new>
{
};
MIGRAPHX_REGISTER_OP(hip_gelu_new)

struct hip_add_gelu_new : binary_device<hip_add_gelu_new, &device::add_gelu_new>
{
};
MIGRAPHX_REGISTER_OP(hip_add_gelu_new)

struct hip_mul_add : ternary_device<hip_mul_add, &device::mul_add>
{
};
MIGRAPHX_REGISTER_OP(hip_mul_add)

struct hip_mul_add_relu : ternary_device<hip_mul_add_relu, &device::mul_add_relu>
{
};
MIGRAPHX_REGISTER_OP(hip_mul_add_relu)

void move_broadcasted_back(std::vector<instruction_ref>& args)
{
    // Ensure the last arguments is the broadcasted one
    auto last = std::prev(args.end());
    auto it =
        std::find_if(args.begin(), last, [](auto arg) { return arg->get_shape().broadcasted(); });
    if(it != last)
        std::swap(*it, *std::prev(last));
}

void move_standard_front(std::vector<instruction_ref>& args)
{
    // Ensure the first arguments is the standard one
    auto last = std::prev(args.end());
    auto it =
        std::find_if(args.begin(), last, [](auto arg) { return arg->get_shape().standard(); });
    if(it != last)
        std::swap(*it, args.front());
}

struct find_layernorm
{
    template <class... Ts>
    static auto multibroadcast_op(Ts... xs)
    {
        return match::name("multibroadcast")(match::arg(0)(xs...));
    }

    static auto x_minus_mean()
    {
        return match::name("gpu::sub")(
            match::arg(0)(match::any().bind("x")),
            match::arg(1)(multibroadcast_op(match::name("gpu::reduce_mean"))));
    }

    static auto variance()
    {
        return match::name("gpu::reduce_mean")(match::arg(0)(
            match::name("gpu::pow")(match::arg(0)(x_minus_mean()),
                                    match::arg(1)(multibroadcast_op(match::has_value(2.0f))))));
    }

    static auto layernorm_onnx()
    {
        return match::name("gpu::div")(
            match::arg(0)(x_minus_mean()),

            match::arg(1)(multibroadcast_op(
                match::name("gpu::sqrt")(match::arg(0)(match::name("gpu::add")(match::either_arg(
                    0, 1)(variance(), multibroadcast_op(match::has_value(1e-12f)))))))));
    }

    auto matcher() const { return layernorm_onnx(); }

    void apply(module& p, match::matcher_result r) const
    {
        auto ins   = r.result;
        auto x_ins = r.instructions["x"];
        auto args  = ins->inputs();

        // We dont fuse for non-standard layouts
        if(not x_ins->get_shape().standard())
            return;

        auto relements = x_ins->get_shape().lens().back();

        if(relements > 1024 or (relements % 4 != 0 and relements > 256))
            return;

        p.replace_instruction(ins, hip_layernorm{}, x_ins, args.back());
    }
};

struct find_triadd_layernorm
{
    auto matcher() const
    {
        return match::name("gpu::layernorm")(match::arg(0)(match::name("gpu::triadd")(
            match::used_once(), match::all_of[match::inputs()](match::standard_shape()))));
    }

    void apply(program& p, const match::matcher_result& r) const
    {
        auto ins    = r.result;
        auto triadd = ins->inputs().front();
        p.replace_instruction(ins, hip_triadd_layernorm{}, triadd->inputs());
    }
};

struct find_gelu
{

    static auto erf_fn()
    {
        return match::name("gpu::erf")(
            match::used_once(),
            match::arg(0)(match::used_once(),
                          match::name("gpu::mul")(match::either_arg(0, 1)(
                              match::none_of(match::has_value(M_SQRT1_2)).bind("x"),
                              match::has_value(M_SQRT1_2)))));
    }

    static auto add_erf()
    {
        return match::name("gpu::add")(
            match::used_once(),
            match::either_arg(0, 1)(erf_fn(), match::args(match::has_value(1.0f))));
    }

    static auto one_half() { return match::args(match::has_value(0.5f)); }

    auto matcher() const
    {
        return match::unordered_tree("gpu::mul", one_half(), add_erf(), match::any());
    }

    void apply(module& p, match::matcher_result r) const
    {
        auto ins   = r.result;
        auto x_ins = r.instructions["x"];
        auto args  = ins->inputs();

        p.replace_instruction(ins, hip_gelu{}, x_ins, args.back());
    }
};

struct find_add_gelu
{
    auto matcher() const
    {
        return match::name("gpu::gelu")(match::arg(0)(match::name("gpu::add").bind("add")));
    }

    void apply(module& p, match::matcher_result r) const
    {
        auto add_ins = r.instructions["add"];
        auto ins     = r.result;
        auto args    = add_ins->inputs();
        move_standard_front(args);
        move_broadcasted_back(args);

        args.back() = ins->inputs().back();
        p.replace_instruction(ins, hip_add_gelu{}, args);
    }
};

struct find_gelu_new
{
    bool fast_math = true;

    static auto pow_fn()
    {
        return match::name("gpu::pow")(match::used_once(),
                                       match::arg(1)(match::args(match::has_value(3.0f))));
    }

    static auto tanh_fn()
    {
        return match::name("gpu::tanh")(
            match::used_once(),
            match::arg(0)(match::name("gpu::mul")(match::either_arg(0, 1)(
                match::args(match::has_value(sqrt(M_2_PI))),
                match::name("gpu::add")(
                    match::any_arg(0, 1)(match::name("gpu::mul")(match::either_arg(0, 1)(
                        match::args(match::has_value(0.044715f)), pow_fn()))))))));
    }

    auto matcher() const
    {
        return match::name("gpu::mul")(
            match::used_once(),
            match::either_arg(0, 1)(
                match::any().bind("x"),
                match::name("gpu::add")(match::any_arg(0, 1)(match::name("gpu::mul")(
                    match::either_arg(0, 1)(match::args(match::has_value(0.5f)), tanh_fn()))))));
    }

    void apply(module& p, match::matcher_result r) const
    {
        auto ins   = r.result;
        auto x_ins = r.instructions["x"];
        auto args  = ins->inputs();

        if(not fast_math)
            p.replace_instruction(ins, hip_gelu_new{}, x_ins, args.back());
        else
            p.replace_instruction(ins, hip_gelu{}, x_ins, args.back());
    }
};

struct find_add_gelu_new
{
    auto matcher() const
    {
        return match::name("gpu::gelu_new")(match::arg(0)(match::name("gpu::add").bind("add")));
    }

    void apply(module& p, match::matcher_result r) const
    {
        auto add_ins = r.instructions["add"];
        auto ins     = r.result;
        auto args    = add_ins->inputs();
        move_standard_front(args);
        move_broadcasted_back(args);

        args.back() = ins->inputs().back();
        p.replace_instruction(ins, hip_add_gelu_new{}, args);
    }
};

struct find_add_clip
{
    auto matcher() const
    {
        return match::name(std::unordered_set<std::string>{"gpu::clip", "gpu::clipped_relu"})(
            match::arg(0)(match::any_of(match::name("gpu::add"),
                                        match::name("gpu::triadd"),
                                        match::any_of[match::inputs()](match::standard_shape()))
                              .bind("add")));
    }

    void apply(module& p, match::matcher_result r) const
    {
        auto add_ins  = r.instructions["add"];
        auto ins      = r.result;
        auto ins_args = ins->inputs();
        auto add_args = add_ins->inputs();
        move_standard_front(add_args);
        move_broadcasted_back(add_args);

        // Use the allocation from the clip operator
        add_args.pop_back();
        add_args.insert(add_args.end(), std::next(ins_args.begin()), ins_args.end());
        if(add_ins->name() == "gpu::add")
            p.replace_instruction(ins, hip_add_clip{}, add_args);
        else if(add_ins->name() == "gpu::triadd")
            p.replace_instruction(ins, hip_triadd_clip{}, add_args);
    }
};

struct find_add_unary
{
    std::string op_name;
    operation binary_add_op;
    operation ternary_add_op;
    auto matcher() const
    {
        return match::name(op_name)(match::arg(0)(
            match::used_once(),
            match::any_of(match::name("gpu::add"),
                          match::name("gpu::triadd"),
                          match::any_of(match::name("@literal"),
                                        match::any_of[match::inputs()](match::standard_shape())))
                .bind("add")));
    }

    void apply(module& p, match::matcher_result r) const
    {
        auto add_ins = r.instructions["add"];
        auto ins     = r.result;
        auto args    = add_ins->inputs();
        move_standard_front(args);
        move_broadcasted_back(args);

        // Use the allocation from the relu operator
        args.back() = ins->inputs().back();
        if(add_ins->name() == "gpu::add")
            p.replace_instruction(ins, binary_add_op, args);
        else if(add_ins->name() == "gpu::triadd")
            p.replace_instruction(ins, ternary_add_op, args);
    }
};

struct find_triadd
{
    auto matcher() const
    {
        return match::name("gpu::add")(match::either_arg(0, 1)(
            match::name("gpu::add")(match::used_once()).bind("add"),
            match::any(match::any_of(match::name("@literal"),
                                     match::any_of[match::inputs()](match::standard_shape())))
                .bind("input")));
    }

    void apply(module& p, match::matcher_result r) const
    {
        auto add_ins   = r.instructions["add"];
        auto input_ins = r.instructions["input"];
        auto ins       = r.result;
        auto args      = add_ins->inputs();

        auto is_broadcasted = [](auto arg) { return arg->get_shape().broadcasted(); };
        if(std::count_if(args.begin(), args.end(), is_broadcasted) > 2)
            return;
        args.insert(args.begin(), input_ins);
        move_standard_front(args);
        move_broadcasted_back(args);

        args.back() = ins->inputs().back();
        p.replace_instruction(ins, hip_triadd{}, args);
    }
};

struct find_mul_add
{
    auto matcher() const
    {
        return match::name("gpu::add")(match::either_arg(0, 1)(
            match::name("gpu::mul")(match::used_once()).bind("mul"), match::any().bind("b")));
    }

    void apply(module& p, match::matcher_result r) const
    {
        auto mul_ins = r.instructions["mul"];
        auto b_ins   = r.instructions["b"];
        auto ins     = r.result;
        auto args    = mul_ins->inputs();
        assert(mul_ins != b_ins);

        move_standard_front(args);
        move_broadcasted_back(args);
        args.insert(std::prev(args.end()), b_ins);

        args.back() = ins->inputs().back();
        p.replace_instruction(ins, hip_mul_add{}, args);
    }
};

struct find_mul_add_relu
{
    auto matcher() const
    {
        return match::name("gpu::relu")(
            match::arg(0)(match::name("gpu::mul_add")(match::used_once()).bind("mul_add")));
    }

    void apply(module& p, match::matcher_result r) const
    {
        auto mul_add_ins = r.instructions["mul_add"];
        auto ins         = r.result;
        auto args        = mul_add_ins->inputs();

        // Use the allocation from the relu operator
        args.back() = ins->inputs().back();
        p.replace_instruction(ins, hip_mul_add_relu{}, args);
    }
};

struct miopen_conv_bias
{
    op::convolution op;
    fusion f          = {};
    fusion::op_t conv = {};
    fusion::op_t bias = {};

    template <class Self, class F>
    static auto reflect(Self& self, F f)
    {
        return op::convolution::reflect(self.op, f);
    }

    std::string name() const { return "gpu::conv_bias"; }
    shape compute_shape(const std::vector<shape>& inputs) const
    {
        check_shapes{inputs, *this}.has(5);
        // TODO: Check slices
        return op.compute_shape({inputs.at(0), inputs.at(1)});
    }
    argument compute(context& ctx, const shape&, const std::vector<argument>& args) const
    {
        auto fargs  = make_fused_args();
        float alpha = 1;
        float beta  = 0;
        miopenSetOpArgsConvForward(fargs.get(), conv, &alpha, &beta, args[1].implicit());
        miopenSetOpArgsBiasForward(fargs.get(), bias, &alpha, &beta, args[3].implicit());
        return f.execute(ctx, fargs, args[0], args[4]);
    }

    void finalize(context& ctx, const shape&, const std::vector<shape>& inputs)
    {
        f    = fusion(inputs[0]);
        conv = f.create_conv(op, inputs[1]);
        bias = f.create_bias(inputs[3]);
        f.compile(ctx);
    }

    shape get_workspace(context& ctx) { return f.get_workspace(ctx); }
    std::ptrdiff_t output_alias(const std::vector<shape>& shapes) const
    {
        return shapes.size() - 1;
    }
};
MIGRAPHX_REGISTER_OP(miopen_conv_bias)

struct miopen_conv_bias_relu
{
    op::convolution op;
    fusion f          = {};
    fusion::op_t conv = {};
    fusion::op_t bias = {};
    fusion::op_t relu = {};

    template <class Self, class F>
    static auto reflect(Self& self, F f)
    {
        return op::convolution::reflect(self.op, f);
    }

    std::string name() const { return "gpu::conv_bias_relu"; }
    shape compute_shape(const std::vector<shape>& inputs) const
    {
        check_shapes{inputs, *this}.has(5);
        // TODO: Check slices
        return op.compute_shape({inputs.at(0), inputs.at(1)});
    }
    argument compute(context& ctx, const shape&, const std::vector<argument>& args) const
    {
        auto fargs  = make_fused_args();
        float alpha = 1;
        float beta  = 0;
        miopenSetOpArgsConvForward(fargs.get(), conv, &alpha, &beta, args[1].implicit());
        miopenSetOpArgsBiasForward(fargs.get(), bias, &alpha, &beta, args[3].implicit());
        miopenSetOpArgsActivForward(fargs.get(), relu, &alpha, &beta, 0, 0, 0);
        return f.execute(ctx, fargs, args[0], args[4]);
    }
    void finalize(context& ctx, const shape&, const std::vector<shape>& inputs)
    {
        f    = fusion(inputs[0]);
        conv = f.create_conv(op, inputs[1]);
        bias = f.create_bias(inputs[3]);
        relu = f.create_relu();
        f.compile(ctx);
    }

    shape get_workspace(context& ctx) { return f.get_workspace(ctx); }
    std::ptrdiff_t output_alias(const std::vector<shape>& shapes) const
    {
        return shapes.size() - 1;
    }
};
MIGRAPHX_REGISTER_OP(miopen_conv_bias_relu)

template <class... Ms>
auto conv_bias(Ms... ms)
{
    return match::name("gpu::add")(
        match::either_arg(0, 1)(bias_shape(match::used_once()).bind("bias"),
                                fusable_conv(match::used_once()).bind("conv")),
        ms...);
}

template <class Op>
void apply_conv_bias(context& ctx, module& p, match::matcher_result r)
{
    auto conv_ins    = r.instructions["conv"];
    auto bias_ins    = r.instructions["bias"];
    auto ins         = r.result;
    auto input_ins   = conv_ins->inputs().at(0);
    auto weights_ins = conv_ins->inputs().at(1);
    auto conv_op     = any_cast<miopen_convolution>(conv_ins->get_operator()).op;
    auto alloc_ins   = ins->inputs().back();
    auto old_ws_ins  = conv_ins->inputs().at(2);

    Op cb{conv_op};
    // TODO: Insert ws allocation
    auto ws = cb.get_workspace(ctx);
    (void)ws;
    p.replace_instruction(ins, cb, input_ins, weights_ins, old_ws_ins, bias_ins, alloc_ins);
}

struct find_conv_bias
{
    context* ctx = nullptr;
    auto matcher() const
    {
        return conv_bias(match::none_of(
            match::output(match::name(std::unordered_set<std::string>{"gpu::relu"}))));
    }

    void apply(module& p, match::matcher_result r) const
    {
        apply_conv_bias<miopen_conv_bias>(*ctx, p, std::move(r));
    }
};

struct find_conv_bias_relu
{
    context* ctx = nullptr;
    auto matcher() const { return match::name("gpu::relu")(match::arg(0)(conv_bias())); }

    void apply(module& p, match::matcher_result r) const
    {
        apply_conv_bias<miopen_conv_bias_relu>(*ctx, p, std::move(r));
    }
};

struct find_gemm_add
{
    auto matcher() const
    {
        return match::name("gpu::add")(
            match::all_of[match::inputs()](match::standard_shape()),
            match::either_arg(0, 1)(match::used_once().bind("c"),
                                    match::name("gpu::gemm")(match::nargs(3)).bind("gemm")));
    }

    void apply(module& p, match::matcher_result r) const
    {
        auto ins      = r.result;
        auto gemm_ins = r.instructions["gemm"];
        auto c_ins    = r.instructions["c"];

        auto gemm = any_cast<rocblas_gemm<op::dot>>(gemm_ins->get_operator());

        // Already fused gemm
        if(not float_equal(gemm.op.beta, 0))
            return;

        if(std::any_of(ins->inputs().begin(), ins->inputs().end(), [](auto i) {
               return not i->get_shape().standard();
           }))
            return;

        auto inputs = gemm_ins->inputs();
        inputs.pop_back();

        auto copy_ins = c_ins;

        // Insert copy
        if(ins == p.end() or c_ins->outputs().size() > 1 or c_ins->inputs().empty())
        {
            copy_ins = p.insert_instruction(ins, hip_copy{}, c_ins, ins->inputs().back());
        }
        inputs.push_back(copy_ins);
        inputs.push_back(copy_ins);

        gemm.op.beta = 1;
        p.replace_instruction(ins, gemm, inputs);
    }
};

struct find_commutative_broadcast
{
    auto matcher() const
    {
        return match::name("gpu::add", "gpu::mul")(match::arg(1)(match::broadcast_shape()));
    }

    void apply(module& p, const match::matcher_result& r) const
    {
        auto ins  = r.result;
        auto args = ins->inputs();
        move_broadcasted_back(args);

        p.replace_instruction(ins, ins->get_operator(), args);
    }
};

void fuse_ops::apply(module& p) const
{
    match::find_matches(p, find_gelu{}, find_gelu_new{fast_math});
    run_passes(p, {dead_code_elimination{}});
    match::find_matches(p, find_triadd{});
    match::find_matches(p,
                        find_layernorm{},
                        find_conv_bias_relu{ctx},
                        find_conv_bias{ctx},
                        find_add_gelu{},
                        find_add_gelu_new{},
                        find_mul_add{},
                        find_mul_add_relu{},
                        find_add_unary{"gpu::relu", hip_add_relu{}, hip_triadd_relu{}},
                        find_add_unary{"gpu::sigmoid", hip_add_sigmoid{}, hip_triadd_sigmoid{}},
                        find_add_unary{"gpu::tanh", hip_add_tanh{}, hip_triadd_tanh{}},
                        find_add_clip{});
    run_passes(p, {dead_code_elimination{}});
    match::find_matches(p, find_triadd_layernorm{}, find_gemm_add{}, find_commutative_broadcast{});
}

} // namespace gpu
} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx
