/***************************************************************************************************
 * Copyright (c) 2015-2018, NVIDIA CORPORATION. All rights reserved.
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
 **************************************************************************************************/

#include "pch.h"

#include <cstring>
#include <map>
#include <string>

#include <mi/base/types.h>
#include <mi/mdl/mdl_mdl.h>
#include <mi/mdl/mdl_symbols.h>
#include <mi/mdl/mdl_types.h>
#include <base/lib/log/i_log_logger.h>
#include <base/data/db/i_db_access.h>
#include <io/scene/mdl_elements/mdl_elements_detail.h> // DETAIL::Type_binder
#include <io/scene/mdl_elements/i_mdl_elements_compiled_material.h>
#include <io/scene/mdl_elements/i_mdl_elements_function_call.h>
#include <io/scene/mdl_elements/i_mdl_elements_function_definition.h>
#include <io/scene/mdl_elements/i_mdl_elements_utilities.h>
#include <io/scene/mdl_elements/mdl_elements_utilities.h>
#include <render/mdl/runtime/i_mdlrt_resource_handler.h>
#include <render/mdl/backends/backends_target_code.h>

#include <mdl/codegenerators/generator_dag/generator_dag_lambda_function.h>
#include <mdl/codegenerators/generator_dag/generator_dag_tools.h>
#include <mdl/codegenerators/generator_dag/generator_dag_dumper.h>
#include <mdl/compiler/compilercore/compilercore_streams.h>

#include "backends_link_unit.h"
#include "backends_backends.h"

namespace MI {

namespace BACKENDS {

namespace {

/// A name register interface.
class IResource_register {
public:
    /// Register a texture index.
    ///
    /// \param index  the texture index
    /// \param name   the DB name of this index
    /// \param type   the type of the texture
    virtual void register_texture(
        size_t                                     index,
        char const                                 *name,
        mi::neuraylib::ITarget_code::Texture_shape type) = 0;

    /// Return the number of texture resources.
    virtual size_t get_texture_count() const = 0;

    /// Register a light profile.
    ///
    /// \param index  the light profile index
    /// \param name   the DB name of this index
    virtual void register_light_profile(
        size_t                                     index,
        char const                                 *name) = 0;

    /// Return the number of light profile resources.
    virtual size_t get_light_profile_count() const = 0;

    /// Register a bsdf measurement.
    ///
    /// \param index  the bsdf measurement index
    /// \param name   the DB name of this index
    virtual void register_bsdf_measurement(
        size_t                                     index,
        char const                                 *name) = 0;

    /// Return the number of bsdf measurement resources.
    virtual size_t get_bsdf_measurement_count() const = 0;
};

/// Helper class to enumerate resources in lambda functions.
class Function_enumerator : public mi::mdl::ILambda_resource_enumerator {
public:
    typedef MISTD::map<MISTD::string, size_t>  Resource_index_map;

    /// Constructor.
    ///
    /// \param reg             a index register interface
    /// \param lambda          currently processed lambda function
    /// \param db_transaction  the current transaction
    Function_enumerator(
        IResource_register        &reg,
        mi::mdl::ILambda_function *lambda,
        DB::Transaction           *db_transaction)
    : m_register(reg)
    , m_lambda(lambda)
    , m_additional_lambda(NULL)
    , m_db_transaction(db_transaction)
    , m_resource_index_map(NULL)
    , m_tex_idx_store(0)
    , m_lp_idx_store(0)
    , m_bm_idx_store(0)
    , m_tex_idx(m_tex_idx_store)
    , m_lp_idx(m_lp_idx_store)
    , m_bm_idx(m_bm_idx_store)
    {
    }

    /// Constructor.
    ///
    /// \param reg       a index register interface
    /// \param lambda    currently processed lambda function
    /// \param trans     the current transaction
    /// \param tex_idx  next texture index Current texture index.
    /// \param lp_idx   next light profile index.
    /// \param bm_idx   next bsdf measurement index.
    /// \param res_map  a map to track which resources are already known
    Function_enumerator(
        IResource_register        &reg,
        mi::mdl::ILambda_function *lambda,
        DB::Transaction           *db_transaction,
        size_t                    &tex_idx,
        size_t                    &lp_idx,
        size_t                    &bm_idx,
        Resource_index_map        &res_map)
    : m_register(reg)
    , m_lambda(lambda)
    , m_additional_lambda(NULL)
    , m_db_transaction(db_transaction)
    , m_resource_index_map(&res_map)
    , m_tex_idx_store(0)
    , m_lp_idx_store(0)
    , m_bm_idx_store(0)
    , m_tex_idx(tex_idx)
    , m_lp_idx(lp_idx)
    , m_bm_idx(bm_idx)
    {
    }

    /// Called for a texture resource.
    ///
    /// \param v  the texture resource or an invalid ref
    virtual void texture(mi::mdl::IValue  const *v)
    {
        if (m_register.get_texture_count() == 0) {
            // index 0 is always the only invalid texture index
            m_register.register_texture(0, "", mi::neuraylib::ITarget_code::Texture_shape_invalid);
        }

        if (mi::mdl::IValue_texture const *r = mi::mdl::as<mi::mdl::IValue_texture>(v)) {
            bool valid = false, is_uvtile = false;
            int width = 0, height = 0, depth = 0;

            MI::MDL::get_texture_attributes(
                m_db_transaction, r, valid, is_uvtile, width, height, depth);

            if (valid) {
                char const *name = resource_to_name(r);

                bool new_entry = true;
                size_t tex_idx;
                if (m_resource_index_map != NULL) {
                    Resource_index_map::const_iterator it(m_resource_index_map->find(name));
                    if (it == m_resource_index_map->end()) {
                        // new entry
                        tex_idx = ++m_tex_idx;
                        new_entry = true;
                        m_resource_index_map->insert(Resource_index_map::value_type(name, tex_idx));
                    } else {
                        // known
                        tex_idx = it->second;
                        new_entry = false;
                    }
                } else {
                    // no map, always new
                    tex_idx = ++m_tex_idx;
                    new_entry = true;
                }

                if (new_entry)
                    m_register.register_texture(tex_idx, name, get_texture_shape(r->get_type()));

                m_lambda->map_tex_resource(v, tex_idx, /*valid=*/true, width, height, depth);
                if (m_additional_lambda)
                    m_additional_lambda->map_tex_resource(
                        v, tex_idx, /*valid=*/true, width, height, depth);

                return;
            }
        }
        // invalid textures are always mapped to zero in the MDL SDK
        m_lambda->map_tex_resource(v, 0, /*valid=*/false, 0, 0, 0);
        if (m_additional_lambda)
            m_additional_lambda->map_tex_resource(v, 0, /*valid=*/false, 0, 0, 0);
    }

    /// Called for a light profile resource.
    ///
    /// \param v  the light profile resource or an invalid ref
    virtual void light_profile(mi::mdl::IValue const *v)
    {
        if (m_register.get_light_profile_count() == 0) {
            // index 0 is always the only invalid light profile index
            m_register.register_light_profile(0, "");
        }

        if (mi::mdl::IValue_resource const *r = mi::mdl::as<mi::mdl::IValue_resource>(v)) {
            bool valid = false;
            float power = 0.0f, maximum = 0.0f;

            MI::MDL::get_light_profile_attributes(m_db_transaction, r, valid, power, maximum);

            if (valid) {
                char const *name = resource_to_name(r);
                if (m_additional_lambda)
                    m_additional_lambda->map_lp_resource(
                        v, m_lp_idx, /*valid=*/true, power, maximum);

                bool new_entry = true;
                size_t lp_idx;
                if (m_resource_index_map != NULL) {
                    Resource_index_map::const_iterator it(m_resource_index_map->find(name));
                    if (it == m_resource_index_map->end()) {
                        // new entry
                        lp_idx = ++m_lp_idx;
                        new_entry = true;
                        m_resource_index_map->insert(Resource_index_map::value_type(name, lp_idx));
                    } else {
                        // known
                        lp_idx = it->second;
                        new_entry = false;
                    }
                } else {
                    // no map, always new
                    lp_idx = ++m_lp_idx;
                    new_entry = true;
                }

                if (new_entry)
                    m_register.register_light_profile(lp_idx, name);

                m_lambda->map_lp_resource(v, lp_idx, /*valid=*/true, power, maximum);

                return;
            }
        }
        // invalid light profiles are always mapped to zero in the MDL SDK
        m_lambda->map_lp_resource(v, 0, /*valid=*/false, 0.0f, 0.0f);
        if (m_additional_lambda)
            m_additional_lambda->map_lp_resource(
                v, 0, /*valid=*/false, 0.0f, 0.0f);
    }

    /// Called for a bsdf measurement resource.
    ///
    /// \param v  the bsdf measurement resource or an invalid_ref
    virtual void bsdf_measurement(mi::mdl::IValue const *v)
    {
        if (m_register.get_bsdf_measurement_count() == 0) {
            // index 0 is always the only invalid bsdf measurement index
            m_register.register_bsdf_measurement(0, "");
        }

        if (mi::mdl::IValue_resource const *r = mi::mdl::as<mi::mdl::IValue_resource>(v)) {
            bool valid = false;

            MI::MDL::get_bsdf_measurement_attributes(m_db_transaction, r, valid);

            if (valid) {
                char const *name = resource_to_name(r);
                if (m_additional_lambda)
                    m_additional_lambda->map_bm_resource(v, m_bm_idx, /*valid=*/true);

                bool new_entry = true;
                size_t bm_idx;
                if (m_resource_index_map != NULL) {
                    Resource_index_map::const_iterator it(m_resource_index_map->find(name));
                    if (it == m_resource_index_map->end()) {
                        // new entry
                        bm_idx = ++m_bm_idx;
                        new_entry = true;
                        m_resource_index_map->insert(Resource_index_map::value_type(name, bm_idx));
                    } else {
                        // known
                        bm_idx = it->second;
                        new_entry = false;
                    }
                } else {
                    // no map, always new
                    bm_idx = ++m_bm_idx;
                    new_entry = true;
                }

                if (new_entry)
                    m_register.register_bsdf_measurement(bm_idx, name);

                m_lambda->map_bm_resource(v, bm_idx, /*valid=*/true);

                return;
            }
        }
        // invalid bsdf measurements are always mapped to zero in the MDL SDK
        m_lambda->map_bm_resource(v, 0, /*valid=*/false);
        if (m_additional_lambda)
            m_additional_lambda->map_bm_resource(v, 0, /*valid=*/false);
    }

    /// Set an additional lambda which should receive registered resources.
    void set_additional_lambda(mi::mdl::ILambda_function *additional_lambda) {
        m_additional_lambda = additional_lambda;
    }

private:
    /// Get the DB name of a resource.
    char const *resource_to_name(mi::mdl::IValue_resource const *r)
    {
        DB::Tag tag = DB::Tag(r->get_tag_value());
        char const *name = m_db_transaction->tag_to_name(tag);
        return name != NULL ? name : "";
    }

    /// Get the ITargetcode::Texture_shape from a MDL type.
    static mi::neuraylib::ITarget_code::Texture_shape get_texture_shape(
        mi::mdl::IType_texture const *type)
    {
        switch (type->get_shape()) {
        case mi::mdl::IType_texture::TS_2D:
            return mi::neuraylib::ITarget_code::Texture_shape_2d;
            break;
        case mi::mdl::IType_texture::TS_3D:
            return mi::neuraylib::ITarget_code::Texture_shape_3d;
            break;
        case mi::mdl::IType_texture::TS_CUBE:
            return mi::neuraylib::ITarget_code::Texture_shape_cube;
            break;
        case mi::mdl::IType_texture::TS_PTEX:
            return mi::neuraylib::ITarget_code::Texture_shape_ptex;
            break;
        }
        ASSERT(M_BACKENDS, !"Unsupported MDL texture shape");
        return mi::neuraylib::ITarget_code::Texture_shape_invalid;
    }

private:
    /// The index register interface.
    IResource_register &m_register;

    /// The processed lambda function.
    mi::mdl::ILambda_function *m_lambda;

    /// Additional lambda function where textures should be registered.
    mi::mdl::ILambda_function *m_additional_lambda;

    /// The current transaction.
    DB::Transaction *m_db_transaction;

    /// The set of known resources if they are tracked.
    Resource_index_map *m_resource_index_map;

    /// Storage for the current texture index.
    size_t m_tex_idx_store;

    /// Storage for the current light profile index.
    size_t m_lp_idx_store;

    /// Storage for the current bsdf measurement index.
    size_t m_bm_idx_store;

    /// Current texture index.
    size_t &m_tex_idx;

    /// Current light profile index.
    size_t &m_lp_idx;

    /// Current bsdf measurement index.
    size_t &m_bm_idx;
};

/// Converts a MI::MDL::IType to a mi::mdl::IType.
///
/// \param tf  the type factory
/// \param t   the type to convert
///
/// \returns the converted type
static mi::mdl::IType const *convert_type(
    mi::mdl::IType_factory *tf,
    MI::MDL::IType const   *t)
{
    switch (t->get_kind()) {
    case MI::MDL::IType::TK_ALIAS:
        {
            MI::MDL::IType_alias const *at   = static_cast<MI::MDL::IType_alias const *>(t);
            mi::base::Handle<MI::MDL::IType const> et(at->get_aliased_type());
            mi::Uint32                 m     = at->get_type_modifiers();
            char const                 *name = at->get_symbol();
            mi::mdl::ISymbol const     *sym   = NULL;

            if (name != NULL && name[0] != '\0') {
                mi::mdl::ISymbol_table *st = tf->get_symbol_table();

                sym = st->create_user_type_symbol(name);
            }

            mi::mdl::IType::Modifiers mod = 0;

            if (m & MI::MDL::IType::MK_UNIFORM)
                mod |= mi::mdl::IType::MK_UNIFORM;
            if (m & MI::MDL::IType::MK_VARYING)
                mod |= mi::mdl::IType::MK_VARYING;

            return tf->create_alias(convert_type(tf, et.get()), sym, mod);
        }
    case MI::MDL::IType::TK_BOOL:
        return tf->create_bool();
    case MI::MDL::IType::TK_INT:
        return tf->create_int();
    case MI::MDL::IType::TK_ENUM:
        {
            MI::MDL::IType_enum const *et = static_cast<MI::MDL::IType_enum const *>(t);

            switch (et->get_predefined_id()) {
            case MI::MDL::IType_enum::EID_USER:
                {
                    char const             *name = et->get_symbol();
                    if (mi::mdl::IType_enum const *et = tf->lookup_enum(name))
                        return et;

                    mi::mdl::ISymbol_table *st   = tf->get_symbol_table();
                    mi::mdl::ISymbol const *sym  = st->create_user_type_symbol(name);
                    mi::mdl::IType_enum    *e    = tf->create_enum(sym);

                    for (size_t i = 0, n = et->get_size(); i < n; ++i) {
                        char const *v = et->get_value_name(i);
                        int        c  = et->get_value_code(i);

                        e->add_value(st->create_symbol(v), c);
                    }
                    return e;
                }
            case MI::MDL::IType_enum::EID_TEX_GAMMA_MODE:
                return tf->get_predefined_enum(mi::mdl::IType_enum::EID_TEX_GAMMA_MODE);
            case MI::MDL::IType_enum::EID_INTENSITY_MODE:
                return tf->get_predefined_enum(mi::mdl::IType_enum::EID_INTENSITY_MODE);
            case MI::MDL::IType_enum::EID_FORCE_32_BIT:
                break;
            }
        }
        break;
    case MI::MDL::IType::TK_FLOAT:
        return tf->create_float();
    case MI::MDL::IType::TK_DOUBLE:
        return tf->create_double();
    case MI::MDL::IType::TK_STRING:
        return tf->create_string();
    case MI::MDL::IType::TK_VECTOR:
        {
            MI::MDL::IType_vector const *vt = static_cast<MI::MDL::IType_vector const *>(t);
            mi::base::Handle<MI::MDL::IType const> et(vt->get_element_type());
            size_t                      n   = vt->get_size();

            return tf->create_vector(
                mi::mdl::cast<mi::mdl::IType_atomic>(convert_type(tf, et.get())),
                n);
        }
    case MI::MDL::IType::TK_MATRIX:
        {
            MI::MDL::IType_matrix const *mt = static_cast<MI::MDL::IType_matrix const *>(t);
            mi::base::Handle<MI::MDL::IType const> et(mt->get_element_type());
            size_t                      n   = mt->get_size();

            return tf->create_matrix(
                mi::mdl::cast<mi::mdl::IType_vector>(convert_type(tf, et.get())),
                n);
        }
    case MI::MDL::IType::TK_COLOR:
        return tf->create_color();
    case MI::MDL::IType::TK_ARRAY:
        {
            MI::MDL::IType_array const *at = static_cast<MI::MDL::IType_array const *>(t);
            mi::base::Handle<MI::MDL::IType const> et(at->get_element_type());
            size_t                     n   = at->get_size();

            return tf->create_array(convert_type(tf, et.get()), n);
        }
    case MI::MDL::IType::TK_STRUCT:
        {
            MI::MDL::IType_struct const *stp = static_cast<MI::MDL::IType_struct const *>(t);

            switch (stp->get_predefined_id()) {
            case MI::MDL::IType_struct::SID_USER:
                {
                    char const             *name = stp->get_symbol();
                    if (mi::mdl::IType_struct const *st = tf->lookup_struct(name))
                        return st;

                    mi::mdl::ISymbol_table *st   = tf->get_symbol_table();
                    mi::mdl::ISymbol const *sym  = st->create_user_type_symbol(name);
                    mi::mdl::IType_struct  *s    = tf->create_struct(sym);

                    for (size_t i = 0, n = stp->get_size(); i < n; ++i) {
                        const char           *fn = stp->get_field_name(i);
                        mi::base::Handle<MI::MDL::IType const> ft(stp->get_field_type(i));

                        s->add_field(convert_type(tf, ft.get()), st->create_symbol(fn));
                    }
                    return s;
                }
            case MI::MDL::IType_struct::SID_MATERIAL_EMISSION:
                return tf->get_predefined_struct(mi::mdl::IType_struct::SID_MATERIAL_EMISSION);
            case MI::MDL::IType_struct::SID_MATERIAL_SURFACE:
                return tf->get_predefined_struct(mi::mdl::IType_struct::SID_MATERIAL_SURFACE);
            case MI::MDL::IType_struct::SID_MATERIAL_VOLUME:
                return tf->get_predefined_struct(mi::mdl::IType_struct::SID_MATERIAL_VOLUME);
            case MI::MDL::IType_struct::SID_MATERIAL_GEOMETRY:
                return tf->get_predefined_struct(mi::mdl::IType_struct::SID_MATERIAL_GEOMETRY);
            case MI::MDL::IType_struct::SID_MATERIAL:
                return tf->get_predefined_struct(mi::mdl::IType_struct::SID_MATERIAL);
            case MI::MDL::IType_struct::SID_FORCE_32_BIT:
                break;
            }
        }
        break;
    case MI::MDL::IType::TK_TEXTURE:
        {
            MI::MDL::IType_texture const *tt = static_cast<MI::MDL::IType_texture const *>(t);

            switch (tt->get_shape()) {
            case MI::MDL::IType_texture::TS_2D:
                return tf->create_texture(mi::mdl::IType_texture::TS_2D);
            case MI::MDL::IType_texture::TS_3D:
                return tf->create_texture(mi::mdl::IType_texture::TS_3D);
            case MI::MDL::IType_texture::TS_CUBE:
                return tf->create_texture(mi::mdl::IType_texture::TS_CUBE);
            case MI::MDL::IType_texture::TS_PTEX:
                return tf->create_texture(mi::mdl::IType_texture::TS_PTEX);
            case MI::MDL::IType_texture::TS_FORCE_32_BIT:
                break;
            }
        }
        break;
    case MI::MDL::IType::TK_LIGHT_PROFILE:
        return tf->create_light_profile();
    case MI::MDL::IType::TK_BSDF_MEASUREMENT:
        return tf->create_bsdf_measurement();
    case MI::MDL::IType::TK_BSDF:
        return tf->create_bsdf();
    case MI::MDL::IType::TK_EDF:
        return tf->create_edf();
    case MI::MDL::IType::TK_VDF:
        return tf->create_vdf();
    case MI::MDL::IType::TK_FORCE_32_BIT:
        break;
    }
    ASSERT(M_BACKENDS, !"Unsupported type");
    return NULL;
}

/// Helper class for building Lambda functions.
class Lambda_builder {
public:
    /// Constructor.
    Lambda_builder(
        mi::mdl::IMDL   *compiler,
        DB::Transaction *db_transaction,
        mi::Float32     mdl_meters_per_scene_unit,
        mi::Float32     mdl_wavelength_min,
        mi::Float32     mdl_wavelength_max,
        bool            compile_consts)
    : m_compiler(compiler, mi::base::DUP_INTERFACE)
    , m_db_transaction(db_transaction)
    , m_mdl_meters_per_scene_unit(mdl_meters_per_scene_unit)
    , m_mdl_wavelength_min(mdl_wavelength_min)
    , m_mdl_wavelength_max(mdl_wavelength_max)
    , m_error(0)
    , m_compile_consts(compile_consts)
    {
    }

    /// Get the error code of the last operation.
    Sint32 get_error_code() const { return m_error; }

    /// Build a lambda function from a call.
    mi::mdl::ILambda_function *env_from_call(
        MDL::Mdl_function_call const *function_call,
        char const                   *fname)
    {
        if (function_call == NULL) {
            m_error = -1;
            return NULL;
        }

        DB::Access<MDL::Mdl_function_definition> definition(
            function_call->get_function_definition(), m_db_transaction);

        if (!definition.is_valid()) {
            m_error = -2;
            return NULL;
        }

        mi::mdl::IType const *mdl_type = definition->get_mdl_return_type(m_db_transaction);
        if (mdl_type == NULL) {
            m_error = -2;
            return NULL;
        }
        mdl_type = mdl_type->skip_type_alias();

        mi::mdl::IDefinition::Semantics sema = definition->get_mdl_semantic();
        if (sema == mi::mdl::IDefinition::DS_INTRINSIC_DAG_ARRAY_CONSTRUCTOR) {
            // need special handling for array constructor because its definition is "broken".
            // However, array constructors are not allowed here
            m_error = -2;
            return NULL;
        }

        bool type_ok = false;
        mi::mdl::IType_struct const *tex_ret_type = NULL;

        // check for base::texture_return or color return type here
        if (mi::mdl::IType_struct const *s_type = mi::mdl::as<mi::mdl::IType_struct>(mdl_type)) {
            if (strcmp("::base::texture_return", s_type->get_symbol()->get_name()) == 0) {
                type_ok = true;
                tex_ret_type = s_type;
            }
        } else if (mi::mdl::is<mi::mdl::IType_color>(mdl_type)) {
            type_ok = true;
        }

        if (!type_ok) {
            m_error = -2;
            return NULL;
        }

        mi::base::Handle<mi::mdl::ILambda_function> lambda(
            m_compiler->create_lambda_function(mi::mdl::ILambda_function::LEC_ENVIRONMENT));

        MDL::Mdl_dag_builder<mi::mdl::IDag_builder> builder(
            m_db_transaction, lambda.get(), m_mdl_meters_per_scene_unit, m_mdl_wavelength_min,
            m_mdl_wavelength_max, /*compiled_material=*/NULL);

        mi::mdl::IType_factory *tf = lambda->get_type_factory();
        MDL::DETAIL::Type_binder type_binder(tf);

        mi::Uint32 count = static_cast<mi::Uint32>(function_call->get_parameter_count());

        MISTD::vector<mi::mdl::DAG_call::Call_argument> mdl_arguments(count);
        mi::base::Handle<const MDL::IExpression_list> arguments(
            function_call->get_arguments());
        for (mi::Uint32 i = 0; i < count; ++i) {
            mi::mdl::IType const *parameter_type =
                definition->get_mdl_parameter_type(m_db_transaction, i);

            mi::base::Handle<MDL::IExpression const> argument(arguments->get_expression(i));
            mdl_arguments[i].arg = builder.int_expr_to_mdl_dag_node(
                parameter_type, argument.get());
            if (mdl_arguments[i].arg == NULL) {
                m_error = -2;
                return NULL;
            }
            mdl_arguments[i].param_name = function_call->get_parameter_name(i);
            parameter_type = tf->import(parameter_type->skip_type_alias());

            mi::mdl::IType const *argument_type = mdl_arguments[i].arg->get_type();
            mi::Sint32 result = type_binder.check_and_bind_type(parameter_type, argument_type);
            switch (result) {
            case 0:
                // nothing to do
                break;
            case -1:
                LOG::mod_log->error(M_BACKENDS, LOG::Mod_log::C_DATABASE,
                    "Type mismatch for argument \"%s\" of function call \"%s\".",
                    mdl_arguments[i].param_name, function_call->get_mdl_function_definition());
                m_error = -2;
                return NULL;
            case -2:
                LOG::mod_log->error(M_BACKENDS, LOG::Mod_log::C_DATABASE,
                    "Array size mismatch for argument \"%s\" of function call \"%s\".",
                    mdl_arguments[i].param_name, function_call->get_mdl_function_definition());
                m_error = -2;
                return NULL;
            default:
                ASSERT(M_BACKENDS, false);
                m_error = -2;
                return NULL;
            }

            mdl_type = tf->import(mdl_type);
        }

        mi::mdl::DAG_call::Call_argument const *p_arguments = count > 0 ? &mdl_arguments[0] : 0;
        mi::mdl::DAG_node const *body = lambda->create_call(
            function_call->get_mdl_function_definition(),
            function_call->get_mdl_semantic(),
            p_arguments,
            count,
            mdl_type);

        // if return type is ::base::texture_return (see above) wrap the
        // DAG node by a select to extract the tint field
        if (tex_ret_type != NULL) {
            mi::mdl::IType const   *f_type;
            mi::mdl::ISymbol const *f_name;

            tex_ret_type->get_field(0, f_type, f_name);

            MISTD::string name(tex_ret_type->get_symbol()->get_name());
            name += '.';
            name += f_name->get_name();
            name += '(';
            name += tex_ret_type->get_symbol()->get_name();
            name += ')';

            mi::mdl::DAG_call::Call_argument args[1];

            args[0].arg        = body;
            args[0].param_name = "s";
            body = lambda->create_call(
                name.c_str(), mi::mdl::IDefinition::DS_INTRINSIC_DAG_FIELD_ACCESS, args, 1, f_type);
        }

        lambda->set_body(body);
        if (fname != NULL)
            lambda->set_name(fname);

        m_error = 0;
        lambda->retain();
        return lambda.get();
    }

