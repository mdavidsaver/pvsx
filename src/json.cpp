/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <sstream>
#include <list>
#include <map>

#include <pvxs/json.h>
#include <pvxs/data.h>

#include <yajl_parse.h>

#include "utilpvt.h"

namespace pvxs {
namespace json {

namespace {
// undef implies API version 0
#ifndef EPICS_YAJL_VERSION
typedef long integer_arg;
typedef unsigned size_arg;
#else
typedef long long integer_arg;
typedef size_t size_arg;
#endif

struct JAny;
// use std::list so that insertion does not invalidate references
typedef std::list<std::pair<std::string, JAny> > JMap;
typedef std::list<JAny> JList;

struct JAny {
    aligned_union<8, bool, double, int64_t, uint64_t, std::string, JMap, JList>::type store;
    enum type_t {
        Placeholder,
        Null,
        Bool,
        Double,
        Int64,
        UInt64,
        String,
        List,
        Map,
    } type = Null;

    template<typename T>
    T& as() { return *reinterpret_cast<T*>(&store); }
    template<typename T>
    const T& as() const { return *reinterpret_cast<const T*>(&store); }

    JAny() : store({}), type(Placeholder) {}
    JAny(const JAny&) = delete;
    JAny& operator=(const JAny&) = delete;

    ~JAny() {
        switch(type) {
        case Placeholder:
        case Null:
        case Bool:
        case Double:
        case Int64:
        case UInt64:
            break; // nothing to do for POD
        case String:
            as<std::string>().~basic_string();
            break;
        case List:
            as<JList>().~JList();
            break;
        case Map:
            as<JMap>().~JMap();
            break;
        }
    }
};

constexpr size_t stkLimit = 10;

struct JContext {
    std::vector<JAny*> stk;
    std::ostringstream msg;

    void exc(std::exception& e) noexcept {
        try {
            msg<<"\nError: "<<e.what(); \
        }catch(...){
            // exception while recording exception...  not much can be done.
        }
    }

