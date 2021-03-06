/***************************************************************************************************
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
 **************************************************************************************************/

#include "pch.h"

#include "i_mdl_elements_module.h"

#include "i_mdl_elements_compiled_material.h"
#include "i_mdl_elements_function_call.h"
#include "i_mdl_elements_function_definition.h"
#include "i_mdl_elements_material_definition.h"
#include "i_mdl_elements_material_instance.h"
#include "i_mdl_elements_utilities.h"
#include "mdl_elements_ast_builder.h"
#include "mdl_elements_detail.h"
#include "mdl_elements_utilities.h"

#include <sstream>
#include <mi/mdl/mdl_code_generators.h>
#include <mi/mdl/mdl_generated_dag.h>
#include <mi/mdl/mdl_mdl.h>
#include <mi/mdl/mdl_modules.h>
#include <mi/mdl/mdl_definitions.h>
#include <mi/mdl/mdl_thread_context.h>
#include <mi/neuraylib/istring.h>
#include <base/system/main/access_module.h>
#include <boost/core/ignore_unused.hpp>
#include <base/lib/log/i_log_logger.h>
#include <base/data/db/i_db_access.h>
#include <base/data/db/i_db_transaction.h>
#include <base/data/serial/i_serializer.h>
#include <mdl/integration/mdlnr/i_mdlnr.h>
#include <mdl/compiler/compilercore/compilercore_modules.h>
#include <io/scene/scene/i_scene_journal_types.h>
#include <io/scene/bsdf_measurement/i_bsdf_measurement.h>
#include <io/scene/lightprofile/i_lightprofile.h>
#include <io/scene/texture/i_texture.h>

namespace MI {

namespace MDL {


// Interface to access Material instances and Function calls in a uniform way
class ICall {
public:
    /// Get the absolute name of the entity.
    virtual char const *get_abs_name() const = 0;

    /// Get the argument list.
    virtual const IExpression_list* get_arguments() const = 0;

    /// Get the parameter types.
    virtual const IType_list* get_parameter_types() const = 0;
};

namespace {

/// Helper class for dropping module imports at destruction.
class Drop_import_scope
{
public:
    Drop_import_scope( const mi::mdl::IModule* module)
      : m_module( module, mi::base::DUP_INTERFACE)
    {
    }

    ~Drop_import_scope() { m_module->drop_import_entries(); }

private:
    mi::base::Handle<const mi::mdl::IModule> m_module;
};

// Implements the ICall interface for a function call.
class Function_call : public ICall
{
    typedef ICall Base;
public:
    /// Get the absolute name of the entity.
    virtual char const *get_abs_name() const
    {
        // so far it is always "material"
        return "material";
    }

    /// Get the argument list.
    virtual const IExpression_list* get_arguments() const
    {
        return m_call.get_arguments();
    }

    /// Get the parameter types.
    virtual const IType_list* get_parameter_types() const
    {
        return m_call.get_parameter_types();
    }

public:
    /// Constructor.
    ///
    /// \param call  a MDL function call
    Function_call(Mdl_function_call const &call)
    : Base()
    , m_call(call)
    {
    }

private:
    // The mdl function call.
    Mdl_function_call const &m_call;
};

// Implements the ICall interface for a material instance.
class Material_call : public ICall
{
    typedef ICall Base;
public:
    /// Get the absolute name of the entity.
    virtual char const *get_abs_name() const
    {
        return m_inst.get_mdl_material_definition();
    }

    /// Get the argument list.
    virtual const IExpression_list* get_arguments() const
    {
        return m_inst.get_arguments();
    }