    /// Build a lambda function from a material sub-expression.
    mi::mdl::ILambda_function *from_sub_expr(
        MDL::Mdl_compiled_material const *compiled_material,
        char const                       *path,
        char const                       *fname)
    {
        mi::mdl::ILambda_function::Lambda_execution_context lec
            = mi::mdl::ILambda_function::LEC_CORE;

        if (strcmp(path, "geometry.displacement") == 0) {
            // only this is the displacement function
            lec = mi::mdl::ILambda_function::LEC_DISPLACEMENT;
        }

        // get the field corresponding to path
        mi::mdl::IType_factory *tf         = m_compiler->get_type_factory();
        mi::mdl::IType const   *field_type = NULL;
        mi::base::Handle<MDL::IExpression const > field(
            compiled_material->lookup_sub_expression(m_db_transaction, path, tf, &field_type));

        if (!field.is_valid_interface()) {
            m_error = -2;
            return NULL;
        }

        // reject constants if not explicitely enabled
        if (!m_compile_consts && field->get_kind() == MDL::IExpression::EK_CONSTANT) {
            m_error = -4;
            return NULL;
        }

        // reject DFs or resources
        field_type = field_type->skip_type_alias();
        if (contains_df_type(field_type) || mi::mdl::is<mi::mdl::IType_resource>(field_type)) {
            m_error = -5;
            return NULL;
        }

        // ok, we found the attribute to compile, create a lambda function ...
        mi::base::Handle<mi::mdl::ILambda_function> lambda(
            m_compiler->create_lambda_function(lec));

        // ... and fill up ...
        MDL::Mdl_dag_builder<mi::mdl::IDag_builder> builder(
            m_db_transaction, lambda.get(),
            m_mdl_meters_per_scene_unit, m_mdl_wavelength_min,
            m_mdl_wavelength_max, compiled_material);

        // add all material parameters to the lambda function
        for (size_t i = 0, n = compiled_material->get_parameter_count(); i < n; ++i) {
            mi::base::Handle<MI::MDL::IValue const> value(compiled_material->get_argument(i));
            mi::base::Handle<MI::MDL::IType const> p_type(value->get_type());

            mi::mdl::IType const *tp = convert_type(lambda->get_type_factory(), p_type.get());

            size_t idx = lambda->add_parameter(tp, compiled_material->get_parameter_name(i));

            /// map the i'th material parameter to this new parameter
            lambda->set_parameter_mapping(i, idx);
        }

        mi::mdl::DAG_node const *body = builder.int_expr_to_mdl_dag_node(field_type, field.get());
        lambda->set_body(body);
        if (fname != NULL)
            lambda->set_name(fname);

        m_error = 0;
        lambda->retain();
        return lambda.get();
    }

    /// Build a distribution function from a material df (for example surface.scattering).
    mi::mdl::IDistribution_function *from_material_df(
        const MDL::Mdl_compiled_material* compiled_material,
        const char* path,
        const char* fname,
        bool include_geometry_normal)
    {
        mi::base::Handle<MDL::IExpression_factory> ef(MDL::get_expression_factory());

        // get the field corresponding to path
        mi::mdl::IType_factory* tf = m_compiler->get_type_factory();
        const mi::mdl::IType* field_type = 0;
        mi::base::Handle<const MDL::IExpression> field(
            compiled_material->lookup_sub_expression(m_db_transaction, path, tf, &field_type));

        if (!field) {
            m_error = -2;
            return NULL;
        }

        // reject constants
        //
        // It would be possible to compile constants into a function returning *always* an
        //  constant, but it does not makes much sense, and we do not support it yet.
        if (!m_compile_consts && field->get_kind() == MDL::IExpression::EK_CONSTANT) {
            m_error = -4;
            return NULL;
        }

        // reject non-DFs
        field_type = field_type->skip_type_alias();
        if (!mi::mdl::is<mi::mdl::IType_df>(field_type)) {
            m_error = -5;
            return NULL;
        }

        // currently only BSDFs are supported
        if (field_type->get_kind() != mi::mdl::IType::TK_BSDF) {
            if (field_type->get_kind() == mi::mdl::IType::TK_EDF) {
                m_error = -8;
                return NULL;
            }
            MDL_ASSERT(field_type->get_kind() == mi::mdl::IType::TK_VDF);
            m_error = -9;
            return NULL;
        }

        // ok, we found the attribute to compile, create a lambda function ...
        mi::base::Handle<mi::mdl::IDistribution_function> dist_func(
            m_compiler->create_distribution_function());
        mi::base::Handle<mi::mdl::ILambda_function> main_df(dist_func->get_main_df());
        if (fname)
            main_df->set_name(fname);

        // ... and fill up ...
        MDL::Mdl_dag_builder<mi::mdl::IDag_builder> builder(
            m_db_transaction, main_df.get(), m_mdl_meters_per_scene_unit,
            m_mdl_wavelength_min, m_mdl_wavelength_max, compiled_material);

        // add all material parameters to the lambda function
        for (size_t i = 0, n = compiled_material->get_parameter_count(); i < n; ++i) {
            MI::MDL::IValue const *value  = compiled_material->get_argument(i);
            MI::MDL::IType const  *p_type = value->get_type();

            mi::mdl::IType const *tp = convert_type(main_df->get_type_factory(), p_type);

            size_t idx = main_df->add_parameter(tp, compiled_material->get_parameter_name(i));

            /// map the i'th material parameter to this new parameter
            main_df->set_parameter_mapping(i, idx);
        }

        const mi::mdl::DAG_node* body = builder.int_expr_to_mdl_dag_node(field_type, field.get());

        mi::base::Handle<const MI::MDL::IExpression_direct_call> mat_body(
            compiled_material->get_body());
        DB::Tag tag = mat_body->get_definition();
        DB::Access<MI::MDL::Mdl_function_definition> definition(tag, m_db_transaction);
        mi::mdl::IType const *mat_type = definition->get_mdl_return_type(m_db_transaction);

        const mi::mdl::DAG_node *material_constructor =
            builder.int_expr_to_mdl_dag_node(mat_type, mat_body.get());

        MDL::Mdl_call_resolver resolver(m_db_transaction);
        mi::mdl::IDistribution_function::Error_code ec =
            dist_func->initialize(material_constructor, body, include_geometry_normal, &resolver);
        switch (ec) {
        case mi::mdl::IDistribution_function::EC_NONE:
            break;
        case mi::mdl::IDistribution_function::EC_UNSUPPORTED_BSDF:
            m_error = -10;
            return NULL;
        case mi::mdl::IDistribution_function::EC_NOT_A_BSDF:
        case mi::mdl::IDistribution_function::EC_INVALID_PARAMETERS:
            MDL_ASSERT(!"Unexpected error.");
            m_error = -10;
            return NULL;
        }

        m_error = 0;
        dist_func->retain();
        return dist_func.get();
    }

    /// Build a lambda function from a material sub-expression.
    size_t add_sub_expr(
        mi::mdl::ILambda_function        *ilambda,
        MDL::Mdl_compiled_material const *compiled_material,
        char const                       *path)
    {
        mi::mdl::Lambda_function *lambda = mi::mdl::impl_cast<mi::mdl::Lambda_function>(ilambda);
        mi::mdl::ILambda_function::Lambda_execution_context lec =
            mi::mdl::ILambda_function::LEC_CORE;

        if (strcmp(path, "geometry.displacement") == 0) {
            // only this is the displacement function
            lec = mi::mdl::ILambda_function::LEC_DISPLACEMENT;
        }
        if (lec != lambda->get_execution_context()) {
            // we cannot mix expressions with different contexts so far
            m_error = -7;
            return 0;
        }

        // get the field corresponding to path
        mi::mdl::IType_factory *tf         = m_compiler->get_type_factory();
        mi::mdl::IType const   *field_type = NULL;
        mi::base::Handle<MDL::IExpression const> field(
            compiled_material->lookup_sub_expression(m_db_transaction, path, tf, &field_type));

        if (!field.is_valid_interface()) {
            m_error = -2;
            return 0;
        }

        // reject constants if not explicitly enabled
        if (!m_compile_consts && field->get_kind() == MDL::IExpression::EK_CONSTANT) {
            m_error = -4;
            return 0;
        }

        // reject DFs or resources
        field_type = field_type->skip_type_alias();
        if (contains_df_type(field_type) || mi::mdl::is<mi::mdl::IType_resource>(field_type)) {
            m_error = -5;
            return 0;
        }

        // ... and fill up ...
        MDL::Mdl_dag_builder<mi::mdl::IDag_builder> builder(
            m_db_transaction, lambda,
            m_mdl_meters_per_scene_unit, m_mdl_wavelength_min,
            m_mdl_wavelength_max, compiled_material);
        mi::mdl::DAG_node const *expr = builder.int_expr_to_mdl_dag_node(field_type, field.get());

        mi::mdl::DAG_node const *body = lambda->get_body();
        if (body != NULL) {
            lambda->store_root_expr(body);
            lambda->set_body(NULL);
        }

        size_t idx = lambda->store_root_expr(expr);

        m_error = 0;
        return idx;
    }

    /// Enumerates all resources in the arguments of the compiled material.
    ///
    /// \param lambda             The lambda function used for converting the values.
    /// \param compiled_material  The compiled material.
    /// \param enumerator         The enumerator collecting the resources.
    void enumerate_resource_arguments(
        mi::mdl::ILambda_function *lambda,
        MDL::Mdl_compiled_material const *compiled_material,
        Function_enumerator &enumerator)
    {
        mi::mdl::IType_factory *type_factory = lambda->get_type_factory();
        mi::mdl::IValue_factory *value_factory = lambda->get_value_factory();

        for (mi::Size i = 0, n = compiled_material->get_parameter_count(); i < n; ++i) {
            mi::base::Handle<MI::MDL::IValue const> arg_val(compiled_material->get_argument(i));

            // skip any non-resources
            MI::MDL::IValue::Kind kind = arg_val->get_kind();
            if (kind != MI::MDL::IValue::VK_TEXTURE &&
                kind != MI::MDL::IValue::VK_LIGHT_PROFILE &&
                kind != MI::MDL::IValue::VK_BSDF_MEASUREMENT)
            {
                continue;
            }

            mi::base::Handle<MI::MDL::IType const> p_type(arg_val->get_type());
            mi::mdl::IType const *tp = convert_type(type_factory, p_type.get());

            mi::mdl::IValue const *mdl_value = int_value_to_mdl_value(
                m_db_transaction, value_factory, tp, arg_val.get());
            switch (kind) {
            case MI::MDL::IValue::VK_TEXTURE:
                enumerator.texture(mdl_value);
                break;
            case MI::MDL::IValue::VK_LIGHT_PROFILE:
                enumerator.light_profile(mdl_value);
                break;
            case MI::MDL::IValue::VK_BSDF_MEASUREMENT:
                enumerator.bsdf_measurement(mdl_value);
                break;
            default:
                MDL_ASSERT(!"unexpected kind");
                break;
            }
        }
    }

private:
    /// Check if the given type contains a *df type.
    ///
    /// \param type  the type to check
    static bool contains_df_type(mi::mdl::IType const *type)
    {
        type = type->skip_type_alias();
        if (mi::mdl::is<mi::mdl::IType_df>(type))
            return true;
        if (mi::mdl::IType_compound const *c_type = mi::mdl::as<mi::mdl::IType_compound>(type)) {
            for (int i = 0, n = c_type->get_compound_size(); i < n; ++i) {
                mi::mdl::IType const *e_tp = c_type->get_compound_type(i);

                if (contains_df_type(e_tp))
                    return true;
            }
        }
        return false;
    }

