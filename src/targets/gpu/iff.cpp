#include <migraphx/gpu/iff.hpp>
#include <migraphx/gpu/context.hpp>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {
namespace gpu {

shape hip_iff::compute_shape(std::vector<shape> inputs) const
{
    inputs.pop_back();
    return op.compute_shape(inputs);
}

argument hip_iff::compute(
    context& ctx,
    const shape&,
    std::vector<argument> args,
    std::vector<module_ref>& modules,
    std::function<std::vector<argument>(
        const module_ref& mdl, context& ctx, const std::vector<argument>& inputs)> run) const
{
    auto arg_cond  = migraphx::gpu::from_gpu(args[0]);
    auto cond      = arg_cond.at<bool>();
    module_ref mdl = cond ? modules[0] : modules[1];

    auto results = run(mdl, ctx, args);

    return results[0];
}

} // namespace gpu
} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx
