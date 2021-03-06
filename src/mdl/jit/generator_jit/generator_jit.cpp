/******************************************************************************
 * Copyright (c) 2012-2018, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/

#include "pch.h"

#include <mi/base/handle.h>

#include <llvm/IR/Module.h>

#include "mdl/compiler/compilercore/compilercore_errors.h"
#include "mdl/compiler/compilercore/compilercore_mdl.h"
#include "mdl/compiler/compilercore/compilercore_tools.h"
#include "mdl/codegenerators/generator_dag/generator_dag_tools.h"
#include "mdl/codegenerators/generator_code/generator_code_hash.h"

#include "generator_jit.h"
#include "generator_jit_code_printer.h"
#include "generator_jit_generated_code.h"
#include "generator_jit_llvm.h"

namespace mi {
namespace mdl {

// Allow impl_cast on Link_unit
template<>
Link_unit_jit const *impl_cast(ILink_unit const *unit) {
    return static_cast<Link_unit_jit const *>(unit);
}

// Creates a JIT code generator.
Code_generator_jit *Code_generator_jit::create_code_generator(
    IAllocator *alloc,
    MDL        *mdl)
{
    Allocator_builder builder(alloc);

    return builder.create<Code_generator_jit>(alloc, mdl, mdl->get_jitted_code());
}

// Constructor.
Code_generator_jit::Code_generator_jit(
    IAllocator  *alloc,
    MDL         *mdl,
    Jitted_code *jitted_code)
: Base(alloc, mdl)
, m_builder(alloc)
, m_jitted_code(mi::base::make_handle_dup(jitted_code))
{
    m_options.add_option(
        MDL_JIT_OPTION_OPT_LEVEL,
        "2",
        "The optimization level of the JIT code generator");
    m_options.add_option(
        MDL_JIT_OPTION_FAST_MATH,
        "true",
        "Enables unsafe math optimizations of the JIT code generator");
    m_options.add_option(
        MDL_JIT_OPTION_DISABLE_EXCEPTIONS,
        "false",
        "Disable exception handling in the generated code");
    m_options.add_option(
        MDL_JIT_OPTION_ENABLE_RO_SEGMENT,
        "false",
        "Enable the creation of a read-only data segment");
    m_options.add_option(
        MDL_JIT_OPTION_WRITE_BITCODE,
        "false",
        "Generate LLVM bitcode instead of LLVM IR code");
    m_options.add_option(
        MDL_JIT_OPTION_LINK_LIBDEVICE,
        "true",
        "Link libdevice into PTX module");
    m_options.add_option(
        MDL_JIT_OPTION_USE_BITANGENT,
        "false",
        "Use bitangent instead of tangent_u, tangent_v in the generated MDL core state");
    m_options.add_option(
        MDL_JIT_OPTION_INCLUDE_UNIFORM_STATE,
        "false",
        "Include the uniform state in the generated MDL core state");
    m_options.add_option(
        MDL_JIT_OPTION_TEX_LOOKUP_CALL_MODE,
        "vtable",
        "The mode for texture lookup functions on GPU (vtable, direct_call or optix_cp)");
    m_options.add_binary_option(
        MDL_JIT_BINOPTION_LLVM_STATE_MODULE,
        "Use this user-specified LLVM implementation for the MDL state module");
    m_options.add_option(
        MDL_JIT_OPTION_MAP_STRINGS_TO_IDS,
        "false",
        "Map string constants to identifiers");
}

// Get the name of the target language.
char const *Code_generator_jit::get_target_language() const
{
    return "executable";
}

// Acquires a const interface.
mi::base::IInterface const *Code_generator_jit::get_interface(
    mi::base::Uuid const &interface_id) const
{
    if (interface_id == IPrinter_interface::IID()) {
        return m_builder.create<JIT_code_printer>(m_builder.get_allocator());
    }
    return Base::get_interface(interface_id);
}

// Compile a whole module.
IGenerated_code_executable *Code_generator_jit::compile(
    IModule const    *module,
    Compilation_mode mode)
{
    Generated_code_jit *result = m_builder.create<Generated_code_jit>(
        m_builder.get_allocator(),
        m_jitted_code.get(),
        module->get_filename());

    if (mode == CM_NATIVE) {
        result->compile_module_to_llvm(module, m_options);
    } else {
        result->compile_module_to_ptx(module, m_options);
    }

    return result;
}

// Compile a lambda function using the JIT into an environment (shader) of a scene.
IGenerated_code_lambda_function *Code_generator_jit::compile_into_environment(
    ILambda_function const    *ilambda,
    ICall_name_resolver const *resolver)
{
    Lambda_function const *lambda = impl_cast<Lambda_function>(ilambda);
    if (lambda == NULL)
        return NULL;

    if (lambda->get_body() == NULL || lambda->get_root_expr_count() != 0) {
        // not a simple lambda
        return NULL;
    }

    IAllocator        *alloc = get_allocator();
    Allocator_builder builder(alloc);

    Generated_code_lambda_function *code =
        builder.create<Generated_code_lambda_function>(m_jitted_code.get());

    Generated_code_lambda_function::Lambda_res_manag res_manag(*code, /*resource_map=*/NULL);

    mi::base::Handle<MDL> compiler(lambda->get_compiler());

    // environment is executed on CPU only
    LLVM_code_generator code_gen(
        m_jitted_code.get(), compiler.get(), code->access_messages(), code->get_llvm_context(),
        /*ptx_mode=*/false,
        Type_mapper::TM_NATIVE_X86,
        /*sm_version=*/0,
        /*has_texture_handler=*/true,
        Type_mapper::SSM_ENVIRONMENT,
        /*num_texture_spaces=*/0,
        /*num_texture_results=*/0,
        m_options,
        /*incremental=*/false,
        get_state_mapping(),
        &res_manag, /*enable_debug=*/false);

    llvm::Function *func = code_gen.compile_environment_lambda(
        /*incremental=*/false, *lambda, resolver);
    if (func != NULL) {
        llvm::Module *module = func->getParent();

        code_gen.jit_compile(module);
        code->set_llvm_module(module);

        // gen the entry point
        void *entry_point = code_gen.get_entry_point(func);
        code->add_entry_point(entry_point);

        // copy the render state usage
        code->set_render_state_usage(code_gen.get_render_state_usage());

        // copy the string constant table.
        for (size_t i = 0, n = code_gen.get_string_constant_count(); i < n; ++i) {
            code->add_mapped_string(code_gen.get_string_constant(i), i);
        }
    } else if (code->access_messages().get_error_message_count() == 0) {
        // on failure, ensure that the code contains an error message
        code_gen.error(INTERNAL_JIT_BACKEND_ERROR, "Compiling environment function failed");
    }
    return code;
}

namespace {

/// Helper class to handle resources in const functions.
class Const_function_enumerator : public ILambda_resource_enumerator {
public:
    /// Constructor.
    ///
    /// \param attr    resource attribute requester interface
    /// \param lambda  currently processed lambda function
    Const_function_enumerator(
        ILambda_resource_attribute *attr,
        Lambda_function            &lambda)
    : m_attr(attr)
    , m_lambda(lambda)
    , m_tex_idx(0)
    , m_lp_idx(0)
    , m_bm_idx(0)
    {
    }

    /// Called for a texture resource.
    ///
    /// \param v  the texture resource or an invalid ref
    void texture(IValue const *v) MDL_FINAL
    {
        bool valid = false;
        int width = 0, height = 0, depth = 0;
        if (IValue_resource const *r = as<IValue_resource>(v))
            m_attr->get_texture_attributes(r, valid, width, height, depth);
        m_lambda.map_tex_resource(v, m_tex_idx++, valid, width, height, depth);
    }

    /// Called for a light profile resource.
    ///
    /// \param v  the light profile resource or an invalid ref
    void light_profile(IValue const *v) MDL_FINAL
    {
        bool valid = false;
        float power = 0.0f, maximum = 0.0f;
        if (IValue_resource const *r = as<IValue_resource>(v))
            m_attr->get_light_profile_attributes(r, valid, power, maximum);
        m_lambda.map_lp_resource(v, m_lp_idx++, valid, power, maximum);
    }

    /// Called for a bsdf measurement resource.
    ///
    /// \param v  the bsdf measurement resource or an invalid_ref
    void bsdf_measurement(IValue const *v) MDL_FINAL
    {
        bool valid = false;
        if (IValue_resource const *r = as<IValue_resource>(v))
            m_attr->get_bsdf_measurement_attributes(r, valid);
        m_lambda.map_bm_resource(v, m_bm_idx++, valid);
    }

private:
    /// The resource attribute requester.
    ILambda_resource_attribute *m_attr;

    /// The processed lambda function.
    Lambda_function            &m_lambda;

    /// Current texture index.
    size_t m_tex_idx;

    /// Current light profile index.
    size_t m_lp_idx;

    /// Current bsdf measurement index.
    size_t m_bm_idx;
};

} // anonymous

// Compile a lambda function using the JIT into a constant function.
IGenerated_code_lambda_function *Code_generator_jit::compile_into_const_function(
    ILambda_function const    *ilambda,
    ICall_name_resolver const  *resolver,
    ILambda_resource_attribute *attr,
    Float4_struct const        world_to_object[4],
    Float4_struct const        object_to_world[4],
    int                        object_id)
{
    Lambda_function const *lambda = impl_cast<Lambda_function>(ilambda);
    if (lambda == NULL)
        return NULL;

    DAG_node const *body = lambda->get_body();
    if (body == NULL || lambda->get_root_expr_count() != 0) {
        // not a simple lambda
        return NULL;
    }

    if (lambda->may_use_varying_state(resolver, body)) {
        // currently we do not support any state access in const functions
        return NULL;
    }

    if (lambda->get_parameter_count() != 0) {
        // FIXME: Add support for class-compilation for const functions
        //    (const functions are not available via Neuray API, only material converter uses them)
        MDL_ASSERT(!"Class-compilation is not supported for const functions, yet");
        return NULL;
    }

    // FIXME: ugly, but ok for now: request all resource meta data through the attr interface
    // a better solution would be to do this outside this compile call
    Const_function_enumerator enumerator(attr, *const_cast<Lambda_function *>(lambda));
    lambda->enumerate_resources(enumerator, body);

    IAllocator        *alloc = get_allocator();
    Allocator_builder builder(alloc);

    mi::base::Handle<Generated_code_lambda_function> code(
        builder.create<Generated_code_lambda_function>(m_jitted_code.get()));

    Generated_code_lambda_function::Lambda_res_manag res_manag(*code, /*resource_map=*/NULL);

    mi::base::Handle<MDL> compiler(lambda->get_compiler());

    // const function are evaluated on the CPU only
    LLVM_code_generator code_gen(
        m_jitted_code.get(), compiler.get(), code->access_messages(), code->get_llvm_context(),
        /*ptx_mode=*/false,
        Type_mapper::TM_NATIVE_X86,
        /*sm_version=*/0,
        /*has_tex_handler=*/true,
        Type_mapper::SSM_NO_STATE,
        /*num_texture_spaces=*/0,
        /*num_texture_results=*/0,
        m_options,
        /*incremental=*/false,
        get_state_mapping(),
        &res_manag,
        /*enable_debug=*/false);

    if (llvm::Function *func = code_gen.compile_const_lambda(
            *lambda, resolver, attr, world_to_object, object_to_world, object_id))
    {
        llvm::Module *module = func->getParent();
        code_gen.jit_compile(module);
        code->set_llvm_module(module);

        // gen the entry point
        void *entry_point = code_gen.get_entry_point(func);
        code->add_entry_point(entry_point);

        // copy the render state usage
        code->set_render_state_usage(code_gen.get_render_state_usage());

        // create the argument block layout if any arguments are captured
        if (code_gen.get_captured_arguments_llvm_type() != NULL) {
            mi::base::Handle<Generated_code_value_layout> layout(
                builder.create<Generated_code_value_layout>(alloc, &code_gen));
            code->add_captured_arguments_layout(layout.get());
        }
    } else if (code->access_messages().get_error_message_count() == 0) {
        // on failure, ensure that the code contains an error message
        code_gen.error(INTERNAL_JIT_BACKEND_ERROR, "Compiling const function failed");
    }

    code->retain();
    return code.get();
}

// Compile a lambda switch function having several roots using the JIT into a
// function computing one of the root expressions.
IGenerated_code_lambda_function *Code_generator_jit::compile_into_switch_function(
    ILambda_function const    *ilambda,
    ICall_name_resolver const *resolver,
    unsigned                  num_texture_spaces,
    unsigned                  num_texture_results)
{
    Lambda_function const *lambda = impl_cast<Lambda_function>(ilambda);
    if (lambda == NULL)
        return NULL;

    if (lambda->get_root_expr_count() < 1) {
        // there must be at least one root
        return NULL;
    }

    // automatically activate deactivate the option if the state is set
    bool uses_ustate = lambda->is_uniform_state_set();
    m_options.set_option(
        MDL_JIT_OPTION_INCLUDE_UNIFORM_STATE, uses_ustate ? "false" : "true");

    IAllocator        *alloc = get_allocator();
    Allocator_builder builder(alloc);

    Generated_code_lambda_function *code =
        builder.create<Generated_code_lambda_function>(m_jitted_code.get());

    Generated_code_lambda_function::Lambda_res_manag res_manag(
        *code, &lambda->get_resource_attribute_map());

    mi::base::Handle<MDL> compiler(lambda->get_compiler());

    // switch functions are used in the core and for displacement, only in the later case
    // a texture handler is available
    LLVM_code_generator code_gen(
        m_jitted_code.get(), compiler.get(), code->access_messages(), code->get_llvm_context(),
        /*ptx_mode=*/false,
        Type_mapper::TM_NATIVE_X86,
        /*sm_version=*/0,
        /*has_tex_handler=*/lambda->get_execution_context() != ILambda_function::LEC_CORE,
        Type_mapper::SSM_CORE,
        num_texture_spaces,
        num_texture_results,
        m_options,
        /*incremental=*/false,
        get_state_mapping(),
        &res_manag, /*enable_debug=*/false);

    // Enable the read-only data segment
    code_gen.enable_ro_data_segment();

    llvm::Function *func = code_gen.compile_switch_lambda(
        /*incremental=*/false, *lambda, resolver);
    if (func != NULL) {
        llvm::Module *module = func->getParent();

        code_gen.jit_compile(module);
        code->set_llvm_module(module);

        size_t data_size = 0;
        char const *data = reinterpret_cast<char const *>(code_gen.get_ro_segment(data_size));

        code->set_ro_segment(data, data_size);

        // gen the entry point
        void *entry_point = code_gen.get_entry_point(func);
        code->add_entry_point(entry_point);

        // copy the render state usage
        code->set_render_state_usage(code_gen.get_render_state_usage());

        // create the argument block layout if any arguments are captured
        if (code_gen.get_captured_arguments_llvm_type() != NULL) {
            mi::base::Handle<Generated_code_value_layout> layout(
                builder.create<Generated_code_value_layout>(alloc, &code_gen));
            code->add_captured_arguments_layout(layout.get());
        }

        // copy the string constant table.
        for (size_t i = 0, n = code_gen.get_string_constant_count(); i < n; ++i) {
            code->add_mapped_string(code_gen.get_string_constant(i), i);
        }
    } else if (code->access_messages().get_error_message_count() == 0) {
        // on failure, ensure that the code contains an error message
        code_gen.error(INTERNAL_JIT_BACKEND_ERROR, "Compiling switch function failed");
    }
    return code;
}