    /// The MDL compiler.
    mi::base::Handle<mi::mdl::IMDL> m_compiler;

    /// The used transaction.
    DB::Transaction *m_db_transaction;

    /// The meters per unit scale factor.
    mi::Float32 m_mdl_meters_per_scene_unit;

    /// The smallest supported wavelength.
    mi::Float32 m_mdl_wavelength_min;

    /// The largest supported wavelength.
    mi::Float32 m_mdl_wavelength_max;

    /// Reported errors is any
    Sint32 m_error;

    /// True, if constants should be compiled (else return error -4 instead)
    bool m_compile_consts;
};

} // anonymous

// --------------------- Target code register --------------------

/// A simple name register for target code.
class Target_code_register : public IResource_register {
public:
    struct Texture_entry {
        Texture_entry(
            size_t                                     index,
            MISTD::string const                        &name,
            mi::neuraylib::ITarget_code::Texture_shape type)
        : m_index(index), m_name(name), m_type(type)
        {
        }

        size_t                                     m_index;
        MISTD::string                              m_name;
        mi::neuraylib::ITarget_code::Texture_shape m_type;
    };

    typedef MISTD::vector<Texture_entry> Texture_resource_table;

    struct Res_entry {
        Res_entry(
            size_t                                     index,
            MISTD::string const                        &name)
        : m_index(index), m_name(name)
        {
        }

        size_t        m_index;
        MISTD::string m_name;
    };

    typedef MISTD::vector<Res_entry> Resource_table;

public:
    /// Constructor.
    Target_code_register()
    : m_texture_table()
    , m_light_profile_table()
    , m_bsdf_measurement_table()
    {
    }

    /// Destructor.
    virtual ~Target_code_register()
    {
    }

    /// Register a texture index.
    ///
    /// \param index  the texture index
    /// \param name   the DB name of this index
    /// \param type   the type of the texture
    virtual void register_texture(
        size_t                                     index,
        char const                                 *name,
        mi::neuraylib::ITarget_code::Texture_shape type)
    {
        m_texture_table.push_back(Texture_entry(index, name, type));
    }

    /// Return the number of texture resources.
    virtual size_t get_texture_count() const
    {
        return m_texture_table.size();
    }

    /// Register a light profile.
    ///
    /// \param index  the light profile index
    /// \param name   the DB name of this index
    virtual void register_light_profile(
        size_t                                     index,
        char const                                 *name)
    {
        m_light_profile_table.push_back(Res_entry(index, name));
    }

    /// Return the number of light profile resources.
    virtual size_t get_light_profile_count() const
    {
        return m_light_profile_table.size();
    }

    /// Register a bsdf measurement.
    ///
    /// \param index  the bsdf measurement index
    /// \param name   the DB name of this index
    virtual void register_bsdf_measurement(
        size_t                                     index,
        char const                                 *name)
    {
        m_bsdf_measurement_table.push_back(Res_entry(index, name));
    }

    /// Return the number of bsdf measurement resources.
    virtual size_t get_bsdf_measurement_count() const
    {
        return m_bsdf_measurement_table.size();
    }

    /// Retrieve the texture resource table.
    Texture_resource_table const &get_texture_table() const { return m_texture_table; }

    /// Retrieve the light profile resource table.
    Resource_table const &get_light_profile_table() const { return m_light_profile_table; }

    /// Retrieve the texture resource table.
    Resource_table const &get_bsdf_measurement_table() const { return m_bsdf_measurement_table; }

private:
    /// The texture resource table.
    Texture_resource_table m_texture_table;

    /// The light profile resource table.
    Resource_table m_light_profile_table;

