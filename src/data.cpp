/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <cstring>
#include <epicsAssert.h>

#include "dataimpl.h"
#include "utilpvt.h"

namespace pvxs {

DEFINE_INST_COUNTER(StructTop);

NoField::NoField()
    :std::runtime_error ("No such field")
{}

NoField::~NoField() {}

NoConvert::~NoConvert() {}

LookupError::LookupError(const std::string& msg)
    :std::runtime_error(msg)
{}

LookupError::~LookupError() {}


std::shared_ptr<const impl::FieldDesc>
Value::Helper::type(const Value& v)
{
    if(v) {
        return std::shared_ptr<const impl::FieldDesc>(v.store->top->desc, v.desc);
    } else {
        return nullptr;
    }
}

Value Value::Helper::build(const void* ptr, StoreType type)
{
    TypeCode base{TypeCode::Null};
    switch (type) {
    case StoreType::Bool:     base = TypeCode::Bool; break;
    case StoreType::Integer:  base = TypeCode::Int64; break;
    case StoreType::UInteger: base = TypeCode::UInt64; break;
    case StoreType::Real:     base = TypeCode::Float64; break;
    case StoreType::String:   base = TypeCode::String; break;
    case StoreType::Array: {
        auto& arr = *static_cast<const shared_array<const void>*>(ptr);
        switch(arr.original_type()) {
#define CASE(TYPE) case ArrayType::TYPE: base = TypeCode::TYPE ## A; break
        CASE(Bool);
        CASE(Int8);
        CASE(Int16);
        CASE(Int32);
        CASE(Int64);
        CASE(UInt8);
        CASE(UInt16);
        CASE(UInt32);
        CASE(UInt64);
        CASE(Float32);
        CASE(Float64);
        CASE(String);
#undef CASE
        case ArrayType::Value:
            base = TypeCode::AnyA;
            break;
        case ArrayType::Null:
            throw std::logic_error("Unable to infer ArrayType::Null");
        }
    }
        break;
    case StoreType::Compound: {
        auto src = *reinterpret_cast<const Value*>(ptr);
        if(src) {
            auto dst = TypeDef(src).create();
            dst.assign(src);
            return dst;
        }
    }
        base = TypeCode::Any;
        break;
    case StoreType::Null:
        throw std::logic_error("Unable to infer ArrayType::Null");
    }

    Value ret(TypeDef(base).create());
    ret.copyIn(ptr, type);

    return ret;
}

Value::Value(const std::shared_ptr<const impl::FieldDesc>& desc)
    :desc(nullptr)
{
    if(!desc)
        return;

    auto top = std::make_shared<StructTop>(desc);

    {
        auto& root = top->members[0];
        root.init(desc->code.storedAs());
        root.top = top.get();
    }

    if(desc->code==TypeCode::Struct) {
        for(auto& pair : desc->mlookup) {
            auto cfld = desc.get() + pair.second;
            auto& mem = top->members.at(pair.second);
            mem.top = top.get();
            mem.init(cfld->code.storedAs());
        }
    }

    this->desc = desc.get();
    decltype (store) val(top, top->members.data()); // alias
    this->store = std::move(val);
}

Value::Value(const std::shared_ptr<const impl::FieldDesc>& desc, Value& parent)
    :Value(desc)
{
    store->top->enclosing = parent.store;
}

Value::~Value() {}

Value Value::cloneEmpty() const
{
    Value ret;
    if(desc) {
        decltype (store->top->desc) fld(store->top->desc, desc);
        ret = Value(fld);
    }
    return ret;
}

Value Value::clone() const
{
    Value ret;
    if(desc) {
        decltype (store->top->desc) fld(store->top->desc, desc);
        ret = Value(fld);
        ret.assign(*this);
    }
    return ret;
}

Value& Value::assign(const Value& o)
{
    if(!store || !o.store)
        throw std::logic_error("Can't assign() to/from empty Value");

    if(type().kind()==Kind::Compound) {
        // pass through Struct and others w/ type
        copyIn(&o, StoreType::Compound);
    } else {
        // unpack other field types
        copyIn(o.store.get(), o.store->code);
    }
    return *this;
}

Value Value::allocMember()
{
    // allocate member type for Struct[] or Union[]
    if(!desc || (desc->code!=TypeCode::UnionA && desc->code!=TypeCode::StructA))
        throw std::runtime_error("allocMember() only meaningful for Struct[] or Union[]");

    decltype (store->top->desc) fld(store->top->desc, desc->members.data());
    return Value::Helper::build(fld, *this);
}

void Value::clear()
{
    if(!desc)
        return;

    for(auto i : range(size_t(0u), desc->size())) {
        auto& s = store.get()[i];
        s.valid = false;

        switch(s.code) {
        case StoreType::Array:
            s.as<shared_array<const void>>().clear();
            break;
        case StoreType::Compound:
        {
            auto& v = s.as<Value>();
            v.desc = nullptr;
            v.store.reset();
        }
            break;
        case StoreType::String:
            s.as<std::string>().clear();
            break;
        case StoreType::Null:
            break; // nothing to do
        case StoreType::Bool:
        case StoreType::UInteger:
        case StoreType::Integer:
        case StoreType::Real:
            memset(&s.store, 0, sizeof(s.store)); // just zero
            break;
        }
    }
}

bool Value::isMarked(bool parents, bool children) const
{
    if(!desc)
        return false;

    if(store->valid)
        return true;

    auto top = store->top;

    if(children && desc->size()>1u) {
        // TODO more efficient
        for(auto bit : range(desc->size()))
        {
            auto cstore = store.get() + bit;
            if(cstore->valid)
                return true;
        }
    }

    if(parents) {
        auto pdesc = desc;
        auto pstore = store.get();
        while(pdesc!=top->desc.get()) {
            pstore -= pdesc->parent_index;
            pdesc -= pdesc->parent_index;

            if(pstore->valid)
                return true;
        }
    }

    return false;
}

Value Value::ifMarked(bool parents, bool children) const
{
    Value ret;
    if(isMarked(parents, children))
        ret = *this;
    return ret;
}

void Value::mark(bool v)
{
    if(!desc)
        return;

    store->valid = v;
    if(!v)
        return;

    auto top = store->top;
    std::shared_ptr<FieldStorage> enc;
    while(top && (enc=top->enclosing.lock())) {
        enc->valid = true;
        top = enc->top;
    }
}

void Value::unmark(bool parents, bool children)
{
    if(!desc)
        return;

    store->valid = false;

    auto top = store->top;

    if(children && desc->size()>1u) {
        // TODO more efficient
        for(auto bit : range(desc->size()))
        {
            (store.get() + bit)->valid = false;
        }
    }

    if(parents) {
        auto pdesc = desc;
        auto pstore = store.get();
        while(pdesc!=top->desc.get()) {
            pstore -= pdesc->parent_index;
            pdesc -= pdesc->parent_index;

            pstore->valid = false;
        }
    }
}

TypeCode Value::type() const
{
    return desc ? desc->code : TypeCode::Null;
}

StoreType Value::storageType() const
{
    return store ? store->code : StoreType::Null;
}

const std::string& Value::id() const
{
    if(!desc)
        throw std::runtime_error("Null Value");
    return desc->id;
}

bool Value::idStartsWith(const std::string& prefix) const
{
    auto ID = this->id();
    return ID.size()>=prefix.size() && prefix==ID.substr(0u, prefix.size());
}

bool Value::_equal(const impl::FieldDesc* A, const impl::FieldDesc* B)
{
    if(A==B) {
        return true;

    } else if(!A ^ !B) {
        return false;

    } else if(!A) { // !A && !B
        return true;

    } else if(A->size()!=B->size()) {
        return false;
    }

    for(auto i : range(A->size())) {
        if(A[i].code!=B[i].code)
            return false;

        if(A[i].code==TypeCode::StructA || A[i].code==TypeCode::UnionA) {
            if(!_equal(&A[i].members[0], &B[i].members[0]))
                return false;

        } else if(A[i].code==TypeCode::Struct || A[i].code==TypeCode::Union) {
            auto it = A[i].mlookup.begin();
            auto end= A[i].mlookup.end();
            auto it2= B[i].mlookup.begin();

            for(;it!=end; ++it, ++it2) {
                if(it->first!=it2->first) {
                    return false; // different field name

                } else if(it->second!=it2->second) {
                    return false; // different field order

                } else if(A[i].code==TypeCode::Union) {
                    if(!_equal(&A[i].members[it->second], &B[i].members[it2->second]))
                        return false;

                } // else if A[i] is Struct, outer loop will reach members
            }
        }
    }

    return true;
}

const std::string &Value::nameOf(const Value& descendant) const
{
    if(!store || !descendant.store)
        throw NoField();

    size_t doffset;
    if(desc->code==TypeCode::Struct) {
        doffset = descendant.desc - desc;
        if(doffset==0 || doffset > desc->mlookup.size())
            throw std::logic_error("not a descendant");

    } else if(desc->code==TypeCode::Union) {
        doffset = descendant.desc - desc->members.data();

    } else {
        throw std::logic_error("nameOf() only implemented for Struct and Union");
    }

    // inefficient, but we don't keep a reverse mapping
    for(auto& it : desc->mlookup) {
        if(it.second == doffset)
            return it.first;
    }

    throw std::logic_error("missing descendant");
}

namespace {
// C-style cast between scalar storage types, and print to string (base 10)
template<typename Src>
bool copyOutScalar(const Src& src, void *ptr, StoreType type)
{
    switch(type) {
    case StoreType::Real:     *reinterpret_cast<double*>(ptr) = double(src); return true;
    case StoreType::Integer:  *reinterpret_cast<int64_t*>(ptr) = int64_t(src); return true;
    case StoreType::UInteger: *reinterpret_cast<uint64_t*>(ptr) = uint64_t(src); return true;
    case StoreType::Bool:     *reinterpret_cast<bool*>(ptr) = bool(src); return true;
    case StoreType::String:   *reinterpret_cast<std::string*>(ptr) = SB()<<src; return true;
    case StoreType::Null:
    case StoreType::Compound:
    case StoreType::Array:
        break;
    }
    return false;
}
}

void Value::copyOut(void *ptr, StoreType type) const
{
    if(!desc)
        throw NoField();

    switch(store->code) {
    case StoreType::Real:     if(copyOutScalar(store->as<double>(), ptr, type)) return; else break;
    case StoreType::Integer:  if(copyOutScalar(store->as<int64_t>(), ptr, type)) return; else break;
    case StoreType::UInteger: if(copyOutScalar(store->as<uint64_t>(), ptr, type)) return; else break;
    case StoreType::Bool: {
        auto& src = store->as<bool>();

        switch(type) {
        case StoreType::Bool:     *reinterpret_cast<bool*>(ptr) = src; return;
        case StoreType::Integer:
        case StoreType::UInteger: *reinterpret_cast<uint64_t*>(ptr) = src; return;
        case StoreType::Real:     *reinterpret_cast<double*>(ptr) = src; return;
        case StoreType::String:   *reinterpret_cast<std::string*>(ptr) = src ? "true" : "false"; return;
        default:
            break;
        }
        break;
    }
    case StoreType::String: {
        auto& src = store->as<std::string>();

        switch(type) {
        case StoreType::String: *reinterpret_cast<std::string*>(ptr) = src; return;
        case StoreType::Integer: {
            *reinterpret_cast<int64_t*>(ptr) = parseTo<int64_t>(src);
            return;
        }
        case StoreType::UInteger: {
            *reinterpret_cast<uint64_t*>(ptr) = parseTo<uint64_t>(src);
            return;
        }
        case StoreType::Real: {
            *reinterpret_cast<double*>(ptr) = parseTo<double>(src);
            return;
        }
        case StoreType::Bool: {
            if(src=="true") { *reinterpret_cast<bool*>(ptr) = true; return; }
            else if(src=="false") { *reinterpret_cast<bool*>(ptr) = false; return; }
        }
        default:
            break;
        }
        break;
    }
    case StoreType::Array: {
        auto& src = store->as<shared_array<const void>>();
        switch (type) {
        case StoreType::Array: *reinterpret_cast<shared_array<const void>*>(ptr) = src; return;
            // TODO: print array
            //       extract [0] as scalar?
        default:
            break;
        }
        break;
    }
    case StoreType::Compound: {
        auto& src = store->as<Value>();
        if(type==StoreType::Compound) {
            // extract Value
            *reinterpret_cast<Value*>(ptr) = src;
            return;

        } else if(src) {
            // automagic deref and delegate assign
            src.copyOut(ptr, type);
            return;

        }

        break;
    }
    case StoreType::Null:
        if(desc->id=="enum_t") {
            // special case handling for NTEnum
            auto index((*this).lookup("index"));
            switch(type) {
            case StoreType::Integer:
                *reinterpret_cast<int64_t*>(ptr) = index.as<int64_t>();
                return;

            case StoreType::UInteger:
                *reinterpret_cast<uint64_t*>(ptr) = index.as<uint64_t>();
                return;

            case StoreType::String: {
                auto idx = index.as<uint64_t>();
                auto choices = (*this).lookup("choices").as<shared_array<const std::string>>();
                if(idx < choices.size()) {
                    *reinterpret_cast<std::string*>(ptr) = choices[idx];
                    return;

                } else {
                    throw NoConvert(SB()<<"enum_t index "<<index<<" out of range for "<<choices.size());
                }
            }
            default:
                break;
            }
        }
        break;
    }

    throw NoConvert(SB()<<"Can't extract "<<this->type()<<" as "<<type);
}

bool Value::tryCopyOut(void *ptr, StoreType type) const
{
    try {
        copyOut(ptr, type);
        return true;
    }catch(NoField&){
        return false;
    }catch(NoConvert&){
        return false;
    }
}

namespace {

// C-style cast between scalar storage types, and print to string (base 10)
template<typename Dest>
bool copyInScalar(Dest& dest, const void *ptr, StoreType type)
{
    switch(type) {
    case StoreType::Real:     dest = Dest(*reinterpret_cast<const double*>(ptr)); return true;
    case StoreType::Integer:  dest = Dest(*reinterpret_cast<const int64_t*>(ptr)); return true;
    case StoreType::UInteger: dest = Dest(*reinterpret_cast<const uint64_t*>(ptr)); return true;
    case StoreType::Bool:     dest = Dest(*reinterpret_cast<const bool*>(ptr)); return true;
    case StoreType::String:   dest = parseTo<Dest>(*reinterpret_cast<const std::string*>(ptr)); return true;
    case StoreType::Null:
    case StoreType::Compound:
    case StoreType::Array:
        break;
    }
    return false;
}
}

void Value::copyIn(const void *ptr, StoreType type)
{
    // control flow should either throw NoField or NoConvert, or update 'store' and
    // reach the mark() at the end.

    if(!desc)
        throw NoField();

    switch(store->code) {
    case StoreType::Real: {
        if(!copyInScalar(store->as<double>(), ptr, type)) throw NoConvert(SB()<<"Unable to assign "<<desc->code<<" with "<<type);
        // truncate as if assigned to narrower type
        if(desc->code==TypeCode::Float32)
            store->as<double>() = float(store->as<double>());
        break;
    }
    case StoreType::Integer: {
        if(!copyInScalar(store->as<int64_t>(), ptr, type)) throw NoConvert(SB()<<"Unable to assign "<<desc->code<<" with "<<type);
        // truncate as if assigned to narrower type
        int64_t orig = store->as<int64_t>();
        switch(desc->code.code) {
        case TypeCode::Int8: orig = int8_t(orig); break;
        case TypeCode::Int16: orig = int16_t(orig); break;
        case TypeCode::Int32: orig = int32_t(orig); break;
        case TypeCode::Int64: orig = int64_t(orig); break;
        default: break;
        }
        store->as<int64_t>() = orig;
        break;
    }
    case StoreType::UInteger: {
        if(!copyInScalar(store->as<uint64_t>(), ptr, type)) throw NoConvert(SB()<<"Unable to assign "<<desc->code<<" with "<<type);
        // truncate as if assigned to narrower type
        uint64_t orig = store->as<uint64_t>();
        switch(desc->code.code) {
        case TypeCode::UInt8: orig = uint8_t(orig); break;
        case TypeCode::UInt16: orig = uint16_t(orig); break;
        case TypeCode::UInt32: orig = uint32_t(orig); break;
        case TypeCode::UInt64: orig = uint64_t(orig); break;
        default: break;
        }
        store->as<uint64_t>() = orig;
        break;
    }
    case StoreType::Bool: {
        auto& dest = store->as<bool>();
        switch(type) {
        case StoreType::Bool:     dest = *reinterpret_cast<const bool*>(ptr); break;
        case StoreType::Integer:
        case StoreType::UInteger: dest = 0!=*reinterpret_cast<const uint64_t*>(ptr); break;
        //case StoreType::Real:  // TODO pick condition.  strict non-zero?  fabs()<0.5 ?
        case StoreType::String:
            if("true"==*reinterpret_cast<const std::string*>(ptr)) { dest = true; break; }
            else if("false"==*reinterpret_cast<const std::string*>(ptr)) { dest = false; break; }
            // fall through
        default:
            throw NoConvert(SB()<<"Unable to assign "<<desc->code<<" with "<<type);
        }
        break;
    }
    case StoreType::String: {
        auto& dest = store->as<std::string>();

        switch(type) {
        case StoreType::String:   dest = *reinterpret_cast<const std::string*>(ptr); break;
        case StoreType::Integer:  dest = SB()<<*reinterpret_cast<const int64_t*>(ptr); break;
        case StoreType::UInteger: dest = SB()<<*reinterpret_cast<const uint64_t*>(ptr); break;
        case StoreType::Real:     dest = SB()<<*reinterpret_cast<const double*>(ptr); break;
        case StoreType::Bool:     dest = (*reinterpret_cast<const bool*>(ptr)) ? "true" : "false"; break;
        default:
            throw NoConvert(SB()<<"Unable to assign "<<desc->code<<" with "<<type);
        }
        break;
    }
    case StoreType::Array: {
        auto& dest = store->as<shared_array<const void>>();
        switch (type) {
        case StoreType::Array: {
            auto& src = *reinterpret_cast<const shared_array<const void>*>(ptr);
            if(src.original_type()==ArrayType::Null || src.empty()) {
                // assignment from untyped or empty
                dest.clear();

            } else if(src.original_type()==ArrayType::Value && desc->code.kind()==Kind::Compound) {
                // assign array of Struct/Union/Any
                auto tsrc  = src.castTo<const Value>();

                bool convert = false;
                if(desc->code!=TypeCode::AnyA) {
                    // enforce member type for Struct[] and Union[]
                    for(auto& val : tsrc) {
                        if(val.desc && val.desc!=desc->members.data()) {
                            convert = true;
                            break;
                        }
                    }
                    if(convert) {
                        shared_array<Value> scratch(tsrc.size());
                        for(auto i : range(tsrc.size())) {
                            if(tsrc[i]) {
                                scratch[i] = allocMember();
                                scratch[i].assign(tsrc[i]);
                            }
                        }
                        dest = scratch.freeze().castTo<const void>();
                    }
                }
                if(!convert)
                    dest = src;

            } else if(src.original_type()!=ArrayType::Value && uint8_t(desc->code.code)==uint8_t(src.original_type())) {
                // assign array of scalar w/o convert
                dest = src;

            } else if(src.original_type()!=ArrayType::Value) {
                // assign array of scalar w/ implicit conversion
                dest = detail::copyAs(desc->code.arrayType(), src.original_type(), src.data(), src.size()).freeze();

            } else {
                throw NoConvert(SB()<<"Unable to assign "<<desc->code<<" with "<<type);
            }
            break;
        }
        default:
            throw NoConvert(SB()<<"Unable to assign "<<desc->code<<" with "<<type);
        }
        break;
    }
    case StoreType::Compound:
        if(type==StoreType::Null) {
            store->as<Value>() = Value(); // unselect Union or Any
            break;

        } else if(desc->code==TypeCode::Any) {
            // assigning variant union.
            auto& val = store->as<Value>();
            if(type==StoreType::Compound) {
                auto& newval = *reinterpret_cast<const Value*>(ptr);
                if(store == newval.store)
                    throw std::logic_error("Any self-assignment would recurse.  Not allowed.");
                val = newval;
                break;

            } else {
                val = Value::Helper::build(ptr, type);
                break;
            }

        } else if(desc->code==TypeCode::Union) {
            auto& val = store->as<Value>();
            if(type==StoreType::Compound) {
                // assign union from Value.  (eg. during Value::clone())
                // select and assign
                auto& src = *reinterpret_cast<const Value*>(ptr);
                for(auto i : range(desc->miter.size())) {
                    auto idx(desc->miter[i].second);

                    if(!_equal(src.desc, &desc->members[idx]))
                        continue;

                    std::shared_ptr<const FieldDesc> udesc(store->top->desc, &desc->members[idx]);
                    Value temp(udesc, *this);
                    temp.assign(src);
                    val = std::move(temp);
                    break;
                }
                if(!val)
                    throw NoConvert("Unsupported assignment to unselected union");
                break;

            } else if(!val) {
                // caller is attempting to assign a value to an unselected discriminating union.
                // attempt convenient, but inefficient auto-selection
                for(auto i : range(desc->miter.size())) {
                    auto idx(desc->miter[i].second);
                    std::shared_ptr<const FieldDesc> udesc(store->top->desc, &desc->members[idx]);
                    Value temp(udesc, *this);
                    try{
                        temp.copyIn(ptr, type);
                    }catch(NoConvert&){
                        continue;
                    }
                    val = std::move(temp);
                    break;
                }
                if(!val)
                    throw NoConvert("Unsupported assignment to unselected union");
                break;

            } else {
                // union member already selected, auto-deref
                val.copyIn(ptr, type);
                break;
            }
        }
        throw NoConvert(SB()<<"Unable to assign "<<desc->code<<" with "<<type);
    case StoreType::Null:
        if(type==StoreType::Compound) {
            auto& src = *reinterpret_cast<const Value*>(ptr);
            if(src.type()==TypeCode::Struct) {
                // copy struct to struct
                // all marked source field may be mapped to destination fields

                for(const auto& sfld : src.imarked()) {
                    if(sfld.type()==TypeCode::Struct) {
                        // entire sub-struct marked.

                        if(auto dfld = (*this)[src.nameOf(sfld)]) {
                            dfld.mark();
                        }

                    } else {
                        auto& name(src.nameOf(sfld));
                        if(auto dfld = (*this)[name]) {
                            try {
                                dfld.copyIn(&sfld.store->store, sfld.store->code);
                            }catch(NoConvert& e){
                                throw NoConvert(SB()<<"field \""<<name<<"\" : "<<e.what());
                            }
                        } else {
                            throw NoField();
                        }
                    }
                }
                if(src.isMarked())
                    mark();

                return;
            }
        } else if(desc->id=="enum_t") {
            auto index((*this).lookup("index"));
            switch(type) {
            case StoreType::Integer:
                index = *reinterpret_cast<const int64_t*>(ptr);
                return;
            case StoreType::UInteger:
                index = *reinterpret_cast<const uint64_t*>(ptr);
                return;
            case StoreType::String: {
                auto& choice(*reinterpret_cast<const std::string*>(ptr));
                auto choices((*this).lookup("choices").as<shared_array<const std::string>>());
                for(auto idx : range(choices.size())) {
                    if(choice == choices[idx]) {
                        index = idx;
                        return;
                    }
                }
                // try to parse as string
                index = parseTo<uint64_t>(choice);
                return;
            }
                break;
            default:
                break;
            }
        }
        throw NoConvert(SB()<<"Unable to assign "<<desc->code<<" with "<<type);
    }

    mark();
}

bool Value::tryCopyIn(const void *ptr, StoreType type)
{
    try {
        copyIn(ptr, type);
        return true;
    }catch(NoField&){
        return false;
    }catch(NoConvert&){
        return false;
    }
}

void Value::traverse(const std::string &expr, bool modify, bool dothrow)
{
    size_t pos=0;
    bool maybedot = false;

    while(desc && pos<expr.size()) {
        if(expr[pos]=='<') {
            // attempt traverse to parent
            if(desc!=store->top->desc.get())
            {
                auto pdesc = desc - desc->parent_index;
                std::shared_ptr<FieldStorage> pstore(store, store.get() - desc->parent_index);
                store = std::move(pstore);
                desc = pdesc;
                pos++;
                continue;
            } else {
                // at top
                store.reset();
                desc = nullptr;
                if(dothrow)
                    throw LookupError(SB()<<"Can't traverse to parent of root with '"<<expr<<"'");
                break;
            }

        }

        if(desc->code.code==TypeCode::Struct) {
            // attempt traverse to member.
            // expect: [0-9a-zA-Z_.]+[\[-$]

            // skip a leading dot
            if(maybedot) {
                if(expr[pos]!='.') {
                    store.reset();
                    desc = nullptr;
                    if(dothrow)
                        throw LookupError(SB()<<"expected '.' at "<<pos<<" in '"<<expr<<"'");
                    break;
                }
                maybedot = false;
                pos++;
            }

            size_t sep = expr.find_first_of("<[-", pos);

            decltype (desc->mlookup)::const_iterator it;

            const auto& name = expr.substr(pos, sep-pos);

            if(sep>0 && (it=desc->mlookup.find(name))!=desc->mlookup.end()) {
                // found it
                auto next = desc+it->second;
                decltype(store) value(store, store.get()+it->second);
                store = std::move(value);
                desc = next;
                pos = sep;

            } else {
                // no such member
                store.reset();
                desc = nullptr;
                if(dothrow) {
                    SB msg;
                    msg<<"no such member field '"<<name<<"'";
                    if(name!=expr)
                        msg<<" in expression '"<<expr<<"'";
                    throw LookupError(msg);
                }
            }

        } else if(desc->code.code==TypeCode::Union || desc->code.code==TypeCode::Any) {
            // attempt to traverse to (and maybe select) member
            // expect: ->[0-9a-zA-Z_]+[.\[-$]

            if(expr.size()-pos >= 2 && expr[pos]=='-' && expr[pos+1]=='>') {
                pos += 2; // skip past "->"

            } else if(pos>0u || desc->code.code!=TypeCode::Union) {
                // expected "->"
                // allow omission at the beginning of an expression when starting from a Union
                store.reset();
                desc = nullptr;
                if(dothrow)
                    throw LookupError(SB()<<"expected -> in '"<<expr<<"'");
                break;
            }

            maybedot = false;

            if(desc->code.code==TypeCode::Any) {
                // select member of Any (may be Null)
                *this = store->as<Value>();

            } else {
                // select member of Union
                size_t sep = expr.find_first_of("<[-.", pos);

                decltype (desc->mlookup)::const_iterator it;
                auto& fld = store->as<Value>();

                if(sep>0 && (it=desc->mlookup.find(expr.substr(pos, sep-pos)))!=desc->mlookup.end()) {
                    // found it.

                    if(modify || fld.desc==&desc->members[it->second]) {
                        // will select, or already selected
                        if(fld.desc!=&desc->members[it->second]) {
                            // select
                            std::shared_ptr<const FieldDesc> mtype(store->top->desc, &desc->members[it->second]);
                            fld = Value(mtype, *this);
                        }
                        pos = sep;
                        *this = fld;
                        maybedot = true;

                    } else {
                        // traversing const Value, can't select Union
                        store.reset();
                        desc = nullptr;
                        if(dothrow)
                            throw LookupError(SB()<<"traversing const Value, can't select Union in '"<<expr<<"'");
                    }

                } else if(fld.desc) {
                    // deref selected
                    *this = fld;

                } else {
                    store.reset();
                    desc = nullptr;
                    if(dothrow)
                        throw LookupError(SB()<<"can't deref. empty Union '"<<expr<<"'");
                }
            }

        } else if(desc->code.isarray() && desc->code.kind()==Kind::Compound) {
            // attempt to traverse into array of Struct, Union, or Any
            // expect: \[[0-9]+\]

            maybedot = false;

            size_t sep = expr.find_first_of(']', pos);

            if(expr[pos]=='['
                    && sep!=std::string::npos && sep-pos>=2)
            {
                auto index = parseTo<uint64_t>(expr.substr(pos+1, sep-1-pos));
                auto& varr = store->as<shared_array<const void>>();
                shared_array<const Value> arr;
                if((varr.original_type()==ArrayType::Value)
                        && index < (arr = varr.castTo<const Value>()).size())
                {
                    *this = arr[index];
                    pos = sep+1;
                    maybedot = true;

                } else {
                    // wrong element type or out of range
                    store.reset();
                    desc = nullptr;
                    if(dothrow)
                        throw std::runtime_error(SB()<<"wrong element type or out of range in '"<<expr<<"'");
                }

            } else {
                // syntax error
                store.reset();
                desc = nullptr;
                if(dothrow)
                    throw std::runtime_error(SB()<<"indexing syntax error in '"<<expr<<"'");
            }

        } else {
            // syntax error or wrong field type (can't index scalar array)
            store.reset();
            desc = nullptr;
            if(dothrow)
                throw std::runtime_error(SB()<<"indexing syntax error or wrong field type (can't index scalar array) in '"<<expr<<"'");
        }
    }

    if(!desc && dothrow)
        throw NoField();
}

Value Value::operator[](const std::string& name)
{
    Value ret(*this);
    ret.traverse(name, true, false);
    return ret;
}

const Value Value::operator[](const std::string& name) const
{
    Value ret(*this);
    ret.traverse(name, false, false);
    return ret;
}

Value Value::lookup(const std::string& name)
{
    Value ret(*this);
    ret.traverse(name, true, true);
    return ret;
}

const Value Value::lookup(const std::string& name) const
{
    Value ret(*this);
    ret.traverse(name, false, true);
    return ret;
}

size_t Value::nmembers() const
{
    switch(desc ? desc->code.code : TypeCode::Null) {
    case TypeCode::Struct:
    case TypeCode::Union:
        return desc->miter.size();
    case TypeCode::StructA:
    case TypeCode::UnionA:
        return desc->members[0].miter.size();
    default:
        return 0u;
    }
}

template<>
Value::Iterable<Value::_IAll>::iterator
Value::Iterable<Value::_IAll>::end() const noexcept
{
    iterator ret{val, 0u};

    if(val && val.type()==TypeCode::Struct) {
        ret.pos = val.desc->mlookup.size();

    } else if(val && val.type()==TypeCode::Union) {
        ret.pos = val.desc->miter.size();
    }
    return ret;
}


template<>
Value
Value::_Iterator<Value::_IAll>::operator*() const noexcept
{
    Value ret;

    if(val.type()==TypeCode::Struct) {
        decltype (ret.store) store(val.store, val.store.get() + 1u + pos);
        ret.store = std::move(store);
        ret.desc = val.desc + 1u + pos;

    } else if(val && val.type()==TypeCode::Union) {
        auto pos_desc = &val.desc->members[val.desc->miter[pos].second];

        if(val.store->as<Value>().desc==pos_desc) {
            // pointing to selected Union field
            ret = val.store->as<Value>();

        } else {
            std::shared_ptr<const FieldDesc> base(val.store, pos_desc);
            ret = Value(base);
        }
    }
    return ret;
}

template<>
Value::Iterable<Value::_IChildren>::iterator
Value::Iterable<Value::_IChildren>::end() const noexcept
{
    iterator ret{val, 0u};

    if(val && (val.type()==TypeCode::Struct || val.type()==TypeCode::Union)) {
        ret.pos = val.desc->miter.size();
    }
    return ret;
}

template<>
Value
Value::_Iterator<Value::_IChildren>::operator*() const noexcept
{
    auto offset = val.desc->miter[pos].second;
    Value ret;

    if(val.type()==TypeCode::Struct) {
        decltype (ret.store) store(val.store, val.store.get() + offset);
        ret.store = std::move(store);
        ret.desc = val.desc + offset;

    } else if(val && val.type()==TypeCode::Union) {
        auto pos_desc = &val.desc->members[val.desc->miter[pos].second];

        if(val.store->as<Value>().desc==pos_desc) {
            // pointing to selected Union field
            ret = val.store->as<Value>();

        } else {
            std::shared_ptr<const FieldDesc> base(val.store, pos_desc);
            ret = Value(base);
        }
    }
    return ret;
}

static
void _next_marked(const Value& ref, size_t& pos, size_t& nextcheck)
{
    if(pos < nextcheck)
        return;

    if(ref.type()==TypeCode::Struct) {
        auto base_desc = Value::Helper::desc(ref);

        while(pos < base_desc->mlookup.size()) {
            auto desc = base_desc + 1u + pos;
            auto S = Value::Helper::store_ptr(ref) + 1u + pos;
            if(S->valid) {
                nextcheck = pos + desc->size();
                return;
            }

            ++pos;
        }
        nextcheck = pos;

    } else if(ref.type()==TypeCode::Union) {
        auto desc = Value::Helper::desc(ref);

        if(pos >= desc->miter.size())
            return; // end of iteration

        const auto& val = Value::Helper::store_ptr(ref)->as<Value>();
        size_t sel_idx = Value::Helper::desc(val) - desc->members.data();
        size_t pos_idx = desc->miter[pos].second;

        if(!val || pos_idx > sel_idx) {
            // no field selected, or pos is after selection
            // end of iteration

            pos = desc->miter.size();

        } else if(pos_idx < sel_idx) {
            // before selected
            // jump forward to selection

            for(auto i : range(pos, desc->miter.size())) {
                if(desc->miter[i].second == sel_idx) {
                    pos = i;
                    return;
                }
            }
            assert(false); // corrupt iterator?
        }
    }
}

template<>
Value::Iterable<Value::_IMarked>::iterator
Value::Iterable<Value::_IMarked>::begin() const noexcept
{
    iterator ret{val, 0u};
    _next_marked(ret.val, ret.pos, ret.nextcheck);
    return ret;
}

template<>
Value::Iterable<Value::_IMarked>::iterator
Value::Iterable<Value::_IMarked>::end() const noexcept
{
    iterator ret{val, 0u};

    if(val && val.type()==TypeCode::Struct) {
        ret.pos = val.desc->mlookup.size();

    } else if(val && val.type()==TypeCode::Union) {
        ret.pos = val.desc->miter.size();
    }
    ret.nextcheck = ret.pos;
    return ret;
}

template<>
Value
Value::_Iterator<Value::_IMarked>::operator*() const noexcept
{
    Value ret;

    if(val.type()==TypeCode::Struct) {
        decltype (ret.store) store(val.store, val.store.get() + 1u + pos);
        ret.store = std::move(store);
        ret.desc = val.desc + 1u + pos;

    } else if(val && val.type()==TypeCode::Union) {
        auto pos_desc = &val.desc->members[val.desc->miter[pos].second];

        if(val.store->as<Value>().desc==pos_desc) {
            // pointing to selected Union field
            ret = val.store->as<Value>();

        } else {
            std::shared_ptr<const FieldDesc> base(val.store, pos_desc);
            ret = Value(base);
        }
    }
    return ret;
}

template<>
Value::_Iterator<Value::_IMarked>&
Value::_Iterator<Value::_IMarked>::operator++() noexcept
{
    pos++;
    _next_marked(val, pos, nextcheck);
    return *this;
}

void cache_sync(Value& cache, Value& dlt)
{
    if(!cache.equalType(dlt))
        throw std::logic_error(SB()<<__func__<<" requires matching types");

    /* co-iterate data and prototype.
     * copy   marked from data -> prototype
     * copy unmarked from prototype -> data
     */
    auto desc = Value::Helper::desc(cache);
    auto delta = Value::Helper::store_ptr(dlt);
    auto complete = Value::Helper::store_ptr(cache);
    auto N = desc->size();
    for(size_t i=0u; i < N; i++, delta++, complete++)
    {
        const auto src = delta->valid ? delta : complete;
        auto       dst = delta->valid ? complete : delta;

        switch(delta->code) {
        case StoreType::Null:
            break;
        case StoreType::Bool:
        case StoreType::UInteger:
        case StoreType::Integer:
        case StoreType::Real:
            memcpy(&dst->store, &src->store, sizeof(src->store));
            break;
        case StoreType::String:
            dst->as<std::string>() = src->as<std::string>();
            break;
        case StoreType::Array:
            dst->as<shared_array<const void>>() = src->as<shared_array<const void>>();
            break;
        case StoreType::Compound:
            dst->as<Value>() = src->as<Value>().clone();
            break;
        }
    }
}

namespace impl {

void FieldStorage::init(StoreType code)
{
    this->code = code;
    switch(code) {
    case StoreType::Null:
        return;
    case StoreType::Bool:
        as<bool>() = false;
        return;
    case StoreType::Integer:
    case StoreType::UInteger:
    case StoreType::Real:
        // just zero 8 bytes
        as<uint64_t>() = 0u;
        return;
    case StoreType::String:
        new(&store) std::string();
        return;
    case StoreType::Compound:
        new(&store) Value();
        return;
    case StoreType::Array:
        new(&store) shared_array<void>();
        return;
    }
    throw std::logic_error("FieldStore::init()");
}

void FieldStorage::deinit()
{
    switch(code) {
    case StoreType::Null:
    case StoreType::Integer:
    case StoreType::UInteger:
    case StoreType::Real:
    case StoreType::Bool:
             break;
    case StoreType::Array:
        as<shared_array<void>>().~shared_array();
        break;
    case StoreType::String:
        as<std::string>().~basic_string();
        break;
    case StoreType::Compound:
        as<Value>().~Value();
        break;
    default:
        throw std::logic_error("FieldStore::deinit()");
    }
    code = StoreType::Null;
}

FieldStorage::~FieldStorage()
{
    deinit();
}

size_t FieldStorage::index() const
{
    const size_t ret = this - top->members.data();
    return ret;
}

}} // namespace pvxs::impl