// Compile a lambda switch function having several roots using the JIT into a
// function computing one of the root expressions for execution on the GPU.
IGenerated_code_executable *Code_generator_jit::compile_into_switch_function_for_gpu(
    ILambda_function const    *ilambda,
    ICall_name_resolver const *resolver,
    unsigned                  num_texture_spaces,
    unsigned                  num_texture_results,
    unsigned                  sm_version)
{
    Lambda_function const *lambda = impl_cast<Lambda_function>(ilambda);
    if (lambda == NULL)
        return NULL;

    if (lambda->get_root_expr_count() < 1) {
        // there must be at least one root
        return NULL;
    }

    // automatically activate deactivate the option if the state is set
    bool uses_ustate = lambda->is_uniform_state_set();
    m_options.set_option(
        MDL_JIT_OPTION_INCLUDE_UNIFORM_STATE, uses_ustate ? "false" : "true");

    IAllocator        *alloc = get_allocator();
    Allocator_builder builder(alloc);

    Generated_code_source *code = builder.create<Generated_code_source>(
        alloc, IGenerated_code_executable::CK_PTX);

    Generated_code_source::Source_res_manag res_manag(alloc, &lambda->get_resource_attribute_map());

    llvm::LLVMContext llvm_context;
    mi::base::Handle<MDL> compiler(lambda->get_compiler());

    // GPU switch functions are used in the core only
    LLVM_code_generator code_gen(
        m_jitted_code.get(), compiler.get(), code->access_messages(), llvm_context,
        /*ptx_mode=*/true,
        Type_mapper::TM_PTX,
        sm_version,
        /*has_tex_handler=*/false,
        Type_mapper::SSM_CORE,
        num_texture_spaces,
        num_texture_results,
        m_options,
        /*incremental=*/false,
        get_state_mapping(),
        &res_manag, /*enable_debug=*/false);

    // Enable the read-only data segment
    code_gen.enable_ro_data_segment();

    llvm::Function *func = code_gen.compile_switch_lambda(
        /*incremental=*/false, *lambda, resolver);
    if (func != NULL) {
        llvm::Module *module = func->getParent();

        code_gen.ptx_compile(module, code->access_src_code());

        // it's now save to drop this module
        code_gen.drop_llvm_module(module);

        size_t data_size = 0;
        char const *data = reinterpret_cast<char const *>(code_gen.get_ro_segment(data_size));

        code->set_ro_segment(data, data_size);

        // copy the render state usage
        code->set_render_state_usage(code_gen.get_render_state_usage());

        // create the argument block layout if any arguments are captured
        if (code_gen.get_captured_arguments_llvm_type() != NULL) {
            mi::base::Handle<Generated_code_value_layout> layout(
                builder.create<Generated_code_value_layout>(alloc, &code_gen));
            code->add_captured_arguments_layout(layout.get());
        }

        // copy the string constant table.
        for (size_t i = 0, n = code_gen.get_string_constant_count(); i < n; ++i) {
            code->add_mapped_string(code_gen.get_string_constant(i), i);
        }
    } else if (code->access_messages().get_error_message_count() == 0) {
        // on failure, ensure that the code contains an error message
        code_gen.error(INTERNAL_JIT_BACKEND_ERROR, "Compiling GPU switch function failed");
    }
    return code;
}

// Compile a lambda function into a generic function using the JIT.
IGenerated_code_lambda_function *Code_generator_jit::compile_into_generic_function(
    ILambda_function const    *ilambda,
    ICall_name_resolver const *resolver,
    unsigned                  num_texture_spaces,
    unsigned                  num_texture_results,
    ILambda_call_transformer  *transformer)
{
    Lambda_function const *lambda = impl_cast<Lambda_function>(ilambda);
    if (lambda == NULL)
        return NULL;

    if (lambda->get_body() == NULL || lambda->get_root_expr_count() != 0) {
        // not a simple lambda
        return NULL;
    }

    // automatically activate deactivate the option if the state is set
    m_options.set_option(
        MDL_JIT_OPTION_INCLUDE_UNIFORM_STATE, lambda->is_uniform_state_set() ? "false" : "true");

    IAllocator        *alloc = get_allocator();
    Allocator_builder builder(alloc);

    mi::base::Handle<Generated_code_lambda_function> code(
        builder.create<Generated_code_lambda_function>(m_jitted_code.get()));

    Generated_code_lambda_function::Lambda_res_manag res_manag(*code, /*resource_map=*/NULL);

    // make sure, all registered resources are also known to the Lambda_res_manag
    res_manag.import_from_resource_attribute_map(&lambda->get_resource_attribute_map());

    mi::base::Handle<MDL> compiler(lambda->get_compiler());

    // generic functions are for CPU only
    LLVM_code_generator code_gen(
        m_jitted_code.get(), compiler.get(), code->access_messages(), code->get_llvm_context(),
        /*ptx_mode=*/false,
        Type_mapper::TM_NATIVE_X86,
        /*sm_version=*/0,
        /*has_tex_handler=*/true,
        Type_mapper::SSM_CORE,
        num_texture_spaces,
        num_texture_results,
        m_options,
        /*incremental=*/false,
        get_state_mapping(),
        &res_manag, /*enable_debug=*/false);

    llvm::Function *func = code_gen.compile_generic_lambda(
        /*incremental=*/false, *lambda, resolver, transformer);
    if (func != NULL) {
        llvm::Module *module = func->getParent();
        code_gen.jit_compile(module);
        code->set_llvm_module(module);

        // gen the entry point
        void *entry_point = code_gen.get_entry_point(func);
        code->add_entry_point(entry_point);

        // copy the render state usage
        code->set_render_state_usage(code_gen.get_render_state_usage());

        // create the argument block layout if any arguments are captured
        if (code_gen.get_captured_arguments_llvm_type() != NULL) {
            mi::base::Handle<Generated_code_value_layout> layout(
                builder.create<Generated_code_value_layout>(alloc, &code_gen));
            code->add_captured_arguments_layout(layout.get());
        }

        // copy the string constant table.
        for (size_t i = 0, n = code_gen.get_string_constant_count(); i < n; ++i) {
            code->add_mapped_string(code_gen.get_string_constant(i), i);
        }
    } else if (code->access_messages().get_error_message_count() == 0) {
        // on failure, ensure that the code contains an error message
        code_gen.error(INTERNAL_JIT_BACKEND_ERROR, "Compiling generic function failed");
    }
    code->retain();
    return code.get();
}

// Compile a lambda function into a LLVM-IR using the JIT.
IGenerated_code_executable *Code_generator_jit::compile_into_llvm_ir(
    ILambda_function const    *ilambda,
    ICall_name_resolver const *resolver,
    unsigned                  num_texture_spaces,
    unsigned                  num_texture_results,
    bool                      enable_simd)
{
    Lambda_function const *lambda = impl_cast<Lambda_function>(ilambda);
    if (lambda == NULL)
        return NULL;

    DAG_node const *body = lambda->get_body();
    if (body == NULL && lambda->get_root_expr_count() < 1) {
        // there must be at least one root or a body
        return NULL;
    }

    // automatically activate deactivate the option if the state is set
    m_options.set_option(
        MDL_JIT_OPTION_INCLUDE_UNIFORM_STATE, lambda->is_uniform_state_set() ? "false" : "true");

    IAllocator        *alloc = get_allocator();
    Allocator_builder builder(alloc);

    Generated_code_source *code = builder.create<Generated_code_source>(
        alloc, IGenerated_code_executable::CK_LLVM_IR);

    Generated_code_source::Source_res_manag res_manag(alloc, &lambda->get_resource_attribute_map());

    llvm::LLVMContext llvm_context;
    mi::base::Handle<MDL> compiler(lambda->get_compiler());

    LLVM_code_generator code_gen(
        m_jitted_code.get(), compiler.get(), code->access_messages(), llvm_context,
        /*ptx_mode=*/false,
        enable_simd ? Type_mapper::TM_BIG_VECTORS : Type_mapper::TM_ALL_SCALAR,
        /*sm_version=*/0,
        /*has_tex_handler=*/true,
        Type_mapper::SSM_CORE,
        num_texture_spaces,
        num_texture_results,
        m_options,
        /*incremental=*/false,
        get_state_mapping(),
        &res_manag, /*enable_debug=*/false);

    // enable name mangling
    code_gen.enable_name_mangling();

    // Enable the read-only data segment
    code_gen.enable_ro_data_segment();

    llvm::Function *func = NULL;
    if (body != NULL) {
        func = code_gen.compile_generic_lambda(
            /*incremental=*/false, *lambda, resolver, /*transformer=*/NULL);
    } else {
        func = code_gen.compile_switch_lambda(
            /*incremental=*/false, *lambda, resolver);
    }
    if (func != NULL) {
        llvm::Module *module = func->getParent();

        if (m_options.get_bool_option(MDL_JIT_OPTION_WRITE_BITCODE))
            code_gen.llvm_bc_compile(module, code->access_src_code());
        else
            code_gen.llvm_ir_compile(module, code->access_src_code());

        // it's now save to drop this module
        code_gen.drop_llvm_module(module);

        size_t data_size = 0;
        char const *data = reinterpret_cast<char const *>(code_gen.get_ro_segment(data_size));

        code->set_ro_segment(data, data_size);

        // create the argument block layout if any arguments are captured
        if (code_gen.get_captured_arguments_llvm_type() != NULL) {
            mi::base::Handle<Generated_code_value_layout> layout(
                builder.create<Generated_code_value_layout>(alloc, &code_gen));
            code->add_captured_arguments_layout(layout.get());
        }

        // copy the string constant table.
        for (size_t i = 0, n = code_gen.get_string_constant_count(); i < n; ++i) {
            code->add_mapped_string(code_gen.get_string_constant(i), i);
        }
    } else if (code->access_messages().get_error_message_count() == 0) {
        // on failure, ensure that the code contains an error message
        code_gen.error(INTERNAL_JIT_BACKEND_ERROR, "Compiling lambda function into LLVM IR failed");
    }
    return code;
}


// Compile a distribution function into native code using the JIT.
IGenerated_code_executable *Code_generator_jit::compile_distribution_function_cpu(
    IDistribution_function const *idist_func,
    ICall_name_resolver const    *resolver,
    unsigned                     num_texture_spaces,
    unsigned                     num_texture_results)
{
    Distribution_function const *dist_func = impl_cast<Distribution_function>(idist_func);

    mi::base::Handle<ILambda_function> root_lambda_handle(dist_func->get_main_df());
    Lambda_function const *root_lambda = impl_cast<Lambda_function>(root_lambda_handle.get());

    // always expect the uniform state to be part of the MDL SDK state structure
    m_options.set_option(MDL_JIT_OPTION_INCLUDE_UNIFORM_STATE, "true");

    IAllocator        *alloc = get_allocator();
    Allocator_builder builder(alloc);

    mi::base::Handle<Generated_code_lambda_function> code(
        builder.create<Generated_code_lambda_function>(m_jitted_code.get()));

    Generated_code_lambda_function::Lambda_res_manag res_manag(*code, /*resource_map=*/NULL);

    // make sure, all registered resources are also known to the Lambda_res_manag
    res_manag.import_from_resource_attribute_map(&root_lambda->get_resource_attribute_map());

    mi::base::Handle<MDL> compiler(dist_func->get_compiler());

    LLVM_code_generator code_gen(
        m_jitted_code.get(), compiler.get(), code->access_messages(), code->get_llvm_context(),
        /*ptx_mode=*/false,
        Type_mapper::TM_NATIVE_X86,
        /*sm_version=*/0,
        /*has_tex_handler=*/true,
        Type_mapper::SSM_CORE,
        num_texture_spaces,
        num_texture_results,
        m_options,
        /*incremental=*/false,
        get_state_mapping(),
        &res_manag, /*enable_debug=*/false);

    LLVM_code_generator::Function_vector llvm_funcs(get_allocator());
    llvm::Module *module = code_gen.compile_distribution_function(
        /*incremental=*/false, *dist_func, resolver, llvm_funcs);

    if (module != NULL) {
        code_gen.jit_compile(module);
        code->set_llvm_module(module);

        // add all generated functions (init, sample, evaluate, pdf) as entrypoints
        for (LLVM_code_generator::Function_vector::const_iterator it = llvm_funcs.begin(),
                end = llvm_funcs.end(); it != end; ++it) {
            code->add_entry_point(code_gen.get_entry_point(*it));
        }

        // copy the render state usage
        code->set_render_state_usage(code_gen.get_render_state_usage());

        // create the argument block layout if any arguments are captured
        if (code_gen.get_captured_arguments_llvm_type() != NULL) {
            mi::base::Handle<Generated_code_value_layout> layout(
                builder.create<Generated_code_value_layout>(alloc, &code_gen));
            code->add_captured_arguments_layout(layout.get());
        }
    } else if (code->access_messages().get_error_message_count() == 0) {
        // on failure, ensure that the code contains an error message
        code_gen.error(INTERNAL_JIT_BACKEND_ERROR, "Compiling CPU DF function failed");
    }
    code->retain();
    return code.get();
}

// Compile a distribution function into a PTX using the JIT.
IGenerated_code_executable *Code_generator_jit::compile_distribution_function_gpu(
    IDistribution_function const *idist_func,
    ICall_name_resolver const    *resolver,
    unsigned                     num_texture_spaces,
    unsigned                     num_texture_results,
    unsigned                     sm_version,
    bool                         ptx_output)
{
    Distribution_function const *dist_func = impl_cast<Distribution_function>(idist_func);

    mi::base::Handle<ILambda_function> root_lambda_handle(dist_func->get_main_df());
    Lambda_function const *root_lambda = impl_cast<Lambda_function>(root_lambda_handle.get());

    // always expect the uniform state to be part of the MDL SDK state structure
    m_options.set_option(MDL_JIT_OPTION_INCLUDE_UNIFORM_STATE, "true");

    IAllocator        *alloc = get_allocator();
    Allocator_builder builder(alloc);

    Generated_code_source *code = builder.create<Generated_code_source>(
        alloc,
        ptx_output ? IGenerated_code_executable::CK_PTX : IGenerated_code_executable::CK_LLVM_IR);

    Generated_code_source::Source_res_manag res_manag(
        alloc, &root_lambda->get_resource_attribute_map());

    llvm::LLVMContext llvm_context;
    mi::base::Handle<MDL> compiler(dist_func->get_compiler());

    LLVM_code_generator code_gen(
        m_jitted_code.get(), compiler.get(), code->access_messages(), llvm_context,
        /*ptx_mode=*/true,
        Type_mapper::TM_PTX,
        sm_version,
        /*has_tex_handler=*/false,
        Type_mapper::SSM_CORE,
        num_texture_spaces,
        num_texture_results,
        m_options,
        /*incremental=*/false,
        get_state_mapping(),
        &res_manag, /*enable_debug=*/false);

    // enable name mangling
    code_gen.enable_name_mangling();

    // Enable the read-only data segment
    code_gen.enable_ro_data_segment();

    LLVM_code_generator::Function_vector llvm_funcs(get_allocator());
    llvm::Module *module = code_gen.compile_distribution_function(
        /*incremental=*/false, *dist_func, resolver, llvm_funcs);

    if (module != NULL) {
        if (ptx_output) {
            code_gen.ptx_compile(module, code->access_src_code());
        }
        else {
            if (m_options.get_bool_option(MDL_JIT_OPTION_WRITE_BITCODE))
                code_gen.llvm_bc_compile(module, code->access_src_code());
            else
                code_gen.llvm_ir_compile(module, code->access_src_code());
        }

        // it's now save to drop this module
        code_gen.drop_llvm_module(module);

        size_t data_size = 0;
        char const *data = reinterpret_cast<char const *>(code_gen.get_ro_segment(data_size));

        code->set_ro_segment(data, data_size);

        // copy the render state usage
        code->set_render_state_usage(code_gen.get_render_state_usage());

        // create the argument block layout if any arguments are captured
        if (code_gen.get_captured_arguments_llvm_type() != NULL) {
            mi::base::Handle<Generated_code_value_layout> layout(
                builder.create<Generated_code_value_layout>(alloc, &code_gen));
            code->add_captured_arguments_layout(layout.get());
        }

        // copy the string constant table.
        for (size_t i = 0, n = code_gen.get_string_constant_count(); i < n; ++i) {
            code->add_mapped_string(code_gen.get_string_constant(i), i);
        }
    } else if (code->access_messages().get_error_message_count() == 0) {
        // on failure, ensure that the code contains an error message
        code_gen.error(INTERNAL_JIT_BACKEND_ERROR, "Compiling GPU DF function failed");
    }
    return code;
}

// Compile a lambda function into a PTX using the JIT.
IGenerated_code_executable *Code_generator_jit::compile_into_ptx(
    ICode_cache               *code_cache,
    ILambda_function const    *ilambda,
    ICall_name_resolver const *resolver,
    unsigned                  num_texture_spaces,
    unsigned                  num_texture_results,
    unsigned                  sm_version,
    bool                      ptx_output)
{
    Lambda_function const *lambda = impl_cast<Lambda_function>(ilambda);
    if (lambda == NULL)
        return NULL;

    DAG_node const *body = lambda->get_body();
    if (body == NULL && lambda->get_root_expr_count() < 1) {
        // there must be at least one root or a body
        return NULL;
    }

    IAllocator        *alloc = get_allocator();
    Allocator_builder builder(alloc);

    Generated_code_source *code = builder.create<Generated_code_source>(
        alloc,
        ptx_output ? IGenerated_code_executable::CK_PTX : IGenerated_code_executable::CK_LLVM_IR);

    unsigned char cache_key[16];

    if (code_cache != NULL) {
        MD5_hasher hasher;

        DAG_hash const *hash = lambda->get_hash();

        // set the generators name
        hasher.update("JIT");

        hasher.update(lambda->get_name());
        hasher.update(hash->data(), hash->size());
        hasher.update(sm_version);

        hasher.update(ptx_output);

        // Beware: the selected options change the generated code, hence we must include them into
        // the key
        hasher.update(lambda->get_execution_context() == ILambda_function::LEC_ENVIRONMENT ?
            Type_mapper::SSM_ENVIRONMENT : Type_mapper::SSM_CORE);
        hasher.update(num_texture_spaces),
        hasher.update(num_texture_results),
        hasher.update(m_options.get_string_option(MDL_CG_OPTION_INTERNAL_SPACE));
        hasher.update(m_options.get_int_option(MDL_JIT_OPTION_OPT_LEVEL));
        hasher.update(m_options.get_bool_option(MDL_JIT_OPTION_FAST_MATH));
        hasher.update(m_options.get_bool_option(MDL_JIT_OPTION_DISABLE_EXCEPTIONS));
        hasher.update(m_options.get_bool_option(MDL_JIT_OPTION_ENABLE_RO_SEGMENT));
        hasher.update(m_options.get_bool_option(MDL_JIT_OPTION_LINK_LIBDEVICE));
        hasher.update(m_options.get_string_option(MDL_JIT_OPTION_TEX_LOOKUP_CALL_MODE));
        hasher.update(m_options.get_bool_option(MDL_JIT_OPTION_MAP_STRINGS_TO_IDS));

        hasher.final(cache_key);

        ICode_cache::Entry const *entry = code_cache->lookup(cache_key);

        if (entry != NULL) {
            // found a hit
            code->access_src_code() = string(entry->code, entry->code_size, alloc);

            code->set_ro_segment(entry->const_seg, entry->const_seg_size);

            // only add a captured arguments layout, if it's non-empty
            if (entry->arg_layout_size != 0) {
                mi::base::Handle<Generated_code_value_layout> layout(
                    builder.create<Generated_code_value_layout>(
                        alloc,
                        entry->arg_layout,
                        entry->arg_layout_size,
                        m_options.get_bool_option(MDL_JIT_OPTION_MAP_STRINGS_TO_IDS)));
                code->add_captured_arguments_layout(layout.get());
            }

            code->set_render_state_usage(entry->render_state_usage);

            // copy the string table if any
            for (size_t i = 0; i < entry->mapped_string_size; ++i) {
                code->add_mapped_string(entry->mapped_strings[i], i);
            }

            return code;
        }
    }

    // automatically activate deactivate the option if the state is set
    m_options.set_option(
        MDL_JIT_OPTION_INCLUDE_UNIFORM_STATE, lambda->is_uniform_state_set() ? "false" : "true");

    Generated_code_source::Source_res_manag res_manag(alloc, &lambda->get_resource_attribute_map());

    llvm::LLVMContext llvm_context;
    mi::base::Handle<MDL> compiler(lambda->get_compiler());

    LLVM_code_generator code_gen(
        m_jitted_code.get(), compiler.get(), code->access_messages(), llvm_context,
        /*ptx_mode=*/true,
        Type_mapper::TM_PTX,
        sm_version,
        /*has_tex_handler=*/false,
        lambda->get_execution_context() == ILambda_function::LEC_ENVIRONMENT ?
            Type_mapper::SSM_ENVIRONMENT : Type_mapper::SSM_CORE,
        num_texture_spaces,
        num_texture_results,
        m_options,
        /*incremental=*/false,
        get_state_mapping(),
        &res_manag, /*enable_debug=*/false);

    // enable name mangling
    code_gen.enable_name_mangling();

    // Enable the read-only data segment
    code_gen.enable_ro_data_segment();

    llvm::Function *func = NULL;
    if (body != NULL) {
        func = code_gen.compile_generic_lambda(
            /*incremental=*/false, *lambda, resolver, /*transformer=*/NULL);
    } else {
        func = code_gen.compile_switch_lambda(
            /*incremental=*/false, *lambda, resolver);
    }
    if (func != NULL) {
        llvm::Module *module = func->getParent();

        if (ptx_output) {
            code_gen.ptx_compile(module, code->access_src_code());
        } else {
            if (m_options.get_bool_option(MDL_JIT_OPTION_WRITE_BITCODE))
                code_gen.llvm_bc_compile(module, code->access_src_code());
            else
                code_gen.llvm_ir_compile(module, code->access_src_code());
        }

        // it's now save to drop this module
        code_gen.drop_llvm_module(module);

        // copy the read-only segment
        size_t data_size = 0;
        char const *data = reinterpret_cast<char const *>(code_gen.get_ro_segment(data_size));
        code->set_ro_segment(data, data_size);

        // copy the render state usage
        code->set_render_state_usage(code_gen.get_render_state_usage());

        // create the argument block layout if any arguments are captured
        mi::base::Handle<Generated_code_value_layout> layout;
        char const *layout_data = NULL;
        size_t layout_data_size = 0;
        if (code_gen.get_captured_arguments_llvm_type() != NULL) {
            layout = mi::base::make_handle(
                builder.create<Generated_code_value_layout>(alloc, &code_gen));
            code->add_captured_arguments_layout(layout.get());
            layout_data = layout->get_layout_data(layout_data_size);
        }

        // copy the string constant table.
        size_t n_strings = code_gen.get_string_constant_count();
        for (size_t i = 0; i < n_strings; ++i) {
            code->add_mapped_string(code_gen.get_string_constant(i), i);
        }

        if (code_cache != NULL) {
            string const &code_str = code->access_src_code();

            Small_VLA<char const *, 8> mappend_strings(get_allocator(), n_strings);
            for (size_t i = 0; i < n_strings; ++i) {
                mappend_strings[i] = code_gen.get_string_constant(i);
            }

            ICode_cache::Entry entry(
                code_str.c_str(),       code_str.size(),
                data,                   data_size,
                layout_data,            layout_data_size,
                mappend_strings.data(), mappend_strings.size(),
                code->get_state_usage());

            code_cache->enter(cache_key, entry);
        }
    } else if (code->access_messages().get_error_message_count() == 0) {
        // on failure, ensure that the code contains an error message
        code_gen.error(INTERNAL_JIT_BACKEND_ERROR, "Compiling lambda function into PTX failed");
    }
    return code;
}

// Get the device library for PTX compilation for the given target architecture.
unsigned char const *Code_generator_jit::get_libdevice_for_gpu(
    size_t   &size)
{
    unsigned min_ptx_version = 0;
    return LLVM_code_generator::get_libdevice(size, min_ptx_version);
}

// Create a link unit.
Link_unit_jit *Code_generator_jit::create_link_unit(
    Compilation_mode mode,
    bool             enable_simd,
    unsigned         sm_version,
    unsigned         num_texture_spaces,
    unsigned         num_texture_results)
{
    // link units always expect the uniform state to be included in the MDL SDK state
    m_options.set_option(MDL_JIT_OPTION_INCLUDE_UNIFORM_STATE, "true");

    Link_unit_jit::Target_kind target_kind;
    Type_mapper::Type_mapping_mode tm_mode;
    switch (mode) {
        case CM_PTX:
            target_kind = Link_unit_jit::TK_CUDA_PTX;
            tm_mode = Type_mapper::TM_PTX;
            break;

        case CM_LLVM_IR:
            target_kind = Link_unit_jit::TK_LLVM_IR;
            tm_mode = enable_simd ? Type_mapper::TM_BIG_VECTORS : Type_mapper::TM_ALL_SCALAR;
            break;

        case CM_NATIVE:
            target_kind = Link_unit_jit::TK_NATIVE;
            tm_mode = Type_mapper::TM_NATIVE_X86;
            break;

        default:
            return NULL;
    }

    return m_builder.create<Link_unit_jit>(
        m_builder.get_allocator(),
        m_jitted_code.get(),
        m_compiler.get(),
        target_kind,
        tm_mode,
        sm_version,
        num_texture_spaces,
        num_texture_results,
        &m_options,
        get_state_mapping(),
        /*enable_debug=*/false
    );
}

// Compile a link unit into a LLVM-IR using the JIT.
IGenerated_code_executable *Code_generator_jit::compile_unit(
    ILink_unit const *iunit)
{
    size_t num_funcs = iunit->get_function_count();
    if (iunit == NULL || num_funcs == 0)
        return NULL;

    Link_unit_jit const &unit = *impl_cast<Link_unit_jit>(iunit);

    IAllocator        *alloc = get_allocator();
    Allocator_builder builder(alloc);

    // now finalize the module
    llvm::Module *module = unit->finalize_module();
    mi::base::Handle<IGenerated_code_executable> code_obj(unit.get_code_object());

    if (module == NULL) {
        // on failure, ensure that the code contains an error message
        if (unit->get_error_message_count() == 0)
            unit->error(INTERNAL_JIT_BACKEND_ERROR, "Compiling link unit failed");
    } else if (unit.get_target_kind() == Link_unit_jit::TK_NATIVE) {
        mi::base::Handle<Generated_code_lambda_function> code(
            code_obj->get_interface<mi::mdl::Generated_code_lambda_function>());

        llvm::Module *module = unit.get_function(0)->getParent();
        unit->jit_compile(module);
        code->set_llvm_module(module);

        // add all generated functions as entry points
        for (size_t i = 0; i < num_funcs; ++i) {
            llvm::Function *func = unit.get_function(i);
            code->add_entry_point(unit->get_entry_point(func));
        }

        // copy the render state usage
        code->set_render_state_usage(unit->get_render_state_usage());

        // add all argument block layouts
        for (size_t i = 0, num = unit.get_arg_block_layout_count(); i < num; ++i)
            code->add_captured_arguments_layout(
                mi::base::make_handle(unit.get_arg_block_layout(i)).get());

        // copy the string constant table
        for (size_t i = 0, n = unit->get_string_constant_count(); i < n; ++i)
            code->add_mapped_string(unit->get_string_constant(i), i);
    } else {
        mi::base::Handle<Generated_code_source> code(
            code_obj->get_interface<mi::mdl::Generated_code_source>());

        if (unit.get_target_kind() == Link_unit_jit::TK_CUDA_PTX) {
            unit->ptx_compile(module, code->access_src_code());
        } else {
            // target kind == TK_LLVM_IR
            if (m_options.get_bool_option(MDL_JIT_OPTION_WRITE_BITCODE))
                unit->llvm_bc_compile(module, code->access_src_code());
            else
                unit->llvm_ir_compile(module, code->access_src_code());
        }

        // set the read-only data segment
        size_t data_size = 0;
        char const *data = reinterpret_cast<char const *>(unit->get_ro_segment(data_size));
        code->set_ro_segment(data, data_size);

        // copy the render state usage
        code->set_render_state_usage(unit->get_render_state_usage());

        // add all argument block layouts
        for (size_t i = 0, num = unit.get_arg_block_layout_count(); i < num; ++i) {
            code->add_captured_arguments_layout(
                mi::base::make_handle(unit.get_arg_block_layout(i)).get());
        }

        // copy the string constant table
        for (size_t i = 0, n = unit->get_string_constant_count(); i < n; ++i) {
            code->add_mapped_string(unit->get_string_constant(i), i);
        }

        // it's now safe to drop this module
        delete module;
    }
    code_obj->retain();
    return code_obj.get();
}

