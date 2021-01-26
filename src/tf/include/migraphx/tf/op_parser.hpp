#ifndef MIGRAPHX_GUARD_AMDMIGRAPHX_TF_REGISTER_OP_PARSER_HPP
#define MIGRAPHX_GUARD_AMDMIGRAPHX_TF_REGISTER_OP_PARSER_HPP

#include <migraphx/config.hpp>
#include <migraphx/auto_register.hpp>
#include <migraphx/tf/tf_parser.hpp>
#include <cstring>
#include <vector>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {
namespace tf {

struct op_desc
{
    std::string tf_name = "";
    std::string op_name   = "";
};

void register_op_parser(const std::string& name, tf_parser::op_func f);
tf_parser::op_func get_op_parser(const std::string& name);
std::vector<std::string> get_op_parsers();

inline std::vector<instruction_ref> implicit_multi_op(std::vector<instruction_ref> inss)
{
    return inss;
}

inline std::vector<instruction_ref> implicit_multi_op(instruction_ref ins) { return {ins}; }

template <class T>
void register_op_parser()
{
    T parser;
    for(auto&& opd : parser.operators())
        register_op_parser(opd.tf_name, [opd, parser](auto&&... xs) {
            return implicit_multi_op(parser.parse(opd, xs...));
        });
}

struct register_op_parser_action
{
    template <class T>
    static void apply()
    {
        register_op_parser<T>();
    }
};

template <class T>
using op_parser = auto_register<register_op_parser_action, T>;

} // namespace tf
} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx

#endif