    /// Get the parameter types.
    virtual const IType_list* get_parameter_types() const
    {
        return m_inst.get_parameter_types();
    }

public:
    /// Constructor.
    ///
    /// \param inst  a MDL material instance
    Material_call(Mdl_material_instance const &inst)
    : Base()
    , m_inst(inst)
    {
    }

private:
    // The MDL material instance.
    Mdl_material_instance const &m_inst;
};

}  // anonymous

mi::Sint32 Mdl_module::create_module(
    DB::Transaction* transaction,
    const char* module_name,
    std::vector<Message>* messages)
{
    ASSERT( M_SCENE, module_name);

    SYSTEM::Access_module<MDLC::Mdlc_module> mdlc_module( false);
    mi::base::Handle<mi::mdl::IMDL> mdl( mdlc_module->get_mdl());

    // Reject invalid module names (in particular, names containing slashes and backslashes).
    if( !is_valid_module_name( module_name, mdl.get()))
        return -1;

    // Check whether the module exists already in the DB.
    std::string db_module_name = add_mdl_db_prefix( module_name);
    DB::Tag db_module_tag = transaction->name_to_tag( db_module_name.c_str());
    if( db_module_tag) {
        if( transaction->get_class_id( db_module_tag) != Mdl_module::id) {
            LOG::mod_log->error( M_SCENE, LOG::Mod_log::C_DATABASE,
                "DB name for module \"%s\" already in use.", db_module_name.c_str());
            return -3;
        }
        return 1;
    }

    Module_cache module_cache( transaction);
    mi::base::Handle<mi::mdl::IThread_context> ctx( mdl->create_thread_context());
    mi::base::Handle<const mi::mdl::IModule> module(
        mdl->load_module( ctx.get(), module_name, &module_cache));
    if( !module.is_valid_interface()) {
        report_messages( ctx->access_messages(), messages);
        return -2;
    }

    mi::Sint32 result
        = create_module_internal( transaction, mdl.get(), module.get(), messages);
    if( result < 0)
        return result;

    return result;
}

namespace {

// Wraps a mi::neuraylib::IReader as mi::mdl::IInput_stream.
class Input_stream : public mi::base::Interface_implement<mi::mdl::IInput_stream>
{
public:
    Input_stream( mi::neuraylib::IReader* reader) : m_reader( reader, mi::base::DUP_INTERFACE) { }
    int read_char()
    { char c; mi::Sint64 result = m_reader->read( &c, 1); return result <= 0 ? -1 : c; }
    const char* get_filename() { return 0; }
private:
    mi::base::Handle<mi::neuraylib::IReader> m_reader;
};

} // namespace

mi::Sint32 Mdl_module::create_module(
    DB::Transaction* transaction,
    const char* module_name,
    mi::neuraylib::IReader* module_source,
    std::vector<Message>* messages)
{
    ASSERT( M_SCENE, module_name);
    ASSERT( M_SCENE, module_source);

    SYSTEM::Access_module<MDLC::Mdlc_module> mdlc_module( false);
    mi::base::Handle<mi::mdl::IMDL> mdl( mdlc_module->get_mdl());

    // Reject invalid module names (in particular, names containing slashes and backslashes).
    if( !is_valid_module_name( module_name, mdl.get()) && strcmp( module_name, "::<neuray>") != 0)
        return -1;

    // Check whether the module exists already in the DB.
    std::string db_module_name = add_mdl_db_prefix( module_name);
    DB::Tag db_module_tag = transaction->name_to_tag( db_module_name.c_str());
    if( db_module_tag) {
        if( transaction->get_class_id( db_module_tag) != Mdl_module::id) {
            LOG::mod_log->error( M_SCENE, LOG::Mod_log::C_DATABASE,
                "DB name for module \"%s\" already in use.", db_module_name.c_str());
            return -3;
        }
        return 1;
    }

    Input_stream module_source_stream( module_source);
    Module_cache module_cache( transaction);
    mi::base::Handle<mi::mdl::IThread_context> ctx( mdl->create_thread_context());
    mi::base::Handle<const mi::mdl::IModule> module( mdl->load_module_from_stream(
        ctx.get(), &module_cache, module_name, &module_source_stream));
    if( !module.is_valid_interface()) {
        report_messages( ctx->access_messages(), messages);
        return -2;
     }

    return create_module_internal( transaction, mdl.get(), module.get(), messages);
}

mi::Sint32 Mdl_module::create_module(
    DB::Transaction* transaction,
    const char* module_name,
    const Variant_data* variant_data,
    mi::Size variant_count,
    std::vector<Message>* messages)
{
    ASSERT( M_SCENE, module_name);
    ASSERT( M_SCENE, variant_data);

    SYSTEM::Access_module<MDLC::Mdlc_module> mdlc_module( false);
    mi::base::Handle<mi::mdl::IMDL> mdl( mdlc_module->get_mdl());

    // Reject invalid module names (in particular, names containing slashes and backslashes).
    if( !is_valid_module_name( module_name, mdl.get()))
        return -1;

    // Check whether the module exists already in the DB.
    std::string db_module_name = add_mdl_db_prefix( module_name);
    DB::Tag db_module_tag = transaction->name_to_tag( db_module_name.c_str());
    if( db_module_tag) {
        if( transaction->get_class_id( db_module_tag) != Mdl_module::id) {
            LOG::mod_log->error( M_SCENE, LOG::Mod_log::C_DATABASE,
                "DB name for module \"%s\" already in use.", db_module_name.c_str());
            return -3;
        }
        return 1;
    }

    // Detect the MDL version we need.
    int max_major = 1, max_minor = 0;
    for( mi::Size i = 0; i < variant_count; ++i) {

        const Variant_data& pd = variant_data[i];
        SERIAL::Class_id class_id = transaction->get_class_id( pd.m_prototype_tag);
        DB::Tag module_tag;
        if( class_id == ID_MDL_MATERIAL_DEFINITION) {
            DB::Access<Mdl_material_definition> prototype( pd.m_prototype_tag, transaction);
            module_tag = prototype->get_module();
        } else if( class_id == ID_MDL_FUNCTION_DEFINITION) {
            DB::Access<Mdl_function_definition> prototype( pd.m_prototype_tag, transaction);
            module_tag = prototype->get_module();
        } else
            return -5;
        DB::Access<Mdl_module> module( module_tag, transaction);
        mi::base::Handle<const mi::mdl::IModule> mdl_module( module->get_mdl_module());

        int major, minor;
        mdl_module->get_version( major, minor);
        if( major > max_major) {
            max_major = major;
            max_minor = minor;
        } else if( major == max_major && minor > max_minor)
            max_minor = minor;
    }

    mi::mdl::IMDL::MDL_version version = mi::mdl::IMDL::MDL_VERSION_1_0;
    switch( max_major) {
        case 1:
            switch( max_minor) {
                case 0:  version = mi::mdl::IMDL::MDL_VERSION_1_0; break;
                case 1:  version = mi::mdl::IMDL::MDL_VERSION_1_1; break;
                case 2:  version = mi::mdl::IMDL::MDL_VERSION_1_2; break;
                case 3:  version = mi::mdl::IMDL::MDL_VERSION_1_3; break;
                case 4:  version = mi::mdl::IMDL::MDL_VERSION_1_4; break;
                default: version = mi::mdl::IMDL::MDL_LATEST_VERSION; break;
            }
            break;
        default:
            version = mi::mdl::IMDL::MDL_LATEST_VERSION;
            break;
    }

    // Create module
    mi::base::Handle<mi::mdl::IModule> module(
        mdl->create_module( /*context=*/NULL, module_name, version));

    Symbol_importer symbol_importer( module.get());

    // Add variants to module
    for( mi::Size i = 0; i < variant_count; ++i) {

        const Variant_data& pd = variant_data[i];
        SERIAL::Class_id class_id = transaction->get_class_id( pd.m_prototype_tag);

        if( class_id == ID_MDL_MATERIAL_DEFINITION) {
            DB::Access<Mdl_material_definition> prototype( pd.m_prototype_tag, transaction);
            mi::Sint32 result = add_variant( symbol_importer, transaction, module.get(),
                prototype, pd.m_variant_name.c_str(), pd.m_defaults.get(),
                pd.m_annotations.get(), messages);
            if( result != 0) {
                LOG::mod_log->error( M_SCENE, LOG::Mod_log::C_DATABASE,
                    "Failed to add variant \"%s\" to the module \"%s\".",
                    pd.m_variant_name.c_str(), module_name);
                return result;
            }

        } else if ( class_id == ID_MDL_FUNCTION_DEFINITION) {
            DB::Access<Mdl_function_definition> prototype( pd.m_prototype_tag, transaction);
            mi::Sint32 result = add_variant( symbol_importer, transaction, module.get(),
                prototype, pd.m_variant_name.c_str(), pd.m_defaults.get(),
                pd.m_annotations.get(), messages);
            if( result != 0) {
                LOG::mod_log->error( M_SCENE, LOG::Mod_log::C_DATABASE,
                    "Failed to add variant \"%s\" to the module \"%s\".",
                    pd.m_variant_name.c_str(), module_name);
                return result;
            }

        } else {
            ASSERT( M_SCENE, false);
            return -5;
        }
    }

    // add all collected imports
    symbol_importer.add_imports();

    Module_cache module_cache( transaction);
    module->analyze( &module_cache, /*ctx=*/NULL);
    if( !module->is_valid()) {
        LOG::mod_log->error( M_SCENE, LOG::Mod_log::C_DATABASE,
            "Failed to create valid module \"%s\".", module_name);
        report_messages( module->access_messages(), messages);
        return -8;
    }

    return create_module_internal( transaction, mdl.get(), module.get(), messages);
}


/// Find the expression a path is pointing on.
///
/// \param transaction  the current transaction
/// \param path         the path
/// \param args         the material arguments
static mi::base::Handle<const IExpression> find_path(
    DB::Transaction* transaction,
    const std::string& path,
    const mi::base::Handle<const IExpression_list>& args)
{
    size_t pos = path.find('.');
    std::string param( path.substr(0, pos));

    mi::base::Handle<const IExpression> expr( args->get_expression( param.c_str()));
    if( !expr.is_valid_interface())
        return mi::base::Handle<const IExpression>();

    for( ; pos != std::string::npos;) {
        size_t p = path.find('.', pos + 1);

        std::string arg( path.substr(pos + 1, p != std::string::npos ? p - pos - 1 : p));
        pos = p;

        IExpression::Kind kind = expr->get_kind();
        if( kind == IExpression::EK_CALL) {
            const mi::base::Handle<const IExpression_call> call(
                expr->get_interface<IExpression_call>());

            DB::Tag tag = call->get_call();
            SERIAL::Class_id class_id = transaction->get_class_id( tag);

            if( class_id == Mdl_function_call::id) {
                // handle function calls
                DB::Access<Mdl_function_call> fcall( tag, transaction);
                mi::base::Handle<const IExpression_list> args( fcall->get_arguments());

                expr = mi::base::make_handle( args->get_expression( arg.c_str()));
            } else if( class_id == Mdl_material_instance::id) {
                // handle material instances
                DB::Access<Mdl_material_instance> mat_def( tag, transaction);
                mi::base::Handle<const IExpression_list> args( mat_def->get_arguments());

                expr = mi::base::make_handle( args->get_expression( arg.c_str()));
            } else {
                // unsupported
                return mi::base::Handle<const IExpression>();
            }
        } else if (kind == IExpression::EK_DIRECT_CALL) {
            const mi::base::Handle<const IExpression_direct_call> call(
                expr->get_interface<IExpression_direct_call>());
            mi::base::Handle<const IExpression_list> args( call->get_arguments());

            expr = mi::base::make_handle( args->get_expression( arg.c_str()));
        } else {
            return mi::base::Handle<const IExpression>();
        }

        if( !expr.is_valid_interface())
            return mi::base::Handle<const IExpression>();
    }
    return expr;
}

// Helper function to work-around missing functionality in Mdl_function_definition, see
// jira iray-897
static bool is_uniform_function(
    DB::Transaction* transaction,
    const Mdl_function_definition& fdef)
{
    DB::Access<Mdl_module> module( fdef.get_module(), transaction);
    mi::base::Handle<const mi::mdl::IModule> imod( module->get_mdl_module());
    mi::mdl::Module const *mod = static_cast<mi::mdl::Module const *>(imod.get());

    // handle first those without a MDL definition
    mi::mdl::IDefinition::Semantics sema = fdef.get_mdl_semantic();
    switch (sema) {
    case mi::mdl::IDefinition::DS_INTRINSIC_DAG_FIELD_ACCESS:
        // More complicated case: theoretically, the result might be uniform
        // even if the argument is varying. But we return the property of the operator
        // itself here, so it is always uniform
        return true;

    case mi::mdl::IDefinition::DS_INTRINSIC_DAG_ARRAY_CONSTRUCTOR:
    case mi::mdl::IDefinition::DS_INTRINSIC_DAG_INDEX_ACCESS:
    case mi::mdl::IDefinition::DS_INTRINSIC_DAG_ARRAY_LENGTH:
    case mi::mdl::IDefinition::DS_INTRINSIC_DAG_SET_OBJECT_ID:
    case mi::mdl::IDefinition::DS_INTRINSIC_DAG_SET_TRANSFORMS:
        // these are always uniform
        return true;

    default:
        ASSERT( M_SCENE, !mi::mdl::is_DAG_semantics(sema) && "DAG semantic not handled");
        if (mi::mdl::semantic_is_operator(sema)) {
            // operators are (except the field select operator) always uniform
            return true;
        }
        break;
    }

    std::string sig(fdef.get_mdl_name());

    mi::mdl::IDefinition const *def = mod->find_signature(sig.c_str(), /*only_exported=*/false);
    ASSERT( M_SCENE, def != NULL);

    // Note: don't use IS_UNIFORM here, it is not consistently set on the std library, because
    // it was not annotated there and the analysis did not enter it because of missing bodies
    return def != NULL && !def->get_property(mi::mdl::IDefinition::DP_IS_VARYING);
}

namespace {
    struct Entry {
        Entry(
            mi::base::Handle<const IExpression> expr,
            bool                                is_uniform)
            : m_expr(expr)
            , m_is_uniform(is_uniform)
        {
        }