// Calculate the state mapping mode from options.
unsigned Code_generator_jit::get_state_mapping() const
{
    unsigned res = 0;

    if (m_options.get_bool_option(MDL_JIT_OPTION_USE_BITANGENT))
        res |= Type_mapper::SM_USE_BITANGENT;

    if (m_options.get_bool_option(MDL_JIT_OPTION_INCLUDE_UNIFORM_STATE))
        res |= Type_mapper::SM_INCLUDE_UNIFORM_STATE;

    return res;
}

// Constructor.
Link_unit_jit::Link_unit_jit(
    IAllocator         *alloc,
    Jitted_code        *jitted_code,
    MDL                *compiler,
    Target_kind        target_kind,
    Type_mapping_mode  tm_mode,
    unsigned           sm_version,
    unsigned           num_texture_spaces,
    unsigned           num_texture_results,
    Options_impl const *options,
    unsigned           state_mapping,
    bool               enable_debug)
: Base(alloc)
, m_target_kind(target_kind)
, m_source_only_llvm_context()
, m_code(create_code_object(jitted_code))
, m_code_gen(
    jitted_code,
    compiler,
    access_messages(),
    *get_llvm_context(),
    /*ptx_mode=*/target_kind == TK_CUDA_PTX,
    tm_mode,
    sm_version,
    /*has_tex_handler=*/target_kind != TK_CUDA_PTX,  // PTX mode: there's no texture handler
    Type_mapper::SSM_CORE,
    num_texture_spaces,
    num_texture_results,
    *options,
    /*incremental=*/true,
    state_mapping,
    /*res_manag=*/NULL,
    enable_debug)
, m_res_manag(create_resource_manager(m_code.get()))
, m_func_infos(alloc)
, m_arg_block_layouts(alloc)
, m_lambdas(alloc)
, m_dist_funcs(alloc)
{
    // For native code, we don't need mangling and read-only data segments
    if (m_target_kind != TK_NATIVE) {
        // enable name mangling
        m_code_gen.enable_name_mangling();

        // enable the read-only data segment
        m_code_gen.enable_ro_data_segment();
    }

    m_code_gen.set_resource_manag(m_res_manag);
}

// Destructor.
Link_unit_jit::~Link_unit_jit()
{
    Allocator_builder builder(get_allocator());

    // Note: the IResource_manager does not have an virtual destructor, hence cast
    // to the right type
    switch (m_target_kind) {
    case TK_NATIVE:
        {
            Generated_code_lambda_function::Lambda_res_manag *res_manag =
                static_cast<Generated_code_lambda_function::Lambda_res_manag *>(m_res_manag);

            builder.destroy(res_manag);
        }
        break;
    case TK_CUDA_PTX:
    case TK_LLVM_IR:
        {
            Generated_code_source::Source_res_manag *res_manag =
                static_cast<Generated_code_source::Source_res_manag *>(m_res_manag);

            builder.destroy(res_manag);
        }
        break;
    }
}

// Creates the code object to be used with this link unit.
IGenerated_code_executable *Link_unit_jit::create_code_object(
    Jitted_code *jitted_code)
{
    Allocator_builder builder(get_allocator());
    switch (m_target_kind) {
    case TK_NATIVE:
        return builder.create<Generated_code_lambda_function>(jitted_code);
    case TK_CUDA_PTX:
        return builder.create<Generated_code_source>(
            get_allocator(), IGenerated_code_executable::CK_PTX);
    case TK_LLVM_IR:
        return builder.create<Generated_code_source>(
            get_allocator(), IGenerated_code_executable::CK_LLVM_IR);
    }
    MDL_ASSERT(!"unsupported target kind");
    return NULL;
}

// Creates the resource manager to be used with this link unit.
IResource_manager *Link_unit_jit::create_resource_manager(
    IGenerated_code_executable *icode)
{
    Allocator_builder builder(get_allocator());
    switch (m_target_kind) {
    case TK_NATIVE:
        {
            // We need to build this manually, as Allocator_builder doesn't support reference
            // parameters in create<>.
            Generated_code_lambda_function::Lambda_res_manag *res_manag =
                builder.alloc<Generated_code_lambda_function::Lambda_res_manag>(1);
            Generated_code_lambda_function *code =
                static_cast<Generated_code_lambda_function *>(icode);
            new (res_manag) Generated_code_lambda_function::Lambda_res_manag(
                *code, /*resource_map=*/NULL);
            return res_manag;
        }
    case TK_CUDA_PTX:
    case TK_LLVM_IR:
        return builder.create<Generated_code_source::Source_res_manag>(
            get_allocator(), (mi::mdl::Resource_attr_map const *)NULL);
    }
    MDL_ASSERT(!"unsupported target kind");
    return NULL;
}

// Update the resource attribute maps for the current lambda function to be compiled.
void Link_unit_jit::update_resource_attribute_map(
    Lambda_function const *lambda)
{
    switch (m_target_kind) {
    case TK_NATIVE:
        {
            Generated_code_lambda_function::Lambda_res_manag *res_manag =
                static_cast<Generated_code_lambda_function::Lambda_res_manag *>(m_res_manag);

            res_manag->import_from_resource_attribute_map(&lambda->get_resource_attribute_map());
        }
        break;
    case TK_CUDA_PTX:
    case TK_LLVM_IR:
        {
            Generated_code_source::Source_res_manag *res_manag =
                static_cast<Generated_code_source::Source_res_manag *>(m_res_manag);

            res_manag->set_resource_attribute_map(&lambda->get_resource_attribute_map());
        }
        break;
    }
}

// Access messages.
const Messages &Link_unit_jit::access_messages() const
{
    switch (m_target_kind)
    {
    case TK_NATIVE:
        {
            mi::base::Handle<Generated_code_lambda_function> native_code(
                m_code->get_interface<mi::mdl::Generated_code_lambda_function>());
            return native_code->access_messages();
        }
    case TK_CUDA_PTX:
    case TK_LLVM_IR:
    default:
        {
            mi::base::Handle<Generated_code_source> source_code(
                m_code->get_interface<mi::mdl::Generated_code_source>());
            return source_code->access_messages();
        }
    }
}

