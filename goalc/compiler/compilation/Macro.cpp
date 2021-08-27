#include "goalc/compiler/Compiler.h"
#include "third-party/fmt/core.h"
#include "common/goos/PrettyPrinter.h"

using namespace goos;

/*!
 * Try to find a macro with the given name in the GOOS "goal_env". Return if it succeeded.
 */
bool Compiler::try_getting_macro_from_goos(const goos::Object& macro_name, goos::Object* dest) {
  Object macro_obj;
  bool got_macro = false;
  if (m_goos.eval_symbol(macro_name, m_goos.goal_env.as_env_ptr(), &macro_obj) &&
      macro_obj.is_macro()) {
    got_macro = true;
  }

  if (got_macro) {
    *dest = macro_obj;
  }
  return got_macro;
}

/*!
 * Expand a macro, then compile the result.
 */
Val* Compiler::compile_goos_macro(const goos::Object& o,
                                  const goos::Object& macro_obj,
                                  const goos::Object& rest,
                                  const goos::Object& name,
                                  Env* env) {
  auto macro = macro_obj.as_macro();
  Arguments args = m_goos.get_args(o, rest, macro->args);
  auto mac_env_obj = EnvironmentObject::make_new();
  auto mac_env = mac_env_obj.as_env_ptr();
  mac_env->parent_env = m_goos.global_environment.as_env_ptr();
  m_goos.set_args_in_env(o, args, macro->args, mac_env);
  m_goos.goal_to_goos.enclosing_method_type = env->function_env()->method_of_type_name;
  auto goos_result = m_goos.eval_list_return_last(macro->body, macro->body, mac_env);
  // make the macro expanded form point to the source where the macro was used for error messages.
  // m_goos.reader.db.inherit_info(o, goos_result);
  m_goos.goal_to_goos.reset();

  auto compile_env_for_macro =
      env->function_env()->alloc_env<MacroExpandEnv>(env, name.as_symbol(), macro->body, o);
  try {
    return compile(goos_result, compile_env_for_macro);
  } catch (CompilerException& ce) {
    if (ce.print_err_stack) {
      bool good_info = false;
      auto info = m_goos.reader.db.get_info_for(o, &good_info);
      fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "Code:\n");
      fmt::print("{}\n", pretty_print::to_string(goos_result, 120));
      fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "From macro: ");
      fmt::print(fg(fmt::color::orange), "{}\n", name.print());
      if (good_info) {
        fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "Used in:\n");
        fmt::print("{}\n", info);
        ce.print_err_stack = false;
      }
      std::string line(80, '-');
      line.push_back('\n');
      fmt::print(line);
    }

    throw;
  } catch (std::runtime_error& e) {
    bool good_info = false;
    auto info = m_goos.reader.db.get_info_for(o, &good_info);
    fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "Code:\n");
    fmt::print("{}\n", pretty_print::to_string(goos_result, 120));
    fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "From macro: ");
    fmt::print(fg(fmt::color::orange), "{}\n", name.print());
    if (good_info) {
      fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "Used in:\n");
      fmt::print("{}\n", info);
    }
    std::string line(80, '-');
    line.push_back('\n');
    fmt::print(line);

    throw;
  }
}

/*!
 * Compile the #cond form, which is a compile-time conditional statement.
 */
Val* Compiler::compile_gscond(const goos::Object& form, const goos::Object& rest, Env* env) {
  if (!rest.is_pair()) {
    throw_compiler_error(form, "#cond must have at least one clause, which must be a form");
  }
  Val* result = nullptr;

  Object lst = rest;
  for (;;) {
    if (lst.is_pair()) {
      Object current_case = lst.as_pair()->car;
      if (!current_case.is_pair()) {
        throw_compiler_error(lst, "Bad case in #cond");
      }

      // check condition:
      Object condition_result = m_goos.eval_with_rewind(current_case.as_pair()->car,
                                                        m_goos.global_environment.as_env_ptr());
      if (m_goos.truthy(condition_result)) {
        if (current_case.as_pair()->cdr.is_empty_list()) {
          return get_none();
        }
        // got a match!
        result = get_none();

        for_each_in_list(current_case.as_pair()->cdr, [&](const Object& o) {
          result = compile_error_guard(o, env);
          if (!dynamic_cast<None*>(result)) {
            result = result->to_reg(o, env);
          }
        });
        return result;
      } else {
        // no match, continue.
        lst = lst.as_pair()->cdr;
      }
    } else if (lst.is_empty_list()) {
      return get_none();
    } else {
      throw_compiler_error(form, "malformed #cond");
    }
  }
}

/*!
 * Compile (quote x) or 'x forms.
 * Current only supports 'thing or '(). Static lists/pairs should be added at some point.
 */
Val* Compiler::compile_quote(const goos::Object& form, const goos::Object& rest, Env* env) {
  auto args = get_va(form, rest);
  va_check(form, args, {{}}, {});
  auto thing = args.unnamed.at(0);
  switch (thing.type) {
    case goos::ObjectType::SYMBOL:
      return compile_get_sym_obj(thing.as_symbol()->name, env);
    case goos::ObjectType::EMPTY_LIST: {
      auto empty_pair = compile_get_sym_obj("_empty_", env);
      empty_pair->set_type(m_ts.make_typespec("pair"));
      return empty_pair;
    }
    case goos::ObjectType::PAIR:
      return compile_static_pair(thing, env);
    default:
      throw_compiler_error(form, "Quote is not yet implemented for {}.", thing.print());
  }
  return get_none();
}

Val* Compiler::compile_define_constant(const goos::Object& form,
                                       const goos::Object& _rest,
                                       Env* env,
                                       bool goos,
                                       bool goal) {
  auto rest = &_rest;
  (void)env;
  if (!rest->is_pair()) {
    throw_compiler_error(form, "invalid constant definition");
  }

  auto sym = pair_car(*rest).as_symbol();
  rest = &pair_cdr(*rest);
  auto value = pair_car(*rest);

  rest = &rest->as_pair()->cdr;
  if (!rest->is_empty_list()) {
    throw_compiler_error(form, "invalid constant definition");
  }

  // GOAL constant
  if (goal) {
    if (m_symbol_types.find(sym->name) != m_symbol_types.end()) {
      throw_compiler_error(form,
                           "Cannot define {} as a constant because "
                           "it is already the name of a symbol of type {}",
                           sym->name, m_symbol_types.at(sym->name).print());
    }
    m_global_constants[sym] = value;
  }

  // GOOS constant
  if (goos) {
    m_goos.global_environment.as_env()->vars[sym] = value;
  }

  m_symbol_info.add_constant(sym->name, form);

  return get_none();
}

/*!
 * Compile defglobalconstant forms, which define a constant in both GOOS and GOAL.
 */
Val* Compiler::compile_defglobalconstant(const goos::Object& form,
                                         const goos::Object& rest,
                                         Env* env) {
  return compile_define_constant(form, rest, env, true, true);
}

/*!
 * Compile a defconstant form, which defines a constant that is only in GOAL.
 */
Val* Compiler::compile_defconstant(const goos::Object& form, const goos::Object& rest, Env* env) {
  return compile_define_constant(form, rest, env, false, true);
}

/*!
 * Compile an "mlet" scoped constant/symbol macro form
 */
Val* Compiler::compile_mlet(const goos::Object& form, const goos::Object& rest, Env* env) {
  auto defs = pair_car(rest);
  auto body = pair_cdr(rest);

  auto fenv = env->function_env();
  auto menv = fenv->alloc_env<SymbolMacroEnv>(env);

  for_each_in_list(defs, [&](const goos::Object& o) {
    auto def_args = get_va(form, o);
    va_check(form, def_args, {goos::ObjectType::SYMBOL, {}}, {});
    menv->macros[def_args.unnamed.at(0).as_symbol()] = def_args.unnamed.at(1);
  });

  Val* result = get_none();
  for_each_in_list(body, [&](const goos::Object& o) {
    result = compile_error_guard(o, menv);
    if (!dynamic_cast<None*>(result)) {
      result = result->to_reg(o, menv);
    }
  });
  return result;
}

bool Compiler::expand_macro_once(const goos::Object& src, goos::Object* out, Env*) {
  if (!src.is_pair()) {
    return false;
  }

  auto first = src.as_pair()->car;
  auto rest = src.as_pair()->cdr;
  if (!first.is_symbol()) {
    return false;
  }

  goos::Object macro_obj;
  if (!try_getting_macro_from_goos(first, &macro_obj)) {
    return false;
  }

  auto macro = macro_obj.as_macro();
  Arguments args = m_goos.get_args(src, rest, macro->args);
  auto mac_env_obj = EnvironmentObject::make_new();
  auto mac_env = mac_env_obj.as_env_ptr();
  mac_env->parent_env = m_goos.global_environment.as_env_ptr();
  m_goos.set_args_in_env(src, args, macro->args, mac_env);

  auto goos_result = m_goos.eval_list_return_last(macro->body, macro->body, mac_env);
  // make the macro expanded form point to the source where the macro was used for error messages.
  // m_goos.reader.db.inherit_info(src, goos_result);

  *out = goos_result;
  return true;
}

goos::Object Compiler::expand_macro_completely(const goos::Object& src, Env* env) {
  goos::Object result = src;
  while (expand_macro_once(result, &result, env)) {
  }
  return result;
}