        mi::base::Handle<const IExpression> m_expr;
        bool                                m_is_uniform;
    };
}

bool Mdl_module::can_enforce_uniform(
    DB::Transaction* transaction,
    mi::base::Handle<const IExpression_list> const &args,
    mi::base::Handle<const IType_list> const &param_types,
    std::string const &path,
    mi::base::Handle<const IExpression> const &p_expr,
    bool &must_be_uniform)
{
    must_be_uniform = false;

    size_t pos = path.find('.');
    std::string param(path.substr(0, pos));

    mi::base::Handle<IExpression const> expr(args->get_expression(param.c_str()));

    mi::base::Handle<IType const> p_type(param_types->get_type(param.c_str()));

    mi::Uint32 modifiers = p_type->get_all_type_modifiers();
    bool is_uniform = (modifiers & IType::MK_UNIFORM) != 0;

    // this parameter IS uniform, start analysis
    typedef std::queue<Entry> Wait_queue;

    Wait_queue queue;

    queue.push(Entry(expr, is_uniform));
    while (!queue.empty()) {
        Entry const &e = queue.front();
        expr       = e.m_expr;
        is_uniform = e.m_is_uniform;

        queue.pop();

        if (is_uniform && expr == p_expr) {
            // the parameter expression is marked uniform in the queue, hence the parameter
            // must be created uniform
            must_be_uniform = true;
        }

        switch (expr->get_kind()) {
        case IExpression::EK_CONSTANT:
            // constants are always uniform
            break;
        case IExpression::EK_CALL:
            {
                mi::base::Handle<IExpression_call const> call(
                    expr->get_interface<IExpression_call>());
                mi::base::Handle<IType const> ret_tp(call->get_type());

                DB::Tag tag = call->get_call();
                SERIAL::Class_id class_id = transaction->get_class_id(tag);

                if (class_id == Mdl_material_instance::id) {
                    if (is_uniform) {
                        // materials are never uniform
                        return false;
                    }
                } else if (class_id == Mdl_function_call::id) {
                    DB::Access<Mdl_function_call> fcall(tag, transaction);
                    DB::Access<Mdl_function_definition> def(
                        fcall->get_function_definition(), transaction);

                    bool auto_must_be_uniform = false;
                    if (is_uniform) {
                        if (ret_tp->get_all_type_modifiers() & IType::MK_UNIFORM) {
                            // return type *IS* uniform, fine, no need to enforce auto parameters
                            auto_must_be_uniform = false;
                        } else if (!is_uniform_function(transaction, *def.get_ptr())) {
                            // called function is not uniform, we found an error
                            return false;
                        } else {
                            // function is uniform and the result must be uniform,
                            // enforce all auto parameters
                            auto_must_be_uniform = true;
                        }
                    }

                    bool is_ternary = def->get_mdl_semantic() ==
                        mi::mdl::operator_to_semantic(mi::mdl::IExpression::OK_TERNARY);

                    // push ALL arguments to the queue
                    mi::base::Handle<IExpression_list const> args(fcall->get_arguments());
                    for (mi::Size i = 0, n = args->get_size(); i < n; ++i) {
                        mi::mdl::IType const *p_type =
                            def->get_mdl_parameter_type(transaction, static_cast<mi::Uint32>(i));
                        mi::mdl::IType::Modifiers mods = p_type->get_type_modifiers();
                        bool p_is_uniform = (mods & mi::mdl::IType::MK_UNIFORM) != 0;
                        bool p_is_varying = (mods & mi::mdl::IType::MK_VARYING) != 0;

                        if (is_ternary && i == 0) {
                            // the condition of the ternary operator inside materials must
                            // be uniform
                            p_is_uniform = true;
                            p_is_varying = false;
                        }

                        expr = args->get_expression(i);
                        queue.push(
                            Entry(expr, !p_is_varying && (auto_must_be_uniform || p_is_uniform)));
                    }
                } else {
                    ASSERT( M_SCENE, !"Unsupported entity kind in function call");
                    return false;
                }
            }
            break;
        case IExpression::EK_PARAMETER:
            // should not happen in this context
            ASSERT( M_SCENE, !"parameter found inside argument expression");
            return false;
        case IExpression::EK_DIRECT_CALL:
            {
                mi::base::Handle<IExpression_direct_call const> call(
                    expr->get_interface<IExpression_direct_call>());
                mi::base::Handle<IType const> ret_tp(call->get_type());

                DB::Tag tag = call->get_definition();
                SERIAL::Class_id class_id = transaction->get_class_id(tag);

                if (class_id == Mdl_material_definition::id) {
                    // materials are never uniform
                    return false;
                } else if (class_id == Mdl_function_definition::id) {
                    DB::Access<Mdl_function_definition> def(tag, transaction);

                    bool auto_must_be_uniform = false;
                    if (is_uniform) {
                        if (ret_tp->get_all_type_modifiers() & IType::MK_UNIFORM) {
                            // return type *IS* uniform, fine, no need to enforce auto parameters
                            auto_must_be_uniform = false;
                        } else if (!is_uniform_function(transaction, *def.get_ptr())) {
                            // called function is not uniform
                            return false;
                        } else {
                            // function is uniform and the result must be uniform,
                            // enforce all auto parameters
                            auto_must_be_uniform = true;
                        }
                    }

                    // push ALL arguments to the queue
                    mi::base::Handle<IExpression_list const> args(call->get_arguments());
                    for (mi::Size i = 0, n = args->get_size(); i < n; ++i) {
                        mi::mdl::IType const *p_type =
                            def->get_mdl_parameter_type(transaction, static_cast<mi::Uint32>(i));
                        mi::mdl::IType::Modifiers mods = p_type->get_type_modifiers();
                        bool p_is_uniform = (mods & mi::mdl::IType::MK_UNIFORM) != 0;
                        bool p_is_varying = (mods & mi::mdl::IType::MK_VARYING) != 0;
                        expr = args->get_expression(i);
                        queue.push(
                            Entry(expr, !p_is_varying && (auto_must_be_uniform || p_is_uniform)));
                    }
                } else {
                    ASSERT( M_SCENE, !"Unsupported entity kind in function call");
                    return false;
                }
            }
            break;
        case IExpression::EK_TEMPORARY:
            // should not happen in this context
            ASSERT( M_SCENE, !"temporary found inside argument expression");
            return false;
        case IExpression::EK_FORCE_32_BIT:
            // not a real kind;
            ASSERT( M_SCENE, false);
            break;
        }
    }

    return true;
}

namespace {

/// Helper class to express on (new) argument.
class New_parameter {
public:
    /// Constructor.
    New_parameter(
        const mi::mdl::ISymbol* sym,
        const mi::base::Handle<const IExpression>& init,
        mi::base::Handle<const IAnnotation_block> annos,
        bool is_uniform)
        : m_sym(sym)
        , m_init(init)
        , m_annos(annos)
        , m_is_uniform(is_uniform)
    {
    }

    /// Get the symbol.
    const mi::mdl::ISymbol* get_sym() const { return m_sym; }

    /// Get the init expression.
    const mi::base::Handle<const IExpression>& get_init() const { return m_init; }

    /// Get the annotations.
    const mi::base::Handle<const IAnnotation_block>& get_annos() const { return m_annos; }

    /// Get the uniform flag.
    bool is_uniform() const { return m_is_uniform; }

private:
    /// The symbol of net new parameter.
    const mi::mdl::ISymbol* m_sym;

    /// The (init) expression of the new parameter.
    mi::base::Handle<const IExpression> m_init;

    /// Annotations for this new parameter.
    mi::base::Handle<const IAnnotation_block> m_annos;