// Get write access to the messages of the generated code.
Messages_impl &Link_unit_jit::access_messages()
{
    switch (m_target_kind)
    {
    case TK_NATIVE:
        {
            mi::base::Handle<Generated_code_lambda_function> native_code(
                m_code->get_interface<mi::mdl::Generated_code_lambda_function>());
            return native_code->access_messages();
        }
    case TK_CUDA_PTX:
    case TK_LLVM_IR:
    default:
        {
            mi::base::Handle<Generated_code_source> source_code(
                m_code->get_interface<mi::mdl::Generated_code_source>());
            return source_code->access_messages();
        }
    }
}

// Get the LLVM context to use with this link unit.
llvm::LLVMContext *Link_unit_jit::get_llvm_context()
{
    if (m_target_kind == TK_NATIVE) {
        mi::base::Handle<Generated_code_lambda_function> native_code(
            m_code->get_interface<mi::mdl::Generated_code_lambda_function>());
        return &native_code->get_llvm_context();
    }
    return &m_source_only_llvm_context;
}
// Add a lambda function to this link unit.
bool Link_unit_jit::add(
    ILambda_function const    *ilambda,
    ICall_name_resolver const *resolver,
    Function_kind              kind,
    size_t                    *arg_block_index)
{
    if (arg_block_index == NULL)
        return false;

    Lambda_function const *lambda = impl_cast<Lambda_function>(ilambda);
    if (lambda == NULL)
        return false;

    DAG_node const *body = lambda->get_body();
    if (body == NULL && lambda->get_root_expr_count() < 1) {
        // there must be at least one root or a body
        return false;
    }

    // we compile a new lambda, do an update of the resource attribute map
    update_resource_attribute_map(lambda);

    // add to lambda list, as m_code_gen will see it
    m_lambdas.push_back(mi::base::make_handle_dup(lambda));

    llvm::Function *func = NULL;
    if (body != NULL) {
        func = m_code_gen.compile_generic_lambda(
            /*incremental=*/true, *lambda, resolver, /*transformer=*/NULL);
    } else {
        func = m_code_gen.compile_switch_lambda(
            /*incremental=*/true, *lambda, resolver);
    }

    if (func != NULL) {
        if (m_code_gen.get_captured_arguments_llvm_type() != NULL) {
            IAllocator        *alloc = get_allocator();
            Allocator_builder builder(alloc);
            m_arg_block_layouts.push_back(
                mi::base::make_handle(
                    builder.create<Generated_code_value_layout>(alloc, &m_code_gen)));
            *arg_block_index = m_arg_block_layouts.size() - 1;
        } else
            *arg_block_index = ~0;

        m_func_infos.push_back(
            Link_unit_jit_function_info(
                string(func->getName().data(), get_allocator()),
                func,
                kind,
                *arg_block_index));

        return true;
    }
    return false;
}

// Add a distribution function to this link unit.
bool Link_unit_jit::add(
    IDistribution_function const *idist_func,
    ICall_name_resolver const    *resolver,
    size_t                       *arg_block_index)
{
    Distribution_function const *dist_func = impl_cast<Distribution_function>(idist_func);
    if (dist_func == NULL)
        return false;

    mi::base::Handle<ILambda_function> root_lambda_handle(dist_func->get_main_df());
    Lambda_function const *root_lambda = impl_cast<Lambda_function>(root_lambda_handle.get());

    // we compile a new lambda, do an update of the resource attribute map
    update_resource_attribute_map(root_lambda);

    // Add to distribution function list, as m_code_gen will see it
    m_dist_funcs.push_back(mi::base::make_handle_dup(idist_func));

    LLVM_code_generator::Function_vector llvm_funcs(get_allocator());
    llvm::Module *module = m_code_gen.compile_distribution_function(
        /*incremental=*/true, *dist_func, resolver, llvm_funcs);

    if (module != NULL) {
        if (m_code_gen.get_captured_arguments_llvm_type() != NULL) {
            IAllocator        *alloc = get_allocator();
            Allocator_builder builder(alloc);
            m_arg_block_layouts.push_back(
                mi::base::make_handle(
                    builder.create<Generated_code_value_layout>(alloc, &m_code_gen)));
            *arg_block_index = m_arg_block_layouts.size() - 1;
        } else
            *arg_block_index = ~0;

        m_func_infos.push_back(
            Link_unit_jit_function_info(
                root_lambda->get_name() + string("_init", get_allocator()),
                llvm_funcs[0],
                FK_DF_INIT,
                *arg_block_index));

        m_func_infos.push_back(
            Link_unit_jit_function_info(
                root_lambda->get_name() + string("_sample", get_allocator()),
                llvm_funcs[1],
                FK_DF_SAMPLE,
                *arg_block_index));

        m_func_infos.push_back(
            Link_unit_jit_function_info(
                root_lambda->get_name() + string("_evaluate", get_allocator()),
                llvm_funcs[2],
                FK_DF_EVALUATE,
                *arg_block_index));

        m_func_infos.push_back(
            Link_unit_jit_function_info(
                root_lambda->get_name() + string("_pdf", get_allocator()),
                llvm_funcs[3],
                FK_DF_PDF,
                *arg_block_index));

        return true;
    }
    return false;
}

// Get the number of functions in this link unit.
size_t Link_unit_jit::get_function_count() const
{
    return m_func_infos.size();
}

// Get the name of the i'th function inside this link unit.
char const *Link_unit_jit::get_function_name(size_t i) const
{
    if (i < m_func_infos.size())
        return m_func_infos[i].m_name.c_str();
    return NULL;
}

// Returns the kind of the i'th function inside this link unit.
ILink_unit::Function_kind Link_unit_jit::get_function_kind(size_t i) const
{
    if (i < m_func_infos.size())
        return m_func_infos[i].m_kind;
    return ILink_unit::FK_INVALID;
}

// Get the index of the target argument block layout for the i'th function inside this link
// unit if used.
size_t Link_unit_jit::get_function_arg_block_layout_index(size_t i) const
{
    if (i < m_func_infos.size())
        return m_func_infos[i].m_arg_block_index;
    return ~0;
}

// Get the LLVM function of the i'th function inside this link unit.
llvm::Function *Link_unit_jit::get_function(size_t i) const
{
    if (i < m_func_infos.size())
        return m_func_infos[i].m_func;
    return NULL;
}

// Get the number of target argument block layouts used by this link unit.
size_t Link_unit_jit::get_arg_block_layout_count() const
{
    return m_arg_block_layouts.size();
}

// Get the i'th target argument block layout used by this link unit.
IGenerated_code_value_layout const *Link_unit_jit::get_arg_block_layout(size_t i) const
{
    if (i < m_arg_block_layouts.size()) {
        IGenerated_code_value_layout const *layout = m_arg_block_layouts[i].get();
        layout->retain();
        return layout;
    }
    return NULL;
}

// Get the LLVM module.
llvm::Module const *Link_unit_jit::get_llvm_module() const
{
    return m_code_gen.get_llvm_module();
}

// Create the jit code generator.
ICode_generator *create_code_generator_jit(IAllocator *alloc, MDL *mdl)
{
    return Code_generator_jit::create_code_generator(alloc, mdl);
}

// Get the jitted code singleton.
Jitted_code *create_jitted_code_singleton(IAllocator *alloc)
{
    return Jitted_code::get_instance(alloc);
}

// Terminate the jitted code singleton.
void terminate_jitted_code_singleton(Jitted_code *jitted_code)
{
    if (jitted_code != NULL)
        jitted_code->release();
}

} // mdl
} // mi
