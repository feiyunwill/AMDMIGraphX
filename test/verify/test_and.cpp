
#include "verify_program.hpp"
#include <migraphx/program.hpp>
#include <migraphx/generate.hpp>
#include <migraphx/make_op.hpp>

struct test_and : verify_program<test_and>
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto* mm = p.get_main_module();
        migraphx::shape s{migraphx::shape::bool_type, {3}};
        auto x = mm->add_parameter("x", s);
        auto y = mm->add_parameter("y", s);
        mm->add_instruction(migraphx::make_op("logical_and"), x, y);
        return p;
    }
};