    /// True if the new parameter must be uniform.
    bool m_is_uniform;
};

}  // anonymous



mi::Sint32 Mdl_module::add_material(
    Symbol_importer& symbol_importer,
    DB::Transaction* transaction,
    mi::mdl::IModule* module,
    const ICall& callee,
    const Material_data& md,
    std::vector<Message>* messages)
{
    typedef std::vector<Parameter_data>::const_iterator Iter;

    mi::base::Handle<const IExpression_list> args(callee.get_arguments());

    if( !args.is_valid_interface() && !md.m_parameters.empty()) {
        // the prototype material has no parameters at all
        return -6;
    }

    // traverse the parameter paths AND collect the types
    std::vector< New_parameter > new_params;
    new_params.reserve( md.m_parameters.size());

    mi::base::Handle<const IType_list> param_types(callee.get_parameter_types());

    mi::mdl::IName_factory &nf = *module->get_name_factory();

    for( Iter it( md.m_parameters.begin()), end(md.m_parameters.end()); it != end; ++it) {
        const Parameter_data& pd = *it;

        mi::base::Handle<const IExpression> expr( find_path( transaction, pd.m_path, args));
        if( !expr.is_valid_interface()) {
            // path does not exists
            return -13;
        }

        bool must_be_uniform = false;
        if (!can_enforce_uniform(
                transaction, args, param_types, pd.m_path, expr, must_be_uniform)) {
            // argument cannot be enforces uniform
            return -15;
        }

        new_params.push_back(
            New_parameter(
                nf.create_symbol(pd.m_name.c_str()),
                expr,
                pd.m_annotations,
                must_be_uniform || pd.m_enforce_uniform));
    }

    // convert annotations to MDL AST
    mi::mdl::IAnnotation_block* mdl_annotation_block = 0;
    mi::Sint32 result = create_annotations(
        transaction, module, md.m_annotations.get(), &symbol_importer, mdl_annotation_block);
    if( result != 0)
        return result;

    mi::mdl::IDeclaration_factory &df = *module->get_declaration_factory();

    // create return type
    const mi::mdl::ISymbol* ret_tp_sym = nf.create_symbol( "material");
    const mi::mdl::ISimple_name* ret_tp_sname = nf.create_simple_name( ret_tp_sym);
    mi::mdl::IQualified_name* ret_tp_qname = nf.create_qualified_name();
    ret_tp_qname->add_component( ret_tp_sname);

    mi::mdl::IType_name *ret_tp_tn = nf.create_type_name( ret_tp_qname);

    // create name
    const mi::mdl::ISymbol* mat_sym = nf.create_symbol( md.m_material_name.c_str());
    const mi::mdl::ISimple_name* mat_sname = nf.create_simple_name( mat_sym);

    // setup the builder
    Mdl_ast_builder ast_builder( module, transaction, args);

    typedef std::vector<New_parameter>::const_iterator NIter;
    for( NIter it( new_params.begin()), end( new_params.end()); it != end; ++it) {
        const New_parameter& pd = *it;

        ast_builder.declare_parameter( pd.get_sym(), pd.get_init());
    }

    // create the body
    mi::mdl::IStatement_factory &sf  = *module->get_statement_factory();
    mi::mdl::IExpression_factory &ef = *module->get_expression_factory();

    mi::mdl::IQualified_name *qname    = ast_builder.create_qualified_name(
        callee.get_abs_name());
    mi::mdl::IType_name      *mat_name = nf.create_type_name(qname);

    mi::mdl::IExpression_reference *ref = ef.create_reference(mat_name);
    mi::mdl::IExpression_call      *call = ef.create_call(ref);

    const mi::mdl::IExpression *res = NULL;
    mi::Size n_params = args->get_size();
    if( n_params > 0) {
        mi::mdl::IExpression_let *let = ef.create_let(call);

        // Note: the temporaries are create with "auto" type
        for( mi::Size i = 0; i < n_params; ++i) {
            mi::base::Handle<const IExpression> arg( args->get_expression( i));
            mi::base::Handle<const IType> arg_tp( arg->get_type());

            mi::mdl::IType_name *tn = ast_builder.create_type_name( arg_tp);
            mi::mdl::IDeclaration_variable *vdecl = df.create_variable( tn, /*exported=*/false);

            const mi::mdl::IExpression* init = ast_builder.transform_expr( arg);
            const mi::mdl::ISymbol* tmp_sym = ast_builder.get_temporary_symbol();

            vdecl->add_variable( ast_builder.to_simple_name( tmp_sym), init);

            let->add_declaration( vdecl);

            const char* pname = args->get_name( i);
            const mi::mdl::ISymbol* psym = nf.create_symbol( pname);
            const mi::mdl::ISimple_name *psname = ast_builder.to_simple_name( psym);
            const mi::mdl::IExpression_reference *ref = ast_builder.to_reference( tmp_sym);

            call->add_argument( ef.create_named_argument( psname, ref));
        }
        res = let;
    } else {
        res = call;
    }

    // collect all necessary imports
    symbol_importer.collect_imports(res);

    mi::mdl::IStatement *stmt = sf.create_expression(res);

    mi::mdl::IDeclaration_function *fdecl = df.create_function(
        ret_tp_tn, /*ret_annos=*/NULL, mat_sname, /*is_clone*/false,
        stmt, mdl_annotation_block, /*is_exported=*/true);

    // add parameter
    ast_builder.remove_parameters();
    for( NIter it( new_params.begin()), end( new_params.end()); it != end; ++it) {
        const New_parameter& pd = *it;

        mi::base::Handle<const IType> ptype( pd.get_init()->get_type());
        mi::mdl::IType_name* tn = ast_builder.create_type_name( ptype);

        if( pd.is_uniform()) {
            tn->set_qualifier( mi::mdl::FQ_UNIFORM);
        }

        // work-around until the expression are correctly typed: resource parameters
        // must be uniform
        IType::Kind kind = ptype->get_kind();
        if (kind == IType::TK_TEXTURE || kind == IType::TK_BSDF_MEASUREMENT ||
            kind == IType::TK_LIGHT_PROFILE)
        {
            tn->set_qualifier(mi::mdl::FQ_UNIFORM);
        }

        const mi::mdl::ISimple_name* sname = nf.create_simple_name( pd.get_sym());
        const mi::mdl::IExpression* init = ast_builder.transform_expr( pd.get_init());

        mi::mdl::IAnnotation_block* p_annos = 0;
        mi::Sint32 result = create_annotations(
            transaction, module, pd.get_annos().get(), &symbol_importer, p_annos);
        if( result != 0)
            return result;

        const mi::mdl::IParameter* param = df.create_parameter( tn, sname, init, p_annos);

        fdecl->add_parameter( param);
        if( init)
            symbol_importer.collect_imports( init);
    }

    // and add the used types from the builder, because these might not directly be visible
    symbol_importer.add_names(ast_builder.get_used_user_types());

    // and finally add it to the module
    module->add_declaration( fdecl);
    return 0;
}


// create return type for variant
template <class T>
static const mi::mdl::IType_name* create_return_type_name(
    DB::Transaction* transaction,
    mi::mdl::IModule* module,
    DB::Access<T> prototype);

template <>
const mi::mdl::IType_name* create_return_type_name(
    DB::Transaction* transaction,
    mi::mdl::IModule* module,
    DB::Access<Mdl_material_definition> prototype)
{
    mi::mdl::IName_factory &nf = *module->get_name_factory();

    const mi::mdl::ISymbol* return_type_symbol = nf.create_symbol( "material");
    const mi::mdl::ISimple_name* return_type_simple_name
        = nf.create_simple_name( return_type_symbol);
    mi::mdl::IQualified_name* return_type_qualified_name =nf.create_qualified_name();
    return_type_qualified_name->add_component( return_type_simple_name);
    return nf.create_type_name( return_type_qualified_name);
}

template <>
const mi::mdl::IType_name* create_return_type_name(
    DB::Transaction* transaction,
    mi::mdl::IModule* module,
    DB::Access<Mdl_function_definition> prototype)
{
    const mi::mdl::IType* ret_type = prototype->get_mdl_return_type( transaction);
    return mi::mdl::create_type_name( ret_type, module);
}

template <class T>
mi::Sint32 Mdl_module::add_variant(
    Symbol_importer& symbol_importer,
    DB::Transaction* transaction,
    mi::mdl::IModule* module,
    DB::Access<T> prototype,
    const char* variant_name,
    const IExpression_list* defaults,
    const IAnnotation_block* annotation_block,
    std::vector<Message>* messages)
{
    mi::base::Handle<IType_factory> tf( get_type_factory());
    mi::base::Handle<IValue_factory> vf( get_value_factory());
    mi::base::Handle<IExpression_factory> ef( get_expression_factory());

    // dereference prototype references
    DB::Tag dereferenced_prototype_tag = prototype->get_prototype();
    if( dereferenced_prototype_tag) {
        SERIAL::Class_id class_id = transaction->get_class_id( dereferenced_prototype_tag);
        ASSERT( M_SCENE, class_id == T::id);
        boost::ignore_unused( class_id);
        prototype.set( dereferenced_prototype_tag, transaction);
    }

    // check that the provided arguments are parameters of the material definition
    // and that their types match the expected types
    mi::base::Handle<const IType_list> expected_types( prototype->get_parameter_types());
    for( mi::Size i = 0; defaults && i < defaults->get_size(); ++i) {
        const char* name = defaults->get_name( i);
        mi::base::Handle<const IType> expected_type( expected_types->get_type( name));
        if( !expected_type)
            return -6;
        mi::base::Handle<const IExpression> default_( defaults->get_expression( i));
        mi::base::Handle<const IType> actual_type( default_->get_type());
        if( !argument_type_matches_parameter_type( tf.get(), actual_type.get(),expected_type.get()))
            return -7;
    }

    // create call expression for variant
    const char* prototype_name = prototype->get_mdl_name();
    const mi::mdl::IExpression_reference* prototype_ref
        = signature_to_reference( module, prototype_name);
    mi::mdl::IExpression_factory* expr_factory = module->get_expression_factory();
    mi::mdl::IExpression_call* variant_call = expr_factory->create_call( prototype_ref);

    mi::mdl::IName_factory &nf = *module->get_name_factory();

    // create defaults for variant
    mi::base::Handle<const IExpression_list> prototype_defaults( prototype->get_defaults());
    mi::Size n = prototype->get_parameter_count();
    for( mi::Size i = 0; defaults && i < n; ++i) {
        const char* arg_name = prototype->get_parameter_name( i);
        mi::base::Handle<const IExpression> default_( defaults->get_expression( arg_name));
        if( !default_)
            continue;
        const mi::mdl::ISymbol* arg_symbol = nf.create_symbol( arg_name);
        const mi::mdl::ISimple_name* arg_simple_name = nf.create_simple_name( arg_symbol);
        const mi::mdl::IType* arg_type
            = prototype->get_mdl_parameter_type( transaction, static_cast<mi::Uint32>( i));
        const mi::mdl::IExpression* arg_expr = int_expr_to_mdl_ast_expr(
           transaction, module, arg_type, default_.get());
        if( !arg_expr)
            return -8;
        const mi::mdl::IArgument* argument
            = expr_factory->create_named_argument( arg_simple_name, arg_expr);
        variant_call->add_argument( argument);
        symbol_importer.collect_imports( arg_expr);
    }

    // create annotations for variant
    mi::mdl::IAnnotation_block* mdl_annotation_block = 0;
    mi::Sint32 result = create_annotations(
        transaction, module, annotation_block, &symbol_importer, mdl_annotation_block);
    if( result != 0)
        return result;

    // add imports required by defaults
    symbol_importer.collect_imports( variant_call);

    // create return type for variant
    const mi::mdl::IType_name* return_type_type_name
        = create_return_type_name( transaction, module, prototype);

    // create body for variant
    mi::mdl::IStatement_factory* stat_factory = module->get_statement_factory();
    const mi::mdl::IStatement_expression* variant_body
        = stat_factory->create_expression( variant_call);

    const mi::mdl::ISymbol* variant_symbol = nf.create_symbol( variant_name);
    const mi::mdl::ISimple_name* variant_simple_name = nf.create_simple_name( variant_symbol);
    mi::mdl::IDeclaration_factory* decl_factory = module->get_declaration_factory();
    mi::mdl::IDeclaration_function* variant_declaration = decl_factory->create_function(
        return_type_type_name, /*ret_annotations*/ 0, variant_simple_name, /*is_clone*/ true,
        variant_body, mdl_annotation_block, /*is_exported*/ true);

    // add declaration to module
    module->add_declaration( variant_declaration);
    return 0;
}

mi::Sint32 Mdl_module::create_annotations(
    DB::Transaction* transaction,
    mi::mdl::IModule* module,
    const IAnnotation_block* annotation_block,
    Symbol_importer* symbol_importer,
    mi::mdl::IAnnotation_block* &mdl_annotation_block)
{
    if( !annotation_block) {
        mdl_annotation_block = 0;
        return 0;
    }

    mi::mdl::IAnnotation_factory* annotation_factory = module->get_annotation_factory();
    mdl_annotation_block = annotation_factory->create_annotation_block();

    for( mi::Size i = 0; i < annotation_block->get_size(); ++i) {
        mi::base::Handle<const IAnnotation> anno( annotation_block->get_annotation( i));
        const char* anno_name = anno->get_name();
        mi::base::Handle<const IExpression_list> anno_args( anno->get_arguments());
        mi::Sint32 result = add_annotation(
            transaction, module, mdl_annotation_block, anno_name, anno_args.get());
        if( result != 0)
            return result;
    }

    symbol_importer->collect_imports( mdl_annotation_block);
    return 0;
}

mi::Sint32 Mdl_module::add_annotation(
    DB::Transaction* transaction,
    mi::mdl::IModule* module,
    mi::mdl::IAnnotation_block* mdl_annotation_block,
    const char* annotation_name,
    const IExpression_list* annotation_args)
{
    if( strncmp( annotation_name, "::", 2) != 0)
        return -10;
    std::string annotation_name_str = add_mdl_db_prefix( annotation_name);

    // compute DB name of module containing the annotation
    std::string anno_db_module_name = annotation_name_str;
    size_t left_paren = anno_db_module_name.find( '(');
    if( left_paren == std::string::npos)
        return -10;
    anno_db_module_name = anno_db_module_name.substr( 0, left_paren);
    size_t last_double_colon = anno_db_module_name.rfind( "::");
    if( last_double_colon == std::string::npos)
        return -10;
    anno_db_module_name = anno_db_module_name.substr( 0, last_double_colon);

    // get definition of the annotation
    DB::Tag anno_db_module_tag = transaction->name_to_tag( anno_db_module_name.c_str());
    if( !anno_db_module_tag)
        return -10;
    DB::Access<Mdl_module> anno_db_module( anno_db_module_tag, transaction);
    mi::base::Handle<const mi::mdl::IModule> anno_mdl_module( anno_db_module->get_mdl_module());
    std::string annotation_name_wo_signature = annotation_name_str.substr( 3, left_paren-3);
    std::string signature = annotation_name_str.substr(
        left_paren+1, annotation_name_str.size()-left_paren-2);
    const mi::mdl::IDefinition* definition = anno_mdl_module->find_annotation(
        annotation_name_wo_signature.c_str(), signature.c_str());
    if( !definition)
        return -10;

    mi::mdl::IName_factory &nf = *module->get_name_factory();

    // compute IQualified_name for annotation name
    mi::mdl::IQualified_name* anno_qualified_name = nf.create_qualified_name();
    anno_qualified_name->set_absolute();
    size_t start = 5; // skip leading "mdl::"
    while( true) {
        size_t end = annotation_name_str.find( "::", start);
        if( end == std::string::npos || end >= left_paren)
            end = left_paren;
        const mi::mdl::ISymbol* anno_symbol
            = nf.create_symbol( annotation_name_str.substr( start, end-start).c_str());
        const mi::mdl::ISimple_name* anno_simple_name = nf.create_simple_name( anno_symbol);
        anno_qualified_name->add_component( anno_simple_name);
        if( end == left_paren)
            break;
        start = end + 2;
    }

    // create annotation
    mi::mdl::IAnnotation_factory* anno_factory = module->get_annotation_factory();
    mi::mdl::IAnnotation* anno = anno_factory->create_annotation( anno_qualified_name);

    // store parameter types from annotation definition in a map by parameter name
    const mi::mdl::IType* type = definition->get_type();
    ASSERT( M_SCENE, type->get_kind() == mi::mdl::IType::TK_FUNCTION);
    const mi::mdl::IType_function* type_function = mi::mdl::as<mi::mdl::IType_function>( type);
    std::map<std::string, const mi::mdl::IType*> parameter_types;
    int parameter_count = type_function->get_parameter_count();
    for( int i = 0; i < parameter_count; ++i) {
        const mi::mdl::IType* parameter_type;
        const mi::mdl::ISymbol* parameter_name;
        type_function->get_parameter( i, parameter_type, parameter_name);
        parameter_types[parameter_name->get_name()] = parameter_type;
    }

    // convert arguments
    mi::mdl::IType_factory* type_factory = module->get_type_factory();
    mi::mdl::IValue_factory* value_factory = module->get_value_factory();
    mi::mdl::IExpression_factory* expression_factory = module->get_expression_factory();
    mi::Size argument_count = annotation_args->get_size();
    for( mi::Size i = 0; i < argument_count; ++i) {

        const char* arg_name = annotation_args->get_name( i);

        mi::base::Handle<const IExpression_constant> arg_expr(
            annotation_args->get_expression<IExpression_constant>( i));
        if( !arg_expr)
            return -9;
        mi::base::Handle<const IValue> arg_value( arg_expr->get_value());
        mi::base::Handle<const IType> arg_type( arg_value->get_type());

        // The legacy API always provides "argument" as argument name. Since it supports only single
        // string arguments we map that argument name to the correct one if all these conditions are
        // met -- even for the non-legacy API.
        if( i == 0
            && parameter_count == 1
            && argument_count == 1
            && strcmp( arg_name, "argument") == 0
            && arg_type->get_kind() == IType::TK_STRING) {
            arg_name = parameter_types.begin()->first.c_str();
        }

        const mi::mdl::IType* mdl_parameter_type = parameter_types[arg_name];
        if( !mdl_parameter_type)
            return -9;
        mdl_parameter_type = type_factory->import( mdl_parameter_type);
        const mi::mdl::IValue* mdl_arg_value = int_value_to_mdl_value(
            transaction, value_factory, mdl_parameter_type, arg_value.get());
        if( !mdl_arg_value)
            return -9;

        const mi::mdl::IExpression* mdl_arg_expr
            = expression_factory->create_literal( mdl_arg_value);
        const mi::mdl::ISymbol* arg_symbol = nf.create_symbol( arg_name);
        const mi::mdl::ISimple_name* arg_simple_name = nf.create_simple_name( arg_symbol);
        const mi::mdl::IArgument* mdl_arg
            = expression_factory->create_named_argument( arg_simple_name, mdl_arg_expr);
        anno->add_argument( mdl_arg);
    }

    mdl_annotation_block->add_annotation( anno);
    return 0;
}

mi::Sint32 Mdl_module::create_module_internal(
    DB::Transaction* transaction,
    mi::mdl::IMDL* mdl,
    const mi::mdl::IModule* module,
    std::vector<Message>* messages,
    DB::Tag* module_tag)
{
    ASSERT( M_SCENE, mdl);
    ASSERT( M_SCENE, module);
    const char* module_name     = module->get_name();
    ASSERT( M_SCENE, module_name);
    const char* module_filename = module->get_filename();
    if( module_filename[0] == '\0')
        module_filename = 0;
    ASSERT( M_SCENE, !mdl->is_builtin_module( module_name) || !module_filename);

    report_messages( module->access_messages(), messages);
    if( !module->is_valid())
        return -2;

    // Check whether the module exists already in the DB.
    std::string db_module_name = add_mdl_db_prefix( module->get_name());
    DB::Tag db_module_tag = transaction->name_to_tag( db_module_name.c_str());
    if( db_module_tag) {
        if( transaction->get_class_id( db_module_tag) != Mdl_module::id) {
            LOG::mod_log->error( M_SCENE, LOG::Mod_log::C_DATABASE,
                "DB name for module \"%s\" already in use.", db_module_name.c_str());
            return -3;
        }
        if( module_tag)
            *module_tag = db_module_tag;
        return 1;
    }

    // Compile the module.
    mi::base::Handle<mi::mdl::ICode_generator_dag> generator_dag
        = mi::base::make_handle( mdl->load_code_generator( "dag"))
            .get_interface<mi::mdl::ICode_generator_dag>();
    // We support local entity usage inside MDL materials in neuray, but ...
    generator_dag->access_options().set_option( MDL_CG_DAG_OPTION_NO_LOCAL_FUNC_CALLS, "false");
    /// ... we need entries for those in the DB, hence generate them
    generator_dag->access_options().set_option( MDL_CG_DAG_OPTION_INCLUDE_LOCAL_ENTITIES, "true");
    /// enable simple_glossy_bsdf() mapping in the IRAY SDK, disable it in the MDL SDK

    Module_cache module_cache( transaction);
    if( !module->restore_import_entries( &module_cache)) {
        LOG::mod_log->error( M_SCENE, LOG::Mod_log::C_DATABASE,
            "Failed to restore imports of module \"%s\".", module->get_name());
        return -4;
    }
    Drop_import_scope scope( module);

    mi::base::Handle<mi::mdl::IGenerated_code> code( generator_dag->compile( module));
    if( !code.is_valid_interface())
        return -2;

    const mi::mdl::Messages& code_messages = code->access_messages();
    report_messages( code_messages, messages);

    // Treat error messages as compilation failures, e.g., "Call to unexported function '...' is
    // not allowed in this context".
    if( code_messages.get_error_message_count() > 0)
        return -2;

    ASSERT( M_SCENE, code->get_kind() == mi::mdl::IGenerated_code::CK_DAG);
    mi::base::Handle<mi::mdl::IGenerated_code_dag> code_dag(
        code->get_interface<mi::mdl::IGenerated_code_dag>());

    update_resource_literals( transaction, code_dag.get(), module_filename, module_name);

    // Collect tags of imported modules, create DB elements on the fly if necessary.
    mi::Uint32 import_count = module->get_import_count();
    std::vector<DB::Tag> imports;
    imports.reserve( import_count);

    for( mi::Uint32 i = 0; i < import_count; ++i) {
        mi::base::Handle<const mi::mdl::IModule> import( module->get_import( i));
        std::string db_import_name = add_mdl_db_prefix( import->get_name());
        DB::Tag import_tag = transaction->name_to_tag( db_import_name.c_str());
        if( import_tag) {
            // Sanity-check for the type of the tag.
            if( transaction->get_class_id( import_tag) != Mdl_module::id)
                return -3;
        } else {
            // The imported module does not yet exist in the DB.
            mi::Sint32 result = create_module_internal(
                transaction, mdl, import.get(), messages, &import_tag);
            if( result < 0) {
                LOG::mod_log->error( M_SCENE, LOG::Mod_log::C_DATABASE,
                    "Failed to initialize imported module \"%s\".", import->get_name());
                return -4;
            }
        }
        imports.push_back( import_tag);
    }

    // Compute DB names of the function definitions in this module.
    mi::Uint32 function_count = code_dag->get_function_count();
    std::vector<std::string> function_names;
    function_names.reserve( function_count);

    for( mi::Uint32 i = 0; i < function_count; ++i) {
        std::string db_function_name = add_mdl_db_prefix( code_dag->get_function_name( i));
        function_names.push_back( db_function_name);
        DB::Tag function_tag = transaction->name_to_tag( db_function_name.c_str());
        if( function_tag) {
            LOG::mod_log->error( M_SCENE, LOG::Mod_log::C_DATABASE,
                "DB name for function definition \"%s\" already in use.", db_function_name.c_str());
            return -3;
        }
    }

    // Compute DB names of the material definitions in this module.
    mi::Uint32 material_count = code_dag->get_material_count();
    std::vector<std::string> material_names;
    material_names.reserve( material_count);

    for( mi::Uint32 i = 0; i < material_count; ++i) {
        std::string db_material_name = add_mdl_db_prefix( code_dag->get_material_name( i));
        material_names.push_back( db_material_name);
        DB::Tag material_tag = transaction->name_to_tag( db_material_name.c_str());
        if( material_tag) {
            LOG::mod_log->error( M_SCENE, LOG::Mod_log::C_DATABASE,
               "DB name for material definition \"%s\" already in use.", db_material_name.c_str());
            return -3;
        }
    }

    if( !mdl->is_builtin_module( module_name)) {
        if( !module_filename)
            LOG::mod_log->info( M_SCENE, LOG::Mod_log::C_IO,
                "Loading module \"%s\".", module_name);
        else if( DETAIL::is_archive_member( module_filename)) {
            const std::string& archive_filename = DETAIL::get_archive_filename( module_filename);
            LOG::mod_log->info( M_SCENE, LOG::Mod_log::C_IO,
                "Loading module \"%s\" from \"%s\".", module_name, archive_filename.c_str());
        } else
            LOG::mod_log->info( M_SCENE, LOG::Mod_log::C_IO,
                "Loading module \"%s\" from \"%s\".", module_name, module_filename);
    }

    // Store the module in the DB.
    Mdl_module* db_module = new Mdl_module( transaction,
        mdl, module, code_dag.get(), imports, function_names, material_names);
    DB::Privacy_level privacy_level = transaction->get_scope()->get_level();
    db_module_tag = transaction->store( db_module, db_module_name.c_str(), privacy_level);
    // Do not use the pointer to the DB element anymore after store().
    db_module = 0;

    // Create DB elements for the function definitions in this module.
    for( mi::Uint32 i = 0; i < function_count; ++i) {
        DB::Tag function_tag = transaction->reserve_tag();
        Mdl_function_definition* db_function = new Mdl_function_definition( transaction,
            db_module_tag, function_tag, code_dag.get(), i, module_filename, module_name);
        if( db_function->is_exported())
            transaction->store(
                function_tag, db_function, function_names[i].c_str(), privacy_level);
        else
            transaction->store_for_reference_counting(
                function_tag, db_function, function_names[i].c_str(), privacy_level);
    }

    // Create DB elements for the material definitions in this module.
    for( mi::Uint32 i = 0; i < material_count; ++i) {
        DB::Tag material_tag = transaction->reserve_tag();
        Mdl_material_definition* db_material = new Mdl_material_definition( transaction,
            db_module_tag, material_tag, code_dag.get(), i, module_filename, module_name);
        if( db_material->is_exported())
            transaction->store(
                material_tag, db_material, material_names[i].c_str(), privacy_level);
        else
            transaction->store_for_reference_counting(
                material_tag, db_material, material_names[i].c_str(), privacy_level);
    }

    if( module_tag)
        *module_tag = db_module_tag;
    return 0;
}

IValue_texture* Mdl_module::create_texture(
    DB::Transaction* transaction,
    const char* file_path,
    IType_texture::Shape shape,
    mi::Float32 gamma,
    bool shared,
    mi::Sint32* errors)
{
    mi::Sint32 dummy_errors = 0;
    if( !errors)
        errors = &dummy_errors;

    if( !transaction || !file_path) {
        *errors = -1;
        return 0;
    }

    if( file_path[0] != '/') {
        *errors = -2;
        return 0;
    }

    DB::Tag tag = DETAIL::mdl_texture_to_tag(
        transaction, file_path, /*module_filename*/ 0, /*module_name*/ 0, shared, gamma);
    if( !tag) {
        *errors = -3;
        return 0;
    }

    *errors = 0;
    mi::base::Handle<IType_factory> tf( get_type_factory());
    mi::base::Handle<IValue_factory> vf( get_value_factory());
    mi::base::Handle<const IType_texture> t( tf->create_texture( shape));
    return vf->create_texture( t.get(), tag);
}

IValue_light_profile* Mdl_module::create_light_profile(
    DB::Transaction* transaction, const char* file_path, bool shared, mi::Sint32* errors)
{
    mi::Sint32 dummy_errors = 0;
    if( !errors)
        errors = &dummy_errors;

    if( !transaction || !file_path) {
        *errors = -1;
        return 0;
    }

    if( file_path[0] != '/') {
        *errors = -2;
        return 0;
    }

    DB::Tag tag = DETAIL::mdl_light_profile_to_tag(
        transaction, file_path, /*module_filename*/ 0, /*module_name*/ 0, shared);
    if( !tag) {
        *errors = -3;
        return 0;
    }

    *errors = 0;
    mi::base::Handle<IValue_factory> vf( get_value_factory());
    return vf->create_light_profile( tag);
}

IValue_bsdf_measurement* Mdl_module::create_bsdf_measurement(
    DB::Transaction* transaction, const char* file_path, bool shared, mi::Sint32* errors)
{
    mi::Sint32 dummy_errors = 0;
    if( !errors)
        errors = &dummy_errors;

    if( !transaction || !file_path) {
        *errors = -1;
        return 0;
    }

    if( file_path[0] != '/') {
        *errors = -2;
        return 0;
    }

    DB::Tag tag = DETAIL::mdl_bsdf_measurement_to_tag(
        transaction, file_path, /*module_filename*/ 0, /*module_name*/ 0, shared);
    if( !tag) {
        *errors = -3;
        return 0;
    }

    *errors = 0;
    mi::base::Handle<IValue_factory> vf( get_value_factory());
    return vf->create_bsdf_measurement( tag);
}

Mdl_module::Mdl_module()
{
    m_tf = get_type_factory();
    m_vf = get_value_factory();
    m_ef = get_expression_factory();
}

Mdl_module::Mdl_module( const Mdl_module& other)
  : SCENE::Scene_element<Mdl_module, ID_MDL_MODULE>( other),
    m_mdl( other.m_mdl),
    m_module( other.m_module),
    m_code_dag( other.m_code_dag),
    m_tf( other.m_tf),
    m_vf( other.m_vf),
    m_ef( other.m_ef),
    m_name( other.m_name),
    m_file_name( other.m_file_name),
    m_api_file_name( other.m_api_file_name),
    m_imports( other.m_imports),
    m_types( other.m_types),
    m_constants( other.m_constants),
    m_annotations( other.m_annotations),
    m_functions( other.m_functions),
    m_materials( other.m_materials),
    m_resource_reference_tags(other.m_resource_reference_tags)
{
}

Mdl_module::Mdl_module(
    DB::Transaction* transaction,
    mi::mdl::IMDL* mdl,
    const mi::mdl::IModule* module,
    mi::mdl::IGenerated_code_dag* code_dag,
    const std::vector<DB::Tag>& imports,
    const std::vector<std::string>& functions,
    const std::vector<std::string>& materials)
{
    ASSERT( M_SCENE, mdl);
    ASSERT( M_SCENE, module);
    ASSERT( M_SCENE, module->get_name());
    ASSERT( M_SCENE, module->get_filename());

    m_tf = get_type_factory();
    m_vf = get_value_factory();
    m_ef = get_expression_factory();

    m_mdl = make_handle_dup( mdl);
    m_module = make_handle_dup( module);
    m_code_dag = make_handle_dup( code_dag);
    m_name = module->get_name();
    m_file_name = module->get_filename();
    m_api_file_name = DETAIL::is_archive_member( m_file_name.c_str())
        ? DETAIL::get_archive_filename( m_file_name.c_str()) : m_file_name;

    m_imports = imports;
    m_functions = functions;
    m_materials = materials;

    // convert types
    m_types = m_tf->create_type_list();
    mi::Uint32 type_count = code_dag->get_type_count();
    for( mi::Uint32 i = 0; i < type_count; ++i) {
        const char* name = code_dag->get_type_name( i);
        const mi::mdl::IType* type = code_dag->get_type( i);

        mi::Uint32 annotation_count = code_dag->get_type_annotation_count( i);
        Mdl_annotation_block annotations( annotation_count);
        for( mi::Uint32 k = 0; k < annotation_count; ++k)
            annotations[k] = code_dag->get_type_annotation( i, k);

        mi::Uint32 member_count = code_dag->get_type_sub_entity_count( i);
        Mdl_annotation_block_vector sub_annotations( member_count);
        for( mi::Uint32 j = 0; j < member_count; ++j) {
            annotation_count = code_dag->get_type_sub_entity_annotation_count( i, j);
            sub_annotations[j].resize( annotation_count);
            for( mi::Uint32 k = 0; k < annotation_count; ++k)
                sub_annotations[j][k] = code_dag->get_type_sub_entity_annotation( i, j, k);
        }

        mi::base::Handle<const IType> type_int(
            mdl_type_to_int_type( m_tf.get(), type, &annotations, &sub_annotations));
        std::string full_name = m_name + "::" + name;
        m_types->add_type( full_name.c_str(), type_int.get());
    }

    // convert constants
    m_constants = m_vf->create_value_list();
    mi::Uint32 constant_count = code_dag->get_constant_count();
    for( mi::Uint32 i = 0; i < constant_count; ++i) {
        const char* name = code_dag->get_constant_name( i);
        const mi::mdl::DAG_constant* constant = code_dag->get_constant_value( i);
        const mi::mdl::IValue* value = constant->get_value();
        mi::base::Handle<IValue> value_int( mdl_value_to_int_value(
            m_vf.get(), transaction, /*type_int*/ 0, value, m_file_name.c_str(), m_name.c_str()));
        std::string full_name = m_name + "::" + name;
        m_constants->add_value( full_name.c_str(), value_int.get());
    }

    // convert module annotations
    mi::Uint32 annotation_count = code_dag->get_module_annotation_count();
    Mdl_annotation_block annotations( annotation_count);
    for( mi::Uint32 i = 0; i < annotation_count; ++i)
        annotations[i] = code_dag->get_module_annotation( i);
    m_annotations = mdl_dag_node_vector_to_int_annotation_block(
        m_ef.get(), transaction, annotations, m_file_name.c_str(), m_name.c_str());

    if (module->get_referenced_resources_count() > 0)
    {
        std::map <std::string, mi::Size> resource_url_2_index;
        for (mi::Size i = 0; i < module->get_referenced_resources_count(); ++i)
            resource_url_2_index.insert(std::make_pair(module->get_referenced_resource_url(i), i));

        // update resource references
        std::set<const mi::mdl::IValue_resource*> resources;
        collect_resource_references(code_dag, resources);
    
        m_resource_reference_tags.resize(module->get_referenced_resources_count());
        for (const auto r : resources) {
        
            const char *key = r->get_string_value();
            const auto& it = resource_url_2_index.find(key);
            if (it != resource_url_2_index.end())
                m_resource_reference_tags[it->second].push_back(DB::Tag(r->get_tag_value()));
        }
    }
}

const char* Mdl_module::get_filename() const
{
    return m_file_name.empty() ? 0 : m_file_name.c_str();
}

const char* Mdl_module::get_api_filename() const
{
    return m_api_file_name.empty() ? 0 : m_api_file_name.c_str();
}

const char* Mdl_module::get_mdl_name() const
{
    return m_name.c_str();
}

mi::Size Mdl_module::get_import_count() const
{
    return m_imports.size();
}

DB::Tag Mdl_module::get_import( mi::Size index) const
{
    if( index >= m_imports.size())
        return DB::Tag( 0);
    return m_imports[index];
}

const IType_list* Mdl_module::get_types() const
{
    m_types->retain();
    return m_types.get();
}

const IValue_list* Mdl_module::get_constants() const
{
    m_constants->retain();
    return m_constants.get();
}

mi::Size Mdl_module::get_function_count() const
{
    return m_functions.size();
}

DB::Tag Mdl_module::get_function( DB::Transaction* transaction, mi::Size index) const
{
    if( index >= m_functions.size())
        return DB::Tag( 0);
    return transaction->name_to_tag( m_functions[index].c_str());
}

const char* Mdl_module::get_function_name( mi::Size index) const
{
    return index >= m_functions.size() ? 0 : m_functions[index].c_str();
}

mi::Size Mdl_module::get_material_count() const
{
    return m_materials.size();
}

DB::Tag Mdl_module::get_material( DB::Transaction* transaction, mi::Size index) const
{
    if( index >= m_materials.size())
        return DB::Tag( 0);
    return transaction->name_to_tag( m_materials[index].c_str());
}

const IAnnotation_block* Mdl_module::get_annotations() const
{
   if( !m_annotations)
        return 0;
    m_annotations->retain();
    return m_annotations.get();
}

const char* Mdl_module::get_material_name( mi::Size index) const
{
    return index >= m_materials.size() ? 0 : m_materials[index].c_str();
}

bool Mdl_module::is_standard_module() const
{
    return m_module->is_stdlib();
}

const std::vector<std::string> Mdl_module::get_function_overloads(
    DB::Transaction* transaction,
    const char* name,
    const IExpression_list* arguments) const
{
    std::vector<std::string> result;
    if( !name)
        return result;

    // compute prefix length (without signature)
    const char* pos = strchr( name,'(');
    size_t prefix_len = pos ? pos - name : strlen( name);

    // find overloads
    for( mi::Size i = 0; i < m_functions.size(); ++i) {
        const char* f = m_functions[i].c_str();
        if( strncmp( f, name, prefix_len) != 0)
            continue;
        const char next = f[prefix_len];
        if( next != '\0' && next != '(')
            continue;
        // no arguments provided, don't check for exact match
        if( !arguments) {
            result.push_back( m_functions[i]);
            continue;
        }
        // arguments provided, check for exact match
        DB::Tag tag = definition_name_to_tag( transaction, f);
        if( !tag)
            continue;
        if( transaction->get_class_id( tag) != Mdl_function_definition::id)
            continue;
        DB::Access<Mdl_function_definition> definition( tag, transaction);
        mi::Sint32 errors = 0;
        // TODO check whether we can avoid the function call creation
        Mdl_function_call* call = definition->create_function_call(
            transaction, arguments, &errors);
        if( call && errors == 0)
            result.push_back( m_functions[i]);
        delete call;
    }

    return result;
}

const std::vector<std::string> Mdl_module::get_function_overloads_by_signature(
    DB::Transaction* transaction,
    const char* name,
    const char* param_sig) const
{
    std::vector<std::string> result;
    if( !name)
        return result;

    // reject names that do no start with the "mdl" prefix
    if( strncmp( name, "mdl", 3) != 0)
        return result;

    mi::base::Handle<const mi::mdl::IOverload_result_set> set(
        m_module->find_overload_by_signature( name + 3, param_sig));
    if( !set.is_valid_interface())
        return result;

    for( char const* name = set->first_signature(); name != NULL; name = set->next_signature())
        result.push_back( add_mdl_db_prefix( name));

    return result;
}

mi::Size Mdl_module::get_resources_count() const
{
    return m_module->get_referenced_resources_count();
}

const char* Mdl_module::get_resource_mdl_file_path(mi::Size index) const
{
    if (index >= m_module->get_referenced_resources_count())
        return nullptr;

    return m_module->get_referenced_resource_url(index);
}

DB::Tag Mdl_module::get_resource_tag(mi::Size index) const
{
    if (index >= m_resource_reference_tags.size())
        return DB::Tag(0);

    // for now, only give access to the first element
    if (m_resource_reference_tags[index].size() == 0)
        return DB::Tag(0);

    return m_resource_reference_tags[index][0];
}

const IType_resource* Mdl_module::get_resource_type(mi::Size index) const
{
    if (index >= m_module->get_referenced_resources_count())
        return nullptr;

    const mi::mdl::IType* t = m_module->get_referenced_resource_type(index);
    return mdl_type_to_int_type<IType_resource>(m_tf.get(), t);
}

const mi::mdl::IModule* Mdl_module::get_mdl_module() const
{
    m_module->retain();
    return m_module.get();
}

const mi::mdl::IGenerated_code_dag* Mdl_module::get_code_dag() const
{
    if( !m_code_dag.is_valid_interface())
        return 0;
    m_code_dag->retain();
    return m_code_dag.get();
}

bool Mdl_module::is_valid_module_name( const char* name, const mi::mdl::IMDL* mdl)
{
    if( !name)
        return false;
    if( name[0] == ':' && name[1] == ':') {
        // skip "::" scope
        name += 2;
    }

    for( ;;) {
        const char *scope = strstr( name, "::");

        std::string ident;
        if( scope)
            ident = std::string( name, scope - name);
        else
            ident = name;

        // the compiler checks an identifier only
        if( !mdl->is_valid_mdl_identifier( ident.c_str()))
            return false;

        if( scope)
            name = scope + 2;
        else
            break;
    }
    return true;
}

const SERIAL::Serializable* Mdl_module::serialize( SERIAL::Serializer* serializer) const
{
    Scene_element_base::serialize( serializer);

    // m_mdl is never serialized (independent of DB element)
    SYSTEM::Access_module<MDLC::Mdlc_module> mdlc_module( false);
    mdlc_module->serialize_module( serializer, m_module.get());

    bool has_code = m_code_dag.is_valid_interface();
    serializer->write( has_code);
    if( has_code)
        mdlc_module->serialize_code_dag( serializer, m_code_dag.get());

    serializer->write( m_name);
    serializer->write( m_file_name);
    serializer->write( m_api_file_name);
    SERIAL::write( serializer, m_imports);
    m_tf->serialize_list( serializer, m_types.get());
    m_vf->serialize_list( serializer, m_constants.get());
    m_ef->serialize_annotation_block( serializer, m_annotations.get());
    SERIAL::write( serializer, m_functions);
    SERIAL::write( serializer, m_materials);
    SERIAL::write(serializer, m_resource_reference_tags);
    
    return this + 1;
}

SERIAL::Serializable* Mdl_module::deserialize( SERIAL::Deserializer* deserializer)
{
    Scene_element_base::deserialize( deserializer);

    // deserialize m_module
    SYSTEM::Access_module<MDLC::Mdlc_module> mdlc_module( false);
    m_mdl = mdlc_module->get_mdl();
    m_module = mdlc_module->deserialize_module( deserializer);

    bool has_code = false;
    deserializer->read( &has_code);
    if( has_code)
        m_code_dag = mdlc_module->deserialize_code_dag( deserializer);

    deserializer->read( &m_name);
    deserializer->read( &m_file_name);
    deserializer->read( &m_api_file_name);
    SERIAL::read( deserializer, &m_imports);
    m_types = m_tf->deserialize_list( deserializer);
    m_constants = m_vf->deserialize_list( deserializer);
    m_annotations = m_ef->deserialize_annotation_block( deserializer);
    SERIAL::read( deserializer, &m_functions);
    SERIAL::read( deserializer, &m_materials);
    SERIAL::read( deserializer, &m_resource_reference_tags);

    return this + 1;
}

void Mdl_module::dump( DB::Transaction* transaction) const
{
    std::ostringstream s;
    s << std::boolalpha;
    mi::base::Handle<const mi::IString> tmp;

    // m_mdl, m_module, m_code_dag missing

    s << "Module MDL name: " << m_name << std::endl;
    s << "File name: " << m_file_name << std::endl;
    s << "API file name: " << m_api_file_name << std::endl;

    s << "Imports: ";
    mi::Size imports_count = m_imports.size();
    for( mi::Size i = 0; i+1 < imports_count; ++i)
        s << "tag " << m_imports[i].get_uint() << ", ";
    if( imports_count > 0)
        s << "tag " << m_imports[imports_count-1].get_uint();
    s << std::endl;

    tmp = m_tf->dump( m_types.get());
    s << "Types: " << tmp->get_c_str() << std::endl;

    tmp = m_vf->dump( transaction, m_constants.get(), /*name*/ 0);
    s << "Constants: " << tmp->get_c_str() << std::endl;

    // m_annotations, m_resource_references missing

    mi::Size function_count = m_functions.size();
    for( mi::Size i = 0; i < function_count; ++i)
        s << "Function definition " << i << ": " << m_functions[i] << std::endl;

    mi::Size material_count = m_materials.size();
    for( mi::Size i = 0; i < material_count; ++i)
        s << "Material definition " << i << ": " << m_materials[i] << std::endl;

    LOG::mod_log->info( M_SCENE, LOG::Mod_log::C_DATABASE, "%s", s.str().c_str());
}

size_t Mdl_module::get_size() const
{
    return sizeof( *this)
        + SCENE::Scene_element<Mdl_module, Mdl_module::id>::get_size()
            - sizeof( SCENE::Scene_element<Mdl_module, Mdl_module::id>)
        + dynamic_memory_consumption( m_name)
        + dynamic_memory_consumption( m_file_name)
        + dynamic_memory_consumption( m_api_file_name)
        + dynamic_memory_consumption( m_imports)
        + dynamic_memory_consumption( m_types)
        + dynamic_memory_consumption( m_constants)
        + dynamic_memory_consumption( m_annotations)
        + dynamic_memory_consumption( m_functions)
        + dynamic_memory_consumption( m_materials)
        + m_module->get_memory_size()
        + (m_code_dag ? m_code_dag->get_memory_size() : 0);
}

DB::Journal_type Mdl_module::get_journal_flags() const
{
    return DB::JOURNAL_NONE;
}

Uint Mdl_module::bundle( DB::Tag* results, Uint size) const
{
    return 0;
}

void Mdl_module::get_scene_element_references( DB::Tag_set* result) const
{
    result->insert( m_imports.begin(), m_imports.end());
    collect_references( m_annotations.get(), result);
    for (const auto& tags : m_resource_reference_tags)
    {
        for(const auto& tag: tags)
            if (tag.is_valid())
                result->insert(tag);
    }
}

} // namespace MDL

} // namespace MI