    /// The bsdf measurement resource table.
    Resource_table m_bsdf_measurement_table;
};

/// Copy Data from the register facility to the target code.
static void fill_resource_tables(Target_code_register const &tc_reg, Target_code *tc)
{
    typedef Target_code_register::Texture_resource_table TRT;
    typedef Target_code_register::Texture_entry          TRTE;

    TRT const &txt_table = tc_reg.get_texture_table();

    for (TRT::const_iterator it(txt_table.begin()), end(txt_table.end()); it != end; ++it) {
        TRTE const &entry = *it;
        tc->add_texture_index(entry.m_index, entry.m_name, entry.m_type);
    }

    typedef Target_code_register::Resource_table RT;
    typedef Target_code_register::Res_entry      RTE;

    RT const &lp_table = tc_reg.get_light_profile_table();

    for (RT::const_iterator it(lp_table.begin()), end(lp_table.end()); it != end; ++it) {
        RTE const &entry = *it;
        tc->add_light_profile_index(entry.m_index, entry.m_name);
    }

    RT const &bm_table = tc_reg.get_bsdf_measurement_table();

    for (RT::const_iterator it(bm_table.begin()), end(bm_table.end()); it != end; ++it) {
        RTE const &entry = *it;
        tc->add_bsdf_measurement_index(entry.m_index, entry.m_name);
    }
}

// --------------------- Target argument block class --------------------

Target_argument_block::Target_argument_block(mi::Size arg_block_size)
: m_size(arg_block_size)
, m_data(new char[arg_block_size])
{
}

Target_argument_block::~Target_argument_block()
{
    delete[] m_data;
    m_data = NULL;
}

char const *Target_argument_block::get_data() const
{
    return m_data;
}

char *Target_argument_block::get_data()
{
    return m_data;
}

Size Target_argument_block::get_size() const
{
    return m_size;
}

mi::neuraylib::ITarget_argument_block *Target_argument_block::clone() const
{
    Target_argument_block *cloned_block = new Target_argument_block(m_size);
    memcpy(cloned_block->get_data(), m_data, m_size);
    return cloned_block;
}

// ---------------------- Target value layout class ---------------------

// Constructor.
Target_value_layout::Target_value_layout(
    mi::mdl::IGenerated_code_value_layout const *layout,
    bool string_ids)
: m_layout(layout, mi::base::DUP_INTERFACE)
, m_strings_mapped_to_ids(string_ids)
{
}

// Get the size of the target argument block.
Size Target_value_layout::get_size() const
{
    if (m_layout == NULL)
        return 0;
    return m_layout->get_size();
}

// Get the number of arguments / elements at the given layout state.
Size Target_value_layout::get_num_elements(
    mi::neuraylib::Target_value_layout_state state) const
{
    if (m_layout == NULL)
        return ~Size(0);
    return m_layout->get_num_elements(
        mi::mdl::IGenerated_code_value_layout::State(state.m_state_offs, state.m_data_offs));
}

// Get the offset, the size and the kind of the argument / element inside the argument
// block at the given layout state.
Size Target_value_layout::get_layout(
    mi::neuraylib::IValue::Kind              &kind,
    Size                                     &arg_size,
    mi::neuraylib::Target_value_layout_state state) const
{
    if (m_layout == NULL) {
        arg_size = 0;
        kind = mi::neuraylib::IValue::VK_INVALID_DF;
        return ~Size(0);
    }

    mi::mdl::IValue::Kind mdl_kind = mi::mdl::IValue::VK_BAD;
    size_t as = arg_size;
    Size offset = m_layout->get_layout(
        mdl_kind,
        as,
        mi::mdl::IGenerated_code_value_layout::State(state.m_state_offs, state.m_data_offs));
    arg_size = as;

    // translate from MDL value kinds to Neuray value kinds
    switch (mdl_kind) {
        #define MAP_KIND(from, to) \
            case mi::mdl::IValue::from: kind = mi::neuraylib::IValue::to; break

        MAP_KIND(VK_BAD,              VK_INVALID_DF);
        MAP_KIND(VK_BOOL,             VK_BOOL);
        MAP_KIND(VK_INT,              VK_INT);
        MAP_KIND(VK_ENUM,             VK_ENUM);
        MAP_KIND(VK_FLOAT,            VK_FLOAT);
        MAP_KIND(VK_DOUBLE,           VK_DOUBLE);
        MAP_KIND(VK_STRING,           VK_STRING);
        MAP_KIND(VK_VECTOR,           VK_VECTOR);
        MAP_KIND(VK_MATRIX,           VK_MATRIX);
        MAP_KIND(VK_ARRAY,            VK_ARRAY);
        MAP_KIND(VK_RGB_COLOR,        VK_COLOR);
        MAP_KIND(VK_STRUCT,           VK_STRUCT);
        MAP_KIND(VK_INVALID_REF,      VK_INVALID_DF);
        MAP_KIND(VK_TEXTURE,          VK_TEXTURE);
        MAP_KIND(VK_LIGHT_PROFILE,    VK_LIGHT_PROFILE);
        MAP_KIND(VK_BSDF_MEASUREMENT, VK_BSDF_MEASUREMENT);

        #undef MAP_KIND
    }

    return offset;
}

// Get the layout state for the i'th argument / element inside the argument value block
// at the given layout state.
mi::neuraylib::Target_value_layout_state Target_value_layout::get_nested_state(
    mi::Size                                 i,
    mi::neuraylib::Target_value_layout_state state) const
{
    if (m_layout == NULL)
        return mi::neuraylib::Target_value_layout_state(~mi::Uint32(0), ~mi::Uint32(0));

    mi::mdl::IGenerated_code_value_layout::State mdl_state =
        m_layout->get_nested_state(
            i,
            mi::mdl::IGenerated_code_value_layout::State(state.m_state_offs, state.m_data_offs));

    return mi::neuraylib::Target_value_layout_state(
        mdl_state.m_state_offs, mdl_state.m_data_offs);
}

// Set the value inside the given block at the given layout state.
Sint32 Target_value_layout::set_value(
    char                                     *block,
    mi::neuraylib::IValue const              *value,
    mi::neuraylib::ITarget_resource_callback *resource_callback,
    mi::neuraylib::Target_value_layout_state state) const
{
    if (block == NULL || value == NULL || resource_callback == NULL)
        return -1;

    mi::neuraylib::IValue::Kind kind;
    Size arg_size;
    Size offs = get_layout(kind, arg_size, state);
    if (value->get_kind() != kind)
        return -3;

    switch (kind) {
        case mi::neuraylib::IValue::VK_BOOL:
            *reinterpret_cast<bool *>(block + offs) =
                static_cast<mi::neuraylib::IValue_bool const *>(value)->get_value();
            return 0;

        case mi::neuraylib::IValue::VK_INT:
            *reinterpret_cast<mi::Sint32 *>(block + offs) =
                static_cast<mi::neuraylib::IValue_int const *>(value)->get_value();
            return 0;

        case mi::neuraylib::IValue::VK_ENUM:
            *reinterpret_cast<mi::Sint32 *>(block + offs) =
                static_cast<mi::neuraylib::IValue_enum const *>(value)->get_value();
            return 0;

        case mi::neuraylib::IValue::VK_FLOAT:
            *reinterpret_cast<mi::Float32 *>(block + offs) =
                static_cast<mi::neuraylib::IValue_float const *>(value)->get_value();
            return 0;

        case mi::neuraylib::IValue::VK_DOUBLE:
            *reinterpret_cast<mi::Float64 *>(block + offs) =
                static_cast<mi::neuraylib::IValue_double const *>(value)->get_value();
            return 0;

        case mi::neuraylib::IValue::VK_STRING:
        {
            ASSERT(M_BACKENDS, !"unsupported string value");
            return -5;
        }

        case mi::neuraylib::IValue::VK_VECTOR:
        case mi::neuraylib::IValue::VK_MATRIX:
        case mi::neuraylib::IValue::VK_ARRAY:
        case mi::neuraylib::IValue::VK_COLOR:
        case mi::neuraylib::IValue::VK_STRUCT:
        {
            mi::neuraylib::IValue_compound const *comp_val =
                static_cast<mi::neuraylib::IValue_compound const *>(value);
            Size num = get_num_elements(state);
            if (comp_val->get_size() != num) return -4;

            // Set all nested values
            for (Size i = 0; i < num; ++i) {
                mi::base::Handle<const mi::neuraylib::IValue> sub_val(comp_val->get_value(i));
                Sint32 err = set_value(
                    block,
                    sub_val.get(),
                    resource_callback,
                    get_nested_state(i, state));
                if (err != 0)
                    return err;
            }
            return 0;
        }

        case mi::neuraylib::IValue::VK_TEXTURE:
        case mi::neuraylib::IValue::VK_LIGHT_PROFILE:
        case mi::neuraylib::IValue::VK_BSDF_MEASUREMENT:
        {
            mi::Uint32 index = resource_callback->get_resource_index(
                static_cast<mi::neuraylib::IValue_resource const *>(value));
            *reinterpret_cast<mi::Uint32 *>(block + offs) = index;
            return 0;
        }

        case mi::neuraylib::IValue::VK_INVALID_DF:
        case mi::neuraylib::IValue::VK_FORCE_32_BIT:
        {
            ASSERT(M_BACKENDS, !"unexpected value type");
            return -5;
        }
    }
    ASSERT(M_BACKENDS, !"unsupported value type");
    return -5;
}

// Set the value inside the given block at the given layout state.
Sint32 Target_value_layout::set_value(
    char                                     *block,
    MI::MDL::IValue const                    *value,
    ITarget_resource_callback_internal       *resource_callback,
    mi::neuraylib::Target_value_layout_state state) const
{
    if (block == NULL || value == NULL || resource_callback == NULL)
        return -1;

    mi::neuraylib::IValue::Kind kind;
    Size arg_size;
    Size offs = get_layout(kind, arg_size, state);

    // MI::MDL::IValue::Kind is identical to mi::neuraylib::IValue::Kind so just cast to compare.
    if (mi::neuraylib::IValue::Kind(value->get_kind()) != kind)
        return -3;

    switch (kind) {
        case mi::neuraylib::IValue::VK_BOOL:
            *reinterpret_cast<bool *>(block + offs) =
                static_cast<MI::MDL::IValue_bool const *>(value)->get_value();
            return 0;

        case mi::neuraylib::IValue::VK_INT:
            *reinterpret_cast<mi::Sint32 *>(block + offs) =
                static_cast<MI::MDL::IValue_int const *>(value)->get_value();
            return 0;

        case mi::neuraylib::IValue::VK_ENUM:
            *reinterpret_cast<mi::Sint32 *>(block + offs) =
                static_cast<MI::MDL::IValue_enum const *>(value)->get_value();
            return 0;

        case mi::neuraylib::IValue::VK_FLOAT:
            *reinterpret_cast<mi::Float32 *>(block + offs) =
                static_cast<MI::MDL::IValue_float const *>(value)->get_value();
            return 0;

        case mi::neuraylib::IValue::VK_DOUBLE:
            *reinterpret_cast<mi::Float64 *>(block + offs) =
                static_cast<MI::MDL::IValue_double const *>(value)->get_value();
            return 0;

        case mi::neuraylib::IValue::VK_STRING:
        {
            if (m_strings_mapped_to_ids) {
                mi::Uint32 id =
                    resource_callback != NULL ?
                    resource_callback->get_string_index(
                        static_cast<MI::MDL::IValue_string const *>(value)) :
                    0u;
                *reinterpret_cast<mi::Uint32 *>(block + offs) = id;
            } else {
                // unmapped string are not supported
                *reinterpret_cast<char **>(block + offs) = NULL;
            }
            return 0;
        }

        case mi::neuraylib::IValue::VK_VECTOR:
        case mi::neuraylib::IValue::VK_MATRIX:
        case mi::neuraylib::IValue::VK_ARRAY:
        case mi::neuraylib::IValue::VK_COLOR:
        case mi::neuraylib::IValue::VK_STRUCT:
        {
            MI::MDL::IValue_compound const *comp_val =
                static_cast<MI::MDL::IValue_compound const *>(value);
            Size num = get_num_elements(state);
            if (comp_val->get_size() != num)
                return -4;

            // Set all nested values
            for (Size i = 0; i < num; ++i) {
                mi::base::Handle<MI::MDL::IValue const> sub_val(comp_val->get_value(i));
                Sint32 err = set_value(
                    block,
                    sub_val.get(),
                    resource_callback,
                    get_nested_state(i, state));
                if (err != 0)
                    return err;
            }
            return 0;
        }

        case mi::neuraylib::IValue::VK_TEXTURE:
        case mi::neuraylib::IValue::VK_LIGHT_PROFILE:
        case mi::neuraylib::IValue::VK_BSDF_MEASUREMENT:
        {
            mi::Uint32 index = resource_callback->get_resource_index(
                static_cast<MI::MDL::IValue_resource const *>(value));
            *reinterpret_cast<mi::Uint32 *>(block + offs) = index;
            return 0;
        }

        case mi::neuraylib::IValue::VK_INVALID_DF:
        case mi::neuraylib::IValue::VK_FORCE_32_BIT:
        {
            ASSERT(M_BACKENDS, !"unexpected value type");
            return -5;
        }
    }
    ASSERT(M_BACKENDS, !"unsupported value type");
    return -5;
}

// ------------------------- LLVM based link unit -------------------------

static mi::mdl::ILink_unit *create_link_unit(Mdl_llvm_backend &llvm_be)
{
    mi::base::Handle<mi::mdl::ICode_generator_jit> be = llvm_be.get_jit_be();
    if (be.is_valid_interface()) {
        mi::mdl::ICode_generator_jit::Compilation_mode comp_mode;

        switch (llvm_be.get_kind()) {
        case mi::neuraylib::IMdl_compiler::MB_CUDA_PTX:
            comp_mode = mi::mdl::ICode_generator_jit::CM_PTX;
            break;

        case mi::neuraylib::IMdl_compiler::MB_LLVM_IR:
            comp_mode = mi::mdl::ICode_generator_jit::CM_LLVM_IR;
            break;

        case mi::neuraylib::IMdl_compiler::MB_NATIVE:
            comp_mode = mi::mdl::ICode_generator_jit::CM_NATIVE;
            break;

        default:
            return NULL;
        }

        return be->create_link_unit(
            comp_mode,
            llvm_be.get_enable_simd(),
            llvm_be.get_sm_version(),
            llvm_be.get_num_texture_spaces(),
            llvm_be.get_num_texture_results());
    }
    return NULL;
}


// Constructor from an LLVM backend.
Link_unit::Link_unit(
    Mdl_llvm_backend &llvm_be,
    DB::Transaction  *transaction)
: m_compiler(llvm_be.get_compiler())
, m_unit(create_link_unit(llvm_be))
, m_target_code(new Target_code(llvm_be.get_strings_mapped_to_ids()))
, m_transaction(transaction)
, m_tc_reg(new Target_code_register())
, m_res_index_map()
, m_tex_idx(0)
, m_lp_idx(0)
, m_bm_idx(0)
, m_compile_consts(llvm_be.get_compile_consts())
, m_strings_mapped_to_ids(llvm_be.get_strings_mapped_to_ids())
{
}


// Destructor.
Link_unit::~Link_unit()
{
    delete m_tc_reg;
}

// Add an MDL environment function call as a function to this link unit.
Sint32 Link_unit::add_environment(
    MDL::Mdl_function_call const *function_call,
    char const                   *fname,
    mi::Float32                  mdl_meters_per_scene_unit,
    mi::Float32                  mdl_wavelength_min,
    mi::Float32                  mdl_wavelength_max)
{
    if (function_call == NULL)
        return -2;

    if (m_transaction == NULL)
        return -1;

    Lambda_builder builder(
        m_compiler.get(),
        m_transaction,
        mdl_meters_per_scene_unit,
        mdl_wavelength_min,
        mdl_wavelength_max,
        m_compile_consts);

    mi::base::Handle<mi::mdl::ILambda_function> lambda(
        builder.env_from_call(function_call, fname));
    if (!lambda.is_valid_interface())
        return builder.get_error_code();

    // enumerate resources ...
    Function_enumerator enumerator(
        *m_tc_reg, lambda.get(), m_transaction, m_tex_idx, m_lp_idx, m_bm_idx, m_res_index_map);
    lambda->enumerate_resources(enumerator, lambda->get_body());

    // ... and add it to the compilation unit
    MDL::Mdl_call_resolver resolver(m_transaction);
    mi::base::Handle<mi::mdl::IGenerated_code_executable> code;

    size_t arg_block_index = ~0;
    bool res = m_unit->add(
        lambda.get(), &resolver, mi::mdl::ILink_unit::FK_ENVIRONMENT, &arg_block_index);
    if (!res)
        return -3;

    ASSERT(M_BACKENDS, arg_block_index == size_t(~0) &&
        "Environments should not have captured arguments");

    return 0;
}

Sint32 Link_unit::add_material_expression(
    MDL::Mdl_compiled_material const *compiled_material,
    char const                       *path,
    char const                       *fname)
{
    if (m_transaction == NULL || compiled_material == NULL || path == NULL)
        return -1;

    Lambda_builder builder(
        m_compiler.get(),
        m_transaction,
        compiled_material->get_mdl_meters_per_scene_unit(),
        compiled_material->get_mdl_wavelength_min(),
        compiled_material->get_mdl_wavelength_max(),
        m_compile_consts);

    mi::base::Handle<mi::mdl::ILambda_function> lambda(
        builder.from_sub_expr(compiled_material, path, fname));
    if (!lambda.is_valid_interface())
        return builder.get_error_code();

    // Enumerate resources ...
    Function_enumerator enumerator(
        *m_tc_reg, lambda.get(), m_transaction, m_tex_idx, m_lp_idx, m_bm_idx, m_res_index_map);
    lambda->enumerate_resources(enumerator, lambda->get_body());

    // ... also enumerate resources from arguments ...
    if (compiled_material->get_parameter_count() != 0)
        builder.enumerate_resource_arguments(lambda.get(), compiled_material, enumerator);

    // ... and add it to the compilation unit
    MDL::Mdl_call_resolver resolver(m_transaction);
    size_t arg_block_index = ~0;
    bool res = m_unit->add(
        lambda.get(), &resolver, mi::mdl::ILink_unit::FK_LAMBDA, &arg_block_index);

    if (!res) {
        MDL::report_messages(m_unit->access_messages(), /*out_messages=*/0);
        return -3;
    }

    // Was a target argument block layout created for this entity?
    if (arg_block_index != size_t(~0)) {
        // Add it to the target code and remember the arguments of the compiled material
        mi::base::Handle<mi::mdl::IGenerated_code_value_layout const> layout(
            m_unit->get_arg_block_layout(arg_block_index));
        mi::Size index = m_target_code->add_argument_block_layout(
            mi::base::make_handle(
                new Target_value_layout(layout.get(), m_strings_mapped_to_ids)).get());
        ASSERT(M_BACKENDS, index == arg_block_index && "Unit and target code should be in sync");

        m_arg_block_comp_material_args.push_back(
            mi::base::make_handle_dup(compiled_material->get_arguments()));
        ASSERT(M_BACKENDS, index == m_arg_block_comp_material_args.size() - 1 &&
            "Unit and arg block material arg list should be in sync");

        (void) index;  // avoid warning about unused variable
    }

    return 0;
}

Sint32 Link_unit::add_material_df(
    MDL::Mdl_compiled_material const *compiled_material,
    char const *path,
    char const *base_fname,
    bool include_geometry_normal)
{
    if (m_transaction == NULL || compiled_material == NULL || path == NULL)
        return -1;

    Lambda_builder lambda_builder(
        m_compiler.get(),
        m_transaction,
        compiled_material->get_mdl_meters_per_scene_unit(),
        compiled_material->get_mdl_wavelength_min(),
        compiled_material->get_mdl_wavelength_max(),
        m_compile_consts);


    // convert from an IExpression-based compiled material sub-expression
    // to a DAG_node-based distribution function consisting of
    //  - a main df containing the DF part and array and struct constants and
    //  - a number of expression lambdas containing the non-DF part
    mi::base::Handle<mi::mdl::IDistribution_function> dist_func(
        lambda_builder.from_material_df(
            compiled_material, path, base_fname, include_geometry_normal));
    if (!dist_func.is_valid_interface()) {
        return lambda_builder.get_error_code();
    }

    mi::base::Handle<mi::mdl::ILambda_function> main_df(
        dist_func->get_main_df());

    // ... enumerate resources: must be done before we compile ...
    //     all resource information will be collected in main_df
    Function_enumerator enumerator(
        *m_tc_reg, main_df.get(), m_transaction, m_tex_idx, m_lp_idx, m_bm_idx, m_res_index_map);
    main_df->enumerate_resources(enumerator, main_df->get_body());

    // ... also enumerate resources from arguments ...
    if (compiled_material->get_parameter_count() != 0)
        lambda_builder.enumerate_resource_arguments(main_df.get(), compiled_material, enumerator);

    size_t expr_lambda_count = dist_func->get_expr_lambda_count();
    for (size_t i = 0; i < expr_lambda_count; ++i) {
        mi::base::Handle<mi::mdl::ILambda_function> lambda(dist_func->get_expr_lambda(i));

        // also register the resources in lambda itself, so we see whether it accesses resources
        enumerator.set_additional_lambda(lambda.get());

        lambda->enumerate_resources(enumerator, lambda->get_body());
    }

    // ... optimize all expression lambdas
    MDL::Call_evaluator    call_evaluator(m_transaction);
    MDL::Mdl_call_resolver resolver(m_transaction);
    for (size_t i = 0, n = dist_func->get_expr_lambda_count(); i < n; ++i) {
        mi::base::Handle<mi::mdl::ILambda_function> lambda(dist_func->get_expr_lambda(i));
        lambda->optimize(&resolver, &call_evaluator);
    }

    // ... and add it to the compilation unit
    size_t arg_block_index = ~0;
    bool res = m_unit->add(dist_func.get(), &resolver, &arg_block_index);

    if (!res) {
        MDL::report_messages(m_unit->access_messages(), /*out_messages=*/0);
        return -3;
    }

    // Was a target argument block layout created for this entity?
    if (arg_block_index != size_t(~0)) {
        // Add it to the target code and remember the arguments of the compiled material
        mi::base::Handle<mi::mdl::IGenerated_code_value_layout const> layout(
            m_unit->get_arg_block_layout(arg_block_index));
        mi::Size index = m_target_code->add_argument_block_layout(
            mi::base::make_handle(
                new Target_value_layout(layout.get(), m_strings_mapped_to_ids)).get());
        ASSERT(M_BACKENDS, index == arg_block_index && "Unit and target code should be in sync");

        m_arg_block_comp_material_args.push_back(
            mi::base::make_handle_dup(compiled_material->get_arguments()));
        ASSERT(M_BACKENDS, index == m_arg_block_comp_material_args.size() - 1 &&
            "Unit and arg block material arg list should be in sync");

        (void) index;  // avoid warning about unused variable
    }

    return 0;
}

// Get the number of functions inside this link unit.
mi::Size Link_unit::get_num_functions() const
{
    return m_unit->get_function_count();
}

// Get the name of the i'th function inside this link unit.
char const *Link_unit::get_function_name(mi::Size i) const
{
    return m_unit->get_function_name(i);
}

// Get the name of the i'th function inside this link unit.
mi::neuraylib::ITarget_code::Function_kind Link_unit::get_function_kind(mi::Size i) const
{
    return mi::neuraylib::ITarget_code::Function_kind(m_unit->get_function_kind(i));
}

// Get the index of the target argument block layout for the i'th function inside this link
// unit if used.
mi::Size Link_unit::get_function_arg_block_layout_index(mi::Size i) const
{
    return mi::Size(m_unit->get_function_arg_block_layout_index(size_t(i)));
}

// Get the number of target argument block layouts used by this link unit.
mi::Size Link_unit::get_arg_block_layout_count() const
{
    return mi::Size(m_unit->get_arg_block_layout_count());
}

// Get the i'th target argument block layout used by this link unit.
mi::mdl::IGenerated_code_value_layout const *Link_unit::get_arg_block_layout(mi::Size i) const
{
    return m_unit->get_arg_block_layout(size_t(i));
}

// Get the MDL link unit.
mi::mdl::ILink_unit *Link_unit::get_compilation_unit() const
{
    m_unit->retain();
    return m_unit.get();
}


// Get the target code of this link unit.
Target_code *Link_unit::get_target_code() const
{
    m_target_code->retain();
    return m_target_code.get();
}

// ------------------------- LLVM based backend -------------------------

Mdl_llvm_backend::Mdl_llvm_backend(
    mi::neuraylib::IMdl_compiler::Mdl_backend_kind kind,
    mi::mdl::IMDL                                  *compiler,
    mi::mdl::ICode_generator_jit                   *jit,
    mi::mdl::ICode_cache                           *code_cache,
    bool                                           string_ids)
  : m_kind(kind),
    m_sm_version(20),
    m_num_texture_spaces(32),  // by default the number of texture spaces is 32
    m_num_texture_results(0),
    m_compiler(compiler, mi::base::DUP_INTERFACE),
    m_jit(jit, mi::base::DUP_INTERFACE),
    m_code_cache(code_cache, mi::base::DUP_INTERFACE),
    m_compile_consts(true),
    m_enable_simd(m_kind != mi::neuraylib::IMdl_compiler::MB_CUDA_PTX),
    m_output_ptx(true),
    m_strings_mapped_to_ids(string_ids)
{
    mi::mdl::Options &options = m_jit->access_options();

    // by default fast math is on
    options.set_option(MDL_JIT_OPTION_FAST_MATH, "true");

    // by default opt-level is 2
    options.set_option(MDL_JIT_OPTION_OPT_LEVEL, "2");

    // by default the internal space of the renderer is "world"
    options.set_option(MDL_CG_OPTION_INTERNAL_SPACE, "coordinate_world");

    // by default we support exceptions
    options.set_option(MDL_JIT_OPTION_DISABLE_EXCEPTIONS, "false");

    // by default we disable the read-only segment
    options.set_option(MDL_JIT_OPTION_ENABLE_RO_SEGMENT, "false");

    // by default we generate LLVM IR
    options.set_option(MDL_JIT_OPTION_WRITE_BITCODE, "false");

    // by default we link libdevice
    options.set_option(MDL_JIT_OPTION_LINK_LIBDEVICE, "true");

    // by default we do NOT use bitangent
    options.set_option(MDL_JIT_OPTION_USE_BITANGENT, "false");

    // by default we do NOT include the uniform state
    options.set_option(MDL_JIT_OPTION_INCLUDE_UNIFORM_STATE, "false");

    // by default we use vtable tex_lookup calls
    options.set_option(MDL_JIT_OPTION_TEX_LOOKUP_CALL_MODE, "vtable");

    // do we map strings to identifiers?
    options.set_option(MDL_JIT_OPTION_MAP_STRINGS_TO_IDS, string_ids ? "true" : "false");
}

/// Currently supported SM versions.
static struct Sm_versions { char const *name; unsigned code; } const known_sms[] = {
    { "20", 20 },
    { "30", 30 },
    { "35", 35 },
    { "37", 37 },
    { "50", 50 },
    { "52", 52 },
    { "60", 60 },
    { "61", 61 },
    { "62", 62 },
    { "70", 70 }
};

Sint32 Mdl_llvm_backend::set_option(
    char const *name,
    char const *value)
{
    if (name == NULL)
        return -1;
    if (value == NULL)
        return -2;

    // common options

    if (strcmp(name, "compile_constants") == 0) {
        if (strcmp(value, "off") == 0) {
            m_compile_consts = false;
        } else if (strcmp(value, "on") == 0) {
            m_compile_consts = true;
        } else {
            return -2;
        }
        return 0;
    }

    if (strcmp(name, "fast_math") == 0) {
        if (strcmp(value, "off") == 0) {
            value = "false";
        } else if (strcmp(value, "on") == 0) {
            value = "true";
        } else {
            return -2;
        }
        m_jit->access_options().set_option(MDL_JIT_OPTION_FAST_MATH, value);
        return 0;
    }
    if (strcmp(name, "opt_level") == 0) {
        if (strcmp(value, "0") == 0 || strcmp(value, "1") == 0 || strcmp(value, "2") == 0) {
            m_jit->access_options().set_option(MDL_JIT_OPTION_OPT_LEVEL, value);
            return 0;
        }
        return -2;
    }
    if (strcmp(name, "num_texture_spaces") == 0) {
        unsigned v = 0;
        if (sscanf(value, "%u", &v) != 1) {
            return -2;
        }
        m_num_texture_spaces = v;
        return 0;
    }
    if (strcmp(name, "internal_space") == 0) {
        if (strcmp(value, "world") == 0) {
            m_jit->access_options().set_option(MDL_CG_OPTION_INTERNAL_SPACE, "coordinate_world");
            return 0;
        } else if (strcmp(value, "object") == 0) {
            m_jit->access_options().set_option(MDL_CG_OPTION_INTERNAL_SPACE, "coordinate_object");
            return 0;
        }
        return -2;
    }

    // llvm specific options

    if (strcmp(name, "enable_exceptions") == 0) {
        // beware, the JIT backend has the inverse option
        if (strcmp(value, "off") == 0) {
            value = "true";
        } else if (strcmp(value, "on") == 0) {
            value = "false";
        } else {
            return -2;
        }
        m_jit->access_options().set_option(MDL_JIT_OPTION_DISABLE_EXCEPTIONS, value);
        return 0;
    }
    if (strcmp(name, "enable_ro_segment") == 0) {
        if (strcmp(value, "off") == 0) {
            value = "false";
        } else if (strcmp(value, "on") == 0) {
            value = "true";
        } else {
            return -2;
        }
        m_jit->access_options().set_option(MDL_JIT_OPTION_ENABLE_RO_SEGMENT, value);
        return 0;
    }
    if (strcmp(name, "num_texture_results") == 0) {
        unsigned v = 0;
        if (sscanf(value, "%u", &v) != 1) {
            return -2;
        }
        m_num_texture_results = v;
        return 0;
    }

    switch (m_kind) {
    case mi::neuraylib::IMdl_compiler::MB_CUDA_PTX:
        if (strcmp(name, "sm_version") == 0) {
            for (size_t i = 0, n = sizeof(known_sms) / sizeof(known_sms[0]); i < n; ++i) {
                if (strcmp(value, known_sms[i].name) == 0) {
                    m_sm_version = known_sms[i].code;
                    return 0;
                }
            }
            return  -2;
        }
        if (strcmp(name, "link_libdevice") == 0) {
            if (strcmp(value, "off") == 0) {
                value = "false";
            } else if (strcmp(value, "on") == 0) {
                value = "true";
            } else {
                return -2;
            }
            m_jit->access_options().set_option(MDL_JIT_OPTION_LINK_LIBDEVICE, value);
            return 0;
        }
        if (strcmp(name, "output_format") == 0) {
            bool enable_bc   = false;
            if (strcmp(value, "PTX") == 0) {
                m_output_ptx = true;
                enable_bc    = false;
            } else if (strcmp(value, "LLVM-IR") == 0) {
                m_output_ptx = false;
                enable_bc    = false;
            } else if (strcmp(value, "LLVM-BC") == 0) {
                m_output_ptx = false;
                enable_bc    = true;
            } else {
                return -2;
            }
            m_jit->access_options().set_option(
                MDL_JIT_OPTION_WRITE_BITCODE, enable_bc ? "true" : "false");
            return 0;
        }
        if (strcmp(name, "tex_lookup_call_mode") == 0) {
            if (strcmp(value, "vtable") == 0) {
            } else if (strcmp(value, "direct_call") == 0) {
            } else if (strcmp(value, "optix_cp") == 0) {
            } else {
                return -2;
            }
            m_jit->access_options().set_option(MDL_JIT_OPTION_TEX_LOOKUP_CALL_MODE, value);
            return 0;
        }
        break;

    case mi::neuraylib::IMdl_compiler::MB_LLVM_IR:
        if (strcmp(name, "enable_simd") == 0) {
            if (strcmp(value, "off") == 0) {
                m_enable_simd = false;
                return 0;
            } else if (strcmp(value, "on") == 0) {
                m_enable_simd = true;
                return 0;
            }
            return -2;
        }
        if (strcmp(name, "write_bitcode") == 0) {
            if (strcmp(value, "off") == 0) {
                value = "false";
            } else if (strcmp(value, "on") == 0) {
                value = "true";
            } else {
                return -2;
            }
            m_jit->access_options().set_option(MDL_JIT_OPTION_WRITE_BITCODE, value);
            return 0;
        }
        break;

    case mi::neuraylib::IMdl_compiler::MB_GLSL:
    case mi::neuraylib::IMdl_compiler::MB_NATIVE:
    case mi::neuraylib::IMdl_compiler::MB_FORCE_32_BIT:
        break;
    }
    return -1;
}

mi::Sint32 Mdl_llvm_backend::set_option_binary(
    char const *name,
    char const *data,
    mi::Size size)
{
    if (strcmp(name, "llvm_state_module") == 0) {
        m_jit->access_options().set_binary_option(
            MDL_JIT_BINOPTION_LLVM_STATE_MODULE, data, size);
        return 0;
    }
    return -1;
}

mi::neuraylib::ITarget_code const *Mdl_llvm_backend::translate_environment(
    DB::Transaction              *transaction,
    MDL::Mdl_function_call const *function_call,
    mi::Float32                  mdl_meters_per_scene_unit,
    mi::Float32                  mdl_wavelength_min,
    mi::Float32                  mdl_wavelength_max,
    char const                   *fname,
    Sint32                       *errors)
{
    Sint32 dummy_errors;
    if (errors == NULL)
        errors = &dummy_errors;

    if (transaction == NULL || function_call == NULL) {
        *errors = -1;
        return NULL;
    }

    Lambda_builder builder(
        m_compiler.get(),
        transaction,
        mdl_meters_per_scene_unit,
        mdl_wavelength_min,
        mdl_wavelength_max,
        m_compile_consts);

    mi::base::Handle<mi::mdl::ILambda_function> lambda(
        builder.env_from_call(function_call, fname));
    if (!lambda.is_valid_interface()) {
        *errors = builder.get_error_code();
        return NULL;
    }

    // enumerate resources: must be done before we compile
    Target_code_register tc_reg;
    Function_enumerator enumerator(tc_reg, lambda.get(), transaction);
    lambda->enumerate_resources(enumerator, lambda->get_body());

    // now compile
    MDL::Mdl_call_resolver resolver(transaction);
    mi::base::Handle<mi::mdl::IGenerated_code_executable> code;

    // currently supported only for LLVM-IR
    switch (m_kind) {
    case mi::neuraylib::IMdl_compiler::MB_LLVM_IR:
        code = mi::base::make_handle(
            m_jit->compile_into_llvm_ir(
                lambda.get(),
                &resolver,
                m_num_texture_spaces,
                m_num_texture_results,
                m_enable_simd));
        break;
    case mi::neuraylib::IMdl_compiler::MB_CUDA_PTX:
        code = mi::base::make_handle(
            m_jit->compile_into_ptx(
                m_code_cache.get(),
                lambda.get(),
                &resolver,
                m_num_texture_spaces,
                m_num_texture_results,
                m_sm_version,
                m_output_ptx));
        break;
    case mi::neuraylib::IMdl_compiler::MB_NATIVE:
        code = mi::base::make_handle(
            m_jit->compile_into_environment(
                lambda.get(),
                &resolver));
        break;
    default:
        break;
    }

    if (!code.is_valid_interface()) {
        *errors = -3;
        return NULL;
    }

    MDL::report_messages(code->access_messages(), /*out_messages=*/0);

    if (!code->is_valid()) {
        *errors = -3;
        return NULL;
    }

    Target_code *tc = new Target_code(code.get(), transaction, m_strings_mapped_to_ids);

    // Enter the resource-table here
    fill_resource_tables(tc_reg, tc);

    add_target_code_function(
        tc, lambda->get_name(), mi::neuraylib::ITarget_code::FK_ENVIRONMENT, ~0);

    size_t ro_size = 0;
    char const *data = code->get_ro_data_segment(ro_size);

    if (data) {
        tc->add_ro_segment("RO", reinterpret_cast<const unsigned char*>(data), ro_size);
    }

    // copy the string constant table
    for (size_t i = 0, n = code->get_string_constant_count(); i < n; ++i) {
        tc->add_string_constant_index(i, code->get_string_constant(i));
    }

    *errors = 0;
    return tc;
}

mi::neuraylib::ITarget_code const *Mdl_llvm_backend::translate_material_expression(
    DB::Transaction                  *transaction,
    MDL::Mdl_compiled_material const *compiled_material,
    char const                       *path,
    char const                       *fname,
    Sint32                           *errors)
{
    Sint32 dummy_errors;
    if (!errors)
        errors = &dummy_errors;

    if (!transaction || !compiled_material || !path) {
        *errors = -1;
        return NULL;
    }

    Lambda_builder builder(
        m_compiler.get(),
        transaction,
        compiled_material->get_mdl_meters_per_scene_unit(),
        compiled_material->get_mdl_wavelength_min(),
        compiled_material->get_mdl_wavelength_max(),
        m_compile_consts);

    mi::base::Handle<mi::mdl::ILambda_function> lambda(
        builder.from_sub_expr(compiled_material, path, fname));
    if (!lambda.is_valid_interface()) {
        *errors = builder.get_error_code();
        return NULL;
    }

    // ... enumerate resources: must be done before we compile ...
    Target_code_register tc_reg;
    Function_enumerator enumerator(tc_reg, lambda.get(), transaction);
    lambda->enumerate_resources(enumerator, lambda->get_body());

    // ... also enumerate resources from arguments ...
    if (compiled_material->get_parameter_count() != 0)
        builder.enumerate_resource_arguments(lambda.get(), compiled_material, enumerator);

    // ... and compile
    MDL::Mdl_call_resolver resolver(transaction);
    mi::base::Handle<mi::mdl::IGenerated_code_executable> code;
    switch (m_kind) {
    case mi::neuraylib::IMdl_compiler::MB_LLVM_IR:
        code = mi::base::make_handle(
            m_jit->compile_into_llvm_ir(
                lambda.get(),
                &resolver,
                m_num_texture_spaces,
                m_num_texture_results,
                m_enable_simd));
        break;
    case mi::neuraylib::IMdl_compiler::MB_CUDA_PTX:
        code = mi::base::make_handle(
            m_jit->compile_into_ptx(
                m_code_cache.get(),
                lambda.get(),
                &resolver,
                m_num_texture_spaces,
                m_num_texture_results,
                m_sm_version,
                m_output_ptx));
        break;
    case mi::neuraylib::IMdl_compiler::MB_NATIVE:
        code = mi::base::make_handle(
            m_jit->compile_into_generic_function(
                lambda.get(),
                &resolver,
                m_num_texture_spaces,
                m_num_texture_results,
                /*transformer=*/NULL));
        break;
    default:
        break;
    }

    if (!code.is_valid_interface()) {
        *errors = -3;
        return NULL;
    }

    MDL::report_messages(code->access_messages(), /*out_messages=*/0);

    if (!code->is_valid()) {
        *errors = -3;
        return NULL;
    }

    Target_code *tc = new Target_code(code.get(), transaction, m_strings_mapped_to_ids);

    // Enter the resource-table here
    fill_resource_tables(tc_reg, tc);

    mi::Size arg_block_index = ~0;
    if (compiled_material->get_parameter_count() != 0) {
        tc->init_argument_block(0, transaction, compiled_material->get_arguments());
        arg_block_index = 0;
    }

    add_target_code_function(
        tc, lambda->get_name(), mi::neuraylib::ITarget_code::FK_LAMBDA, arg_block_index);

    size_t ro_size = 0;
    char const *data = code->get_ro_data_segment(ro_size);
    if (data != NULL) {
        tc->add_ro_segment("RO", reinterpret_cast<const unsigned char*>(data), ro_size);
    }

    // copy the string constant table
    for (size_t i = 0, n = code->get_string_constant_count(); i < n; ++i) {
        tc->add_string_constant_index(i, code->get_string_constant(i));
    }

    *errors = 0;
    return tc;
}

mi::neuraylib::ITarget_code const *Mdl_llvm_backend::translate_material_expressions(
    DB::Transaction                  *transaction,
    MDL::Mdl_compiled_material const *compiled_material,
    char const * const               paths[],
    mi::Uint32                       path_cnt,
    char const                       *fname,
    mi::Sint32                       *errors)
{
    Sint32 dummy_errors;
    if (!errors)
        errors = &dummy_errors;

    if (!transaction || !compiled_material || !paths || path_cnt < 1) {
        *errors = -1;
        return NULL;
    }

    if (compiled_material->get_parameter_count() > 0) {
        *errors = -6;
        return NULL;
    }

    Lambda_builder builder(
        m_compiler.get(),
        transaction,
        compiled_material->get_mdl_meters_per_scene_unit(),
        compiled_material->get_mdl_wavelength_min(),
        compiled_material->get_mdl_wavelength_max(),
        m_compile_consts);

    // create the first expression
    mi::base::Handle<mi::mdl::ILambda_function> lambda(
        builder.from_sub_expr(compiled_material, paths[0], fname));
    if (!lambda.is_valid_interface()) {
        *errors = builder.get_error_code();
        return NULL;
    }

    mi::mdl::DAG_node const *body = lambda->get_body();
    if (body != NULL) {
        // transform to switch lambda
        lambda->store_root_expr(body);
        lambda->set_body(NULL);
    }

    // add all others
    for (mi::Uint32 i = 1; i < path_cnt; ++i) {
        if (builder.add_sub_expr(lambda.get(), compiled_material, paths[i]) == 0) {
            *errors = builder.get_error_code();
            return NULL;
        }
    }

    // ... enumerate resources: must be done before we compile ...
    Target_code_register tc_reg;
    Function_enumerator enumerator(tc_reg, lambda.get(), transaction);

    for (mi::Uint32 i = 0; i < path_cnt; ++i) {
        lambda->enumerate_resources(enumerator, lambda->get_root_expr(i));
    }

    // ... also enumerate resources from arguments ...
    if (compiled_material->get_parameter_count() != 0)
        builder.enumerate_resource_arguments(lambda.get(), compiled_material, enumerator);

    // ... and compile
    MDL::Mdl_call_resolver resolver(transaction);
    mi::base::Handle<mi::mdl::IGenerated_code_executable> code;
    switch (m_kind) {
    case mi::neuraylib::IMdl_compiler::MB_LLVM_IR:
        code = mi::base::make_handle(
            m_jit->compile_into_llvm_ir(
            lambda.get(),
            &resolver,
            m_num_texture_spaces,
            m_num_texture_results,
            m_enable_simd));
        break;
    case mi::neuraylib::IMdl_compiler::MB_CUDA_PTX:
        code = mi::base::make_handle(
            m_jit->compile_into_ptx(
                m_code_cache.get(),
                lambda.get(),
                &resolver,
                m_num_texture_spaces,
                m_num_texture_results,
                m_sm_version,
                m_output_ptx));
        break;
    case mi::neuraylib::IMdl_compiler::MB_NATIVE:
        code = mi::base::make_handle(
            m_jit->compile_into_generic_function(
                lambda.get(),
                &resolver,
                m_num_texture_spaces,
                m_num_texture_results,
                /*transformer=*/NULL));
        break;
    default:
        break;
    }

    if (!code.is_valid_interface()) {
        *errors = -3;
        return NULL;
    }

    MDL::report_messages(code->access_messages(), /*out_messages=*/0);

    if (!code->is_valid()) {
        *errors = -3;
        return NULL;
    }

    Target_code *tc = new Target_code(code.get(), transaction, m_strings_mapped_to_ids);

    // Enter the resource-table here
    fill_resource_tables(tc_reg, tc);

    mi::Size arg_block_index = ~0;
    if (compiled_material->get_parameter_count() != 0) {
        tc->init_argument_block(0, transaction, compiled_material->get_arguments());
        arg_block_index = 0;
    }

    add_target_code_function(
        tc, lambda->get_name(), mi::neuraylib::ITarget_code::FK_SWITCH_LAMBDA, arg_block_index);

    size_t ro_size = 0;
    char const *data = code->get_ro_data_segment(ro_size);
    if (data != NULL) {
        tc->add_ro_segment("RO", reinterpret_cast<const unsigned char*>(data), ro_size);
    }

    // copy the string constant table
    for (size_t i = 0, n = code->get_string_constant_count(); i < n; ++i) {
        tc->add_string_constant_index(i, code->get_string_constant(i));
    }

    *errors = 0;
    return tc;
}

static mi::mdl::Matrix4x4_struct const &convert_matrix(
    mi::Float32_4_4_struct const &world_to_obj)
{
    // we assume the structs have the same layout here
    return reinterpret_cast<mi::mdl::Matrix4x4_struct const &>(world_to_obj.xx);
}

static mi::mdl::Float4_struct const &get_row(
    mi::mdl::Matrix4x4_struct const &matrix,
    size_t                          index)
{
    return reinterpret_cast<mi::mdl::Float4_struct const &>(matrix.elements[4 * index]);
}

mi::neuraylib::ITarget_code const *Mdl_llvm_backend::translate_material_expression_uniform_state(
    DB::Transaction                  *transaction,
    MDL::Mdl_compiled_material const *compiled_material,
    char const                       *path,
    char const                       *fname,
    mi::Float32_4_4_struct const     &world_to_obj,
    mi::Float32_4_4_struct const     &obj_to_world,
    mi::Sint32                       object_id,
    mi::Sint32                       *errors)
{
    Sint32 dummy_errors;
    if (errors == NULL)
        errors = &dummy_errors;

    if (!transaction || !compiled_material || !path) {
        *errors = -1;
        return NULL;
    }

    if (compiled_material->get_parameter_count() > 0) {
        *errors = -6;
        return NULL;
    }

    Lambda_builder builder(
        m_compiler.get(),
        transaction,
        compiled_material->get_mdl_meters_per_scene_unit(),
        compiled_material->get_mdl_wavelength_min(),
        compiled_material->get_mdl_wavelength_max(),
        m_compile_consts);

    mi::base::Handle<mi::mdl::ILambda_function> lambda(
        builder.from_sub_expr(compiled_material, path, fname));
    if (!lambda.is_valid_interface()) {
        *errors = builder.get_error_code();
        return NULL;
    }

    mi::mdl::DAG_node const *body = lambda->get_body();

    mi::mdl::Matrix4x4_struct const &w2o(convert_matrix(world_to_obj));
    mi::mdl::Float4_struct w2o_vec[4];
    w2o_vec[0] = get_row(w2o, 0);
    w2o_vec[1] = get_row(w2o, 1);
    w2o_vec[2] = get_row(w2o, 2);
    w2o_vec[3] = get_row(w2o, 3);

    mi::mdl::Matrix4x4_struct const &o2w(convert_matrix(obj_to_world));
    mi::mdl::Float4_struct o2w_vec[4];
    o2w_vec[0] = get_row(o2w, 0);
    o2w_vec[1] = get_row(o2w, 1);
    o2w_vec[2] = get_row(o2w, 2);
    o2w_vec[3] = get_row(o2w, 3);

    MDL::Mdl_call_resolver resolver(transaction);
    body = lambda->set_uniform_context(&resolver, body, w2o_vec, o2w_vec, object_id);
    lambda->set_body(body);

    // ... enumerate resources: must be done before we compile ...
    Target_code_register tc_reg;
    Function_enumerator enumerator(tc_reg, lambda.get(), transaction);
    lambda->enumerate_resources(enumerator, body);

    // ... also enumerate resources from arguments ...
    if (compiled_material->get_parameter_count() != 0)
        builder.enumerate_resource_arguments(lambda.get(), compiled_material, enumerator);

    // ... and compile
    mi::base::Handle<mi::mdl::IGenerated_code_executable> code;
    switch (m_kind) {
    case mi::neuraylib::IMdl_compiler::MB_LLVM_IR:
        code = mi::base::make_handle(
            m_jit->compile_into_llvm_ir(
                lambda.get(),
                &resolver,
                m_num_texture_spaces,
                m_num_texture_results,
                m_enable_simd));
        break;
    case mi::neuraylib::IMdl_compiler::MB_CUDA_PTX:
        code = mi::base::make_handle(
            m_jit->compile_into_ptx(
                m_code_cache.get(),
                lambda.get(),
                &resolver,
                m_num_texture_spaces,
                m_num_texture_results,
                m_sm_version,
                m_output_ptx));
        break;
    case mi::neuraylib::IMdl_compiler::MB_NATIVE:
        code = mi::base::make_handle(
            m_jit->compile_into_generic_function(
                lambda.get(),
                &resolver,
                m_num_texture_spaces,
                m_num_texture_results,
                /*transformer=*/NULL));
        break;
    default:
        break;
    }

    if (!code.is_valid_interface()) {
        *errors = -3;
        return NULL;
    }

    MDL::report_messages(code->access_messages(), /*out_messages=*/0);

    if (!code->is_valid()) {
        *errors = -3;
        return NULL;
    }

    Target_code *tc = new Target_code(code.get(), transaction, m_strings_mapped_to_ids);

    // Enter the resource-table here
    fill_resource_tables(tc_reg, tc);

    mi::Size arg_block_index = ~0;
    if (compiled_material->get_parameter_count() != 0) {
        tc->init_argument_block(0, transaction, compiled_material->get_arguments());
        arg_block_index = 0;
    }

    add_target_code_function(
        tc, lambda->get_name(), mi::neuraylib::ITarget_code::FK_LAMBDA, arg_block_index);

    size_t ro_size = 0;
    char const *data = code->get_ro_data_segment(ro_size);
    if (data != NULL) {
        tc->add_ro_segment("RO", reinterpret_cast<const unsigned char*>(data), ro_size);
    }

    // copy the string constant table
    for (size_t i = 0, n = code->get_string_constant_count(); i < n; ++i) {
        tc->add_string_constant_index(i, code->get_string_constant(i));
    }

    *errors = 0;
    return tc;
}

const mi::neuraylib::ITarget_code* Mdl_llvm_backend::translate_material_df(
    DB::Transaction* transaction,
    const MDL::Mdl_compiled_material* compiled_material,
    const char* path,
    const char* base_fname,
    bool include_geometry_normal,
    Sint32* errors)
{
    Sint32 dummy_errors;
    if (!errors)
        errors = &dummy_errors;

    Lambda_builder lambda_builder(
        m_compiler.get(),
        transaction,
        compiled_material->get_mdl_meters_per_scene_unit(),
        compiled_material->get_mdl_wavelength_min(),
        compiled_material->get_mdl_wavelength_max(),
        m_compile_consts);


    // convert from an IExpression-based compiled material sub-expression
    // to a DAG_node-based distribution function consisting of
    //  - a main df containing the DF part and array and struct constants and
    //  - a number of expression lambdas containing the non-DF part
    mi::base::Handle<mi::mdl::IDistribution_function> dist_func(
        lambda_builder.from_material_df(
            compiled_material, path, base_fname, include_geometry_normal));
    if (!dist_func.is_valid_interface()) {
        *errors = lambda_builder.get_error_code();
        return NULL;
    }

    mi::base::Handle<mi::mdl::ILambda_function> main_df(
        dist_func->get_main_df());

    // ... enumerate resources: must be done before we compile ...
    //     all resource information will be collected in main_df
    Target_code_register tc_reg;
    Function_enumerator enumerator(tc_reg, main_df.get(), transaction);
    main_df->enumerate_resources(enumerator, main_df->get_body());

    // ... also enumerate resources from arguments ...
    if (compiled_material->get_parameter_count() != 0)
        lambda_builder.enumerate_resource_arguments(main_df.get(), compiled_material, enumerator);

    size_t expr_lambda_count = dist_func->get_expr_lambda_count();
    for (size_t i = 0; i < expr_lambda_count; ++i) {
        mi::base::Handle<mi::mdl::ILambda_function> lambda(dist_func->get_expr_lambda(i));

        // also register the resources in lambda itself, so we see whether it accesses resources
        enumerator.set_additional_lambda(lambda.get());

        lambda->enumerate_resources(enumerator, lambda->get_body());
    }

    // ... optimize all expression lambdas
    MDL::Call_evaluator    call_evaluator(transaction);
    MDL::Mdl_call_resolver resolver(transaction);
    for (size_t i = 0, n = dist_func->get_expr_lambda_count(); i < n; ++i) {
        mi::base::Handle<mi::mdl::ILambda_function> lambda(dist_func->get_expr_lambda(i));
        lambda->optimize(&resolver, &call_evaluator);
    }

    // ... and compile
    mi::base::Handle<mi::mdl::IGenerated_code_executable> code;

    switch (m_kind) {
    case mi::neuraylib::IMdl_compiler::MB_CUDA_PTX:
        code = mi::base::make_handle(
            m_jit->compile_distribution_function_gpu(
                dist_func.get(),
                &resolver,
                m_num_texture_spaces,
                m_num_texture_results,
                m_sm_version,
                m_output_ptx));
        break;
    case mi::neuraylib::IMdl_compiler::MB_NATIVE:
        code = mi::base::make_handle(
            m_jit->compile_distribution_function_cpu(
                dist_func.get(),
                &resolver,
                m_num_texture_spaces,
                m_num_texture_results));
        break;
    default:
        break;
    }

    if (!code.is_valid_interface()) {
        *errors = -3;
        return NULL;
    }

    MDL::report_messages(code->access_messages(), /*out_messages=*/0);

    if (!code->is_valid()) {
        *errors = -3;
        return NULL;
    }

    Target_code *tc = new Target_code(code.get(), transaction, m_strings_mapped_to_ids);

    // Enter the resource-table here
    fill_resource_tables(tc_reg, tc);

    mi::Size arg_block_index = ~0;
    if (compiled_material->get_parameter_count() != 0) {
        tc->init_argument_block(0, transaction, compiled_material->get_arguments());
        arg_block_index = 0;
    }

    add_target_code_function(
        tc, main_df->get_name() + MISTD::string("_init"),
        mi::neuraylib::ITarget_code::FK_DF_INIT, arg_block_index);
    add_target_code_function(
        tc, main_df->get_name() + MISTD::string("_sample"),
        mi::neuraylib::ITarget_code::FK_DF_SAMPLE, arg_block_index);
    add_target_code_function(
        tc, main_df->get_name() + MISTD::string("_evaluate"),
        mi::neuraylib::ITarget_code::FK_DF_EVALUATE, arg_block_index);
    add_target_code_function(
        tc, main_df->get_name() + MISTD::string("_pdf"),
        mi::neuraylib::ITarget_code::FK_DF_PDF, arg_block_index);

    size_t ro_size = 0;
    const char* data = code->get_ro_data_segment(ro_size);
    if (data) {
        tc->add_ro_segment("RO", reinterpret_cast<const unsigned char*>(data), ro_size);
    }

    // copy the string constant table
    for (size_t i = 0, n = code->get_string_constant_count(); i < n; ++i) {
        tc->add_string_constant_index(i, code->get_string_constant(i));
    }

    *errors = 0;
    return tc;
}

mi::Uint8 const *Mdl_llvm_backend::get_device_library(
    Size &size) const
{
    if (m_kind == mi::neuraylib::IMdl_compiler::MB_CUDA_PTX) {
        size_t              s  = 0;
        unsigned char const *r = m_jit->get_libdevice_for_gpu(s);

        size = Size(s);
        return r;
    }
    size = 0;
    return NULL;
}

mi::neuraylib::ITarget_code const *Mdl_llvm_backend::translate_link_unit(
    Link_unit const *lu,
    mi::Sint32      *errors)
{
    Sint32 dummy_errors;
    if (errors == NULL)
        errors = &dummy_errors;

    mi::base::Handle<mi::mdl::IGenerated_code_executable> code(
        m_jit->compile_unit(mi::base::make_handle(lu->get_compilation_unit()).get()));

    if (!code.is_valid_interface()) {
        *errors = -2;
        return NULL;
    }

    MDL::report_messages(code->access_messages(), /*out_messages=*/0);

    if (!code->is_valid()) {
        *errors = -2;
        return NULL;
    }

    mi::base::Handle<Target_code> tc(lu->get_target_code());
    tc->finalize(code.get(), lu->get_transaction());

    // Enter the resource-table here
    fill_resource_tables(*lu->get_tc_reg(), tc.get());

    // Add all functions to the target code
    for (size_t i = 0, n_args = lu->get_num_functions(); i < n_args; ++i) {
        char const *fname = lu->get_function_name(i);
        add_target_code_function(
            tc.get(),
            fname,
            mi::neuraylib::ITarget_code::Function_kind(lu->get_function_kind(i)),
            lu->get_function_arg_block_layout_index(i));
    }

    // Copy the string constant table: This must be done before the target argument
    // block is created, because it might contain string values (mapped to IDs).
    for (size_t i = 0, n_args = code->get_string_constant_count(); i < n_args; ++i) {
        tc->add_string_constant_index(i, code->get_string_constant(i));
    }

    // Create all target argument blocks, now that all resources are known
    {
        MISTD::vector<mi::base::Handle<MDL::IValue_list const> > const &args =
            lu->get_arg_block_comp_material_args();
        DB::Transaction *trans = lu->get_transaction();
        for (size_t i = 0, n_args = args.size(); i < n_args; ++i) {
            tc->init_argument_block(
                i,
                trans,
                args[i].get());
        }
    }

    size_t ro_size = 0;
    char const *data = code->get_ro_data_segment(ro_size);
    if (data != NULL) {
        tc->add_ro_segment("RO", reinterpret_cast<const unsigned char*>(data), ro_size);
    }

    *errors = 0;
    tc->retain();
    return tc.get();
}

void Mdl_llvm_backend::add_target_code_function(
    Target_code *tc,
    MISTD::string const &name,
    mi::neuraylib::ITarget_code::Function_kind func_kind,
    mi::Size arg_block_index)
{
    size_t index = tc->add_function(name, func_kind, arg_block_index);
    if (m_kind == mi::neuraylib::IMdl_compiler::MB_CUDA_PTX) {
        // PTX prototype
        MISTD::string p(".extern .func " + name);
        if (func_kind == mi::neuraylib::ITarget_code::FK_DF_INIT)
            p += "(.param .b64 a, .param .b64 b, .param .b64 c, .param .b64 d);";
        else if (func_kind == mi::neuraylib::ITarget_code::FK_SWITCH_LAMBDA)
            p += "(.param .b64 a, .param .b64 b, .param .b64 c, .param .b64 d, .param .b64 e, "
                ".param .b64 f);";
        else
            p += "(.param .b64 a, .param .b64 b, .param .b64 c, .param .b64 d, .param .b64 e);";
        tc->set_function_prototype(index, mi::neuraylib::ITarget_code::SL_PTX, p);

        // CUDA prototype
        p = "extern " + name;
        if (func_kind == mi::neuraylib::ITarget_code::FK_DF_INIT)
            p += "(void *, void *, void *, void *);";
        else if (func_kind == mi::neuraylib::ITarget_code::FK_SWITCH_LAMBDA)
            p += "(void *, void *, void *, void *, void *, int);";
        else
            p += "(void *, void *, void *, void *, void *);";

        tc->set_function_prototype(index, mi::neuraylib::ITarget_code::SL_CUDA, p);
    }

    // TODO: native, LLVM-IR
}


} // namespace BACKENDS

} // namespace MI