    JAny& setup_top() {
        assert(!stk.empty());
        auto& top = *stk.back();
        if(top.type==JAny::Placeholder) {
            return top;
        } else if(top.type==JAny::List) {
            auto& list = top.as<JList>();
            list.emplace_back();
            auto& newtop = list.back();
            stk.push_back(&newtop);
            return newtop;
        } else {
            throw std::logic_error("invalid stack state for array");
        }
    }
    void consume_top() {
        assert(!stk.empty());
        stk.pop_back();
    }
};

struct YHandle {
    yajl_handle handle;
    YHandle(yajl_handle handle)
        :handle(handle)
    {
        if(!handle)
            throw std::runtime_error("yajl_alloc fails");
    }
    ~YHandle() {
        yajl_free(handle);
    }
    operator yajl_handle() { return handle; }
};

#define TRY \
    auto ctx = static_cast<JContext*>(raw); \
    try

#define CATCH() \
    catch(std::exception& e){ \
        ctx->exc(e); \
        return 0; \
    }

int jvalue_null(void * raw) noexcept {
    TRY {
        auto& top = ctx->setup_top();
        top.type = JAny::Null;
        ctx->consume_top();

        return 1;
    }CATCH()
}

int jvalue_boolean(void * raw, int val) noexcept {
    TRY {
        auto& top = ctx->setup_top();
        top.type = JAny::Bool;
        top.as<bool>() = val;
        ctx->consume_top();

        return 1;
    }CATCH()
}

int jvalue_integer(void * raw, integer_arg val) noexcept {
    TRY {
        auto& top = ctx->setup_top();
        top.type = JAny::Int64;
        top.as<int64_t>() = val;
        ctx->consume_top();

        return 1;
    }CATCH()
}

int jvalue_real(void * raw, double val) noexcept {
    TRY {
        auto& top = ctx->setup_top();
        top.type = JAny::Double;
        top.as<double>() = val;
        ctx->consume_top();

        return 1;
    }CATCH()
}

int jvalue_string(void * raw, const unsigned char * val, size_arg len) {
    TRY {
        auto& top = ctx->setup_top();
        new (&top.store) std::string((const char*)val, len);
        top.type = JAny::String;
        ctx->consume_top();

        return 1;
    }CATCH()
}

int jvalue_start_map(void * raw) noexcept {
    TRY {
        if(ctx->stk.size() >= stkLimit)
            throw std::runtime_error("JSON structure too deep!");

        auto& top = ctx->setup_top();
        new (&top.store) JMap();
        top.type = JAny::Map;

        return 1;
    }CATCH()
}

int jvalue_map_key(void * raw, const unsigned char * val, size_arg len) {
    TRY {
        assert(!ctx->stk.empty());
        auto& top = *ctx->stk.back();
        assert(top.type==JAny::Map);

        auto& map = top.as<JMap>();
        map.emplace_back(std::piecewise_construct,
                         std::forward_as_tuple((const char*)val, len),
                         std::forward_as_tuple());
        ctx->stk.push_back(&map.back().second);

        return 1;
    }CATCH()
}

int jvalue_end_map(void * raw) noexcept {
    TRY {
        assert(!ctx->stk.empty());
        auto& top = ctx->stk.back();
        assert(top->type==JAny::Map);

        ctx->consume_top();

        return 1;
    }CATCH()
}

int jvalue_start_array(void * raw) noexcept {
    TRY {
        if(ctx->stk.size() >= stkLimit)
            throw std::runtime_error("JSON structure too deep!");

        auto& top = ctx->stk.back();
        assert(top->type==JAny::Placeholder);
        new (&top->store) JList();
        top->type = JAny::List;

        return 1;
    }CATCH()
}

int jvalue_end_array(void * raw) noexcept {
    TRY {
        assert(!ctx->stk.empty());
        auto& top = ctx->stk.back();
        assert(top->type==JAny::List);

        ctx->consume_top();

        return 1;
    }CATCH()
}

const yajl_callbacks jvalue_cbs = {
    jvalue_null,
    jvalue_boolean,
    jvalue_integer,
    jvalue_real,
    nullptr,
    jvalue_string,
    jvalue_start_map,
    jvalue_map_key,
    jvalue_end_map,
    jvalue_start_array,
    jvalue_end_array,
};

Value infer_from_ast(const JAny& src) {
    throw std::logic_error("Not implemented");
}

void apply_ast(Value& dest, const JAny& src)
{
    if(dest.type()==TypeCode::Any) {
        Value node(infer_from_ast(src));
        apply_ast(node, src);
        dest.from(node);
        return;
    }

    switch(src.type) {
    case JAny::Placeholder:
        throw std::logic_error("placeholder in JSON AST");
    case JAny::Null:
        dest = unselect;
        break;
    case JAny::Bool:
        dest.from(src.as<bool>());
        break;
    case JAny::Double:
        dest.from(src.as<double>());
        break;
    case JAny::Int64:
        dest.from(src.as<int64_t>());
        break;
    case JAny::UInt64:
        dest.from(src.as<uint64_t>());
        break;
    case JAny::String:
        dest.from(src.as<std::string>());
        break;
    case JAny::Map: {
        auto& map = src.as<JMap>();
        for(auto& pair : map) {
            auto node(dest.lookup(pair.first));
            apply_ast(node, pair.second);
        }
    }
        break;
    case JAny::List: {
        auto& list = src.as<JList>();
        auto dtype(dest.type());

        if(dtype==TypeCode::StructA || dtype==TypeCode::UnionA) {
            shared_array<Value> elems(list.size());
            size_t i=0;
            for(auto& elem : list) {
                if(elem.type!=JAny::Null) {
                    auto& eval = elems[i] = dest.allocMember();
                    apply_ast(eval, elem);
                }
                i++;
            }
            dest.from(elems.freeze());

        } else if(dtype==TypeCode::AnyA) {
            shared_array<Value> elems(list.size());
            size_t i=0;
            for(auto& elem : list) {
                if(elem.type!=JAny::Null) {
                    auto& eval = elems[i] = infer_from_ast(elem);
                    apply_ast(eval, elem);
                }
                i++;
            }
            dest.from(elems.freeze());

        } else { // array of scalar type
            auto arr(allocArray(dtype.arrayType(), list.size()));
            auto dtype(arr.original_type());
            auto esize(elementSize(dtype));
            auto cur(arr.data());

            for(auto& elem : list) {
                ArrayType stype;
                switch(elem.type) {
                case JAny::Bool: stype = ArrayType::Bool; break;
                case JAny::Double: stype = ArrayType::Float64; break;
                case JAny::Int64: stype = ArrayType::Int64; break;
                case JAny::UInt64: stype = ArrayType::UInt64; break;
                case JAny::String: stype = ArrayType::String; break;
                default:
                    throw std::runtime_error(SB()<<"Can't assign "<<dtype<<" with compound value");
                }
                detail::convertArr(dtype, cur, stype, &elem.store, 1);

                cur = esize + (char*)cur;
            }

            dest.from(arr.freeze());
        }
    }
        break;
    }
}

void parse_into_ast(const Parse& p, JAny& top)
{
    auto cur = p.base;
    auto len = p.count;

    // ignore leading space
    for(; len && isspace(*cur); len--, cur++) {}

    // PoS YAJL does not parse bare numbers correctly, so handle as a special case...
    if(cur[0]!='"' && cur[0]!='{' && cur[0]!='[') {
        std::string num(cur, len);

        if(num.find_first_of('.')!=num.npos) { // float
            top.type = JAny::Double;
            top.as<double>() = parseTo<double>(num);
        } else { // integer
            top.type = JAny::Int64;
            top.as<int64_t>() = parseTo<int64_t>(num);
        }

        return;
    }

    // parse into AST
    JContext ctx;
    ctx.stk.push_back(&top);

#ifndef EPICS_YAJL_VERSION
    yajl_parser_config conf;
    memset(&conf, 0, sizeof(conf));
    conf.allowComments = 1;
    conf.checkUTF8 = 1;
    YHandle handle(yajl_alloc(&jvalue_cbs, &conf, NULL, &ctx));
#else
    YHandle handle(yajl_alloc(&jvalue_cbs, NULL, &ctx));

    yajl_config(handle, yajl_allow_comments, 1);
#endif

    auto sts(yajl_parse(handle, (const unsigned char*)cur, len));
    auto consumed(yajl_get_bytes_consumed(handle));

    switch(sts) {
    case yajl_status_ok:
        for(; consumed < len; consumed++) {
            if(!isspace(cur[consumed]))
                throw std::runtime_error("Trailing junk after JSON");
        }
        // success
        break;
    case yajl_status_client_canceled:
        throw std::runtime_error(ctx.msg.str());
#ifndef EPICS_YAJL_VERSION
    case yajl_status_insufficient_data:
        throw std::runtime_error("JSON incomplete");
        break;
#endif
    case yajl_status_error: {
        auto raw(yajl_get_error(handle, 1, (const unsigned char*)cur, len));
        try {
            ctx.msg<<"\nJSON Syntax error: "<<raw;
        } catch(...) {
            // error while reporting error.  not much can be done...
        }
        yajl_free_error(handle, raw);
        throw std::runtime_error(ctx.msg.str());
    }
    }

    if(!ctx.stk.empty()) {
#ifdef EPICS_YAJL_VERSION
        throw std::runtime_error("JSON incomplete");
#else
        throw std::logic_error("json parse stack unclean");
#endif
    }
}

} // namespace

void Parse::into(Value& v) const
{
    JAny top;
    parse_into_ast(*this, top);
    apply_ast(v, top);
}

Value Parse::as() const
{
    JAny top;
    parse_into_ast(*this, top);
    auto ret(infer_from_ast(top));
    apply_ast(ret, top);
    return ret;
}

}} // namespace pvxs::json
