
#include "verify_program.hpp"
#include <migraphx/program.hpp>
#include <migraphx/generate.hpp>
#include <migraphx/serialize.hpp>

#include <migraphx/make_op.hpp>

#include <migraphx/op/common.hpp>

struct test_rnn_bi_3args : verify_program<test_rnn_bi_3args>
{
    migraphx::program create_program() const
    {
        std::size_t batch_size  = 2;
        std::size_t seq_len     = 10;
        std::size_t hidden_size = 4;
        std::size_t input_size  = 3;
        std::size_t num_dirct   = 2;
        float clip              = 0.0f;

        migraphx::program p;
        auto* mm = p.get_main_module();
        migraphx::shape in_shape{migraphx::shape::float_type, {seq_len, batch_size, input_size}};
        migraphx::shape w_shape{migraphx::shape::float_type, {num_dirct, hidden_size, input_size}};
        migraphx::shape r_shape{migraphx::shape::float_type, {num_dirct, hidden_size, hidden_size}};
        migraphx::shape b_shape{migraphx::shape::float_type, {num_dirct, 2 * hidden_size}};
        migraphx::shape ih_shape{migraphx::shape::float_type, {num_dirct, batch_size, hidden_size}};

        auto seq    = mm->add_parameter("seq", in_shape);
        auto w      = mm->add_parameter("w", w_shape);
        auto r      = mm->add_parameter("r", r_shape);
        auto output = mm->add_instruction(
            migraphx::make_op(
                "rnn",
                {{"hidden_size", hidden_size},
                 {"actv_func",
                  migraphx::to_value(std::vector<migraphx::operation>{migraphx::make_op("tanh"),
                                                                      migraphx::make_op("tanh")})},
                 {"direction", migraphx::to_value(migraphx::op::rnn_direction::bidirectional)},
                 {"clip", clip}}),
            seq,
            w,
            r);
        mm->add_instruction(migraphx::make_op("rnn_last_hs_output"), output);

        return p;
    }
    std::string section() const { return "rnn"; }
};
