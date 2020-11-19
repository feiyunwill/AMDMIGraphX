
#include "verify_program.hpp"
#include <migraphx/program.hpp>
#include <migraphx/generate.hpp>
#include <migraphx/operators.hpp>

template <class Op, int Axis, migraphx::shape::type_t T>
struct test_reduce_op_small : verify_program<test_reduce_op_small<Op, Axis, T>>
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{T, {3, 4, 8, 8}};
        auto x = p.add_parameter("x", s);
        p.add_instruction(Op{{1}}, x);
        return p;
    };
};

template struct test_reduce_op_small<migraphx::op::reduce_sum, 2, migraphx::shape::int32_type>;
template struct test_reduce_op_small<migraphx::op::reduce_mean, 2, migraphx::shape::int32_type>;
template struct test_reduce_op_small<migraphx::op::reduce_max, 2, migraphx::shape::int32_type>;
template struct test_reduce_op_small<migraphx::op::reduce_min, 2, migraphx::shape::int32_type>;

template struct test_reduce_op_small<migraphx::op::reduce_sum, 2, migraphx::shape::half_type>;
template struct test_reduce_op_small<migraphx::op::reduce_mean, 2, migraphx::shape::half_type>;
template struct test_reduce_op_small<migraphx::op::reduce_max, 2, migraphx::shape::half_type>;
template struct test_reduce_op_small<migraphx::op::reduce_min, 2, migraphx::shape::half_type>;
template struct test_reduce_op_small<migraphx::op::reduce_prod, -2, migraphx::shape::half_type>;
